/*
 * thumbguard - keeps runaway QuickLook thumbnail processes under control.
 *
 * Background: macOS generates thumbnails through extensions in
 * /System/Library/ExtensionKit/Extensions/ (WebThumbnailExtension.appex and
 * siblings). WebThumbnailExtension can be driven into endless loops by HTML
 * files that reference external resources; its WebKit render processes
 * (com.apple.WebKit.WebContent.*) are then left orphaned, spinning at ~100 %
 * CPU, and they pile up. OfficeThumbnailExtension does the same on certain
 * .xlsm files.
 *
 * These extensions run through ExtensionKit, not PlugInKit - which is why the
 * commonly recommended "pluginkit -e disable" has no effect on them.
 *
 * How it works: one sysctl call per tick returns the process list. Only name
 * matches are inspected further, and every process is classified exactly once.
 * A render process is considered a target when its responsible process (the
 * same value Activity Monitor uses to build the name "WebThumbnailExtension
 * Web Content") is a thumbnail extension - Safari tabs use the same binary and
 * are therefore left alone.
 *
 * Processes are killed based on sustained CPU load rather than brief spikes, so
 * ordinary thumbnails are still produced. Repeat offenders are put on a
 * temporary block list and suspended on sight, which breaks the restart loop of
 * the ThumbnailsAgent.
 */

#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* Private but stable for many years: returns the responsible process of an XPC
 * child. Should it ever disappear, only linking fails, never the running code. */
extern int responsibility_get_pid_responsible_for_pid(pid_t);

#define TG_VERSION "1.0"

#define MAX_TRACKED 256
#define MAX_OFFENDERS 16
#define LABEL_LEN 64

#ifndef SSTOP
#define SSTOP 4                 /* suspended process, see sys/proc.h */
#endif

/* Process classification. UNKNOWN is never cached. */
enum { CLS_UNKNOWN = -1, CLS_INNOCENT = 0, CLS_TARGET = 1, CLS_FROZEN = 2 };

typedef struct {
    pid_t pid;
    int64_t started;      /* p_starttime in us - detects recycled PIDs */
    uint64_t cpu;         /* CPU time in mach units */
    uint64_t stamp;       /* mach_absolute_time of that sample */
    int cls;
    int strikes;
    int seen;
    time_t frozen_at;
    char label[LABEL_LEN];
} track_t;

/* Repeat offenders: killed too often -> suspend on sight for a while. */
typedef struct {
    char label[LABEL_LEN];
    int kills;
    time_t window_start;
    time_t blocked_until;
} offender_t;

static track_t g_track[MAX_TRACKED];
static int g_track_n;
static offender_t g_off[MAX_OFFENDERS];
static int g_off_n;

static volatile sig_atomic_t g_stop;

/* Settings */
static int opt_interval_ms = 1000;
static double opt_cpu_pct = 50.0;
static int opt_strikes = 3;
static int opt_block_mode;      /* 1 = suspend every target on sight */
static int opt_dry_run;
static int opt_foreground;
static int opt_kill_window = 120;   /* s: window for counting repeat offences */
static int opt_kill_limit = 3;      /* kills within the window until blocking */
static int opt_block_secs = 600;    /* s: how long a block lasts */
static char opt_log[PATH_MAX];
static int opt_all_ext;             /* 1 = watch every thumbnail extension */
static int opt_freeze_secs = 20;    /* s: hold time before a suspended one dies */

/* Only these extensions are watched. TextThumbnailExtension and
 * ImageThumbnailExtension behave well and are allowed to work longer when many
 * files show up at once, so they are deliberately not on the list. */
static char opt_watch[512] = "WebThumbnailExtension,OfficeThumbnailExtension";

static long g_kills;

/* ------------------------------------------------------------------ Logging */

static void log_rotate(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 256 * 1024) {
        char old[PATH_MAX];
        snprintf(old, sizeof(old), "%s.1", path);
        rename(path, old);
    }
}

static void logmsg(const char *fmt, ...)
{
    char line[1024];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    int n = (int)strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S ", &tm);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    strlcat(line, "\n", sizeof(line));

    if (opt_foreground) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (opt_log[0]) {
        log_rotate(opt_log);
        FILE *f = fopen(opt_log, "a");
        if (f) {
            fputs(line, f);
            fclose(f);
        }
    }
}

/* --------------------------------------------------------- Process inspection */

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Cheap pre-filter on the short name, which comes for free with the scan. */
static int comm_candidate(const char *comm)
{
    return strstr(comm, "Thumbnail") != NULL ||
           strncmp(comm, "com.apple.WebKit", 16) == 0;
}

/* Any thumbnail extension? The ThumbnailsAgent itself does not live under
 * /ExtensionKit/ and is therefore excluded - it needs to keep running. */
static int is_thumb_extension(const char *path)
{
    return strstr(path, "/ExtensionKit/Extensions/") != NULL &&
           strstr(path, "Thumbnail") != NULL &&
           strstr(path, ".appex/") != NULL;
}

/* Is this extension on the watch list? */
static int is_watched_extension(const char *path)
{
    if (!is_thumb_extension(path))
        return 0;
    if (opt_all_ext)
        return 1;

    for (const char *p = opt_watch; *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char needle[160];
        if (len > 0 && len + 9 < sizeof(needle)) {
            /* Match the appex name exactly, not just anywhere in the path. */
            memcpy(needle, "/", 1);
            memcpy(needle + 1, p, len);
            memcpy(needle + 1 + len, ".appex/", 8);
            if (strstr(path, needle))
                return 1;
        }
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

static int classify(pid_t pid, pid_t ppid, const char *path,
                    char *label, size_t lsz)
{
    if (is_thumb_extension(path))
        return is_watched_extension(path)
                   ? (strlcpy(label, basename_of(path), lsz), CLS_TARGET)
                   : CLS_INNOCENT;

    if (strstr(path, "com.apple.WebKit.WebContent") == NULL)
        return CLS_INNOCENT;

    /* Render process: decide by its responsible process. */
    pid_t rpid = responsibility_get_pid_responsible_for_pid(pid);
    if (rpid <= 0)
        return CLS_UNKNOWN;             /* not set yet - try again next tick */
    if (rpid == pid)
        return CLS_INNOCENT;

    char rpath[PROC_PIDPATHINFO_MAXSIZE];
    rpath[0] = 0;
    if (proc_pidpath(rpid, rpath, sizeof(rpath)) > 0) {
        if (is_watched_extension(rpath)) {
            snprintf(label, lsz, "%s Web Content", basename_of(rpath));
            return CLS_TARGET;
        }
        return CLS_INNOCENT;            /* Safari, for instance - hands off */
    }

    /* The responsible process is gone: this render process is orphaned. With no
     * client left it cannot deliver anything - even a Safari tab would be dead
     * weight in this state. Restricted to PPID 1 (reparented to launchd) so
     * freshly spawned children are not caught by mistake. */
    if (ppid == 1) {
        strlcpy(label, "orphaned WebContent", lsz);
        return CLS_TARGET;
    }
    return CLS_UNKNOWN;
}

/* ---------------------------------------------------------------- Block list */

static offender_t *offender_for(const char *label)
{
    for (int i = 0; i < g_off_n; i++)
        if (strcmp(g_off[i].label, label) == 0)
            return &g_off[i];
    if (g_off_n < MAX_OFFENDERS) {
        offender_t *o = &g_off[g_off_n++];
        memset(o, 0, sizeof(*o));
        strlcpy(o->label, label, sizeof(o->label));
        return o;
    }
    return NULL;
}

static int is_blocked(const char *label, time_t now)
{
    offender_t *o = offender_for(label);
    return o && o->blocked_until > now;
}

static void note_kill(const char *label, time_t now)
{
    offender_t *o = offender_for(label);
    if (!o)
        return;
    if (o->window_start == 0 || now - o->window_start > opt_kill_window) {
        o->window_start = now;
        o->kills = 0;
    }
    o->kills++;
    if (o->kills >= opt_kill_limit && o->blocked_until <= now) {
        o->blocked_until = now + opt_block_secs;
        o->kills = 0;
        o->window_start = now;
        if (opt_block_secs >= 60)
            logmsg("BLOCK   %s - %d restarts within %ds, suspended on sight for %d min",
                   label, opt_kill_limit, opt_kill_window, opt_block_secs / 60);
        else
            logmsg("BLOCK   %s - %d restarts within %ds, suspended on sight for %ds",
                   label, opt_kill_limit, opt_kill_window, opt_block_secs);
    }
}

/* --------------------------------------------------------------- Tracking */

static track_t *track_find(pid_t pid, int64_t started)
{
    for (int i = 0; i < g_track_n; i++)
        if (g_track[i].pid == pid && g_track[i].started == started)
            return &g_track[i];
    return NULL;
}

static track_t *track_add(pid_t pid, int64_t started)
{
    if (g_track_n >= MAX_TRACKED)
        return NULL;
    track_t *t = &g_track[g_track_n++];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->started = started;
    t->cls = CLS_UNKNOWN;
    return t;
}

static void track_sweep(void)
{
    for (int i = 0; i < g_track_n;) {
        if (!g_track[i].seen)
            g_track[i] = g_track[--g_track_n];
        else
            g_track[i++].seen = 0;
    }
}

static uint64_t cpu_time_of(pid_t pid)
{
    struct proc_taskinfo pti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti)) != (int)sizeof(pti))
        return 0;
    return pti.pti_total_user + pti.pti_total_system;
}

static void do_kill(track_t *t, const char *why, double pct)
{
    if (opt_dry_run) {
        logmsg("DRY-RUN would kill PID %d (%s) - %s, %.0f %% CPU",
               t->pid, t->label, why, pct);
        t->strikes = 0;
        return;
    }
    if (kill(t->pid, SIGKILL) == 0) {
        g_kills++;
        logmsg("KILL    PID %-6d %-40s %s, %.0f %% CPU", t->pid, t->label, why, pct);
        note_kill(t->label, time(NULL));
    } else if (errno != ESRCH) {
        logmsg("ERROR   PID %d could not be killed: %s", t->pid, strerror(errno));
    }
    t->cls = CLS_INNOCENT;      /* do not touch again */
    t->strikes = 0;
}

/* Suspend instead of killing outright: a stopped process stops burning CPU, yet
 * the ThumbnailsAgent is still waiting for its reply and does not fire off the
 * next one a second later. After a short hold time it is removed. */
static void do_freeze(track_t *t, const char *why)
{
    if (opt_dry_run) {
        logmsg("DRY-RUN would suspend PID %d (%s) - %s", t->pid, t->label, why);
        return;
    }
    if (kill(t->pid, SIGSTOP) == 0) {
        t->cls = CLS_FROZEN;
        t->frozen_at = time(NULL);
        t->strikes = 0;
        logmsg("SUSPEND PID %-6d %-40s %s", t->pid, t->label, why);
    } else if (errno != ESRCH) {
        logmsg("ERROR   PID %d could not be suspended: %s", t->pid, strerror(errno));
        do_kill(t, why, 0.0);
    }
}

/* Never leave suspended processes behind on shutdown. */
static void release_frozen(void)
{
    int n = 0;
    for (int i = 0; i < g_track_n; i++) {
        if (g_track[i].cls == CLS_FROZEN) {
            kill(g_track[i].pid, SIGKILL);
            n++;
        }
    }
    if (n)
        logmsg("Cleanup: killed %d suspended process(es)", n);
}

static void scan(void)
{
    static struct kinfo_proc *buf;
    static size_t bufsz;

    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
        return;
    len += len / 4;                 /* headroom for processes starting meanwhile */
    if (len > bufsz) {
        void *nb = realloc(buf, len);
        if (!nb)
            return;
        buf = nb;
        bufsz = len;
    }
    if (sysctl(mib, 3, buf, &len, NULL, 0) < 0)
        return;

    int n = (int)(len / sizeof(struct kinfo_proc));
    uint64_t now_mach = mach_absolute_time();
    time_t now = time(NULL);
    char path[PROC_PIDPATHINFO_MAXSIZE];

    for (int i = 0; i < n; i++) {
        struct extern_proc *p = &buf[i].kp_proc;
        pid_t pid = p->p_pid;
        if (pid <= 1)
            continue;
        if (!comm_candidate(p->p_comm))
            continue;

        int64_t started = (int64_t)p->p_starttime.tv_sec * 1000000 +
                          p->p_starttime.tv_usec;

        track_t *t = track_find(pid, started);
        if (!t) {
            t = track_add(pid, started);
            if (!t)
                continue;
        }
        t->seen = 1;

        if (t->cls == CLS_INNOCENT)
            continue;

        if (t->cls == CLS_FROZEN) {
            if (now - t->frozen_at >= opt_freeze_secs) {
                kill(pid, SIGKILL);
                g_kills++;
                logmsg("KILL    PID %-6d %-40s after %ds hold time",
                       pid, t->label, opt_freeze_secs);
                t->cls = CLS_INNOCENT;
            }
            continue;
        }

        if (t->cls == CLS_UNKNOWN) {
            if (proc_pidpath(pid, path, sizeof(path)) <= 0)
                continue;
            t->cls = classify(pid, buf[i].kp_eproc.e_ppid, path,
                              t->label, sizeof(t->label));
            if (t->cls != CLS_TARGET)
                continue;
            logmsg("WATCH   PID %-6d %s", pid, t->label);

            /* Already suspended without us doing it: leftover from an earlier
             * run that was killed hard. Clear it out. */
            if (p->p_stat == SSTOP) {
                kill(pid, SIGKILL);
                g_kills++;
                logmsg("KILL    PID %-6d %-40s leftover, was suspended",
                       pid, t->label);
                t->cls = CLS_INNOCENT;
                continue;
            }
        }

        /* From here on: confirmed target. */
        if (opt_block_mode || is_blocked(t->label, now)) {
            do_freeze(t, opt_block_mode ? "block mode" : "blocked");
            continue;
        }

        uint64_t cpu = cpu_time_of(pid);
        if (cpu == 0)
            continue;

        if (t->stamp == 0) {            /* first sample: just remember it */
            t->cpu = cpu;
            t->stamp = now_mach;
            continue;
        }

        uint64_t dc = cpu - t->cpu;
        uint64_t dt = now_mach - t->stamp;
        t->cpu = cpu;
        t->stamp = now_mach;
        if (dt == 0)
            continue;

        /* CPU time and clock share the same mach unit, so their ratio is the
         * load - no timebase conversion needed. */
        double pct = 100.0 * (double)dc / (double)dt;

        if (pct >= opt_cpu_pct) {
            t->strikes++;
            if (t->strikes >= opt_strikes)
                do_kill(t, "sustained load", pct);
        } else {
            t->strikes = 0;
        }
    }

    track_sweep();
}

/* ---------------------------------------------------------------- Status */

static int cmd_status(void)
{
    /* Unbuffered so output of the helpers we invoke appears in the right order. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("thumbguard %s\n\n", TG_VERSION);

    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
        return 1;
    len += len / 4;
    struct kinfo_proc *buf = malloc(len);
    if (!buf)
        return 1;
    if (sysctl(mib, 3, buf, &len, NULL, 0) < 0) {
        free(buf);
        return 1;
    }

    int n = (int)(len / sizeof(struct kinfo_proc));
    char path[PROC_PIDPATHINFO_MAXSIZE], label[LABEL_LEN];
    int found = 0;

    printf("Watching: %s\n\n", opt_all_ext ? "all thumbnail extensions" : opt_watch);
    printf("Running thumbnail processes:\n");
    for (int i = 0; i < n; i++) {
        pid_t pid = buf[i].kp_proc.p_pid;
        if (pid <= 1 || !comm_candidate(buf[i].kp_proc.p_comm))
            continue;
        if (proc_pidpath(pid, path, sizeof(path)) <= 0)
            continue;

        label[0] = 0;
        int cls = classify(pid, buf[i].kp_eproc.e_ppid, path, label, sizeof(label));
        int watched = (cls == CLS_TARGET);
        if (!watched) {
            /* List unwatched thumbnail extensions as well. */
            if (!is_thumb_extension(path))
                continue;
            strlcpy(label, basename_of(path), sizeof(label));
        }

        struct proc_taskinfo pti;
        double secs = 0;
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti)) == (int)sizeof(pti)) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            secs = (double)(pti.pti_total_user + pti.pti_total_system) *
                   tb.numer / tb.denom / 1e9;
        }
        printf("  [%s] PID %-6d  CPU time %7.1fs  %s\n",
               watched ? "x" : " ", pid, secs, label);
        found++;
    }
    if (!found)
        printf("  none\n");
    else
        printf("  ([x] = watched)\n");
    free(buf);

    int rc = system("launchctl print gui/$UID/local.thumbguard >/dev/null 2>&1");
    printf("\nService: %s\n", rc == 0 ? "running" : "not loaded");

    struct stat lst;
    if (opt_log[0] && stat(opt_log, &lst) == 0 && lst.st_size > 0) {
        printf("\nRecent log entries from %s:\n", opt_log);
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd), "tail -n 12 '%s' | sed 's/^/  /'", opt_log);
        if (system(cmd) != 0)
            printf("  (not readable)\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ Startup */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(void)
{
    printf(
        "thumbguard %s - stops runaway QuickLook thumbnail processes\n\n"
        "Usage: thumbguard [options]\n\n"
        "  --status           show running target processes and service state\n"
        "  --foreground       run in the foreground, log to the console\n"
        "  --dry-run          touch nothing, only log what would happen\n"
        "  --block            block mode: suspend every target on sight\n"
        "                     (no more HTML/Office thumbnails)\n"
        "  --interval=MS      tick length in milliseconds (default %d)\n"
        "  --cpu=PERCENT      load at which a tick counts as busy (default %.0f)\n"
        "  --strikes=N        consecutive busy ticks before acting (default %d)\n"
        "  --watch=A,B        extensions to watch (default:\n"
        "                     %s)\n"
        "  --all-extensions   watch every thumbnail extension\n"
        "  --kill-limit=N     kills within the window before blocking (default %d)\n"
        "  --kill-window=S    length of that window in seconds (default %d)\n"
        "  --block-secs=S     how long a block lasts, in seconds (default %d)\n"
        "  --freeze-secs=S    hold time for suspended processes (default %d)\n"
        "  --log=PATH         log file\n"
        "  --version          print version\n\n",
        TG_VERSION, opt_interval_ms, opt_cpu_pct, opt_strikes, opt_watch,
        opt_kill_limit, opt_kill_window, opt_block_secs, opt_freeze_secs);
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    if (home)
        snprintf(opt_log, sizeof(opt_log), "%s/Library/Logs/thumbguard.log", home);

    int want_status = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--status"))          want_status = 1;
        else if (!strcmp(a, "--foreground")) opt_foreground = 1;
        else if (!strcmp(a, "--dry-run"))    opt_dry_run = 1;
        else if (!strcmp(a, "--block"))      opt_block_mode = 1;
        else if (!strncmp(a, "--interval=", 11)) opt_interval_ms = atoi(a + 11);
        else if (!strncmp(a, "--cpu=", 6))       opt_cpu_pct = atof(a + 6);
        else if (!strncmp(a, "--strikes=", 10))  opt_strikes = atoi(a + 10);
        else if (!strncmp(a, "--watch=", 8))     strlcpy(opt_watch, a + 8, sizeof(opt_watch));
        else if (!strcmp(a, "--all-extensions")) opt_all_ext = 1;
        else if (!strncmp(a, "--kill-limit=", 13))  opt_kill_limit = atoi(a + 13);
        else if (!strncmp(a, "--kill-window=", 14)) opt_kill_window = atoi(a + 14);
        else if (!strncmp(a, "--block-secs=", 13))  opt_block_secs = atoi(a + 13);
        else if (!strncmp(a, "--freeze-secs=", 14)) opt_freeze_secs = atoi(a + 14);
        else if (!strncmp(a, "--log=", 6))       strlcpy(opt_log, a + 6, sizeof(opt_log));
        else if (!strcmp(a, "--version")) { printf("thumbguard %s\n", TG_VERSION); return 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "Unknown option: %s\n", a); usage(); return 2; }
    }

    if (opt_interval_ms < 100)  opt_interval_ms = 100;
    if (opt_strikes < 1)        opt_strikes = 1;
    if (opt_cpu_pct < 1)        opt_cpu_pct = 1;
    if (opt_kill_limit < 1)     opt_kill_limit = 1;
    if (opt_kill_window < 1)    opt_kill_window = 1;
    if (opt_block_secs < 0)     opt_block_secs = 0;

    if (want_status)
        return cmd_status();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    logmsg("Start (version %s, tick %d ms, threshold %.0f %%, %d ticks%s%s)",
           TG_VERSION, opt_interval_ms, opt_cpu_pct, opt_strikes,
           opt_block_mode ? ", block mode" : "",
           opt_dry_run ? ", dry run" : "");

    struct timespec ts = {
        .tv_sec = opt_interval_ms / 1000,
        .tv_nsec = (long)(opt_interval_ms % 1000) * 1000000L
    };

    while (!g_stop) {
        scan();
        nanosleep(&ts, NULL);
    }

    release_frozen();
    logmsg("Stop (%ld process(es) killed)", g_kills);
    return 0;
}

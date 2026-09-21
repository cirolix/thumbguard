# Thumbguard — macOS WebThumbnailExtension Web Content Blocker / Killer

Keeps runaway QuickLook thumbnail processes on macOS under control —
`WebThumbnailExtension` and `OfficeThumbnailExtension`, along with the WebKit
render processes they leave behind.

## Quick start

```bash
git clone https://github.com/cirolix/thumbguard.git
cd thumbguard
./install.sh
```

That is all. The script builds the binary, installs it to
`~/.local/bin/thumbguard` and registers a LaunchAgent — the service is running
right away and comes back at every login. No administrator rights required.

**See what it is doing:**

```bash
thumbguard --status                      # watched processes and service state
tail -f ~/Library/Logs/thumbguard.log    # live log
```

A log line looks like this:

```
2026-09-21 12:31:50 KILL    PID 85631  OfficeThumbnailExtension    sustained load, 100 % CPU
```

**Still getting heat?** Switch to block mode — no HTML/Office thumbnails at
all, but the load cannot build up in the first place:

```bash
./install.sh --block
```

**Remove everything again:**

```bash
./uninstall.sh
```

### Requirements

* macOS on Apple Silicon or Intel; tested on macOS 26 (Darwin 25.6.0)
* Command line tools for the compiler: `xcode-select --install`
* If `thumbguard --status` is not found, `~/.local/bin` is not on your `PATH` —
  either call `~/.local/bin/thumbguard --status` directly or add that directory
  to your shell profile

### Already running hot right now?

The installer only guards against *new* runaway processes. Clear out the ones
that are already spinning:

```bash
pkill -9 -f "com.apple.WebKit.WebContent.EnhancedSecurity"
pkill -9 -f "ExtensionKit/Extensions/(Web|Office)ThumbnailExtension"
```

Nothing is lost here — these are thumbnail renderers with no user data. Safari
normally uses the plain `com.apple.WebKit.WebContent`, so ordinary browsing is
unaffected; only if you run Lockdown Mode will Safari tabs reload. Once the
service is installed, this cleanup is handled automatically.

## The problem

macOS generates thumbnails through extensions in
`/System/Library/ExtensionKit/Extensions/`. Two of them regularly choke:

* **WebThumbnailExtension** on HTML files that reference external resources
  (images, iframes, scripts from remote servers),
* **OfficeThumbnailExtension** on certain `.xlsm` files, especially the
  leftovers in `~/Library/Containers/com.microsoft.Excel/Data/tmp/Content.MSO/`.

The real damage happens afterwards: the WebKit render processes
(`com.apple.WebKit.WebContent.EnhancedSecurity`) are left **orphaned**, spin
at ~100 % CPU indefinitely, and **pile up**. In the case that prompted this
tool there were eleven of them at once — roughly 700 % CPU, several cores at
full tilt, with the heat and fan noise to match.

Activity Monitor lists them as “WebThumbnailExtension Web Content”.

### Why the usual scripts don't help

Nearly every solution floating around relies on

```
pluginkit -e disable -i <bundle-id>
```

That does nothing for these two extensions: they run through **ExtensionKit**,
not PlugInKit. `pluginkit` simply does not know them — verify with
`pluginkit -m -i com.apple.quicklook.thumbnail.WebExtension`, which returns
`(no matches)`. The command appears to succeed while changing nothing.

## The solution

A small background service that checks the process list on a tick and steps in
before load can build up.

**It reliably tells thumbnail renderers apart from Safari tabs.** Both use the
same binary. The distinction is made through the *responsible process* — the
same value Activity Monitor uses to build the name “WebThumbnailExtension Web
Content”. Only when a watched thumbnail extension sits behind it does the
service act. Safari tabs are left alone.

An orphaned render process is treated as garbage as well: with its client gone,
it can no longer deliver anything.

### When it steps in

Not on brief spikes — that would leave you without thumbnails. What is measured
is **sustained** load: a target process has to stay above 50 % CPU for three
consecutive ticks (by default, roughly three seconds) before it is killed.
Normal thumbnails finish long before that.

### Repeat offenders

Kill such a process and the ThumbnailsAgent immediately starts the next one —
with a whole folder of problem files, that becomes a restart loop. An extension
that needed three interventions within two minutes therefore goes on a
temporary block list (ten minutes by default).

Blocked processes are **suspended rather than killed outright** (`SIGSTOP`).
That is the key trick: a suspended process stops burning CPU, while the
ThumbnailsAgent keeps waiting for its reply instead of firing off a replacement
every second. After a 20-second hold time it is removed. In testing this
brought the number of interventions down from 21 to 7 with the same effect.

Suspended processes are cleaned up when the service stops; if one is left
behind by a hard crash, the next start clears it out.

## Resource usage

One single `sysctl` call per tick for the entire process list. Only name
matches are inspected further, and every process is classified **exactly
once** — after that the verdict is cached and costs nothing. Measured while
idle: **0.05 % CPU and 1.9 MB RAM**.

## What the installer does

It builds the program, places it at `~/.local/bin/thumbguard` and registers a
LaunchAgent that starts automatically at every login. No administrator rights
required — the target processes run under the same user account. The service is
named `local.thumbguard`; its log lives at `~/Library/Logs/thumbguard.log`.

`./uninstall.sh` reverses all of it and leaves only the log file behind.

## Usage

`thumbguard --status` shows what is being watched and whether the service is up:

```
Watching: WebThumbnailExtension,OfficeThumbnailExtension

Running thumbnail processes:
  [ ] PID 86747   CPU time     0.6s  TextThumbnailExtension
  [x] PID 85631   CPU time     6.5s  OfficeThumbnailExtension
  ([x] = watched)

Service: running
```

## Modes

**Normal (default).** Thumbnails for HTML and Office files keep being
generated; only processes under sustained load are removed.

**Block mode.** Every target process is suspended and killed on sight:

```bash
./install.sh --block
```

This means **no** thumbnails for HTML and Office files at all — Finder shows
the generic document icons instead. In exchange, the load can never build up in
the first place. Useful if normal operation is not enough.

Back to normal: `./install.sh` with no arguments.

## Options

All values can be passed at install time, e.g.
`./install.sh --cpu=70 --strikes=5`:

| Option | Default | Meaning |
|---|---|---|
| `--interval=MS` | 1000 | tick length in milliseconds |
| `--cpu=PERCENT` | 50 | load at which a tick counts as busy |
| `--strikes=N` | 3 | consecutive busy ticks before acting |
| `--watch=A,B` | Web, Office | extensions to watch |
| `--all-extensions` | off | watch every thumbnail extension |
| `--kill-limit=N` | 3 | kills within the window before blocking |
| `--kill-window=S` | 120 | length of that window, in seconds |
| `--block-secs=S` | 600 | how long a block lasts |
| `--freeze-secs=S` | 20 | hold time for suspended processes |
| `--dry-run` | off | touch nothing, only log what would happen |

`TextThumbnailExtension` and `ImageThumbnailExtension` are deliberately **not**
watched: they behave well and are entitled to work longer when many files show
up at once. A test with 150 text files showed that generic monitoring would
otherwise catch them in the middle of perfectly legitimate work.

## What else helps

Both triggers can be defused independently:

```bash
# reset the thumbnail cache
qlmanage -r cache

# Excel leftovers that the Office extension chokes on
rm -rf ~/Library/Containers/com.microsoft.Excel/Data/tmp/Content.MSO/*
```

Avoid leaving folders full of HTML files (project directories, build output)
open in Finder's gallery or icon view — that is the usual trigger.

## Note on the system interface used

Working out “which process requested this renderer” relies on
`responsibility_get_pid_responsible_for_pid()`. The function is undocumented
but has been stable across many macOS releases. Should it disappear in a future
version, compilation fails — the running service never misbehaves silently.

Tested on macOS 26 (Darwin 25.6.0), Apple Silicon.

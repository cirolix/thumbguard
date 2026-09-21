CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= $(HOME)/.local
LABEL    = local.thumbguard
PLIST    = $(HOME)/Library/LaunchAgents/$(LABEL).plist

all: thumbguard

thumbguard: thumbguard.c
	$(CC) $(CFLAGS) -o $@ $<

install: thumbguard
	@./install.sh

uninstall:
	@./uninstall.sh

status: thumbguard
	@./thumbguard --status

log:
	@tail -f "$(HOME)/Library/Logs/thumbguard.log"

clean:
	rm -f thumbguard

.PHONY: all install uninstall status log clean

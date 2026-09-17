CC      ?= cc
CFLAGS  ?= -std=c99 -Os -Wall -Wextra -Werror -fno-exceptions -D_DARWIN_C_SOURCE
LDFLAGS ?= -lsqlite3 -framework CoreFoundation -framework ImageIO -framework CoreGraphics
PREFIX  ?= /usr/local

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  BANNER_S := src/banner.s
else ifeq ($(UNAME_M),x86_64)
  BANNER_S := src/banner_x86_64.s
else
  $(error unsupported arch $(UNAME_M); need arm64 or x86_64 Darwin)
endif

SRCS := src/main.c src/args.c src/util.c src/fs_dir.c src/copy.c src/thumbs.c src/html.c src/iso.c src/pipeline.c src/photos_db.c
OBJS := $(SRCS:.c=.o) src/banner.o

.PHONY: all clean strip install check-size test dist help

all: arcsync

arcsync: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c src/arcsync.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/banner.o: $(BANNER_S)
	$(CC) -c -o $@ $(BANNER_S)

strip: arcsync
	strip -x arcsync

check-size: strip
	@sz=$$(stat -f%z arcsync); \
	echo "arcsync stripped size: $$sz bytes (limit $$((233*1024)))"; \
	if [ "$$sz" -gt $$((233*1024)) ]; then \
	  echo "FAIL: exceeds 233 KiB"; exit 1; \
	fi

install: arcsync
	install -d $(PREFIX)/bin
	install -m 755 arcsync $(PREFIX)/bin/arcsync
	install -d $(PREFIX)/share/man/man1
	-install -m 644 man/arcsync.1 $(PREFIX)/share/man/man1/arcsync.1

test: arcsync
	./arcsync --version >/dev/null
	./arcsync --help >/dev/null
	./arcsync -n --dir testdata/vacation --media none --out /tmp/arcsync-test.iso
	./arcsync --dir testdata/vacation --media none --force --out /tmp/arcsync-test.iso
	@test -f /tmp/arcsync-test.iso
	@MNT=$$(hdiutil attach -nobrowse /tmp/arcsync-test.iso | awk '/\/Volumes\//{print $$NF; exit}'); \
	  test -n "$$MNT"; \
	  test -f "$$MNT/index.html"; \
	  test -d "$$MNT/media"; \
	  hdiutil detach "$$MNT" >/dev/null; \
	  echo "make test: ISO mount OK"

clean:
	rm -f arcsync $(OBJS) src/banner.o

help:
	@echo "targets: all strip check-size test install clean"

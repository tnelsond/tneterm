PKGS    = sdl3 harfbuzz freetype2 fontconfig
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lutil -lm
PREFIX ?= /usr/local

OBJS = main.o term.o font.o

tnelterm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): term.h font.h

install: tnelterm
	install -Dm755 tnelterm $(DESTDIR)$(PREFIX)/bin/tnelterm

clean:
	rm -f tnelterm $(OBJS)

.PHONY: install clean

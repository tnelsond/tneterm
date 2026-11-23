PKGS    = sdl3 harfbuzz freetype2 fontconfig
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lutil -lm
PREFIX ?= /usr/local

OBJS = main.o term.o font.o

tneterm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): term.h font.h

install: tneterm
	install -Dm755 tneterm $(DESTDIR)$(PREFIX)/bin/tneterm

clean:
	rm -f tneterm $(OBJS)

.PHONY: install clean

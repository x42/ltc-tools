PREFIX ?= /usr/local
bindir = $(PREFIX)/bin

VERSION=0.2.0

CFLAGS+=`pkg-config --cflags ltc jack` -DVERSION=\"$(VERSION)\" -Wall
LOADLIBES=`pkg-config --libs ltc jack` -lm

# TODO these are only needed to ltcdump
CFLAGS+=`pkg-config --cflags sndfile`
LOADLIBES+=`pkg-config --libs sndfile`

all: jltcdump jltcgen ltcdump

man: jltcdump.1 jltcgen.1 ltcdump.1

jltcdump: jltcdump.c

jltcgen: jltcgen.c

ltcdump: ltcdump.c

jltcdump.1: jltcdump
	help2man -N -n 'JACK LTC decoder' -o jltcdump.1 ./jltcdump

jltcgen.1: jltcgen
	help2man -N -n 'JACK LTC generator' -o jltcgen.1 ./jltcdump

ltcdump.1: ltcdump
	help2man -N -n 'parse LTC from file' -o ltcdump.1 ./ltcdump


clean:
	rm -f jltcdump jltcgen ltcdump 

install: jltcdump
	install -d $(DESTDIR)$(bindir)
	install -m755 jltcdump $(DESTDIR)$(bindir)
	install -m755 jltcgen $(DESTDIR)$(bindir)
	install -m755 ltcdump $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/jltcdump
	rm -f $(DESTDIR)$(bindir)/jltcgen
	rm -f $(DESTDIR)$(bindir)/ltcdump
	-rmdir $(DESTDIR)$(bindir)

.PHONY: all clean install uninstall man

PREFIX ?= /usr/local
bindir = $(PREFIX)/bin

VERSION=0.2.6

CFLAGS+=`pkg-config --cflags ltc jack` -DVERSION=\"$(VERSION)\" -Wall -g
LOADLIBES=`pkg-config --libs ltc jack` -lm

# TODO these are only needed to ltcdump and ltcgen
CFLAGS+=`pkg-config --cflags sndfile`
LOADLIBES+=`pkg-config --libs sndfile`

all: jltcdump jltcgen ltcdump jltc2mtc ltcgen

man: jltcdump.1 jltcgen.1 ltcdump.1 jltc2mtc.1 ltcgen.1

jltcdump: jltcdump.c

jltcgen: jltcgen.c

ltcdump: ltcdump.c

jltc2mtc: jltc2mtc.c

ltcgen: ltcgen.c

jltcdump.1: jltcdump
	help2man -N -n 'JACK LTC decoder' -o jltcdump.1 ./jltcdump

jltcgen.1: jltcgen
	help2man -N -n 'JACK LTC generator' -o jltcgen.1 ./jltcdump

ltcdump.1: ltcdump
	help2man -N -n 'parse LTC from file' -o ltcdump.1 ./ltcdump

jltc2mtc.1: jltc2mtc
	help2man -N -n 'translate LTC into MTC' -o jltc2mtc.1 ./jltc2mtc

ltcgen.1: ltcgen
	help2man -N -n 'LTC file encoder' -o ltcgen.1 ./ltcgen

clean:
	rm -f jltcdump jltcgen ltcdump jltc2mtc ltcgen

install: jltcdump
	install -d $(DESTDIR)$(bindir)
	install -m755 jltcdump $(DESTDIR)$(bindir)
	install -m755 jltcgen $(DESTDIR)$(bindir)
	install -m755 ltcdump $(DESTDIR)$(bindir)
	install -m755 ltcgen $(DESTDIR)$(bindir)
	install -m755 jltc2mtc $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/jltcdump
	rm -f $(DESTDIR)$(bindir)/jltcgen
	rm -f $(DESTDIR)$(bindir)/ltcdump
	rm -f $(DESTDIR)$(bindir)/jltc2mtc
	rm -f $(DESTDIR)$(bindir)/ltcgen
	-rmdir $(DESTDIR)$(bindir)

.PHONY: all clean install uninstall man

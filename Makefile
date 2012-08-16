PREFIX ?= /usr/local
bindir = $(PREFIX)/bin

CFLAGS=`pkg-config --cflags ltc jack` -DVERSION=\"0.2.0\" -Wall
LOADLIBES=`pkg-config --libs ltc jack` -lm

all: jltcdump

jltcdump: jltcdump.c

jltcdump.1: jltcdump
	help2man -N -n 'JACK LTC dump' -o jltcdump.1 ./jltcdump

clean:
	rm -f jltcdump

install: jltcdump
	install -d $(DESTDIR)$(bindir)
	install -m755 jltcdump $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/jltcdump
	-rmdir $(DESTDIR)$(bindir)

.PHONY: all clean install uninstall

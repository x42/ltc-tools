PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1
CFLAGS ?= -Wall -g -O2

VERSION=0.6.0

ifeq ($(shell pkg-config --atleast-version=1.1.0 ltc || echo no), no)
  $(error "https://github.com/x42/libltc version >= 1.1.0 is required - install libltc-dev")
endif
ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(warning "http://jackaudio.org is recommended - install libjack-dev or libjack-jackd2-dev")
  $(warning "The applications 'jltcdump', 'jltcgen' and 'jltc2mtc' are not built")
  $(warning "and 'make install' will fail.")
else
  APPS+=jltcdump jltcgen jltc2mtc
  CFLAGS+=`pkg-config --cflags ltc jack`
  LOADLIBES=`pkg-config --libs ltc jack`
endif
ifeq ($(shell pkg-config --exists sndfile || echo no), no)
  $(warning "http://www.mega-nerd.com/libsndfile/ is recommended - install libsndfile-dev")
  $(warning "The applications 'ltcdump' and 'jltcgen' are not built")
  $(warning "and 'make install' will fail.")
else
  APPS+=ltcdump ltcgen
  CFLAGS+=`pkg-config --cflags sndfile`
  LOADLIBES+=`pkg-config --libs sndfile`
endif

ifeq ($(APPS),)
  $(error "At least one of libjack or libsndfile is needed")
endif

CFLAGS+=-DVERSION=\"$(VERSION)\"
LOADLIBES+=-lm -lrt -lpthread

all: $(APPS)

man: jltcdump.1 jltcgen.1 ltcdump.1 jltc2mtc.1 ltcgen.1

jltcdump: jltcdump.c ltcframeutil.c ltcframeutil.h

jltcgen: jltcgen.c timecode.c timecode.h common_ltcgen.c common_ltcgen.h

ltcdump: ltcdump.c ltcframeutil.c ltcframeutil.h

jltc2mtc: jltc2mtc.c ltcframeutil.c ltcframeutil.h

ltcgen: ltcgen.c timecode.c timecode.h common_ltcgen.c common_ltcgen.h

jltcdump.1: jltcdump
	help2man -N -n 'JACK LTC decoder' -o jltcdump.1 ./jltcdump

jltcgen.1: jltcgen
	help2man -N -n 'JACK LTC generator' -o jltcgen.1 ./jltcgen

ltcdump.1: ltcdump
	help2man -N -n 'parse LTC from file' -o ltcdump.1 ./ltcdump

jltc2mtc.1: jltc2mtc
	help2man -N -n 'translate LTC into MTC' -o jltc2mtc.1 ./jltc2mtc

ltcgen.1: ltcgen
	help2man -N -n 'LTC file encoder' -o ltcgen.1 ./ltcgen

clean:
	rm -f jltcdump jltcgen ltcdump jltc2mtc ltcgen

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

install-bin: jltcdump jltcgen jltcdump jltc2mtc ltcgen
	install -d $(DESTDIR)$(bindir)
	install -m755 jltcdump $(DESTDIR)$(bindir)
	install -m755 jltcgen $(DESTDIR)$(bindir)
	install -m755 ltcdump $(DESTDIR)$(bindir)
	install -m755 ltcgen $(DESTDIR)$(bindir)
	install -m755 jltc2mtc $(DESTDIR)$(bindir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jltcdump
	rm -f $(DESTDIR)$(bindir)/jltcgen
	rm -f $(DESTDIR)$(bindir)/ltcdump
	rm -f $(DESTDIR)$(bindir)/jltc2mtc
	rm -f $(DESTDIR)$(bindir)/ltcgen
	-rmdir $(DESTDIR)$(bindir)

install-man: 
	install -d $(DESTDIR)$(mandir)
	install -m644 jltcdump.1 $(DESTDIR)$(mandir)
	install -m644 jltcgen.1 $(DESTDIR)$(mandir)
	install -m644 ltcdump.1 $(DESTDIR)$(mandir)
	install -m644 ltcgen.1 $(DESTDIR)$(mandir)
	install -m644 jltc2mtc.1 $(DESTDIR)$(mandir)

uninstall-man:
	rm -f $(DESTDIR)$(mandir)/jltcdump.1
	rm -f $(DESTDIR)$(mandir)/jltcgen.1
	rm -f $(DESTDIR)$(mandir)/ltcdump.1
	rm -f $(DESTDIR)$(mandir)/jltc2mtc.1
	rm -f $(DESTDIR)$(mandir)/ltcgen.1
	-rmdir $(DESTDIR)$(mandir)


.PHONY: all clean install uninstall man install-man install-bin uninstall-man uninstall-bin

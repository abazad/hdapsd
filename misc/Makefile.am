if HAVE_SYSTEMD
udevrulesdir = $(udevdir)/rules.d
udevrules_DATA = hdapsd.rules
systemdsystemunit_DATA = \
	hdapsd@.service
noinst_DATA = hdapsd.service
CLEANFILES = hdapsd@.service hdapsd.service

edit = sed \
	-e 's|@sbindir[@]|$(sbindir)|g'

hdapsd@.service: Makefile
	rm -f $@ $@.tmp
	srcdir=''; \
	  test -f ./$@.in || srcdir=$(srcdir)/; \
	  $(edit) $${srcdir}$@.in >$@.tmp

	mv $@.tmp $@

hdapsd@.service: hdapsd@.service.in

hdapsd.service: Makefile
	rm -f $@ $@.tmp
	srcdir=''; \
	  test -f ./$@.in || srcdir=$(srcdir)/; \
	  $(edit) $${srcdir}$@.in >$@.tmp

	mv $@.tmp $@

hdapsd.service: hdapsd.service.in

endif

if HAVE_LIBCONFIG
sysconf_DATA = hdapsd.conf
endif

EXTRA_DIST = \
	hdapsd.conf hdapsd.rules hdapsd@.service.in hdapsd.service.in

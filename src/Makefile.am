bin_PROGRAMS = gooroom-autostart-program

bin_SCRIPTS = grac-reloader.py

gooroom_autostart_program_SOURCES = main.c

gooroom_autostart_program_CFLAGS =		\
	-DLOCALEDIR=\"$(localedir)\"	\
	$(GLIB_CFLAGS)							\
	$(GTK_CFLAGS)							\
	$(CURL_CFLAGS)							\
	$(JSON_C_CFLAGS)						\
	$(POLKIT_CFLAGS)

gooroom_autostart_program_LDADD =	\
	$(GLIB_LIBS)				\
	$(GTK_LIBS)					\
	$(CURL_LIBS)				\
	$(JSON_C_LIBS)				\
	$(POLKIT_LIBS)

@INTLTOOL_POLICY_RULE@
polkitdir = $(datadir)/polkit-1/actions
polkit_in_files = kr.gooroom.autostart.program.policy.in
polkit_DATA = $(polkit_in_files:.policy.in=.policy)

CLEANFILES = kr.gooroom.autostart.program.policy

EXTRA_DIST = $(polkit_in_files)


bin_PROGRAMS = gooroom-autostart-program

gooroom_autostart_program_SOURCES = main.c

gooroom_autostart_program_CFLAGS =	\
	-DDATADIR=\"$(datadir)\"		\
	-DLOCALEDIR=\"$(localedir)\"	\
	$(GLIB_CFLAGS)		\
	$(GTK_CFLAGS)		\
	$(CURL_CFLAGS)		\
	$(JSON_C_CFLAGS)	\
	$(XFCONF_CFLAGS)	\
	$(DBUS_CFLAGS)		\
	$(DBUS_GLIB_CFLAGS)	\
	$(LIBNOTIFY_CFLAGS)	\
	$(LIBXFCE4UTIL_CFLAGS)

gooroom_autostart_program_LDADD =	\
	$(GLIB_LIBS)	\
	$(GTK_LIBS)		\
	$(CURL_LIBS)	\
	$(JSON_C_LIBS)	\
	$(XFCONF_LIBS)	\
	$(DBUS_LIBS)	\
	$(DBUS_GLIB_LIBS)	\
	$(LIBNOTIFY_LIBS)	\
	$(LIBXFCE4UTIL_LIBS)

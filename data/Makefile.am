@INTLTOOL_DESKTOP_RULE@
desktopdir = $(sysconfdir)/xdg/autostart
desktop_in_files = gooroom-autostart-program.desktop.in.in
desktop_DATA = $(desktop_in_files:.desktop.in.in=.desktop)

@INTLTOOL_POLICY_RULE@
polkitdir = $(datadir)/polkit-1/actions
polkit_in_files = kr.gooroom.autostart.program.policy.in.in
polkit_DATA = $(polkit_in_files:.policy.in.in=.policy)

EXTRA_DIST = $(desktop_in_files)
DISTCLEANFILES = $(desktop_DATA) $(polkit_DATA)

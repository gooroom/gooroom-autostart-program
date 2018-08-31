/*
 * Copyright (c) 2015 - 2017 gooroom <gooroom@gooroom.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>

#include <dbus/dbus.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <libnotify/notify.h>

#include <xfconf/xfconf.h>
#include <libxfce4util/libxfce4util.h>
#include <gconf/gconf-client.h>

#include "dockitem_file_template.h"

#define	GRM_USER		".grm-user"


static guint timeout_id = 0;
static gint not_matched_count = 0;
static GDBusProxy *agent_proxy = NULL;

json_object *
JSON_OBJECT_GET (json_object *root_obj, const char *key)
{
	if (!root_obj) return NULL;

	json_object *ret_obj = NULL;

	json_object_object_get_ex (root_obj, key, &ret_obj);

	return ret_obj;
}

static gchar *
get_grm_user_data (void)
{
	gchar *file = NULL;
	gchar *data = NULL;

	file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (!g_file_test (file, G_FILE_TEST_EXISTS)) {
		g_error ("No such file or directory : %s", file);
		goto error;
	}
	
	g_file_get_contents (file, &data, NULL, NULL);

error:
	g_free (file);

	return data;
}

static long
strtoday (const char *date /* yyyy-mm-dd */)
{
	int year = 0, month = 0, day = 0;

	if ((date == NULL) || (sscanf (date, "%d-%d-%d", &year, &month, &day) == 0))
		return -1;

	if (year != 0 && month != 0 && day != 0) {
		GDateTime *dt = g_date_time_new_local (year, month, day, 0, 0, 0);
		long days = (long)(g_date_time_to_unix (dt) / (24 * 60 * 60));
		g_date_time_unref (dt);

		return days;
	}

	return -1;
}

static gboolean
download_with_curl (const gchar *download_url, const gchar *download_path)
{
	CURL *curl;
	CURLcode res = CURLE_OK;
	gboolean ret = FALSE;

	g_return_val_if_fail (download_url != NULL, FALSE);
	g_return_val_if_fail (download_path != NULL, FALSE);

	curl_global_init (CURL_GLOBAL_ALL);

	curl = curl_easy_init (); 

	if (!curl)
		goto error;

	FILE *fp = fopen (download_path, "w");
	if (!fp)
		goto error;

	curl_easy_setopt (curl, CURLOPT_URL, download_url);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 3);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);

	res = curl_easy_perform (curl);
	curl_easy_cleanup (curl);

	fclose(fp);

	ret = (res == CURLE_OK);

error:
	curl_global_cleanup ();

	return ret;
}

static gchar *
download_favicon (const gchar *favicon_url, gint num)
{
	g_return_val_if_fail (favicon_url != NULL, NULL);

	gchar *favicon_path = NULL;

	favicon_path = g_strdup_printf ("%s/favicon-%.02d", g_get_user_cache_dir (), num);

	download_with_curl (favicon_url, favicon_path);

	if (!g_file_test (favicon_path, G_FILE_TEST_EXISTS))
		goto error;

	struct stat st;
	if (lstat (favicon_path, &st) == -1)
		goto error;

	if (st.st_size == 0)
		goto error;

	return favicon_path;


error:
	g_free (favicon_path);

	return g_strdup ("applications-other");
}

static gchar *
get_desktop_directory (json_object *obj)
{
	g_return_val_if_fail (obj != NULL, NULL);

	gchar *desktop_dir = NULL;
	const char *val = json_object_get_string (obj);

	if (g_strcmp0 (val, "bar") == 0) {
		desktop_dir = g_build_filename (g_get_user_data_dir (), "applications/custom", NULL);
	} else {
		desktop_dir = g_build_filename (g_get_user_data_dir () ,"applications", NULL);
	}

	if (!g_file_test (desktop_dir, G_FILE_TEST_EXISTS)) {
		if (g_mkdir_with_parents (desktop_dir, 0755) == -1) {
			g_free (desktop_dir);
			return NULL;
		}
	}

	return desktop_dir;
}

static gboolean
create_desktop_file (json_object *obj, const gchar *dt_file_name, gint num)
{
	g_return_val_if_fail ((obj != NULL) || (dt_file_name != NULL), FALSE);

	gboolean ret = FALSE;
	GKeyFile *keyfile = NULL;

	keyfile = g_key_file_new ();

	json_object_object_foreach (obj, key, val) {
		const gchar *value = json_object_get_string (val);
		gchar *d_key = g_ascii_strdown (key, -1);

		if (d_key && g_strcmp0 (d_key, "icon") == 0) {
			if (g_str_has_prefix (value, "http://") || g_str_has_prefix (value, "https://")) {
				gchar *icon_file = download_favicon (value, num);
				if (icon_file) {
					g_key_file_set_string (keyfile, "Desktop Entry", "Icon", icon_file);
					g_free (icon_file);
				} else {
					g_key_file_set_string (keyfile, "Desktop Entry", "Icon", "applications-other");
				}
			} else {
				g_key_file_set_string (keyfile, "Desktop Entry", "Icon", value);
			}
		} else {
			gchar *new_key = NULL;

			if (g_strcmp0 (d_key, "name") == 0) {
				new_key = g_strdup ("Name");
			} else if (g_strcmp0 (d_key, "comment") == 0) {
				new_key = g_strdup ("Comment");
			} else if (g_strcmp0 (d_key, "exec") == 0) {
				new_key = g_strdup ("Exec");
			}

			if (new_key) {
				g_key_file_set_string (keyfile, "Desktop Entry", new_key, value);
				g_free (new_key);
			}
		}

		g_free (d_key);
	}

	g_key_file_set_string (keyfile, "Desktop Entry", "Type", "Application");
	g_key_file_set_string (keyfile, "Desktop Entry", "Terminal", "false");
	g_key_file_set_string (keyfile, "Desktop Entry", "StartupNotify", "true");

	ret = g_key_file_save_to_file (keyfile, dt_file_name, NULL);
	g_key_file_free (keyfile);

	return ret;
}

static gboolean
restart_dockbarx_async (gpointer data)
{
	g_spawn_command_line_sync ("xfce4-panel -r", NULL, NULL, NULL, NULL);

	gchar *cmd = g_find_program_in_path ("pkill");
	if (cmd) {
		gchar *cmdline = g_strdup_printf ("%s -f 'python.*xfce4-dockbarx-plug'", cmd);
		g_spawn_command_line_async (cmdline, NULL);
		g_free (cmdline);
	}
	g_free (cmd);

	return FALSE;
}

static gboolean
run_control_center_async (gpointer data)
{
	gchar *cmd = g_find_program_in_path ("gooroom-control-center");
	if (cmd) {
		gchar *cmdline = g_strdup_printf ("%s user", cmd);
		g_spawn_command_line_async (cmdline, NULL);
		g_free (cmdline);
	}
	g_free (cmd);

	return FALSE;
}

static gboolean
find_launcher (GSList *list, const gchar *launcher)
{
	GSList *l = NULL;
	for (l = list; l; l = l->next) {
		gchar *elm = (gchar *)l->data;
		if (g_strrstr (elm, launcher) != NULL) {
			return TRUE;
		}
	}

	return FALSE;
}

static void
dockbarx_launchers_set (GSList *launchers)
{
	g_return_if_fail (launchers != NULL);

	GConfClient *client = NULL;

	client = gconf_client_get_default ();
	gconf_client_set_list (client, "/apps/dockbarx/launchers", GCONF_VALUE_STRING, launchers, NULL);
	g_object_unref (client);
}

static GSList *
dockbarx_launchers_get (void)
{
	GConfClient *client = NULL;
	GSList *old_launchers, *new_launchers = NULL;

	client = gconf_client_get_default ();
	old_launchers = gconf_client_get_list (client, "/apps/dockbarx/launchers", GCONF_VALUE_STRING, NULL);

	GSList *l = NULL;
	for (l = old_launchers; l != NULL; l = l->next) {
		gchar *launcher = (gchar *)l->data;
		gchar *desktop = g_strrstr (launcher, ";")+1;
		if (g_file_test (desktop, G_FILE_TEST_EXISTS)) {
			new_launchers = g_slist_append (new_launchers, g_strdup (launcher));
		}
	}

	g_object_unref (client);
	g_slist_free_full (old_launchers, (GDestroyNotify) g_free);

	return new_launchers;
}

static gchar *
find_wallpaper (const gchar *wallpaper_name)
{
	GDir *dir;
	const gchar *file;
	GList *list = NULL, *l = NULL;
	gchar *ret_path = NULL;

	list = g_list_append (list, g_strdup ("/usr/share/backgrounds/gooroom"));
	list = g_list_append (list, g_build_filename (g_get_user_data_dir (), "backgrounds", NULL));

	for (l = list; l != NULL; l = l->next) {
		gchar *background = (gchar *)l->data;
		dir = g_dir_open (background, 0, NULL);

		if (G_UNLIKELY (dir == NULL))
			continue;

		/* Iterate over filenames in the directory */
		while ((file = g_dir_read_name (dir)) != NULL) {
			if (g_strcmp0 (file, wallpaper_name) == 0) {
				ret_path = g_build_filename (background, file, NULL);
				break;
			}
		}

		/* Close directory handle */
		g_dir_close (dir);

		if (ret_path) break;
	}

	g_list_free_full (list, g_free);

	return ret_path;
}

static gboolean
icon_theme_exists (const gchar *icon_theme)
{
	guint i = 0;
	gboolean ret = FALSE;
	gchar **icon_theme_dirs;
	GDir *dir;
	const gchar  *file;

	/* Determine directories to look in for icon themes */
	xfce_resource_push_path (XFCE_RESOURCE_ICONS, DATADIR G_DIR_SEPARATOR_S "icons");
	icon_theme_dirs = xfce_resource_dirs (XFCE_RESOURCE_ICONS);
	xfce_resource_pop_path (XFCE_RESOURCE_ICONS);

	/* Iterate over all base directories */
	for (i = 0; icon_theme_dirs[i] != NULL; ++i) {
		/* Open directory handle */
		dir = g_dir_open (icon_theme_dirs[i], 0, NULL);

		/* Try next base directory if this one cannot be read */
		if (G_UNLIKELY (dir == NULL))
			continue;

		/* Iterate over filenames in the directory */
		while ((file = g_dir_read_name (dir)) != NULL) {
			if (g_strcmp0 (file, icon_theme) == 0) {
				ret = TRUE;
				break;
			}
		}

		/* Close directory handle */
		g_dir_close (dir);

		if (ret) break;
	}

	g_strfreev (icon_theme_dirs);

	return ret;
}

static gboolean
check_dockbarx_launchers (gpointer data)
{
	if (!data) {
		if (timeout_id) {
			g_source_remove (timeout_id);
			timeout_id = 0;
		}
		not_matched_count = 0;

		g_timeout_add (300, (GSourceFunc) restart_dockbarx_async, NULL);

		return FALSE;
	}

	gboolean matched = TRUE;
	GSList *l = NULL;
	GSList *new_launchers = (GSList *)data;
	GSList *old_launchers = dockbarx_launchers_get ();

	for (l = new_launchers; l; l = l->next) {
		const gchar *launcher = (const gchar *)l->data;
		if (!find_launcher (old_launchers, launcher)) {
			matched = FALSE;
			break;
		}
	}

	g_slist_free_full (old_launchers, (GDestroyNotify) g_free);

	if (!matched) {
		if (not_matched_count > 3) {
			if (timeout_id) {
				g_source_remove (timeout_id);
				timeout_id = 0;
			}

			not_matched_count = 0;

			GtkWidget *dialog = gtk_message_dialog_new (NULL,
					GTK_DIALOG_MODAL,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_OK,
					_("User Configuration Error"));

			gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
					_("Failed to set user's favorite menu.\nPlease login again."));
			gtk_window_set_title (GTK_WINDOW (dialog), _("Warning"));
			gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);

			return FALSE;
		}

		not_matched_count++;

		return TRUE;
	}

	if (timeout_id) {
		g_source_remove (timeout_id);
		timeout_id = 0;
	}

	not_matched_count = 0;

	g_slist_free_full (new_launchers, (GDestroyNotify) g_free);
	g_timeout_add (300, (GSourceFunc) restart_dockbarx_async, NULL);

	return FALSE;
}

static void
make_direct_url (json_object *root_obj, GSList *launchers)
{
	g_return_if_fail (root_obj != NULL);

	json_object *apps_obj = NULL;
	apps_obj = JSON_OBJECT_GET (root_obj, "apps");

	g_return_if_fail (apps_obj != NULL);

	gint i = 0, len = 0;;
	len = json_object_array_length (apps_obj);

	for (i = 0; i < len; i++) {
		json_object *app_obj = json_object_array_get_idx (apps_obj, i);

		if (app_obj) {
			gchar *dt_file_name = NULL;
			json_object *dt_obj = NULL, *pos_obj = NULL;
			dt_obj = JSON_OBJECT_GET (app_obj, "desktop");
			pos_obj = JSON_OBJECT_GET (app_obj, "position");

			if (dt_obj && pos_obj) {
				gchar *dt_dir_name = get_desktop_directory (pos_obj);
				if (dt_dir_name) {
					dt_file_name = g_strdup_printf ("%s/shortcut-%.02d.desktop", dt_dir_name, i);
				}
				g_free (dt_dir_name);

				if (create_desktop_file (dt_obj, dt_file_name, i)) {
					gchar *launcher = g_strdup_printf ("shortcut-%.02d;%s", i, dt_file_name);
					if (!find_launcher (launchers, launcher)) {
						launchers = g_slist_append (launchers, launcher);
					}
				} else {
					g_error ("Could not create desktop file : %s", dt_file_name);
				}
			}
			g_free (dt_file_name);
		}
	}

	dockbarx_launchers_set (launchers);
}

static void
remove_custom_desktop_files ()
{
	gchar *remove_dir = g_build_filename (g_get_user_data_dir (), "applications/custom", NULL);
    if (g_file_test (remove_dir, G_FILE_TEST_EXISTS)) {
		gchar *cmd, *cmdline;

		cmd = g_find_program_in_path ("rm");
		cmdline = g_strdup_printf ("%s -rf %s", cmd, remove_dir);

		g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, NULL);

		g_free (cmd);
		g_free (cmdline);
	}

	g_free (remove_dir);
}

static void
dock_launcher_update (void)
{
	GSList *new_launchers = NULL;
	gchar *data = get_grm_user_data ();

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
            json_object *obj1 = NULL, *obj2 = NULL, *obj2_1 = NULL, *obj3= NULL;
			obj1 = JSON_OBJECT_GET (root_obj, "data");
			obj2 = JSON_OBJECT_GET (obj1, "loginInfo");
			obj2_1 = JSON_OBJECT_GET (obj2, "user_id");
			obj3 = JSON_OBJECT_GET (obj1, "desktopInfo");
			if (obj2_1) {
				const char *value = json_object_get_string (obj2_1);
				if (g_strcmp0 (value, g_get_user_name ()) == 0) {
					if (obj3) {
						new_launchers = dockbarx_launchers_get ();
						make_direct_url (obj3, new_launchers);
					}
				}
			}
			json_object_put (root_obj);
		}
	}

	g_free (data);

	if (new_launchers) {
		timeout_id = g_timeout_add (500, (GSourceFunc) check_dockbarx_launchers, new_launchers);
	}
}

static gboolean
is_online_user (const gchar *username)
{
	gboolean ret = FALSE;

	struct passwd *entry = getpwnam (username);
	if (entry) {
		gchar **tokens = g_strsplit (entry->pw_gecos, ",", -1);
		if (g_strv_length (tokens) > 4 ) {
			if (tokens[4] && (g_strcmp0 (tokens[4], "gooroom-online-account") == 0)) {
				ret = TRUE;
			}
		}
		g_strfreev (tokens);
	}

	return ret;
}

static void
show_message (const gchar *title, const gchar *msg, const gchar *icon)
{
	GtkWidget *dialog;

	dialog = GTK_WIDGET (gtk_message_dialog_new (NULL,
				GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
				GTK_MESSAGE_INFO,
				GTK_BUTTONS_OK,
				NULL));

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), msg, NULL);

	gtk_window_set_title (GTK_WINDOW (dialog), title);
	gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

static gint
check_shadow_expiry (long lastchg, int maxdays)
{
	gint warndays = 7;
	gint daysleft = 9999;
	long curdays;

	curdays = (long)(time(NULL) / (60 * 60 * 24));

	if ((curdays - lastchg) >= (maxdays - warndays)) {
		daysleft = (gint)((lastchg + maxdays) - curdays);
	}

	return daysleft;
}

static void
list_sorted (gpointer key, gpointer value, gpointer user_data)
{
  GSList **listp = user_data;

  *listp = g_slist_insert_sorted (*listp, key, (GCompareFunc) g_utf8_collate);
}

static void
set_icon_theme (const gchar *icon_theme)
{
	XfconfChannel *channel = xfconf_channel_new ("xsettings");
	if (channel) {
		if (icon_theme_exists (icon_theme)) {
			xfconf_channel_set_string (channel, "/Net/IconThemeName", icon_theme);
		}
		g_object_unref (channel);
	}
}

static void
set_wallpaper (const char *wallpaper_name, const gchar *wallpaper_url)
{
	g_return_if_fail (wallpaper_name != NULL);

	gchar *wallpaper_path = NULL;

	wallpaper_path = find_wallpaper (wallpaper_name);

	if (!wallpaper_path) {
		g_return_if_fail (wallpaper_url != NULL);

		/* obtain filename from url */
		gchar *filename = g_strrstr (wallpaper_url, "/") + 1;
		if (filename) {
			gchar *background_dir = g_build_filename (g_get_user_data_dir (), "backgrounds", NULL);
			if (!g_file_test (background_dir, G_FILE_TEST_EXISTS)) {
				g_mkdir_with_parents (background_dir, 0744);
			}

			/* build download path */
			wallpaper_path = g_build_filename (background_dir, filename, NULL);

			download_with_curl (wallpaper_url, wallpaper_path);

			g_free (background_dir);
		}
	}

	g_return_if_fail (wallpaper_path != NULL);

	if (g_file_test (wallpaper_path, G_FILE_TEST_EXISTS)) {
		XfconfChannel *channel = xfconf_channel_new ("xfce4-desktop");
		if (channel) {
			GHashTable *table = xfconf_channel_get_properties (channel, "/backdrop");
			if (table) {
				GSList *sorted_contents = NULL, *l = NULL;

				g_hash_table_foreach (table, (GHFunc)list_sorted, &sorted_contents);

				for (l = sorted_contents; l != NULL; l = l->next) {
					gchar *property = (gchar *)l->data;
					if (g_str_has_suffix (property, "image-path") ||
							g_str_has_suffix (property, "last-image") ||
							g_str_has_suffix (property, "last-single-image")) {
						xfconf_channel_set_string (channel, property, wallpaper_path);
					}
				}

				g_slist_free (sorted_contents);
				g_hash_table_destroy (table);
			}
			g_object_unref (channel);
		}
	}

	g_free (wallpaper_path);
}

static void
handle_desktop_configuration (void)
{
	gchar *data = get_grm_user_data ();

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
			json_object *obj1 = NULL, *obj2 = NULL, *obj3_1 = NULL, *obj3_2 = NULL, *obj3_3 = NULL;
			obj1 = JSON_OBJECT_GET (root_obj, "data");
			obj2 = JSON_OBJECT_GET (obj1, "desktopInfo");
			obj3_1 = JSON_OBJECT_GET (obj2, "themeNm");
			obj3_2 = JSON_OBJECT_GET (obj2, "wallpaperNm");
			obj3_3 = JSON_OBJECT_GET (obj2, "wallpaperFile");

			if (obj3_1) {
				const char *icon_theme = json_object_get_string (obj3_1);

				/* set icon theme */
				set_icon_theme (icon_theme);
			}

			if (obj3_2 && obj3_3) {
				const char *wallpaper_name = json_object_get_string (obj3_2);
				const char *wallpaper_url = json_object_get_string (obj3_3);

				/* set wallpaper */
				set_wallpaper (wallpaper_name, wallpaper_url);
			}

			json_object_put (root_obj);
		}
	}

	g_free (data);
}

static void
handle_password_expiration (void)
{
	gchar *data = get_grm_user_data ();

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
			gboolean need_change_passwd = FALSE;
			gboolean passwd_init = FALSE;
			json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL;
			json_object *obj2_1 = NULL, *obj2_2 = NULL, *obj2_3 = NULL;

			obj1 = JSON_OBJECT_GET (root_obj, "data");
			obj2 = JSON_OBJECT_GET (obj1, "loginInfo");
			obj2_1 = JSON_OBJECT_GET (obj2, "pwd_last_day");
			obj2_2 = JSON_OBJECT_GET (obj2, "pwd_max_day");
			obj2_3 = JSON_OBJECT_GET (obj2, "pwd_temp_yn");
			if (obj2_3) {
				const char *value = json_object_get_string (obj2_3);
				if (value && g_strcmp0 (value, "Y") == 0) {
					passwd_init = TRUE;
					const gchar *msg = _("Your password has been issued temporarily.\nFor security reasons, please change your password immediately.");
					show_message (_("Change Password"), msg, "dialog-error");
					need_change_passwd = TRUE;
				}
			}

			if (!passwd_init && obj2_1 && obj2_2) {
				const char *value = json_object_get_string (obj2_1);
				int max_days = json_object_get_int (obj2_2);
				long last_days = strtoday (value);

				if (last_days != -1 && max_days != -1) {
					gint daysleft = check_shadow_expiry (last_days, max_days);

					if (daysleft > 0 && daysleft < 9999) {
						gchar *msg = g_strdup_printf (_("You have %d days to change your password.\nPlease change your password within %d days."), daysleft, daysleft);
						show_message (_("Change Password"), msg, "dialog-warn");
						g_free (msg);
						need_change_passwd = TRUE;
					} else if (daysleft == 0) {
						show_message (_("Change Password"), _("Password change period is until today.\nPlease change your password today."), "dialog-warn");
						need_change_passwd = TRUE;
					} else if (daysleft < 0){
						show_message (_("Change Password"), _("The password change period has already passed.\nFor security reasons, please change your password immediately."), "dialog-warn");
						need_change_passwd = TRUE;
					}
				}
			}
			json_object_put (root_obj);

			if (need_change_passwd) {
				g_timeout_add (100, (GSourceFunc) run_control_center_async, NULL);
			}
		}
	}

	g_free (data);
}

static void
reload_grac_service_done_cb (GObject *source, GAsyncResult *res, gpointer data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source);

	g_dbus_proxy_call_finish (proxy, res, NULL);
}

static void
reload_grac_service (void)
{
	if (!agent_proxy) {
		agent_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
				G_DBUS_CALL_FLAGS_NONE,
				NULL,
				"kr.gooroom.agent",
				"/kr/gooroom/agent",
				"kr.gooroom.agent",
				NULL,
				NULL);
	}

	if (agent_proxy) {
		const gchar *arg = "{\"module\":{\"module_name\":\"daemon_control\",\"task\":{\"task_name\":\"daemon_reload\",\"in\":{\"service\":\"grac-device-daemon.service\"}}}}";

		g_dbus_proxy_call (agent_proxy, "do_task",
			g_variant_new ("(s)", arg),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			reload_grac_service_done_cb, NULL);
	}
}

static void
dpms_off_time_set (gpointer user_data)
{
	XfconfChannel *channel = XFCONF_CHANNEL (user_data);

	gchar *data = get_grm_user_data ();

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
			json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL;

			obj1 = JSON_OBJECT_GET (root_obj, "data");
			obj2 = JSON_OBJECT_GET (obj1, "loginInfo");
			obj3 = JSON_OBJECT_GET (obj2, "dpms_off_time");
			if (obj3) {
				int dpms_off_time = json_object_get_int (obj3);
				if (dpms_off_time >= 0 && dpms_off_time <= 60) {
					xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-ac-off", dpms_off_time);
					xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-battery-off", dpms_off_time);
				}
			}
			json_object_put (root_obj);
		}
	}

	g_free (data);
}

static void
agent_signal_cb (GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, gpointer user_data)
{
	if (g_str_equal (signal_name, "dpms_on_x_off")) {

		g_return_if_fail (user_data != NULL);

		XfconfChannel *channel = XFCONF_CHANNEL (user_data);

		gint32 value = 0;
		g_variant_get (parameters, "(i)", &value);

		if (value >= 0 && value <= 60) {
			xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-ac-off", value);
			xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-battery-off", value);
		}
	} else if (g_str_equal (signal_name, "update_operation")) {
		gint32 value = -1;
		g_variant_get (parameters, "(i)", &value);

		NotifyNotification *notification;
		gchar *cmdline = NULL;
		const gchar *message;
		const gchar *icon = "software-update-available-symbolic";
		const gchar *summary = _("Update Blocking Function");
		if (value == 0) {
			message = _("Update blocking function has been disabled.");
			cmdline = g_find_program_in_path ("gooroom-update-launcher");
		} else if (value == 1) {
			message = _("Update blocking function has been enabled.");
			gchar *cmd = g_find_program_in_path ("pkill");
			if (cmd) cmdline = g_strdup_printf ("%s -f '/usr/lib/gooroom/gooroomUpdate/gooroomUpdate.py'", cmd);
			g_free (cmd);
		}

		g_spawn_command_line_async (cmdline, NULL);
		g_free (cmdline);

		notify_init (PACKAGE_NAME);
		notification = notify_notification_new (summary, message, icon);

		notify_notification_set_urgency (notification, NOTIFY_URGENCY_NORMAL);
		notify_notification_set_timeout (notification, NOTIFY_EXPIRES_DEFAULT);
		notify_notification_show (notification, NULL);
		g_object_unref (notification);
	}
}

static void
gooroom_agent_bind_signal (gpointer data)
{
	if (!agent_proxy) {
		agent_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
				G_DBUS_CALL_FLAGS_NONE,
				NULL,
				"kr.gooroom.agent",
				"/kr/gooroom/agent",
				"kr.gooroom.agent",
				NULL,
				NULL);
	}

	if (agent_proxy) {
		g_signal_connect (agent_proxy, "g-signal", G_CALLBACK (agent_signal_cb), data);
	}
}

static gboolean
logout_session_cb (gpointer data)
{
	gchar *cmd = NULL;

	cmd = g_find_program_in_path ("xfce4-session-logout");
	if (cmd) {
		gchar *cmdline = g_strdup_printf ("%s -l", cmd);
		if (!g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, NULL)) {
			gchar *systemctl = g_find_program_in_path ("systemctl");
			if (systemctl) {
				gchar *reboot = g_strdup_printf ("%s reboot -i", systemctl);
				g_spawn_command_line_sync (reboot, NULL, NULL, NULL, NULL);
				g_free (reboot);
			}
			g_free (systemctl);
		}
		g_free (cmdline);
	}
	g_free (cmd);

	g_timeout_add (100, (GSourceFunc) gtk_main_quit, NULL);

	return FALSE;
}

static gboolean
start_job_on_offline (gpointer data)
{
	/* reload grac service */
	reload_grac_service ();

	g_timeout_add (100, (GSourceFunc) gtk_main_quit, NULL);

	return FALSE;
}

static gboolean
start_job_on_online (gpointer data)
{
	gchar *file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
		/* configure desktop */
		handle_desktop_configuration ();

		/* handle the Direct URL items */
		dock_launcher_update ();

		dpms_off_time_set (data);

		/* reload grac service */
		reload_grac_service ();

		gooroom_agent_bind_signal (data);

		/* password expiration warning */
//		handle_password_expiration ();
	} else {
		GtkWidget *message = gtk_message_dialog_new (NULL,
				GTK_DIALOG_MODAL,
				GTK_MESSAGE_ERROR,
				GTK_BUTTONS_OK,
				_("Terminating Session"));

		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (message),
				_("Could not found user's settings file.\nAfter 10 seconds, the user will be logged out."));

		gtk_window_set_title (GTK_WINDOW (message), _("Notifications"));

		g_timeout_add (1000 * 10, (GSourceFunc) logout_session_cb, data);

		gtk_dialog_run (GTK_DIALOG (message));
		gtk_widget_destroy (message);
	}

	g_free (file);

	return FALSE;
}

int
main (int argc, char **argv)
{
	GError *error = NULL;
	XfconfChannel *channel = NULL;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	if (!agent_proxy) {
		agent_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
				G_DBUS_CALL_FLAGS_NONE,
				NULL,
				"kr.gooroom.agent",
				"/kr/gooroom/agent",
				"kr.gooroom.agent",
				NULL,
				NULL);
	}

	if (is_online_user (g_get_user_name ())) {
		/* Initialize xfconf */
		if (!xfconf_init (&error)) {
			/* Print error and exit */
			g_error ("Failed to connect to xfconf daemon: %s.", error->message);
			g_error_free (error);
		}

		channel = xfconf_channel_new ("xfce4-power-manager");

		remove_custom_desktop_files ();

		g_timeout_add (200, (GSourceFunc) start_job_on_online, channel);
	} else {
		g_timeout_add (200, (GSourceFunc) start_job_on_offline, NULL);
	}

	gtk_main ();

	if (agent_proxy)
		g_object_unref (agent_proxy);

	if (channel)
		g_object_unref (channel);

	xfconf_shutdown ();

	return 0;
}

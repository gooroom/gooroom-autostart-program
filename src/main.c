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
#include <polkit/polkit.h>

#include <glib.h>
#include <glib/gstdio.h>
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




static gboolean
authenticate (const gchar *action_id)
{
	GPermission *permission;
	permission = polkit_permission_new_sync (action_id, NULL, NULL, NULL);

	if (!g_permission_get_allowed (permission)) {
		if (g_permission_acquire (permission, NULL, NULL)) {
			return TRUE;
		}
		return FALSE;
	}

	return TRUE;
}

static void
dpms_off_time_update (gint32 value, XfconfChannel *channel)
{
	if (value >= 0 && value <= 60) {
		xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-ac-off", value);
		xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-battery-off", value);
	}
}

static GDBusProxy *
agent_proxy_get (void)
{
	if (agent_proxy == NULL) {
		agent_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
				G_DBUS_CALL_FLAGS_NONE,
				NULL,
				"kr.gooroom.agent",
				"/kr/gooroom/agent",
				"kr.gooroom.agent",
				NULL,
				NULL);
	}

	return agent_proxy;
}

static gboolean
get_object_path (gchar **object_path, const gchar *service_name)
{
	GVariant   *variant;
	GDBusProxy *proxy;
	GError     *error = NULL;

	proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
			G_DBUS_CALL_FLAGS_NONE, NULL,
			"org.freedesktop.systemd1",
			"/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager",
			NULL, &error);

	if (!proxy) {
		g_error_free (error);
		return FALSE;
	}

	variant = g_dbus_proxy_call_sync (proxy, "GetUnit",
			g_variant_new ("(s)", service_name),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

	if (!variant) {
		g_error_free (error);
	} else {
		g_variant_get (variant, "(o)", object_path);
		g_variant_unref (variant);
	}

	g_object_unref (proxy);

	return TRUE;
}

static gboolean
is_systemd_service_active (const gchar *service_name)
{
	gboolean ret = FALSE;

	GVariant   *variant;
	GDBusProxy *proxy;
	GError     *error = NULL;
	gchar      *obj_path = NULL;

	get_object_path (&obj_path, service_name);
	if (!obj_path) {
		goto done;
	}

	proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
			G_DBUS_CALL_FLAGS_NONE, NULL,
			"org.freedesktop.systemd1",
			obj_path,
			"org.freedesktop.DBus.Properties",
			NULL, &error);

	if (!proxy)
		goto done;

	variant = g_dbus_proxy_call_sync (proxy, "GetAll",
			g_variant_new ("(s)", "org.freedesktop.systemd1.Unit"),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

	if (variant) {
		gchar *output = NULL;
		GVariant *asv = g_variant_get_child_value(variant, 0);
		GVariant *value = g_variant_lookup_value(asv, "ActiveState", NULL);
		if(value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
			output = g_variant_dup_string(value, NULL);
			if (g_strcmp0 (output, "active") == 0) {
				ret = TRUE;;
			}
			g_free (output);
		}

		g_variant_unref (variant);
	}

	g_object_unref (proxy);

done:
	if (error)
		g_error_free (error);

	return ret;
}

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

	FILE *fp = fopen (download_path, "wb");
	if (!fp)
		goto error;

	curl_easy_setopt (curl, CURLOPT_URL, download_url);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 10);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);
	if (g_str_has_prefix (curl, "https://")) {
		const gchar *GOOROOM_CERT = "/etc/ssl/certs/gooroom_client.crt";
		const gchar *GOOROOM_PRIVATE_KEY = "/etc/ssl/private/gooroom_client.key";
		curl_easy_setopt (curl, CURLOPT_SSLCERT, GOOROOM_CERT);
		curl_easy_setopt (curl, CURLOPT_SSLKEY, GOOROOM_PRIVATE_KEY);
	}

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
	g_remove (favicon_path);

	download_with_curl (favicon_url, favicon_path);

	if (!g_file_test (favicon_path, G_FILE_TEST_EXISTS))
		goto error;

	// check file size
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

static gboolean
has_application (GList *list, GAppInfo *appinfo)
{
	const gchar *id;

	if (appinfo) {
		id = g_app_info_get_id (appinfo);
	} else {
		id = NULL;
	}

	if (!id) return FALSE;

	GList *l = NULL;
	for (l = list; l; l = l->next) {
		GAppInfo *appinfo = G_APP_INFO (l->data);
		if (appinfo) {
			const gchar *_id = g_app_info_get_id (appinfo);
			if (g_str_equal (id, _id))
				return TRUE;
		}
	}

	return FALSE;
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
	/* we don't want to show in application launcher */
	g_key_file_set_string (keyfile, "Desktop Entry", "NoDisplay", "true");

	ret = g_key_file_save_to_file (keyfile, dt_file_name, NULL);
	g_key_file_free (keyfile);

	return ret;
}

static gboolean
desktop_has_name (const gchar *id, const gchar *name)
{
	gboolean ret = FALSE;

	gchar *desktop = g_build_filename ("/usr/share/applications", id, NULL);
	GKeyFile *keyfile = g_key_file_new ();

    if (g_key_file_load_from_file (keyfile,
                                   desktop,
                                   G_KEY_FILE_KEEP_COMMENTS |
                                   G_KEY_FILE_KEEP_TRANSLATIONS,
                                   NULL)) {
		gsize num_keys, i;
		gchar **keys = g_key_file_get_keys (keyfile, "Desktop Entry", &num_keys, NULL);

		for (i = 0; i < num_keys; i++) {
			if (!g_str_has_prefix (keys[i], "Name"))
				continue;

			gchar *value = g_key_file_get_value (keyfile, "Desktop Entry", keys[i], NULL);
			if (value) {
				if (strstr (value, name) != NULL) {
					ret = TRUE;
				}
			}
			g_free (value);
		}
		g_strfreev (keys);
	}
	g_key_file_free (keyfile);
	g_free (desktop);

	return ret;
}

static gchar *
find_desktop_by_id (GList *apps, const gchar *find_str)
{
	GList *l = NULL;
	gchar *ret = NULL;

	if (!find_str || g_str_equal (find_str, ""))
		return NULL;

	for (l = apps; l; l = l->next) {
		GAppInfo *appinfo = G_APP_INFO (l->data);
		if (appinfo) { 
			const gchar *id = g_app_info_get_id (appinfo);

			if (g_str_equal (id, find_str)) {
				ret = g_strdup (id);
				break;
			}

			if (desktop_has_name (id, find_str)) {
				ret = g_strdup (id);
				break;
			}
		}
	}

	return ret;
}

static gchar *
get_dpms_off_time_from_json (const gchar *data)
{
	g_return_val_if_fail (data != NULL, NULL);

	gchar *ret = NULL;

	enum json_tokener_error jerr = json_tokener_success;
	json_object *root_obj = json_tokener_parse_verbose (data, &jerr);

	if (jerr == json_tokener_success) {
		json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL, *obj4 = NULL;
		obj1 = JSON_OBJECT_GET (root_obj, "module");
		obj2 = JSON_OBJECT_GET (obj1, "task");
		obj3 = JSON_OBJECT_GET (obj2, "out");
		obj4 = JSON_OBJECT_GET (obj3, "status");
		if (obj4) {
			const char *val = json_object_get_string (obj4);
			if (val && g_strcmp0 (val, "200") == 0) {
				json_object *obj = JSON_OBJECT_GET (obj3, "screen_time");
				ret = g_strdup (json_object_get_string (obj));
			}
		}
		json_object_put (root_obj);
	}

	return ret;
}

static gchar *
get_blacklist_from_json (const gchar *data)
{
	g_return_val_if_fail (data != NULL, NULL);

	gchar *ret = NULL;

	enum json_tokener_error jerr = json_tokener_success;
	json_object *root_obj = json_tokener_parse_verbose (data, &jerr);

	if (jerr == json_tokener_success) {
		json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL, *obj4 = NULL;
		obj1 = JSON_OBJECT_GET (root_obj, "module");
		obj2 = JSON_OBJECT_GET (obj1, "task");
		obj3 = JSON_OBJECT_GET (obj2, "out");
		obj4 = JSON_OBJECT_GET (obj3, "status");
		if (obj4) {
			const char *val = json_object_get_string (obj4);
			if (val && g_strcmp0 (val, "200") == 0) {
				json_object *obj = JSON_OBJECT_GET (obj3, "black_list");
				ret = g_strdup (json_object_get_string (obj));
			}
		}
		json_object_put (root_obj);
	}

	return ret;
}

static void
save_application_blacklist (gchar *blacklist)
{
	g_return_if_fail (blacklist != NULL);

	GSettingsSchema *schema;

	schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                              "apps.gooroom-applauncher-plugin",
                                              TRUE);
	if (schema) {
		gchar **filters;
		GSettings *settings;

		filters = g_strsplit (blacklist, ",", -1);

		settings = g_settings_new_full (schema, NULL, NULL);
		g_settings_set_strv (settings, "blacklist", (const char * const *) filters);
		g_object_unref (settings);

		g_strfreev (filters);

		g_settings_schema_unref (schema);
	}
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

//static gboolean
//run_control_center_async (gpointer data)
//{
//	gchar *cmd = g_find_program_in_path ("gooroom-control-center");
//	if (cmd) {
//		gchar *cmdline = g_strdup_printf ("%s user", cmd);
//		g_spawn_command_line_async (cmdline, NULL);
//		g_free (cmdline);
//	}
//	g_free (cmd);
//
//	return FALSE;
//}

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

	guint i = 0;
	GSList *l = NULL;
	gchar **array = NULL;
	gchar *cmd, *cmdline;

//	gconftool-2 --type list --list-type string --set /apps/dockbarx/launchers '[glade;/usr/share/applications/gparted.desktop]'";
	cmd = g_find_program_in_path ("gconftool-2");

	array = g_new0 (gchar *, g_slist_length (launchers) + 1);

	for (l = launchers; l; l = l->next) {
		array[i++] = g_strdup ((gchar *)l->data);
	}
	array[i] = NULL;

	gchar *strlist = g_strjoinv (",", array);

	cmdline = g_strdup_printf ("%s --type list --list-type string --set /apps/dockbarx/launchers '[%s]'", cmd, strlist);

	g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, NULL);

	g_free (cmd);
	g_free (cmdline);
	g_free (strlist);
	g_strfreev (array);
}

static GSList *
dockbarx_launchers_get (void)
{
	GConfClient *gconf;
	GSList *old_launchers, *new_launchers = NULL;

	gconf = gconf_client_get_default ();

	old_launchers = gconf_client_get_list (gconf, "/apps/dockbarx/launchers", GCONF_VALUE_STRING, NULL);

	GSList *l = NULL;
	for (l = old_launchers; l != NULL; l = l->next) {
		gchar *launcher = (gchar *)l->data;
		gchar *desktop = g_strrstr (launcher, ";")+1;
		if (g_file_test (desktop, G_FILE_TEST_EXISTS)) {
			new_launchers = g_slist_append (new_launchers, g_strdup (launcher));
		}
	}

	g_slist_free_full (old_launchers, (GDestroyNotify) g_free);
	g_object_unref (gconf);

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

		g_timeout_add (500, (GSourceFunc) restart_dockbarx_async, NULL);

		return FALSE;
	}

	gboolean matched = TRUE;
	GSList *l = NULL;
	GSList *new_launchers = (GSList *)data;
	GSList *old_launchers = dockbarx_launchers_get ();

	// old_launchers and new_launchers must be same
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

			g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);

			gtk_widget_show (dialog);

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
	g_timeout_add (500, (GSourceFunc) restart_dockbarx_async, NULL);

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
						dockbarx_launchers_set (new_launchers);
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

//static void
//show_message (const gchar *title, const gchar *msg, const gchar *icon)
//{
//	GtkWidget *dialog;
//
//	dialog = GTK_WIDGET (gtk_message_dialog_new (NULL,
//				GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
//				GTK_MESSAGE_INFO,
//				GTK_BUTTONS_OK,
//				NULL));
//
//	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), msg, NULL);
//
//	gtk_window_set_title (GTK_WINDOW (dialog), title);
//	gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
//
//	gtk_dialog_run (GTK_DIALOG (dialog));
//	gtk_widget_destroy (dialog);
//}

//static gint
//check_shadow_expiry (long lastchg, int maxdays)
//{
//	gint warndays = 7;
//	gint daysleft = 9999;
//	long curdays;
//
//	curdays = (long)(time(NULL) / (60 * 60 * 24));
//
//	if ((curdays - lastchg) >= (maxdays - warndays)) {
//		daysleft = (gint)((lastchg + maxdays) - curdays);
//	}
//
//	return daysleft;
//}

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
			g_remove (wallpaper_path);

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

//static void
//handle_password_expiration (void)
//{
//	gchar *data = get_grm_user_data ();
//
//	if (data) {
//		enum json_tokener_error jerr = json_tokener_success;
//		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
//		if (jerr == json_tokener_success) {
//			gboolean need_change_passwd = FALSE;
//			gboolean passwd_init = FALSE;
//			json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL;
//			json_object *obj2_1 = NULL, *obj2_2 = NULL, *obj2_3 = NULL;
//
//			obj1 = JSON_OBJECT_GET (root_obj, "data");
//			obj2 = JSON_OBJECT_GET (obj1, "loginInfo");
//			obj2_1 = JSON_OBJECT_GET (obj2, "pwd_last_day");
//			obj2_2 = JSON_OBJECT_GET (obj2, "pwd_max_day");
//			obj2_3 = JSON_OBJECT_GET (obj2, "pwd_temp_yn");
//			if (obj2_3) {
//				const char *value = json_object_get_string (obj2_3);
//				if (value && g_strcmp0 (value, "Y") == 0) {
//					passwd_init = TRUE;
//					const gchar *msg = _("Your password has been issued temporarily.\nFor security reasons, please change your password immediately.");
//					show_message (_("Change Password"), msg, "dialog-error");
//					need_change_passwd = TRUE;
//				}
//			}
//
//			if (!passwd_init && obj2_1 && obj2_2) {
//				const char *value = json_object_get_string (obj2_1);
//				int max_days = json_object_get_int (obj2_2);
//				long last_days = strtoday (value);
//
//				if (last_days != -1 && max_days != -1) {
//					gint daysleft = check_shadow_expiry (last_days, max_days);
//
//					if (daysleft > 0 && daysleft < 9999) {
//						gchar *msg = g_strdup_printf (_("You have %d days to change your password.\nPlease change your password within %d days."), daysleft, daysleft);
//						show_message (_("Change Password"), msg, "dialog-warn");
//						g_free (msg);
//						need_change_passwd = TRUE;
//					} else if (daysleft == 0) {
//						show_message (_("Change Password"), _("Password change period is until today.\nPlease change your password today."), "dialog-warn");
//						need_change_passwd = TRUE;
//					} else if (daysleft < 0){
//						show_message (_("Change Password"), _("The password change period has already passed.\nFor security reasons, please change your password immediately."), "dialog-warn");
//						need_change_passwd = TRUE;
//					}
//				}
//			}
//			json_object_put (root_obj);
//
//			if (need_change_passwd) {
//				g_timeout_add (100, (GSourceFunc) run_control_center_async, NULL);
//			}
//		}
//	}
//
//	g_free (data);
//}

static void
reload_grac_service_done_cb (GObject *source, GAsyncResult *res, gpointer data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source);

	g_dbus_proxy_call_finish (proxy, res, NULL);
}

static void
reload_grac_service (void)
{
	if (!authenticate ("kr.gooroom.autostart.program.systemctl"))
		return;

	GDBusProxy  *proxy = NULL;
	gboolean     success = FALSE;
	const gchar *service_name = "grac-device-daemon.service";

	proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
			G_DBUS_CALL_FLAGS_NONE,
			NULL,
			"org.freedesktop.systemd1",
			"/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager",
			NULL, NULL);

	if (proxy) {
		GVariant *variant = NULL;
		variant = g_dbus_proxy_call_sync (proxy, "ReloadUnit",
				g_variant_new ("(ss)", service_name, "replace"),
				G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

		if (variant) {
			g_variant_unref (variant);
			success = TRUE;
		}

		g_object_unref (proxy);
	}

#if 0
	if (!success) {
		GtkWidget *dlg = gtk_message_dialog_new (NULL,
				GTK_DIALOG_MODAL,
				GTK_MESSAGE_INFO,
				GTK_BUTTONS_CLOSE,
				NULL);

		const gchar *secondary_text = _("Failed to restart GRAC service.\nPlease login again.");
		gtk_window_set_title (GTK_WINDOW (dlg), _("GRAC Service Start Failure"));
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dlg), "%s", secondary_text);
		g_signal_connect (dlg, "response", G_CALLBACK (gtk_widget_destroy), NULL);

		gtk_widget_show (dlg);
	}
#endif
}

static void
request_dpms_off_time_done_cb (GObject      *source_object,
                               GAsyncResult *res,
                               gpointer      user_data)
{
	GVariant *variant;
	gchar *data = NULL;
	XfconfChannel *channel;

	channel = XFCONF_CHANNEL (user_data);

	variant = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, NULL);
	if (variant) {
		GVariant *v;
		g_variant_get (variant, "(v)", &v);
		if (v) {
			data = g_variant_dup_string (v, NULL);
			g_variant_unref (v);
		}
		g_variant_unref (variant);
	}

	if (data) {
		gchar *value = get_dpms_off_time_from_json (data);
		dpms_off_time_update (atoi (value), channel);
		g_free (value);
		g_free (data);
	}
}

static void
dpms_off_time_set (gpointer data)
{
	agent_proxy = agent_proxy_get ();
	if (agent_proxy) {
		const gchar *json = "{\"module\":{\"module_name\":\"config\",\"task\":{\"task_name\":\"dpms_off_time\",\"in\":{\"login_id\":\"%s\"}}}}";

		gchar *arg = g_strdup_printf (json, g_get_user_name ());

		g_dbus_proxy_call (agent_proxy,
                           "do_task",
                           g_variant_new ("(s)", arg),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           request_dpms_off_time_done_cb,
                           data);
		g_free (arg);
	}
}

static void
agent_signal_cb (GDBusProxy *proxy,
                 gchar *sender_name,
                 gchar *signal_name,
                 GVariant *parameters,
                 gpointer user_data)
{
	g_return_if_fail (user_data != NULL);

	XfconfChannel *channel = XFCONF_CHANNEL (user_data);

	if (g_str_equal (signal_name, "dpms_on_x_off")) {
		gint32 value = 0;
		g_variant_get (parameters, "(i)", &value);
		dpms_off_time_update (value, channel);
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
	} else if (g_str_equal (signal_name, "app_black_list")) {
		GVariant *v = NULL;
		gchar *blacklist = NULL;
		g_variant_get (parameters, "(v)", &v);
		if (v) {
			blacklist = g_variant_dup_string (v, NULL);
			g_variant_unref (v);
		}

		if (blacklist) {
			save_application_blacklist (blacklist);
			g_free (blacklist);
		}
	}
}

static void
gooroom_agent_bind_signal (gpointer data)
{
	agent_proxy = agent_proxy_get ();
	if (agent_proxy) {
		g_signal_connect (agent_proxy, "g-signal", G_CALLBACK (agent_signal_cb), data);
	}
}

static void
request_app_blacklist_done_cb (GObject      *source_object,
                               GAsyncResult *res,
                               gpointer      user_data)
{
	GVariant *variant;
	gchar *data = NULL;

	variant = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, NULL);
	if (variant) {
		GVariant *v;
		g_variant_get (variant, "(v)", &v);
		if (v) {
			data = g_variant_dup_string (v, NULL);
			g_variant_unref (v);
		}
		g_variant_unref (variant);
	}

	if (data) {
		gchar *blacklist = get_blacklist_from_json (data);
		if (blacklist) {
			save_application_blacklist (blacklist);
			g_free (blacklist);
		}
		g_free (data);
	}
}

static void
application_blacklist_update ()
{
	agent_proxy = agent_proxy_get ();
	if (agent_proxy) {
		const gchar *json = "{\"module\":{\"module_name\":\"config\",\"task\":{\"task_name\":\"get_app_list\",\"in\":{\"login_id\":\"%s\"}}}}";

		gchar *arg = g_strdup_printf (json, g_get_user_name ());

		g_dbus_proxy_call (agent_proxy,
                           "do_task",
                           g_variant_new ("(s)", arg),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           request_app_blacklist_done_cb,
                           NULL);
		g_free (arg);
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

static void
start_job_on_online (gpointer data)
{
	gchar *file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
		/* configure desktop */
		handle_desktop_configuration ();

		/* handle the Direct URL items */
		dock_launcher_update ();
	} else {
		GtkWidget *message = gtk_message_dialog_new (NULL,
				GTK_DIALOG_MODAL,
				GTK_MESSAGE_ERROR,
				GTK_BUTTONS_OK,
				NULL);

		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (message),
				_("Could not found user's settings file.\nAfter 10 seconds, the user will be logged out."));

		gtk_window_set_title (GTK_WINDOW (message), _("Terminating Session"));

		g_signal_connect (message, "response", G_CALLBACK (gtk_widget_destroy), NULL);

		g_timeout_add (1000 * 10, (GSourceFunc) logout_session_cb, data);

		gtk_widget_show (message);
	}

	g_free (file);
}

static gboolean
start_job (gpointer data)
{
	remove_custom_desktop_files ();

	if (is_online_user (g_get_user_name ())) {
		start_job_on_online (data);
	}

	/* reload grac service */
	reload_grac_service ();

	dpms_off_time_set (data);

	application_blacklist_update ();

	gooroom_agent_bind_signal (data);

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

	if (!xfconf_init (&error)) {
		g_error ("Failed to connect to xfconf daemon: %s.", error->message);
		g_error_free (error);
	}

	channel = xfconf_channel_new ("xfce4-power-manager");

	g_timeout_add (200, (GSourceFunc) start_job, channel);

	gtk_main ();

	if (agent_proxy)
		g_object_unref (agent_proxy);

	if (channel)
		g_object_unref (channel);

	xfconf_shutdown ();

	return 0;
}

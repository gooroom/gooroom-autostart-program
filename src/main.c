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

#include <dbus/dbus.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>


#include <xfconf/xfconf.h>

#include "dockitem_file_template.h"

#define	GRM_USER		".grm-user"


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

static gchar *
download_favicon (const gchar *favicon_url, gint num)
{
	if (!g_file_test (g_get_user_cache_dir (), G_FILE_TEST_EXISTS)) {
		return NULL;
	}

	CURL *curl;
	gchar *favicon_path = NULL;

	curl_global_init (CURL_GLOBAL_ALL);

	curl = curl_easy_init (); 

	if (curl) {
		favicon_path = g_strdup_printf ("%s/favicon-%.02d", g_get_user_cache_dir (), num);

		FILE *fp = fopen (favicon_path, "w");
		curl_easy_setopt (curl, CURLOPT_URL, favicon_url);
		curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, NULL);
		curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);
		curl_easy_perform (curl);
		curl_easy_cleanup (curl);
		fclose(fp);
	}

	curl_global_cleanup ();

	return favicon_path;
}


static gchar *
get_desktop_directory (json_object *obj)
{
	g_return_val_if_fail (obj != NULL, NULL);

	gchar *desktop_dir = NULL;
	const char *val = json_object_get_string (obj);

	if (g_strcmp0 (val, "bar") == 0) {
		desktop_dir = g_strdup_printf ("%s/applications/custom", g_get_user_data_dir ());
	} else {
		desktop_dir = g_strdup_printf ("%s/applications", g_get_user_data_dir ());
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
		const char *value = json_object_get_string (val);
		gchar *d_key = g_ascii_strdown (key, -1);

		if (g_strcmp0 (d_key, "icon") == 0) {
			if (g_str_has_prefix (value, "http://") || g_str_has_prefix (value, "https://")) {
				gchar *icon_file = download_favicon (value, num);
				if (icon_file) {
					g_key_file_set_string (keyfile, "Desktop Entry", "Icon", icon_file);
					g_free (icon_file);
				} else {
					g_key_file_set_string (keyfile, "Desktop Entry", "Icon", "plank");
				}
			} else {
				g_key_file_set_string (keyfile, "Desktop Entry", "Icon", value);
			}
		} else {
			const char *new_key;

			if (g_strcmp0 (d_key, "name") == 0) {
				new_key = "Name";
			} else if (g_strcmp0 (d_key, "comment") == 0) {
				new_key = "Comment";
			} else if (g_strcmp0 (d_key, "exec") == 0) {
				new_key = "Exec";
			} else {
				new_key = NULL;
			}

			if (new_key)
				g_key_file_set_string (keyfile, "Desktop Entry", new_key, value);
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

static void
make_direct_url (json_object *root_obj)
{
	g_return_if_fail (root_obj != NULL);

	json_object *apps_obj = NULL;
	json_object_object_get_ex (root_obj, "apps", &apps_obj);

	g_return_if_fail (apps_obj != NULL);

	gint i = 0, len = 0;;
	len = json_object_array_length (apps_obj);

	for (i = 0; i < len; i++) {
		json_object *app_obj = json_object_array_get_idx (apps_obj, i);

		if (app_obj) {
			gchar *dt_file_name = NULL;
			json_object *dt_obj = NULL, *pos_obj = NULL;

			json_object_object_get_ex (app_obj, "desktop", &dt_obj);
			json_object_object_get_ex (app_obj, "position", &pos_obj);

			if (dt_obj && pos_obj) {
				gchar *dt_dir_name = get_desktop_directory (pos_obj);
				if (dt_dir_name) {
					dt_file_name = g_strdup_printf ("%s/shortcut-%.02d.desktop", dt_dir_name, i);
				}
				g_free (dt_dir_name);

				if (create_desktop_file (dt_obj, dt_file_name, i)) {
					gchar *di_file_dir = g_strdup_printf ("%s/plank/dock1/launchers", g_get_user_config_dir ());
					if (g_file_test (di_file_dir, G_FILE_TEST_EXISTS)) {
						/* create dock item file */
						gchar *di_file_name = g_strdup_printf ("%s/plank/dock1/launchers/shortcut-%.02d.dockitem", g_get_user_config_dir (), i);
						gchar *di_file_contents = g_strdup_printf (dockitem_file_template, dt_file_name);

						g_file_set_contents (di_file_name, di_file_contents, -1, NULL);
						g_free (di_file_name);
						g_free (di_file_contents);
					} else {
						g_error ("No such file or directory : %s", di_file_dir);
					}
					g_free (di_file_dir);
				} else {
					g_error ("Could not create desktop file : %s", dt_file_name);
				}

				json_object_put (pos_obj);
				json_object_put (dt_obj);
			}

			g_free (dt_file_name);

			json_object_put (app_obj);
		}
	}

	json_object_put (apps_obj);
}

static void
remove_all_dock_items ()
{
	gchar *remove_dir = g_strdup_printf ("%s/applications/custom", g_get_user_data_dir ());
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
generate_dock_items (void)
{
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
						make_direct_url (obj3);
					}
				}
			}
			json_object_put (obj1);
			json_object_put (obj2);
			json_object_put (obj2_1);
			json_object_put (obj3);
		}
		json_object_put (root_obj);
	}

	g_free (data);
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
handle_password_expiration (void)
{
	gchar *data = get_grm_user_data ();

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
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
					} else if (daysleft == 0) {
						show_message (_("Change Password"), _("Password change period is until today.\nPlease change your password today."), "dialog-warn");
					} else if (daysleft < 0){
						show_message (_("Change Password"), _("The password change period has already passed.\nFor security reasons, please change your password immediately."), "dialog-warn");
					}
				}
			}
			json_object_put (obj1);
			json_object_put (obj2);
			json_object_put (obj2_1);
			json_object_put (obj2_2);
		}
		json_object_put (root_obj);
	}

	g_free (data);
}

static void
reload_grac_service (void)
{
	GDBusProxy *proxy;

	proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
			G_DBUS_CALL_FLAGS_NONE,
			NULL,
			"kr.gooroom.agent",
			"/kr/gooroom/agent",
			"kr.gooroom.agent",
			NULL,
			NULL);

	if (proxy) {
		const gchar *arg = "{\"module\":{\"module_name\":\"daemon_control\",\"task\":{\"task_name\":\"daemon_reload\",\"in\":{\"service\":\"grac-device-daemon.service\"}}}}";

		g_dbus_proxy_call_sync (proxy, "do_task",
				g_variant_new ("(s)", arg),
				G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

		g_object_unref (proxy);
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
			json_object_put (obj1);
			json_object_put (obj2);
			json_object_put (obj3);
		}
		json_object_put (root_obj);
	}

	g_free (data);
}

static DBusHandlerResult
handle_signal_cb (DBusConnection *connection, DBusMessage *msg, void *user_data)
{
    if (dbus_message_is_signal (msg, "kr.gooroom.agent", "dpms_on_x_off")) {
		if (user_data) {
			XfconfChannel *channel = XFCONF_CHANNEL (user_data);

			DBusError error;
			dbus_error_init(&error);

			gint32 value = 0;

			if (!dbus_message_get_args (msg, &error, DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID)) {
				g_error ("Could not read the value : %s", error.message);
				dbus_error_free (&error);
			}

			if (value >= 0 && value <= 60) {
				xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-ac-off", value);
				xfconf_channel_set_uint (channel, "/xfce4-power-manager/dpms-on-battery-off", value);
			}
		}
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
gooroom_agent_bind_signal (gpointer data)
{
	DBusError error;

	dbus_error_init (&error);
	DBusConnection *conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);

	if (dbus_error_is_set (&error)) {
		g_error ("Could not get System BUS connection: %s", error.message);
		dbus_error_free (&error);
		return;
	}

	dbus_connection_setup_with_g_main (conn, NULL);

	gchar *rule = "type='signal',interface='kr.gooroom.agent'";
	dbus_bus_add_match (conn, rule, &error);

	if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
		return;
	}

	dbus_connection_add_filter (conn, handle_signal_cb, data, NULL);
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

	gtk_main_quit ();

	return FALSE;
}

static gboolean
start_job (gpointer data)
{
	gchar *file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
		/* handle the Direct URL items */
		generate_dock_items ();

		/* password expiration warning */
		handle_password_expiration ();

		dpms_off_time_set (data);

		/* reload grac service */
		reload_grac_service ();

		gooroom_agent_bind_signal (data);
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

	if (is_online_user (g_get_user_name ())) {
		/* Initialize xfconf */
		if (!xfconf_init (&error)) {
			/* Print error and exit */
			g_error ("Failed to connect to xfconf daemon: %s.", error->message);
			g_error_free (error);
		}

		channel = xfconf_channel_new ("xfce4-power-manager");

		remove_all_dock_items ();

		g_timeout_add (3000, (GSourceFunc) start_job, channel);
	} else {
		g_timeout_add (100, (GSourceFunc) gtk_main_quit, NULL);
	}

	gtk_main ();

	if (channel)
		g_object_unref (channel);

	xfconf_shutdown ();

	return 0;
}

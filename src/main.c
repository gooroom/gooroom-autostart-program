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

#include <curl/curl.h>
#include <json-c/json.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <polkit/polkit.h>

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

static gboolean
generate_dock_items (gpointer user_data)
{
	gchar *file = NULL;
	gchar *data = NULL;

	file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

    if (!g_file_test (file, G_FILE_TEST_EXISTS)) {
		g_error ("No such file or directory : %s", file);
		goto done;
	}
	
	g_file_get_contents (file, &data, NULL, NULL);

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

done:
	g_free (file);

	return FALSE;
}

static gboolean
is_online_user (const gchar *username)
{
	gboolean ret = FALSE;
	gchar *file = NULL;

	file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
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
	}

	g_free (file);

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

static gboolean
handle_password_expiration (gpointer user_data)
{
	gchar *file = NULL;
	gchar *data = NULL;

	file = g_strdup_printf ("/var/run/user/%d/gooroom/%s", getuid (), GRM_USER);

	if (!g_file_test (file, G_FILE_TEST_EXISTS)) {
		g_error ("No such file or directory : %s", file);
		goto done;
	}
	
	g_file_get_contents (file, &data, NULL, NULL);

	if (data) {
		enum json_tokener_error jerr = json_tokener_success;
		json_object *root_obj = json_tokener_parse_verbose (data, &jerr);
		if (jerr == json_tokener_success) {
			gboolean passwd_init = FALSE;
			json_object *obj1 = NULL, *obj2 = NULL, *obj3= NULL;
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


done:
	g_free (file);

	return FALSE;
}

static void
reload_grac_service (void)
{
#if 0
	if (!authenticate ("kr.gooroom.start.programs"))
		return;

	gchar *cmd, *pkexec;

	pkexec = g_find_program_in_path ("pkexec");
	cmd = g_strdup_printf ("%s /usr/bin/grac-reloader.py", pkexec);

	g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

	g_free (pkexec);
	g_free (cmd);
#endif
}

int
main (int argc, char **argv)
{
	gint exit_timeout = 100;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	/* reload grac service */
	reload_grac_service ();

	if (is_online_user (g_get_user_name ())) {
		exit_timeout = 10000;

		/* handle the Direct URL items */
		remove_all_dock_items ();
		g_timeout_add (3000, (GSourceFunc) generate_dock_items, NULL);

		/* password expiration warning */
		g_timeout_add (4000, (GSourceFunc) handle_password_expiration, NULL);
	}

	g_timeout_add (exit_timeout, (GSourceFunc) gtk_main_quit, NULL);

	gtk_main ();

	return 0;
}

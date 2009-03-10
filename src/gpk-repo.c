/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <locale.h>

#include <glade/glade.h>
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include <dbus/dbus-glib.h>
#include <gconf/gconf-client.h>
#include <packagekit-glib/packagekit.h>

#include "egg-unique.h"
#include "egg-debug.h"
#include "egg-string.h"

#include "gpk-gnome.h"
#include "gpk-common.h"
#include "gpk-error.h"
#include "gpk-animated-icon.h"
#include "gpk-enum.h"

static GladeXML *glade_xml = NULL;
static GtkListStore *list_store = NULL;
static PkClient *client = NULL;
static PkBitfield roles;
static GConfClient *gconf_client;
static gboolean show_details;
static GtkTreePath *path_global = NULL;

enum {
	REPO_COLUMN_ENABLED,
	REPO_COLUMN_TEXT,
	REPO_COLUMN_ID,
	REPO_COLUMN_ACTIVE,
	REPO_COLUMN_LAST
};

/**
 * gpk_repo_find_iter_model_cb:
 **/
static gboolean
gpk_repo_find_iter_model_cb (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, const gchar *repo_id)
{
	gchar *repo_id_tmp = NULL;
	gtk_tree_model_get (model, iter, REPO_COLUMN_ID, &repo_id_tmp, -1);
	if (strcmp (repo_id_tmp, repo_id) == 0) {
		path_global = gtk_tree_path_copy (path);
		return TRUE;
	}
	return FALSE;
}

/**
 * gpk_repo_mark_nonactive_cb:
 **/
static gboolean
gpk_repo_mark_nonactive_cb (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gtk_list_store_set (GTK_LIST_STORE(model), iter, REPO_COLUMN_ACTIVE, FALSE, -1);
	return FALSE;
}

/**
 * gpk_repo_mark_nonactive:
 **/
static void
gpk_repo_mark_nonactive (GtkTreeModel *model)
{
	gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc) gpk_repo_mark_nonactive_cb, NULL);
}

/**
 * gpk_repo_model_get_iter:
 **/
static gboolean
gpk_repo_model_get_iter (GtkTreeModel *model, GtkTreeIter *iter, const gchar *id)
{
	gboolean ret = TRUE;
	path_global = NULL;
	gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc) gpk_repo_find_iter_model_cb, (gpointer) id);
	if (path_global == NULL) {
		gtk_list_store_append (GTK_LIST_STORE(model), iter);
	} else {
		ret = gtk_tree_model_get_iter (model, iter, path_global);
		gtk_tree_path_free (path_global);
	}
	return ret;
}

/**
 * gpk_repo_remove_nonactive_cb:
 **/
static gboolean
gpk_repo_remove_nonactive_cb (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gboolean *ret)
{
	gboolean active;
	gtk_tree_model_get (model, iter, REPO_COLUMN_ACTIVE, &active, -1);
	if (!active) {
		*ret = TRUE;
		gtk_list_store_remove (GTK_LIST_STORE(model), iter);
		return TRUE;
	}
	return FALSE;
}

/**
 * gpk_repo_remove_nonactive:
 **/
static void
gpk_repo_remove_nonactive (GtkTreeModel *model)
{
	gboolean ret;
	/* do this again and again as removing in gtk_tree_model_foreach causes errors */
	do {
		ret = FALSE;
		gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc) gpk_repo_remove_nonactive_cb, &ret);
	} while (ret);
}

/**
 * gpk_button_help_cb:
 **/
static void
gpk_button_help_cb (GtkWidget *widget, gboolean  data)
{
	gpk_gnome_help ("software-sources");
}

static void
gpk_misc_installed_toggled (GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
	GtkWidget *widget;
	GtkTreeModel *model = (GtkTreeModel *)data;
	GtkTreeIter iter;
	GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
	gboolean installed;
	gchar *repo_id;
	gboolean ret;
	GError *error = NULL;

	/* do we have the capability? */
	if (pk_bitfield_contain (roles, PK_ROLE_ENUM_REPO_ENABLE) == FALSE) {
		egg_debug ("can't change state");
		return;
	}

	/* set insensitive until we've done this */
	widget = glade_xml_get_widget (glade_xml, "treeview_repo");
	gtk_widget_set_sensitive (widget, FALSE);

	/* get toggled iter */
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter,
			    REPO_COLUMN_ENABLED, &installed,
			    REPO_COLUMN_ID, &repo_id, -1);

	/* do something with the value */
	installed ^= 1;

	/* do this to the repo */
	egg_debug ("setting %s to %i", repo_id, installed);
	ret = pk_client_reset (client, &error);
	if (!ret) {
		egg_warning ("failed to reset client: %s", error->message);
		g_error_free (error);
		goto out;
	}
	ret = pk_client_repo_enable (client, repo_id, installed, &error);
	if (!ret) {
		egg_warning ("could not set repo enabled state: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* set new value */
	gtk_list_store_set (GTK_LIST_STORE(model), &iter, REPO_COLUMN_ENABLED, installed, -1);

out:
	/* clean up */
	g_free (repo_id);
	gtk_tree_path_free (path);
}

/**
 * gpk_repo_detail_cb:
 **/
static void
gpk_repo_detail_cb (PkClient *client_, const gchar *repo_id,
		    const gchar *description, gboolean enabled, gpointer data)
{
	GtkTreeIter iter;
	GtkTreeView *treeview = GTK_TREE_VIEW (glade_xml_get_widget (glade_xml, "treeview_repo"));
	GtkTreeModel *model = gtk_tree_view_get_model (treeview);

	egg_debug ("repo = %s:%s:%i", repo_id, description, enabled);

	gpk_repo_model_get_iter (model, &iter, repo_id);
	gtk_list_store_set (list_store, &iter,
			    REPO_COLUMN_ENABLED, enabled,
			    REPO_COLUMN_TEXT, description,
			    REPO_COLUMN_ID, repo_id,
			    REPO_COLUMN_ACTIVE, TRUE,
			    -1);

	/* sort after each entry, which is okay as there shouldn't be many */
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE(list_store), REPO_COLUMN_TEXT, GTK_SORT_ASCENDING);
}

/**
 * gpk_treeview_add_columns:
 **/
static void
gpk_treeview_add_columns (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeModel *model = gtk_tree_view_get_model (treeview);

	/* column for installed toggles */
	renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (renderer, "toggled", G_CALLBACK (gpk_misc_installed_toggled), model);

	/* TRANSLATORS: column if the source is enabled */
	column = gtk_tree_view_column_new_with_attributes (_("Enabled"), renderer,
							   "active", REPO_COLUMN_ENABLED, NULL);
	gtk_tree_view_append_column (treeview, column);

	/* column for text */
	renderer = gtk_cell_renderer_text_new ();
	/* TRANSLATORS: column for the source description */
	column = gtk_tree_view_column_new_with_attributes (_("Software Source"), renderer,
							   "markup", REPO_COLUMN_TEXT, NULL);
	gtk_tree_view_column_set_sort_column_id (column, REPO_COLUMN_TEXT);
	gtk_tree_view_append_column (treeview, column);
}

/**
 * gpk_repos_treeview_clicked_cb:
 **/
static void
gpk_repos_treeview_clicked_cb (GtkTreeSelection *selection, gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *repo_id;

	/* This will only work in single or browse selection mode! */
	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		gtk_tree_model_get (model, &iter, REPO_COLUMN_ID, &repo_id, -1);
		egg_debug ("selected row is: %s", repo_id);
		g_free (repo_id);
	} else {
		egg_debug ("no row selected");
	}
}

/**
 * gpk_repo_finished_cb:
 **/
static void
gpk_repo_finished_cb (PkClient *client_, PkExitEnum exit, guint runtime, gpointer data)
{
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkWidget *widget;

	/* set sensitive now we've done this */
	widget = glade_xml_get_widget (glade_xml, "treeview_repo");
	gtk_widget_set_sensitive (widget, TRUE);

	/* remove the items that are not used */
	treeview = GTK_TREE_VIEW (glade_xml_get_widget (glade_xml, "treeview_repo"));
	model = gtk_tree_view_get_model (treeview);
	gpk_repo_remove_nonactive (model);
}

/**
 * gpk_repo_status_changed_cb:
 **/
static void
gpk_repo_status_changed_cb (PkClient *client_, PkStatusEnum status, gpointer data)
{
	const gchar *text;
	GtkWidget *widget;

	widget = glade_xml_get_widget (glade_xml, "viewport_animation_preview");
	if (status == PK_STATUS_ENUM_FINISHED) {
		gtk_widget_hide (widget);
		widget = glade_xml_get_widget (glade_xml, "image_animation");
		gpk_animated_icon_enable_animation (GPK_ANIMATED_ICON (widget), FALSE);
		return;
	}

	/* set the text and show */
	gtk_widget_show (widget);
	widget = glade_xml_get_widget (glade_xml, "label_animation");
	text = gpk_status_enum_to_localised_text (status);
	gtk_label_set_label (GTK_LABEL (widget), text);

	/* set icon */
	widget = glade_xml_get_widget (glade_xml, "image_animation");
	gpk_set_animated_icon_from_status (GPK_ANIMATED_ICON (widget), status, GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_widget_show (widget);
}

/**
 * gpk_repo_error_code_cb:
 **/
static void
gpk_repo_error_code_cb (PkClient *client_, PkErrorCodeEnum code, const gchar *details, gpointer data)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (glade_xml, "dialog_repo");
	/* TRANSLATORS: for one reason or another, we could not enable or disable a software source */
	gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to change status"),
				gpk_error_enum_to_localised_text (code), details);
}

/**
 * gpk_repo_repo_list_refresh:
 **/
static void
gpk_repo_repo_list_refresh (void)
{
	gboolean ret;
	GError *error = NULL;
	PkBitfield filters;
	GtkTreeView *treeview;
	GtkTreeModel *model;

	/* mark the items as not used */
	treeview = GTK_TREE_VIEW (glade_xml_get_widget (glade_xml, "treeview_repo"));
	model = gtk_tree_view_get_model (treeview);
	gpk_repo_mark_nonactive (model);

	egg_debug ("refreshing list");
	ret = pk_client_reset (client, &error);
	if (!ret) {
		egg_warning ("failed to reset client: %s", error->message);
		g_error_free (error);
		return;
	}
	if (!show_details)
		filters = pk_bitfield_value (PK_FILTER_ENUM_NOT_DEVELOPMENT);
	else
		filters = pk_bitfield_value (PK_FILTER_ENUM_NONE);
	ret = pk_client_get_repo_list (client, filters, &error);
	if (!ret) {
		egg_warning ("failed to get repo list: %s", error->message);
		g_error_free (error);
	}
}

/**
 * gpk_repo_repo_list_changed_cb:
 **/
static void
gpk_repo_repo_list_changed_cb (PkControl *control, gpointer data)
{
	gpk_repo_repo_list_refresh ();
}

/**
 * gpk_repo_checkbutton_details:
 **/
static void
gpk_repo_checkbutton_details (GtkWidget *widget, gpointer data)
{
	show_details = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
	egg_debug ("Changing %s to %i", GPK_CONF_REPO_SHOW_DETAILS, show_details);
	gconf_client_set_bool (gconf_client, GPK_CONF_REPO_SHOW_DETAILS, show_details, NULL);
	gpk_repo_repo_list_refresh ();
}

/**
 * gpk_repo_activated_cb
 **/
static void
gpk_repo_activated_cb (EggUnique *egg_unique, gpointer data)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (glade_xml, "dialog_repo");
	gtk_window_present (GTK_WINDOW (widget));
}

/**
 * gpk_repo_create_custom_widget:
 **/
static GtkWidget *
gpk_repo_create_custom_widget (GladeXML *xml, gchar *func_name, gchar *name,
			       gchar *string1, gchar *string2,
			       gint int1, gint int2, gpointer user_data)
{
	if (egg_strequal (name, "image_animation"))
		return gpk_animated_icon_new ();
	egg_warning ("name unknown='%s'", name);
	return NULL;
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	gboolean verbose = FALSE;
	GOptionContext *context;
	GtkWidget *main_window;
	GtkWidget *widget;
	GtkTreeSelection *selection;
	PkControl *control;
	EggUnique *egg_unique;
	gboolean ret;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
		  _("Show extra debugging information"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	if (! g_thread_supported ())
		g_thread_init (NULL);
	dbus_g_thread_init ();
	g_type_init ();

	context = g_option_context_new (NULL);
	g_option_context_set_summary (context, _("Software Source Viewer"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	egg_debug_init (verbose);
	gtk_init (&argc, &argv);

	/* TRANSLATORS: title to pass to to the user if there are not enough privs */
	ret = gpk_check_privileged_user (_("Software source viewer"), TRUE);
	if (!ret)
		return 1;

        /* add application specific icons to search path */
        gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
                                           GPK_DATA G_DIR_SEPARATOR_S "icons");

	/* are we already activated? */
	egg_unique = egg_unique_new ();
	ret = egg_unique_assign (egg_unique, "org.freedesktop.PackageKit.Repo");
	if (!ret)
		goto unique_out;
	g_signal_connect (egg_unique, "activated",
			  G_CALLBACK (gpk_repo_activated_cb), NULL);

	gconf_client = gconf_client_get_default ();

	client = pk_client_new ();
	g_signal_connect (client, "repo-detail",
			  G_CALLBACK (gpk_repo_detail_cb), NULL);
	g_signal_connect (client, "status-changed",
			  G_CALLBACK (gpk_repo_status_changed_cb), NULL);
	g_signal_connect (client, "finished",
			  G_CALLBACK (gpk_repo_finished_cb), NULL);
	g_signal_connect (client, "error-code",
			  G_CALLBACK (gpk_repo_error_code_cb), NULL);

	control = pk_control_new ();
	g_signal_connect (control, "repo-list-changed",
			  G_CALLBACK (gpk_repo_repo_list_changed_cb), NULL);
	roles = pk_control_get_actions (control, NULL);

	/* use custom widgets */
	glade_set_custom_handler (gpk_repo_create_custom_widget, NULL);

	glade_xml = glade_xml_new (GPK_DATA "/gpk-repo.glade", NULL, NULL);
	main_window = glade_xml_get_widget (glade_xml, "dialog_repo");
	gtk_window_set_icon_name (GTK_WINDOW (main_window), GPK_ICON_SOFTWARE_SOURCES);

	/* Get the main window quit */
	g_signal_connect_swapped (main_window, "delete_event", G_CALLBACK (gtk_main_quit), NULL);

	widget = glade_xml_get_widget (glade_xml, "button_close");
	g_signal_connect_swapped (widget, "clicked", G_CALLBACK (gtk_main_quit), NULL);
	widget = glade_xml_get_widget (glade_xml, "button_help");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_button_help_cb), NULL);

	widget = glade_xml_get_widget (glade_xml, "checkbutton_detail");
	show_details = gconf_client_get_bool (gconf_client, GPK_CONF_REPO_SHOW_DETAILS, NULL);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), show_details);
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_repo_checkbutton_details), NULL);

	/* set a size, if the screen allows */
	gpk_window_set_size_request (GTK_WINDOW (main_window), 500, 300);

	/* create list stores */
	list_store = gtk_list_store_new (REPO_COLUMN_LAST, G_TYPE_BOOLEAN,
					 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);

	/* create repo tree view */
	widget = glade_xml_get_widget (glade_xml, "treeview_repo");
	gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
				 GTK_TREE_MODEL (list_store));

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	g_signal_connect (selection, "changed",
			  G_CALLBACK (gpk_repos_treeview_clicked_cb), NULL);

	/* add columns to the tree view */
	gpk_treeview_add_columns (GTK_TREE_VIEW (widget));
	gtk_tree_view_columns_autosize (GTK_TREE_VIEW (widget));

	/* show window */
	gtk_widget_show (main_window);

	/* focus back to the close button */
	widget = glade_xml_get_widget (glade_xml, "button_close");
	gtk_widget_grab_focus (widget);

	if (pk_bitfield_contain (roles, PK_ROLE_ENUM_GET_REPO_LIST)) {
		/* get the update list */
		gpk_repo_repo_list_refresh ();
	} else {
		gpk_repo_detail_cb (client, "default",
				   _("Getting software source list not supported by backend"), FALSE, NULL);
		widget = glade_xml_get_widget (glade_xml, "treeview_repo");
		gtk_widget_set_sensitive (widget, FALSE);
		widget = glade_xml_get_widget (glade_xml, "checkbutton_detail");
		gtk_widget_set_sensitive (widget, FALSE);
	}

	/* wait */
	gtk_main ();

	g_object_unref (glade_xml);
	g_object_unref (list_store);
	g_object_unref (gconf_client);
	g_object_unref (client);
	g_object_unref (control);
unique_out:
	g_object_unref (egg_unique);

	return 0;
}

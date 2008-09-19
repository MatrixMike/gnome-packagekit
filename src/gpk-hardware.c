/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Scott Reeves <sreeves@novell.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gconf/gconf-client.h>
#include <libnotify/notify.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus.h>

#include <pk-client.h>
#include <pk-common.h>
#include <pk-task-list.h>

#include "egg-debug.h"
#include "egg-string.h"
#include "egg-string-list.h"

#include "gpk-client.h"
#include "gpk-common.h"
#include "gpk-hardware.h"

static void     gpk_hardware_class_init	(GpkHardwareClass *klass);
static void     gpk_hardware_init	(GpkHardware      *hardware);
static void     gpk_hardware_finalize	(GObject	  *object);

#define GPK_HARDWARE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GPK_TYPE_HARDWARE, GpkHardwarePrivate))
#define GPK_HARDWARE_LOGIN_DELAY	50 /* seconds */

struct GpkHardwarePrivate
{
	GConfClient		*gconf_client;
	DBusGConnection		*connection;
	DBusGProxy		*proxy;
	gchar			**package_ids;
};

G_DEFINE_TYPE (GpkHardware, gpk_hardware, G_TYPE_OBJECT)

/**
 * gpk_hardware_install_package:
 **/
static gboolean
gpk_hardware_install_package (GpkHardware *hardware)
{
	GError *error = NULL;
	GpkClient *gclient = NULL;
	gboolean ret;

	gclient = gpk_client_new ();

	ret = gpk_client_install_package_ids (gclient, hardware->priv->package_ids, &error);
	if (!ret) {
		egg_warning ("failed to install package: %s", error->message);
		g_error_free (error);
		error = NULL;
	}

	g_object_unref (gclient);
	return ret;
}

/**
 * gpk_hardware_libnotify_cb:
 **/
static void
gpk_hardware_libnotify_cb (NotifyNotification *notification, gchar *action, gpointer data)
{
	GpkHardware *hardware = GPK_HARDWARE (data);

	if (egg_strequal (action, "install-driver")) {
		gpk_hardware_install_package (hardware);
	} else if (egg_strequal (action, "do-not-show-prompt-hardware")) {
		egg_debug ("set %s to FALSE", GPK_CONF_PROMPT_HARDWARE);
		gconf_client_set_bool (hardware->priv->gconf_client, GPK_CONF_PROMPT_HARDWARE, FALSE, NULL);
	} else {
		egg_warning ("unknown action id: %s", action);
	}
}

/**
 * gpk_hardware_check_for_driver_available:
 **/
static void
gpk_hardware_check_for_driver_available (GpkHardware *hardware)
{
	gboolean ret;
	guint length;
	gchar *message = NULL;
	NotifyNotification *notification;
	GError *error = NULL;
	gchar *package_name = NULL;
	PkPackageList *list = NULL;
	PkClient *client = NULL;

	client = pk_client_new ();
	ret = pk_client_what_provides (client, pk_bitfield_value (PK_FILTER_ENUM_NOT_INSTALLED),
				       PK_PROVIDES_ENUM_HARDWARE_DRIVER, "unused", &error);
	if (!ret) {
		egg_warning ("Error calling pk_client_what_provides :%s", error->message);
		g_error_free (error);
		error = NULL;
		goto out;
	}

	/* If there are no driver packages available just return */
	list = pk_client_get_package_list (client);
	length = pk_package_list_get_size (list);
	if (length == 0) {
		egg_debug ("no drivers available");
		goto out;
	}

	package_name = pk_package_list_to_string (list);
	if (hardware->priv->package_ids != NULL)
		g_strfreev (hardware->priv->package_ids);
	hardware->priv->package_ids = pk_package_list_to_strv (list);

	message = g_strdup_printf ("%s %s", package_name, _("is needed for this hardware"));
	notification = notify_notification_new (_("Install package?"), message, "help-browser", NULL);
	notify_notification_set_timeout (notification, NOTIFY_EXPIRES_NEVER);
	notify_notification_set_urgency (notification, NOTIFY_URGENCY_LOW);
	notify_notification_add_action (notification, "install-package",
			_("Install package"), gpk_hardware_libnotify_cb, hardware, NULL);
	notify_notification_add_action (notification, "do-not-show-prompt-hardware",
					_("Do not show this again"), gpk_hardware_libnotify_cb, hardware, NULL);
	ret = notify_notification_show (notification, &error);
	if (!ret) {
		egg_warning ("error: %s", error->message);
		g_error_free (error);
	}

out:
	g_free (package_name);
	g_free (message);
	if (list != NULL)
		g_object_unref (list);
	g_object_unref (client);
}

/**
 * gpk_hardware_device_added_cb:
 * FIXME - not sure about this method signature
 **/
static void
gpk_hardware_device_added_cb (DBusGProxy *proxy, GObject *object, GpkHardware *hardware)
{
	gpk_hardware_check_for_driver_available (hardware);
}

/**
 * gpk_hardware_timeout_cb:
 * @data: This class instance
 **/
static gboolean
gpk_hardware_timeout_cb (gpointer data)
{
	gpk_hardware_check_for_driver_available (GPK_HARDWARE (data));
	return FALSE;
}

/**
 * gpk_hardware_init:
 * @hardware: This class instance
 **/
static void
gpk_hardware_init (GpkHardware *hardware)
{
	gboolean ret;
	GError *error = NULL;

	hardware->priv = GPK_HARDWARE_GET_PRIVATE (hardware);
	hardware->priv->gconf_client = gconf_client_get_default ();
	hardware->priv->package_ids = NULL;

	/* should we check and show the user */
	ret = gconf_client_get_bool (hardware->priv->gconf_client, GPK_CONF_PROMPT_HARDWARE, NULL);
	if (!ret) {
		egg_debug ("hardware driver checking disabled in GConf");
		return;
	}

	hardware->priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error != NULL) {
		egg_warning ("Cannot connect to bus: %s", error->message);
		g_error_free (error);
		return;
	}
	hardware->priv->proxy = dbus_g_proxy_new_for_name (hardware->priv->connection,
							   "org.freedesktop.Hal",
							   "/org/freedesktop/Hal/Manager",
							   "org.freedesktop.Hal.Manager");
	dbus_g_proxy_connect_signal (hardware->priv->proxy, "DeviceAdded",
				     G_CALLBACK (gpk_hardware_device_added_cb), hardware, NULL);

	/* check at startup (plus delay) and see if there is cold plugged hardware needing drivers */
	g_timeout_add_seconds (GPK_HARDWARE_LOGIN_DELAY, gpk_hardware_timeout_cb, hardware);
}

/**
 * gpk_hardware_class_init:
 * @klass: The GpkHardwareClass
 **/
static void
gpk_hardware_class_init (GpkHardwareClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gpk_hardware_finalize;
	g_type_class_add_private (klass, sizeof (GpkHardwarePrivate));
}

/**
 * gpk_hardware_finalize:
 * @object: The object to finalize
 **/
static void
gpk_hardware_finalize (GObject *object)
{
	GpkHardware *hardware;

	g_return_if_fail (GPK_IS_HARDWARE (object));

	hardware = GPK_HARDWARE (object);

	g_return_if_fail (hardware->priv != NULL);
	g_object_unref (hardware->priv->gconf_client);
	g_strfreev (hardware->priv->package_ids);

	G_OBJECT_CLASS (gpk_hardware_parent_class)->finalize (object);
}

/**
 * gpk_hardware_new:
 *
 * Return value: a new GpkHardware object.
 **/
GpkHardware *
gpk_hardware_new (void)
{
	GpkHardware *hardware;
	hardware = g_object_new (GPK_TYPE_HARDWARE, NULL);
	return GPK_HARDWARE (hardware);
}


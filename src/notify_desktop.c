/**
 * collectd - src/notify_desktop.c
 * Copyright (C) 2008       Sebastian Harl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Sebastian Harl <sh at tokkee.org>
 **/

/*
 * This plugin sends desktop notifications to a notification daemon.
 */

#include "collectd.h"

#include "common.h"
#include "plugin.h"

#include <glib.h>
#include <libnotify/notify.h>

#ifndef NOTIFY_CHECK_VERSION
#define NOTIFY_CHECK_VERSION(x, y, z) 0
#endif

#define log_info(...) INFO("notify_desktop: " __VA_ARGS__)
#define log_warn(...) WARNING("notify_desktop: " __VA_ARGS__)
#define log_err(...) ERROR("notify_desktop: " __VA_ARGS__)

#define DEFAULT_TIMEOUT 5000

static int okay_timeout = DEFAULT_TIMEOUT;
static int warn_timeout = DEFAULT_TIMEOUT;
static int fail_timeout = DEFAULT_TIMEOUT;

static int set_timeout(oconfig_item_t *ci, int *timeout) {
  if ((0 != ci->children_num) || (1 != ci->values_num) ||
      (OCONFIG_TYPE_NUMBER != ci->values[0].type)) {
    log_err("%s expects a single number argument.", ci->key);
    return 1;
  }

  *timeout = (int)ci->values[0].value.number;
  if (0 > *timeout)
    *timeout = DEFAULT_TIMEOUT;
  return 0;
} /* set_timeout */

static int c_notify_config(oconfig_item_t *ci) {
  for (int i = 0; i < ci->children_num; ++i) {
    oconfig_item_t *c = ci->children + i;

    if (0 == strcasecmp(c->key, "OkayTimeout"))
      set_timeout(c, &okay_timeout);
    else if (0 == strcasecmp(c->key, "WarningTimeout"))
      set_timeout(c, &warn_timeout);
    else if (0 == strcasecmp(c->key, "FailureTimeout"))
      set_timeout(c, &fail_timeout);
  }
  return 0;
} /* c_notify_config */

static int c_notify(const notification_t *n,
                    user_data_t __attribute__((unused)) * user_data) {
  NotifyNotification *notification = NULL;
  NotifyUrgency urgency = NOTIFY_URGENCY_LOW;
  int timeout = okay_timeout;

  char summary[1024];

  if (NOTIF_WARNING == n->severity) {
    urgency = NOTIFY_URGENCY_NORMAL;
    timeout = warn_timeout;
  } else if (NOTIF_FAILURE == n->severity) {
    urgency = NOTIFY_URGENCY_CRITICAL;
    timeout = fail_timeout;
  }

  ssnprintf(summary, sizeof(summary), "collectd %s notification",
           (NOTIF_FAILURE == n->severity)
               ? "FAILURE"
               : (NOTIF_WARNING == n->severity)
                     ? "WARNING"
                     : (NOTIF_OKAY == n->severity) ? "OKAY" : "UNKNOWN");

  notification = notify_notification_new(summary, n->message, NULL
#if NOTIFY_CHECK_VERSION(0, 7, 0)
                                         );
#else
                                         ,
                                         NULL);
#endif
  if (NULL == notification) {
    log_err("Failed to create a new notification.");
    return -1;
  }

  notify_notification_set_urgency(notification, urgency);
  notify_notification_set_timeout(notification, timeout);

  if (!notify_notification_show(notification, NULL))
    log_err("Failed to display notification.");

  g_object_unref(G_OBJECT(notification));
  return 0;
} /* c_notify */

static int c_notify_shutdown(void) {
  plugin_unregister_init("notify_desktop");
  plugin_unregister_notification("notify_desktop");
  plugin_unregister_shutdown("notify_desktop");

  if (notify_is_initted())
    notify_uninit();
  return 0;
} /* c_notify_shutdown */

static int c_notify_init(void) {
  char *name = NULL;
  char *vendor = NULL;
  char *version = NULL;
  char *spec_version = NULL;

  if (!notify_init(PACKAGE_STRING)) {
    log_err("Failed to initialize libnotify.");
    return -1;
  }

  if (!notify_get_server_info(&name, &vendor, &version, &spec_version))
    log_warn("Failed to get the notification server info. "
             "Check if you have a notification daemon running.");
  else {
    log_info("Found notification daemon: %s (%s) %s (spec version %s)", name,
             vendor, version, spec_version);
    free(name);
    free(vendor);
    free(version);
    free(spec_version);
  }

  plugin_register_notification("notify_desktop", c_notify,
                               /* user_data = */ NULL);
  plugin_register_shutdown("notify_desktop", c_notify_shutdown);
  return 0;
} /* c_notify_init */

void module_register(void) {
  plugin_register_complex_config("notify_desktop", c_notify_config);
  plugin_register_init("notify_desktop", c_notify_init);
  return;
} /* module_register */

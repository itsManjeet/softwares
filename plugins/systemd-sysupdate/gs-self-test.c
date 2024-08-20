/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (c) 2024 Codethink Limited
 * Copyright (c) 2024 GNOME Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib/gstdio.h>

#include "gnome-software-private.h"
#include "gs-test.h"

#include "config.h"
#include "gs-systemd-sysupdated-generated.h"

/*
 * Here we do the integration test, which means we validate the
 * results indirectly from plugin-loader's point of view without
 * touching the plugin (code under test).
 */

/* While g_auto(GMutex) and g_auto(GCond) are available, we can't use them as
 * our mutexes and conds are in variables. This works around that limitation by
 * allowing us to automate initializing and clearing any GMutex and GCond. */

typedef void GsMutexGuard;

static inline GsMutexGuard *
gs_mutex_guard_new (GMutex *mutex)
{
	g_mutex_init (mutex);
	return (GsMutexGuard *) mutex;
}

static inline void
gs_mutex_guard_free (GsMutexGuard *guard)
{
	g_mutex_clear ((GMutex *) guard);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsMutexGuard, gs_mutex_guard_free)

#define GS_MUTEX_AUTO_GUARD(mutex, var) \
  g_autoptr (GsMutexGuard) G_GNUC_UNUSED var = gs_mutex_guard_new (mutex)

typedef void GsCondGuard;

static inline GsCondGuard *
gs_cond_guard_new (GCond *cond)
{
	g_cond_init (cond);
	return (GsCondGuard *) cond;
}

static inline void
gs_cond_guard_free (GsCondGuard *guard)
{
	g_cond_clear ((GCond *) guard);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsCondGuard, gs_cond_guard_free)

#define GS_COND_AUTO_GUARD(cond, var) \
  g_autoptr (GsCondGuard) G_GNUC_UNUSED var = gs_cond_guard_new (cond)

typedef struct {
	GMutex lock;
	GCond cond;
} GsMonitor;

/* Fake update target info reported by the mocked service */
typedef struct {
	const gchar *class;
	const gchar *name;
	const gchar *object_path;
	const gchar *current_version;
	const gchar *latest_version;
} UpdateTargetInfo;

/* Expected app info to be created by the plugin */
typedef struct {
	const gchar *id;
	const gchar *version;
	const GsAppState state;
	const AsComponentKind kind;
	/* metadata `SystemdSysupdated::Target`, this value must be the
	 * same as the name of the associated update target (assume app
	 * to target is one-to-one mapping) */
	const gchar *metadata_target;
} UpdateAppInfo;

/* Wrapper of the target info and expected app */
typedef struct {
	const UpdateTargetInfo target_info;
	const UpdateAppInfo app_info;
} UpdateTarget;

static const UpdateTarget target_host = {
	.target_info = {
		.class = "host",
		.name = "os-upgrade",
		.object_path = "/org/freedesktop/sysupdate1/target/host",
		.current_version = "host@t.0",
		.latest_version = "host@t.1",
	},
	.app_info = {
		.id = "systemd-sysupdate.os-upgrade",
		.version = "host@t.1",
		.state = GS_APP_STATE_AVAILABLE,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "os-upgrade",
	},
};

static const UpdateTarget target_component_no_source = {
	.target_info = {
		.class = "component",
		.name = "no-source",
		.object_path = "/org/freedesktop/sysupdate1/target/component_no_source",
		.current_version = "",
		.latest_version = "",
	},
	.app_info = {
		.id = "systemd-sysupdate.no-source",
		.version = "",
		.state = GS_APP_STATE_UNKNOWN,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "no-source",
	},
};

static const UpdateTarget target_component_installed = {
	.target_info = {
		.class = "component",
		.name = "installed",
		.object_path = "/org/freedesktop/sysupdate1/target/component_installed",
		.current_version = "component-installed@t.0",
		.latest_version = "",
	},
	.app_info = {
		.id = "systemd-sysupdate.installed",
		.version = "component-installed@t.0",
		.state = GS_APP_STATE_INSTALLED,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "installed",
	},
};

static const UpdateTarget target_component_available = {
	.target_info = {
		.class = "component",
		.name = "available",
		.object_path = "/org/freedesktop/sysupdate1/target/component_available",
		.current_version = "",
		.latest_version = "component-available@t.1",
	},
	.app_info = {
		.id = "systemd-sysupdate.available",
		.version = "component-available@t.1",
		.state = GS_APP_STATE_AVAILABLE,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "available",
	},
};

static const UpdateTarget target_component_updatable = {
	.target_info = {
		.class = "component",
		.name = "updatable",
		.object_path = "/org/freedesktop/sysupdate1/target/component_updatable",
		.current_version = "component-updatable@t.0",
		.latest_version = "component-updatable@t.1",
	},
	.app_info = {
		.id = "systemd-sysupdate.updatable",
		.version = "component-updatable@t.1",
		.state = GS_APP_STATE_UPDATABLE,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "updatable",
	},
};

static const UpdateTarget target_component_updatable_v2 = {
	.target_info = {
		.class = "component",
		.name = "updatable",
		.object_path = "/org/freedesktop/sysupdate1/target/component_updatable",
		.current_version = "component-updatable@t.0",
		.latest_version = "component-updatable@t.2",
	},
	.app_info = {
		.id = "systemd-sysupdate.updatable",
		.version = "component-updatable@t.2",
		.state = GS_APP_STATE_UPDATABLE,
		.kind = AS_COMPONENT_KIND_OPERATING_SYSTEM,
		.metadata_target = "updatable",
	},
};

/* By test case mock systemd-sysupdated reply setup data
 */
typedef struct {
	const UpdateTarget *targets[5]; /* currently the max number required among all the test cases */
	GMutex lock; /* used in `Target.Update()` to check if code-under-test starts to wait for signal JobRemoved() */
	GCond cond;
} MockSysupdatedSetupData;

static void
mock_sysupdated_reply_method_call_manager_introspect (GDBusConnection       *connection,
                                                      const gchar           *sender,
                                                      const gchar           *object_path,
                                                      const gchar           *interface_name,
                                                      const gchar           *method_name,
                                                      GVariant              *parameters,
                                                      GDBusMethodInvocation *invocation,
                                                      gpointer               user_data)
{
	g_autoptr(GVariant) reply = NULL;

	reply = g_variant_new ("(s)", "<fake-xml-data>");
	g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
}

static void
mock_sysupdated_reply_method_call_manager_list_targets (GDBusConnection       *connection,
                                                        const gchar           *sender,
                                                        const gchar           *object_path,
                                                        const gchar           *interface_name,
                                                        const gchar           *method_name,
                                                        GVariant              *parameters,
                                                        GDBusMethodInvocation *invocation,
                                                        gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;
	g_autoptr(GVariant) reply = NULL;
	GVariantBuilder builder;

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sso)"));
	for (guint i = 0; targets[i] != NULL; i++) {
		g_variant_builder_add (&builder,
		                       "(sso)",
		                       targets[i]->target_info.class,
		                       targets[i]->target_info.name,
		                       targets[i]->target_info.object_path);
	}
	reply = g_variant_new ("(@a(sso))",
	                       g_variant_builder_end (&builder));
	g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
}

static void
mock_sysupdated_reply_method_call_target_properties_get_all (GDBusConnection       *connection,
                                                             const gchar           *sender,
                                                             const gchar           *object_path,
                                                             const gchar           *interface_name,
                                                             const gchar           *method_name,
                                                             GVariant              *parameters,
                                                             GDBusMethodInvocation *invocation,
                                                             gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;

	for (guint i = 0; targets[i] != NULL; i++) {
		if (g_strcmp0 (targets[i]->target_info.object_path, object_path) == 0) {
			g_autoptr(GVariant) reply = NULL;
			const gchar *interface = NULL;

			g_assert_true (g_str_has_prefix (object_path, "/org/freedesktop/sysupdate1/target/"));

			g_variant_get (parameters, "(&s)", &interface);
			g_assert_true (g_str_equal (interface, "org.freedesktop.sysupdate1.Target") ||
			               g_str_equal (interface, "org.freedesktop.DBus.Properties"));

			reply = g_variant_new_parsed ("({'Version': <%s>},)",
			                              targets[i]->target_info.current_version);
			g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
			return;
		}
	}

	if (g_strcmp0 ("/org/freedesktop/sysupdate1/job/_2", object_path) == 0) {
		g_autoptr(GVariant) reply = NULL;
		const gchar *interface = NULL;

		g_variant_get (parameters, "(&s)", &interface);
		g_assert_cmpstr (interface, ==, "org.freedesktop.sysupdate1.Job");

		reply = g_variant_new_parsed ("({'': <%s>},)", "");
		g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
		return;
	}

	g_debug ("unexpected object_path = `%s`", object_path);
	g_assert_not_reached ();
};

static void
mock_sysupdated_reply_method_call_target_check_new (GDBusConnection       *connection,
                                                    const gchar           *sender,
                                                    const gchar           *object_path,
                                                    const gchar           *interface_name,
                                                    const gchar           *method_name,
                                                    GVariant              *parameters,
                                                    GDBusMethodInvocation *invocation,
                                                    gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;

	for (guint i = 0; targets[i] != NULL; i++) {
		if (g_strcmp0 (targets[i]->target_info.object_path, object_path) == 0) {
			g_autoptr(GVariant) reply = NULL;

			reply = g_variant_new ("(s)",
			                       targets[i]->target_info.latest_version);
			g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
			return;
		}
	}

	g_debug ("unexpected object_path = `%s`", object_path);
	g_assert_not_reached ();
}

static void
mock_sysupdated_reply_method_call_target_describe (GDBusConnection       *connection,
                                                   const gchar           *sender,
                                                   const gchar           *object_path,
                                                   const gchar           *interface_name,
                                                   const gchar           *method_name,
                                                   GVariant              *parameters,
                                                   GDBusMethodInvocation *invocation,
                                                   gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;

	for (guint i = 0; targets[i] != NULL; i++) {
		if (g_strcmp0 (targets[i]->target_info.object_path, object_path) == 0) {
			g_autoptr(GVariant) reply = NULL;
			const gchar *version = NULL;
			gboolean offline = FALSE;
			gboolean is_latest = FALSE;
			g_autofree gchar *json = NULL;

			g_variant_get (parameters, "(&sb)", &version, &offline);
			g_assert_cmpstr (version, ==, targets[i]->app_info.version);
			g_assert_false (offline);

			is_latest = g_strcmp0 (version, targets[i]->target_info.latest_version) == 0;
			json = g_strdup_printf ("{\"version\":\"%s\",\"newest\":%s,\"available\":%s,\"installed\":%s,\"obsolete\":%s,\"protected\":false,\"changelog_urls\":[],\"contents\":[]}",
			                        version,
			                        is_latest ? "true" : "false",
			                        targets[i]->app_info.state == GS_APP_STATE_AVAILABLE ? "true" : "false",
			                        targets[i]->app_info.state == GS_APP_STATE_INSTALLED ? "true" : "false",
			                        !is_latest ? "true" : "false");

			reply = g_variant_new ("(s)", json);
			g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
			return;
		}
	}

	g_debug ("unexpected object_path = `%s`", object_path);
	g_assert_not_reached ();
}

static void
mock_sysupdated_reply_method_call_target_get_version (GDBusConnection       *connection,
                                                      const gchar           *sender,
                                                      const gchar           *object_path,
                                                      const gchar           *interface_name,
                                                      const gchar           *method_name,
                                                      GVariant              *parameters,
                                                      GDBusMethodInvocation *invocation,
                                                      gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;

	for (guint i = 0; targets[i] != NULL; i++) {
		if (g_strcmp0 (targets[i]->target_info.object_path, object_path) == 0) {
			g_autoptr(GVariant) reply = NULL;

			reply = g_variant_new ("(s)",
			                       targets[i]->target_info.current_version);
			g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));
			return;
		}
	}

	g_debug ("unexpected object_path = `%s`", object_path);
	g_assert_not_reached ();
}

static void
mock_sysupdated_reply_method_call_target_update (GDBusConnection       *connection,
                                                 const gchar           *sender,
                                                 const gchar           *object_path,
                                                 const gchar           *interface_name,
                                                 const gchar           *method_name,
                                                 GVariant              *parameters,
                                                 GDBusMethodInvocation *invocation,
                                                 gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;

	for (guint i = 0; targets[i] != NULL; i++) {
		if (g_strcmp0 (targets[i]->target_info.object_path, object_path) == 0) {
			g_autoptr(GVariant) reply = NULL;
			const gchar *version = NULL;
			G_MUTEX_AUTO_LOCK (&setup_data->lock, locker);

			g_variant_get (parameters, "(&s)", &version);
			g_assert_cmpstr (version, ==, ""); /* always update to the latest version for now */

			reply = g_variant_new ("(sto)",
			                       targets[i]->target_info.latest_version,
			                       2,
			                       "/org/freedesktop/sysupdate1/job/_2");
			g_dbus_method_invocation_return_value (invocation, g_steal_pointer (&reply));

			/* signal the test code that it has already replyed to
			 * the method_call `Target.Update()`, which means plugin
			 * should now start to wait for the signal
			 * `JobRemoved()` */
			g_cond_signal (&setup_data->cond);
			return;
		}
	}

	g_debug ("unexpected object_path = `%s`", object_path);
	g_assert_not_reached ();
}

static void
mock_sysupdated_reply_method_call_job_cancel (GDBusConnection       *connection,
                                              const gchar           *sender,
                                              const gchar           *object_path,
                                              const gchar           *interface_name,
                                              const gchar           *method_name,
                                              GVariant              *parameters,
                                              GDBusMethodInvocation *invocation,
                                              gpointer               user_data)
{
	MockSysupdatedSetupData *setup_data = (MockSysupdatedSetupData *) user_data;
	G_MUTEX_AUTO_LOCK (&setup_data->lock, locker);

	/* no parameters */
	g_dbus_method_invocation_return_value (invocation, NULL);

	/* signal test code that cancel has been replied and it can move
	 * on to emit signal JobRemoved() */
	g_cond_signal (&setup_data->cond);
}

static void
mock_sysupdated_server_method_call (GDBusConnection       *connection,
                                    const gchar           *sender,
                                    const gchar           *object_path,
                                    const gchar           *interface_name,
                                    const gchar           *method_name,
                                    GVariant              *parameters,
                                    GDBusMethodInvocation *invocation,
                                    gpointer               user_data)
{
	GDBusInterfaceMethodCallFunc handle_method_call_reply = NULL;

	if (g_strcmp0 (interface_name, "org.freedesktop.DBus.Introspectable") == 0) {
		if (g_strcmp0 (method_name, "Introspect") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_manager_introspect;
		}
	} else if (g_strcmp0 (interface_name, "org.freedesktop.DBus.Properties") == 0) {
		if (g_strcmp0 (method_name, "GetAll") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_target_properties_get_all;
		}
	} else if (g_strcmp0 (interface_name, "org.freedesktop.sysupdate1.Manager") == 0) {
		if (g_strcmp0 (method_name, "ListTargets") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_manager_list_targets;
		}
	} else if (g_strcmp0 (interface_name, "org.freedesktop.sysupdate1.Target") == 0) {
		if (g_strcmp0 (method_name, "CheckNew") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_target_check_new;
		}
		else if (g_strcmp0 (method_name, "Describe") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_target_describe;
		}
		else if (g_strcmp0 (method_name, "GetVersion") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_target_get_version;
		}
		else if (g_strcmp0 (method_name, "Update") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_target_update;
		}
	} else if (g_strcmp0 (interface_name, "org.freedesktop.sysupdate1.Job") == 0) {
		if (g_strcmp0 (method_name, "Cancel") == 0) {
			handle_method_call_reply = mock_sysupdated_reply_method_call_job_cancel;
		}
	}

	if (handle_method_call_reply == NULL) {
		g_debug ("mock systemd-sysupdated service does not implement reply to `%s.%s()`",
		         interface_name,
		         method_name);
		g_assert_not_reached ();
	}

	handle_method_call_reply (connection,
	                          sender,
	                          object_path,
	                          interface_name,
	                          method_name,
	                          parameters,
	                          invocation,
	                          user_data);
}

static GVariant *
mock_sysupdated_server_get_property (GDBusConnection  *connection,
                                     const gchar      *sender,
                                     const gchar      *object_path,
                                     const gchar      *interface_name,
                                     const gchar      *property_name,
                                     GError          **error,
                                     gpointer          user_data)
{
	g_debug ("%s %s %s", __func__, interface_name, property_name);

	if (g_strcmp0 (interface_name, "org.freedesktop.sysupdate1.Job") == 0) {
		if (g_strcmp0 (property_name, "Id") == 0) {
			return g_variant_new ("t", 0);
		} else if (g_strcmp0 (property_name, "Type") == 0) {
			return g_variant_new ("s", "");
		} else if (g_strcmp0 (property_name, "Offline") == 0) {
			return g_variant_new ("b", FALSE);
		} else if (g_strcmp0 (property_name, "Progress") == 0) {
			return g_variant_new ("u", 0);
		}
	}

	g_debug ("mock systemd-sysupdated service does not implement getting property `%s.%s()`",
	         interface_name,
	         property_name);
	g_assert_not_reached ();
}

static gboolean
mock_sysupdated_server_set_property (GDBusConnection  *connection,
                                     const gchar      *sender,
                                     const gchar      *object_path,
                                     const gchar      *interface_name,
                                     const gchar      *property_name,
                                     GVariant         *value,
                                     GError          **error,
                                     gpointer          user_data)
{
	g_debug ("%s %s %s", __func__, interface_name, property_name);

	g_debug ("mock systemd-sysupdated service does not implement setting property `%s.%s()`",
	         interface_name,
	         property_name);
	g_assert_not_reached ();
}

static const GDBusInterfaceVTable mock_sysupdated_server_vtable =
{
  .method_call = mock_sysupdated_server_method_call,
  .get_property = mock_sysupdated_server_get_property,
  .set_property = mock_sysupdated_server_set_property,
};

/* Structure of test data setup only once in the beginning and be
 * passed to all the test cases
 */
typedef struct {
	/* test bus */
	GTestDBus *bus;
	GDBusConnection *connection;
	guint owner_id;
	guint registration_id;
	GSList *registration_ids;

	/* mock systemd-sysupdated service thread*/
	GMainContext *server_context;
	GMainLoop *server_loop;
	GThread *server_thread;

	/* can only load once per process */
	GsPluginLoader *plugin_loader;
} TestData;

static gpointer
mock_sysupdated_server_thread_cb (TestData *test_data)
{
	g_main_context_push_thread_default (test_data->server_context);
	{
		g_main_loop_run (test_data->server_loop);
	}
	g_main_context_pop_thread_default (test_data->server_context);
	return NULL;
}

typedef struct {
	GDBusConnection *connection;
	const gchar *sender;
	const gchar *object_path;
	const gchar *interface_name;
	const gchar *signal_name;
	GVariant *parameters;

	GMutex lock;
	GCond cond;
} EmitSignalData;

static gboolean
emit_signal_cb (gpointer user_data)
{
	EmitSignalData *data = (EmitSignalData *) user_data;
	g_autoptr(GError) error = NULL;
	G_MUTEX_AUTO_LOCK (&data->lock, locker);

	g_dbus_connection_emit_signal (data->connection,
	                               data->sender,
	                               data->object_path,
	                               data->interface_name,
	                               data->signal_name,
	                               g_steal_pointer (&data->parameters),
	                               &error);
	g_assert_no_error (error);

	g_dbus_connection_flush_sync (data->connection, NULL, &error);
	g_assert_no_error (error);

	g_cond_signal (&data->cond);

	return G_SOURCE_REMOVE;
}

/* Append an event to the server's context to emit the signal, and wait for the
 * server's thread to emit it. */
static void
mock_sysupdated_emit_signal_job_removed (TestData *test_data,
                                         gint      job_status)
{
	EmitSignalData data = {
		.connection = test_data->connection,
		.sender = "org.freedesktop.sysupdate1",
		.object_path = "/org/freedesktop/sysupdate1",
		.interface_name = "org.freedesktop.sysupdate1.Manager",
		.signal_name = "JobRemoved",
		.parameters = g_variant_new ("(toi)", 2, "/org/freedesktop/sysupdate1/job/_2", job_status),
	};
	g_autoptr(GError) error = NULL;
	GS_MUTEX_AUTO_GUARD (&data.lock, lock);
	GS_COND_AUTO_GUARD (&data.cond, cond);
	G_MUTEX_AUTO_LOCK (&data.lock, locker);

	gs_test_flush_main_context ();

	g_main_context_invoke (test_data->server_context,
	                       emit_signal_cb,
	                       &data);
	g_cond_wait (&data.cond, &data.lock);

	/* this is a workaround for we want to wait until the signal
	 * emitted has been dispatched and is received by the plugin.
	 * we are using the main context here due to currently the
	 * signal subscriptions are done in the `setup()` and was run on
	 * the main context in the test `main()`. */
	g_main_context_iteration (NULL, TRUE);
}

/* Append an event to the server's context to emit the signal, and wait for the
 * server's thread to emit it. */
static void
mock_sysupdated_emit_signal_properties_changed (TestData *test_data,
                                                guint     progress_percentage)
{
	EmitSignalData data = {
		.connection = test_data->connection,
		.sender = "org.freedesktop.sysupdate1",
		.object_path = "/org/freedesktop/sysupdate1/job/_2",
		.interface_name = "org.freedesktop.DBus.Properties",
		.signal_name = "PropertiesChanged",
		.parameters = NULL,
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) parameters = NULL;
	const gchar *invalidated_properties[] = {NULL};
	GVariantBuilder builder;
	GS_MUTEX_AUTO_GUARD (&data.lock, lock);
	GS_COND_AUTO_GUARD (&data.cond, cond);
	G_MUTEX_AUTO_LOCK (&data.lock, locker);

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&builder,
	                       "{sv}",
	                       "Progress",
	                       g_variant_new_uint32 (progress_percentage));
	data.parameters = g_variant_new ("(s@a{sv}@as)",
	                                 "org.freedesktop.sysupdate1.Job",
	                                 g_variant_builder_end (&builder),
	                                 g_variant_new_strv (invalidated_properties, -1)),

	gs_test_flush_main_context ();

	g_main_context_invoke (test_data->server_context,
	                       emit_signal_cb,
	                       &data);
	g_cond_wait (&data.cond, &data.lock);

	/* the same as the `mock_sysupdated_emit_signal_job_removed()` */
	g_main_context_iteration (NULL, TRUE);
}

typedef struct {
	GDBusConnection *connection;
	const gchar *object_path;
	GDBusInterfaceInfo *interface_info;
	gpointer user_data;
	guint registration_id;

	GMutex lock;
	GCond cond;
} RegisterObjectData;

static gboolean
register_object_cb (gpointer user_data)
{
	RegisterObjectData *data = (RegisterObjectData *) user_data;
	g_autoptr(GError) error = NULL;
	G_MUTEX_AUTO_LOCK (&data->lock, locker);

	data->registration_id = g_dbus_connection_register_object (data->connection,
	                                                           data->object_path,
	                                                           data->interface_info,
	                                                           &mock_sysupdated_server_vtable,
	                                                           data->user_data,
	                                                           NULL,
	                                                           &error);
	g_assert_no_error (error);
	g_assert (data->registration_id > 0);
	g_cond_signal (&data->cond);

	return G_SOURCE_REMOVE;
}

static guint
mock_sysupdated_register_object (TestData *test_data,
                                 const gchar *object_path,
                                 GDBusInterfaceInfo *interface_info,
                                 gpointer user_data)
{
	RegisterObjectData data = {
		.connection = test_data->connection,
		.object_path = object_path,
		.interface_info = interface_info,
		.user_data = user_data,
		.registration_id = 0,
	};
	GS_MUTEX_AUTO_GUARD (&data.lock, lock);
	GS_COND_AUTO_GUARD (&data.cond, cond);
	G_MUTEX_AUTO_LOCK (&data.lock, locker);

	g_main_context_invoke (test_data->server_context,
	                       register_object_cb,
	                       &data);
	g_cond_wait (&data.cond, &data.lock);

	return data.registration_id;
}

typedef struct {
	GDBusConnection *connection;
	guint registration_id;

	GMutex lock;
	GCond cond;
} UnregisterObjectData;

static gboolean
unregister_object_cb (gpointer user_data)
{
	UnregisterObjectData *data = (UnregisterObjectData *) user_data;
	G_MUTEX_AUTO_LOCK (&data->lock, locker);

	g_dbus_connection_unregister_object (data->connection,
	                                     data->registration_id);
	g_cond_signal (&data->cond);

	return G_SOURCE_REMOVE;
}

static void
mock_sysupdated_unregister_object (TestData *test_data,
                                   guint     registration_id)
{
	UnregisterObjectData data = {
		.connection = test_data->connection,
		.registration_id = registration_id,
	};
	GS_MUTEX_AUTO_GUARD (&data.lock, lock);
	GS_COND_AUTO_GUARD (&data.cond, cond);
	G_MUTEX_AUTO_LOCK (&data.lock, locker);

	g_main_context_invoke (test_data->server_context,
	                       unregister_object_cb,
	                       &data);
	g_cond_wait (&data.cond, &data.lock);
}

static void
mock_sysupdated_test_setup (TestData                *test_data,
                            MockSysupdatedSetupData *setup_data)
{
	/* Configure mock `systemd-sysupdated` server's reply based on
	 * the given `user_data` */
	const UpdateTarget **targets = (const UpdateTarget **) setup_data->targets;
	guint registration_id = 0;

	/* since the server thread already started running on a
	 * different context, we now need to invoke the object
	 * registration on the thread context */

	/* register manager object */
	{
		/* org.freedesktop.sysupdate1.Manager */
		registration_id = mock_sysupdated_register_object (test_data,
		                                                   "/org/freedesktop/sysupdate1",
		                                                   gs_systemd_sysupdate_manager_interface_info (),
		                                                   setup_data);
		test_data->registration_ids = g_slist_append (test_data->registration_ids,
		                                              GUINT_TO_POINTER (registration_id));
	}

	/* register target objects */
	for (guint i = 0; targets[i] != NULL; i++) {
		/* org.freedesktop.DBus.Properties */
		registration_id = mock_sysupdated_register_object (test_data,
		                                                   targets[i]->target_info.object_path,
		                                                   gs_systemd_sysupdate_org_freedesktop_dbus_properties_interface_info (),
		                                                   setup_data);
		test_data->registration_ids = g_slist_append (test_data->registration_ids,
		                                              GUINT_TO_POINTER (registration_id));

		/* org.freedesktop.sysupdate1.Target */
		registration_id = mock_sysupdated_register_object (test_data,
		                                                   targets[i]->target_info.object_path,
		                                                   gs_systemd_sysupdate_target_interface_info (),
		                                                   setup_data);
		test_data->registration_ids = g_slist_append (test_data->registration_ids,
		                                              GUINT_TO_POINTER (registration_id));
	}

	/* register job objects. here we use the same job ID hard-coded
	 * everywhere in this file */
	{
		/* org.freedesktop.sysupdate1.Job */
		registration_id = mock_sysupdated_register_object (test_data,
		                                                   "/org/freedesktop/sysupdate1/job/_2",
		                                                   gs_systemd_sysupdate_job_interface_info (),
		                                                   setup_data);
		test_data->registration_ids = g_slist_append (test_data->registration_ids,
		                                              GUINT_TO_POINTER (registration_id));
	}
}

static void
mock_sysupdated_test_teardown (TestData *test_data)
{
	/* clean-up all objects registered to the test bus */
	for (GSList *node = test_data->registration_ids; node != NULL; node = node->next) {
		mock_sysupdated_unregister_object (test_data,
		                                   GPOINTER_TO_UINT (node->data));
	}
	g_clear_pointer (&test_data->registration_ids, g_slist_free);
}

static void
bus_set_up (TestData *test_data)
{
	g_autoptr(GError) error = NULL;

	test_data->server_context = g_main_context_new ();

	g_main_context_push_thread_default (test_data->server_context);
	{
		/* start test D-Bus daemon */
		test_data->bus = g_test_dbus_new (G_TEST_DBUS_NONE);
		g_test_dbus_up (test_data->bus);

		/* create bus connection */
		test_data->connection = g_dbus_connection_new_for_address_sync (g_test_dbus_get_bus_address (test_data->bus),
		                                                                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
		                                                                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		                                                                NULL, NULL, &error);
		g_assert_no_error (error);

		/* we need at least the manager to reply to the plugin's
		* self-disable query in the constructor */
		test_data->owner_id = g_bus_own_name_on_connection (test_data->connection,
		                                                    "org.freedesktop.sysupdate1",
		                                                    G_BUS_NAME_OWNER_FLAGS_NONE,
		                                                    NULL, NULL, NULL, NULL);
		test_data->registration_id = g_dbus_connection_register_object (test_data->connection,
		                                                                "/org/freedesktop/sysupdate1",
		                                                                gs_systemd_sysupdate_org_freedesktop_dbus_introspectable_interface_info (),
		                                                                &mock_sysupdated_server_vtable,
		                                                                NULL, NULL, &error);
		g_assert_no_error (error);
		test_data->registration_ids = NULL;
	}
	g_main_context_pop_thread_default (test_data->server_context);

	/* push mock systemd-sysupdated service to server thread */
	test_data->server_loop = g_main_loop_new (test_data->server_context, FALSE);
	test_data->server_thread = g_thread_new ("mock systemd-sysupdated service",
	                                         (GThreadFunc) mock_sysupdated_server_thread_cb,
	                                         test_data);
}

static gboolean
mock_sysupdated_server_loop_is_running_cb (gpointer user_data)
{
	GsMonitor *monitor = user_data;
	G_MUTEX_AUTO_LOCK (&monitor->lock, locker);
	g_cond_signal (&monitor->cond);
	return G_SOURCE_REMOVE;
}

static void
bus_teardown (TestData *test_data)
{
	/* clean-up mock systemd-sysupdated service and server thread */
	if (test_data->server_thread != NULL) {
		/* Ensure the thread's main loop is running before trying to
		 * quit it, otherwise we would deadlock trying to join a
		 * never-ending thread. */
		{
			GsMonitor monitor;
			g_autoptr(GSource) source = g_idle_source_new ();
			GS_MUTEX_AUTO_GUARD (&monitor.lock, lock);
			GS_COND_AUTO_GUARD (&monitor.cond, cond);
			G_MUTEX_AUTO_LOCK (&monitor.lock, locker);

			g_source_set_callback (source, mock_sysupdated_server_loop_is_running_cb, &monitor, NULL);
			g_source_attach (source, test_data->server_context);
			g_cond_wait (&monitor.cond, &monitor.lock);
			g_main_loop_quit (test_data->server_loop);
		}

		g_clear_pointer (&test_data->server_thread, g_thread_join);
	}
	g_clear_pointer (&test_data->server_loop, g_main_loop_unref);

	g_main_context_push_thread_default (test_data->server_context);
	{
		/* clean-up bus connection */
		g_dbus_connection_unregister_object (test_data->connection,
		                                     test_data->registration_id);
		g_clear_pointer (&test_data->registration_ids, g_slist_free);
		g_bus_unown_name (test_data->owner_id);
		if (test_data->connection != NULL) {
			g_dbus_connection_close_sync (test_data->connection, NULL, NULL);
		}

		/* stop test D-Bus daemon */
		g_test_dbus_down (test_data->bus);
		g_clear_pointer (&test_data->bus, g_object_unref);
	}
	g_main_context_pop_thread_default (test_data->server_context);
	g_clear_pointer (&test_data->server_context, g_main_context_unref);
}

static gint
compare_apps_by_name (GsApp *app1, GsApp *app2, gpointer user_data)
{
	/* Negative value if a < b; zero if a = b; positive value if a > b. */
	return g_ascii_strcasecmp (gs_app_get_name (app1),
	                           gs_app_get_name (app2));
}

static void
invoke_plugin_loader_refresh_metadata_assert_no_error (GsPluginLoader *plugin_loader)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ret;

	plugin_job = gs_plugin_job_refresh_metadata_new (0, /* always refresh */
	                                                 GS_PLUGIN_REFRESH_METADATA_FLAGS_NONE);
	ret = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
	gs_test_flush_main_context ();

	g_assert_no_error (error);
	g_assert_true (ret);
}

static GsAppList *
invoke_plugin_loader_list_upgrades_assert_no_error (GsPluginLoader *plugin_loader)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GError) error = NULL;

	plugin_job = gs_plugin_job_list_distro_upgrades_new (GS_PLUGIN_LIST_DISTRO_UPGRADES_FLAGS_NONE,
	                                                     GS_PLUGIN_REFINE_FLAGS_NONE);
	list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
	gs_test_flush_main_context ();

	g_assert_no_error (error);
	g_assert_nonnull (list);

	gs_app_list_sort (list, (GsAppListSortFunc) compare_apps_by_name, NULL);
	return g_steal_pointer (&list);
}

static GsAppList *
invoke_plugin_loader_list_apps_for_update_assert_no_error (GsPluginLoader *plugin_loader)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(GsAppQuery) query = NULL;
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GError) error = NULL;

	query = gs_app_query_new ("is-for-update", GS_APP_QUERY_TRISTATE_TRUE,
	                          "refine-flags", GS_PLUGIN_REFINE_FLAGS_NONE,
	                          NULL);
	plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
	list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
	gs_test_flush_main_context ();

	g_assert_no_error (error);
	g_assert_nonnull (list);

	gs_app_list_sort (list, (GsAppListSortFunc) compare_apps_by_name, NULL);
	return g_steal_pointer (&list);
}

static GsAppList *
invoke_plugin_loader_list_apps_assert_no_error (GsPluginLoader *plugin_loader, const gchar **keywords)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GsAppQuery) query = NULL;
	g_autoptr(GError) error = NULL;

	query = gs_app_query_new ("keywords", keywords, NULL);
	plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
	g_clear_object (&query);

	list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
	gs_test_flush_main_context ();

	g_assert_no_error (error);
	g_assert_nonnull (list);

	gs_app_list_sort (list, (GsAppListSortFunc) compare_apps_by_name, NULL);
	return g_steal_pointer (&list);
}

typedef struct {
	GsPluginLoader *plugin_loader;
	GsPluginJob *plugin_job;
	GCancellable *cancellable;
	GError **error;
	gboolean ret;

	GThread *plugin_thread;
} RunPluginJobActionData;

static gpointer
run_plugin_job_action_thread_cb (gpointer user_data)
{
	RunPluginJobActionData *data = (RunPluginJobActionData *) user_data;

	data->ret = gs_plugin_loader_job_action (data->plugin_loader,
	                                         data->plugin_job,
	                                         data->cancellable,
	                                         data->error);

	return NULL;
}

static void
invoke_plugin_loader_upgrade_download_assert_no_error (GsPluginLoader *plugin_loader, GsApp *app)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ret;

	plugin_job = gs_plugin_job_download_upgrade_new (app,
	                                                 GS_PLUGIN_DOWNLOAD_UPGRADE_FLAGS_NONE);
	ret = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
	gs_test_flush_main_context ();

	g_assert_no_error (error);
	g_assert_true (ret);
}

static RunPluginJobActionData *
invoke_plugin_loader_upgrade_trigger_begin (GsPluginLoader *plugin_loader, GsApp *app, GError **error)
{
	RunPluginJobActionData *data = g_slice_new (RunPluginJobActionData);

	data->plugin_loader = plugin_loader;
	data->plugin_job = gs_plugin_job_trigger_upgrade_new (app,
	                                                      GS_PLUGIN_TRIGGER_UPGRADE_FLAGS_NONE);
	data->cancellable = g_cancellable_new ();
	data->error = error;
	data->ret = FALSE;

	data->plugin_thread = g_thread_new ("invoke-plugin-loader-upgrade-trigger-background",
	                                    (GThreadFunc) run_plugin_job_action_thread_cb,
	                                    data);
	return g_steal_pointer (&data);
}

static gboolean
invoke_plugin_loader_upgrade_trigger_end (RunPluginJobActionData *data)
{
	gboolean ret;

	g_clear_pointer (&data->plugin_thread, g_thread_join);
	ret = data->ret;

	g_clear_pointer (&data->plugin_job, g_object_unref);
	g_slice_free (RunPluginJobActionData, data);
	return ret;
}

static RunPluginJobActionData *
invoke_plugin_loader_update_apps_begin (GsPluginLoader *plugin_loader, GsAppList *list_updates, GError **error)
{
	RunPluginJobActionData *data = g_slice_new (RunPluginJobActionData);

	data->plugin_loader = plugin_loader;
	data->plugin_job = gs_plugin_job_update_apps_new (list_updates,
	                                                  GS_PLUGIN_UPDATE_APPS_FLAGS_NONE);
	data->cancellable = g_cancellable_new ();
	data->error = error;
	data->ret = FALSE;

	data->plugin_thread = g_thread_new ("invoke-plugin-loader-update-apps-background",
	                                    (GThreadFunc) run_plugin_job_action_thread_cb,
	                                    data);
	return g_steal_pointer (&data);
}

static gboolean
invoke_plugin_loader_update_apps_end (RunPluginJobActionData *data)
{
	return invoke_plugin_loader_upgrade_trigger_end (data);
}

static void
validate_app_assert_as_expected (GsApp *app, const UpdateTarget *target)
{
	g_assert_cmpstr (gs_app_get_id (app), ==, target->app_info.id);
	g_assert_cmpstr (gs_app_get_version (app), ==, target->app_info.version);
	g_assert_cmpint (gs_app_get_state (app), ==, target->app_info.state);
	g_assert_cmpint (gs_app_get_kind (app), ==, target->app_info.kind);
	g_assert_cmpstr (gs_app_get_metadata_item (app, "SystemdSysupdated::Target"), ==, target->app_info.metadata_target);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_creatable_func (TestData *test_data)
{
	/* Validate if plugin can create app upgrade (host) from the
	 * update target */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_upgrades = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		validate_app_assert_as_expected (gs_app_list_index (list_upgrades, 0),
		                                 &target_host);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_unsearchable_func (TestData *test_data)
{
	/* Validate if the app upgrade (host) cannot be search by the
	 * specific keyword 'sysupdate' */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* unlike app updates (component), which are allowed to be
	 * searched with the specific keyword 'sysupdate', the app
	 * upgrade relies on customized action handles which might have
	 * some trouble if user trigger the upgrade from the app page.
	 * as a result, for now we just make it an un-searchable app
	 * (host) to the user. */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_upgrades = NULL;
		g_autoptr(GsAppList) list_searchable = NULL;
		const gchar *keywords[2] = {"sysupdate", NULL};

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		list_searchable = invoke_plugin_loader_list_apps_assert_no_error (plugin_loader,
		                                                                  keywords);
		g_assert_cmpint (gs_app_list_length (list_searchable), ==, 0);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_upgradable_func (TestData *test_data)
{
	/* Validate if plugin can handle app upgrade (host)
	 */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_upgrades = NULL;
		g_autoptr(GError) error = NULL;
		GsApp *app = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		app = gs_app_list_index (list_upgrades, 0);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

		invoke_plugin_loader_upgrade_download_assert_no_error (plugin_loader, app);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_UPDATABLE);

		/* make sure process starts to wait for the signal before
		 * we emit it, otherwise the signal might happenens before
		 * the method call and be ignored by the plugin */
		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_upgrade_trigger_begin (plugin_loader, app, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* emit `job_status` = `0` as update success */
			mock_sysupdated_emit_signal_job_removed (test_data, 0);

			ret = invoke_plugin_loader_upgrade_trigger_end (g_steal_pointer (&data));
		}
		g_assert_no_error (error);
		g_assert_true (ret);

		/* app state changes on update succeed */
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_PENDING_INSTALL);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_trackable_func (TestData *test_data)
{
	/* Validate if plugin can track and update app upgrade (host)
	 * progress percentage to app */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_upgrades = NULL;
		g_autoptr(GError) error = NULL;
		GsApp *app = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		app = gs_app_list_index (list_upgrades, 0);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

		invoke_plugin_loader_upgrade_download_assert_no_error (plugin_loader, app);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_UPDATABLE);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_upgrade_trigger_begin (plugin_loader, app, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* The mock server can only return the default value for
			 * properties, so we need to wait for the plugin to
			 * retrieve the default progress value before emitting
			 * its updated value. */
			while (gs_app_get_progress (app) == GS_APP_PROGRESS_UNKNOWN) {
				g_usleep (100);
			}

			/* Signal the update has progressed. */
			mock_sysupdated_emit_signal_properties_changed (test_data, 50);
			/* Wait for the plugin thread to handle the update. */
			while (gs_app_get_progress (app) != 50) {
				g_usleep (100);
			}
			g_assert_cmpint (gs_app_get_progress (app), ==, 50);

			/* emit job-removed to end the job */
			mock_sysupdated_emit_signal_job_removed (test_data, 0);

			ret = invoke_plugin_loader_upgrade_trigger_end (g_steal_pointer (&data));
		}
		g_assert_no_error (error);
		g_assert_true (ret);

		/* app state changes on update succeed */
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_PENDING_INSTALL);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_recoverable_func (TestData *test_data)
{
	/* Validate if plugin can recovery the app state on app upgrade
	 * (host) upgrade failed */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_upgrades = NULL;
		g_autoptr(GError) error = NULL;
		GsApp *app = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		app = gs_app_list_index (list_upgrades, 0);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

		invoke_plugin_loader_upgrade_download_assert_no_error (plugin_loader, app);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_UPDATABLE);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_upgrade_trigger_begin (plugin_loader, app, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* emit `job_status` = non-zero value indicates update failure */
			mock_sysupdated_emit_signal_job_removed (test_data, -2);

			ret = invoke_plugin_loader_upgrade_trigger_end (g_steal_pointer (&data));
		}
		g_assert_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED);
		g_assert_false (ret);

		/* app state recovers on update failed */
		g_assert_cmpint (gs_app_get_state (app), ==, setup_data.targets[0]->app_info.state);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_upgrade_cancellable_func (TestData *test_data)
{
	/* Validate if plugin can handle app upgrade (host) upgrade
	 * cancellation */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_host,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_upgrades = NULL;
		g_autoptr(GError) error = NULL;
		GsApp *app = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_upgrades = invoke_plugin_loader_list_upgrades_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_upgrades), ==, 1);

		app = gs_app_list_index (list_upgrades, 0);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

		invoke_plugin_loader_upgrade_download_assert_no_error (plugin_loader, app);
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_UPDATABLE);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_upgrade_trigger_begin (plugin_loader, app, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* cancel the job, error should be set automatically */
			g_cancellable_cancel (data->cancellable);
			/* Wait for the plugin thread to handle `Job.Cancel()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* emit `job_status` = -1 as what real service returns */
			mock_sysupdated_emit_signal_job_removed (test_data, -1);

			ret = invoke_plugin_loader_upgrade_trigger_end (g_steal_pointer (&data));
		}
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_assert_false (ret);

		/* app state recovers on update failed */
		g_assert_cmpint (gs_app_get_state (app), ==, setup_data.targets[0]->app_info.state);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_creatable_func (TestData *test_data)
{
	/* Validate if plugin can create app update (component) from the
	 * update target */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_no_source,
			&target_component_installed,
			&target_component_available,
			&target_component_updatable,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* although the plugin still creates app for the 'no-source'
	 * component, it should be set to a state that will be filtered
	 * and not be seen by the user */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 3);

		/* apps are sorted in the alphabetical order in the helper function */
		validate_app_assert_as_expected (gs_app_list_index (list_updates, 0),
		                                 &target_component_available);
		validate_app_assert_as_expected (gs_app_list_index (list_updates, 1),
		                                 &target_component_installed);
		validate_app_assert_as_expected (gs_app_list_index (list_updates, 2),
		                                 &target_component_updatable);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_searchable_func (TestData *test_data)
{
	/* Validate if app update (component) can be search with the
	 * specific keyword */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_no_source,
			&target_component_installed,
			&target_component_available,
			&target_component_updatable,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;
		g_autoptr(GsAppList) list_searchable = NULL;
		const gchar *keywords[2] = {"sysupdate", NULL};

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 3);

		/* currently allow user to use the specific keyword
		 * 'sysupdate' to search for the update apps */
		list_searchable = invoke_plugin_loader_list_apps_assert_no_error (plugin_loader,
		                                                                  keywords);
		g_assert_cmpint (gs_app_list_length (list_searchable), ==, 3);

		/* apps are sorted inside the helper function */
		g_assert_cmpstr (gs_app_get_id (gs_app_list_index (list_searchable, 0)),
		                 ==, target_component_available.app_info.id);
		g_assert_cmpstr (gs_app_get_id (gs_app_list_index (list_searchable, 1)),
		                 ==, target_component_installed.app_info.id);
		g_assert_cmpstr (gs_app_get_id (gs_app_list_index (list_searchable, 2)),
		                 ==, target_component_updatable.app_info.id);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_updatable_func (TestData *test_data)
{
	/* Validate if plugin can handle app update (component)
	 */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_available,
			&target_component_updatable,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_updates = NULL;
		g_autoptr(GError) error = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 2);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_update_apps_begin (plugin_loader, list_updates, &error);
			for (guint i = 0; i < gs_app_list_length (list_updates); i++) {
				/* Wait for the plugin thread to handle `Target.Update()`. */
				g_cond_wait (&setup_data.cond, &setup_data.lock);

				/* emit `job_status` = `0` as update success */
				mock_sysupdated_emit_signal_job_removed (test_data, 0);
			}
			ret = invoke_plugin_loader_update_apps_end (g_steal_pointer (&data));
		}
		g_assert_no_error (error);
		g_assert_true (ret);

		/* app state changes on update succeed */
		for (guint i = 0; i < gs_app_list_length (list_updates); i++) {
			GsApp *app = gs_app_list_index (list_updates, i);
			g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_INSTALLED);
		}
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_trackable_func (TestData *test_data)
{
	/* Validate if plugin can update the app update (component)
	 * progress reported to app */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_available,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* use only one app update (component) here since the plugin
	 * does not control the app update order in the app list */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_updates = NULL;
		g_autoptr(GError) error = NULL;
		GsApp *app = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 1);

		app = gs_app_list_index (list_updates, 0);
		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_update_apps_begin (plugin_loader, list_updates, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* The mock server can only return the default value for
			 * properties, so we need to wait for the plugin to
			 * retrieve the default progress value before emitting
			 * its updated value. */
			while (gs_app_get_progress (app) == GS_APP_PROGRESS_UNKNOWN) {
				g_usleep (100);
			}

			/* Signal the update has progressed. */
			mock_sysupdated_emit_signal_properties_changed (test_data, 50);
			/* Wait for the plugin thread to handle the update. */
			while (gs_app_get_progress (app) != 50) {
				g_usleep (100);
			}
			g_assert_cmpint (gs_app_get_progress (app), ==, 50);

			/* emit job-removed to end the job */
			mock_sysupdated_emit_signal_job_removed (test_data, 0);

			ret = invoke_plugin_loader_update_apps_end (g_steal_pointer (&data));
		}
		g_assert_no_error (error);
		g_assert_true (ret);

		/* app state changes on update succeed */
		g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_INSTALLED);
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_recoverable_func (TestData *test_data)
{
	/* Validate if plugin can recover app state when the app update
	 * (component) failed */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_available,
			&target_component_updatable,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* it might be just a choice, currently in the plugin, the
	 * update chain stops on any of the update failure happenes */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_updates = NULL;
		g_autoptr(GError) error = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 2);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_update_apps_begin (plugin_loader, list_updates, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* emit `job_status` = non-zero as update failure */
			mock_sysupdated_emit_signal_job_removed (test_data, -2);

			/* as the 1st job failed, the 2nd job will not run
			 * based on the plugin's current implementation */
			ret = invoke_plugin_loader_update_apps_end (g_steal_pointer (&data));
		}
		g_assert_no_error (error); /* single app update error will not be propagated */
		g_assert_true (ret);

		/* if the 2nd job is somehow triggered, this test case will
		 * fail because of the timeout. as a result, we only need to
		 * check both apps are not installed here */
		for (guint i = 0; i < gs_app_list_length (list_updates); i++) {
			GsApp *app = gs_app_list_index (list_updates, i);
			g_assert_cmpint (gs_app_get_state (app), !=, GS_APP_STATE_INSTALLED);
		}
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_app_update_cancellable_func (TestData *test_data)
{
	/* Validate if plugin can handle app update (component) update
	 * cancellation */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_available,
			&target_component_updatable,
			NULL
		},
	};
	GS_MUTEX_AUTO_GUARD (&setup_data.lock, lock);
	GS_COND_AUTO_GUARD (&setup_data.cond, cond);

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		RunPluginJobActionData *data = NULL;
		g_autoptr(GsAppList) list_updates = NULL;
		g_autoptr(GError) error = NULL;
		gboolean ret;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 2);

		{
			G_MUTEX_AUTO_LOCK (&setup_data.lock, locker);

			data = invoke_plugin_loader_update_apps_begin (plugin_loader, list_updates, &error);
			/* Wait for the plugin thread to handle `Target.Update()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* cancel the job, error should be set automatically */
			g_cancellable_cancel (data->cancellable);
			/* Wait for the plugin thread to handle `Job.Cancel()`. */
			g_cond_wait (&setup_data.cond, &setup_data.lock);

			/* emit `job_status` = -1 as what real service returns */
			mock_sysupdated_emit_signal_job_removed (test_data, -1);

			ret = invoke_plugin_loader_update_apps_end (g_steal_pointer (&data));
		}
		g_assert_nonnull (error);
		g_assert_false (ret);

		for (guint i = 0; i < gs_app_list_length (list_updates); i++) {
			GsApp *app = gs_app_list_index (list_updates, i);
			g_assert_cmpint (gs_app_get_state (app), !=, GS_APP_STATE_INSTALLED);
		}
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_metadata_target_updatable_func (TestData *test_data)
{
	/* Validate if plugin can track target's latest version by
	 * updating the currently stored target and app */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_updatable,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* latest version = v1 */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 1);

		g_assert_cmpstr (gs_app_get_version (gs_app_list_index (list_updates, 0)),
		                 ==, "component-updatable@t.1");
	}
	mock_sysupdated_test_teardown (test_data);

	/* latest version = v2 */
	setup_data.targets[0] = &target_component_updatable_v2;
	setup_data.targets[1] = NULL;
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 1);

		g_assert_cmpstr (gs_app_get_version (gs_app_list_index (list_updates, 0)),
		                 ==, "component-updatable@t.2");
	}
	mock_sysupdated_test_teardown (test_data);
}

static void
gs_plugin_systemd_sysupdate_metadata_target_removable_func (TestData *test_data)
{
	/* Validate if plugin can remove the target stored in case it
	 * has been removed from the configuration */
	GsPluginLoader *plugin_loader = test_data->plugin_loader;
	MockSysupdatedSetupData setup_data = {
		.targets = {
			&target_component_available,
			NULL
		},
	};

	if (!gs_plugin_loader_get_enabled (plugin_loader, "systemd-sysupdate")) {
		g_test_skip ("not enabled");
		return;
	}

	/* 1st setup, after refresh metadata there should have one app
	 * in the list */
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 1);
	}
	mock_sysupdated_test_teardown (test_data);

	/* 2nd setup, after refresh metadata the list should become
	 * empty now */
	setup_data.targets[0] = NULL;
	mock_sysupdated_test_setup (test_data, &setup_data);
	{
		g_autoptr(GsAppList) list_updates = NULL;

		invoke_plugin_loader_refresh_metadata_assert_no_error (plugin_loader);
		list_updates = invoke_plugin_loader_list_apps_for_update_assert_no_error (plugin_loader);
		g_assert_cmpint (gs_app_list_length (list_updates), ==, 0);
	}
	mock_sysupdated_test_teardown (test_data);
}

int
main (int argc, char **argv)
{
	TestData test_data;
	g_autoptr(GError) error = NULL;
	gboolean ret;
	int res;

	const gchar * const allowlist[] = {
		"systemd-sysupdate",
		NULL,
	};

	gs_test_init (&argc, &argv);
	g_setenv ("GS_XMLB_VERBOSE", "1", TRUE);

	/* setup test D-Bus, mock systemd-sysupdate service */
	bus_set_up (&test_data);

	/* we can only load this once per process. */

	/* although we only need to use the system bus in our test, the
	 * underlying `g_test_dbus_up()` will always override the
	 * environment variable `DBUS_SESSION_BUS_ADDRESS`. As a
	 * workaround, we also pass the connection created as the
	 * session bus to the `plugin-loader` to prevent it from setting
	 * up another session bus connection */
	test_data.plugin_loader = gs_plugin_loader_new (test_data.connection,
	                                                test_data.connection);
	gs_plugin_loader_add_location (test_data.plugin_loader, LOCALPLUGINDIR);
	gs_plugin_loader_add_location (test_data.plugin_loader, LOCALPLUGINDIR_CORE);
	ret = gs_plugin_loader_setup (test_data.plugin_loader,
	                              allowlist,
	                              NULL,
	                              NULL,
	                              &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* plugin tests go here */

	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-creatable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_creatable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-unsearchable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_unsearchable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-upgradable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_upgradable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-trackable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_trackable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-recoverable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_recoverable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-upgrade-cancellable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_upgrade_cancellable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-creatable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_creatable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-searchable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_searchable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-updatable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_updatable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-trackable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_trackable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-recoverable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_recoverable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/app-update-cancellable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_app_update_cancellable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/metadata-target-updatable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_metadata_target_updatable_func);
	g_test_add_data_func ("/gnome-software/plugins/systemd-sysupdate/metadata-target-removable",
	                      &test_data,
	                      (GTestDataFunc) gs_plugin_systemd_sysupdate_metadata_target_removable_func);

	/* start */
	res = g_test_run ();

	/* clean-up */
	bus_teardown (&test_data);

	return res;
}

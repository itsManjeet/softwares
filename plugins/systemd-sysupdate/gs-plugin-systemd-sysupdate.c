/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (c) 2024 Codethink Limited
 * Copyright (c) 2024 GNOME Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include <gnome-software.h>

#include "gs-plugin-systemd-sysupdate.h"
#include "gs-systemd-sysupdated-generated.h"

/*
 * This plugin only works when there is `systemd-sysupdated` service
 * presented in current host.
 *
 * The term `target` here refers to an available 'update target'
 * which is pre-configured under the directory
 * `/usr/lib/sysupdate.<target_name>.d/` and is shipped together
 * with the distro. These targets' configurations can also be
 * overridden through the user configurations under the directory
 * `/etc/sysupdate.<target_name>.d/`. Targets are parsed by the
 * `systemd-sysupdated` service on demand during runtime.
 *
 * Based on the definition in `systemd-sysupdated`, there are
 * several possible 'class' types of a target:
 *  - 'OS upgrade' managed by `systemd-sysupdate`
 *  - 'OS component' managed by `systemd-sysupdate`
 *  - 'system extension' managed by `systemd-sysext`
 *  - 'system configuration extension' managed by `systemd-confext`
 *  - 'portable service' managed by `systemd-portabled`
 *  - 'container and virtual machine' managed by `systemd-machined`
 *
 * In this plugin, we ask `systemd-sysupdated` to report the targets
 * information in the method `refresh_metadata_async()` and save
 * them as metadata in a hashmap.
 *
 * Corresponding apps including one os-upgrade app (class = 'host')
 * created in `list_distro_upgrades_async()` and the other apps
 * created in `list_apps_async()` are saved in the per-plugin cache.
 */

/* `systemd-sysupdated` D-Bus connection timeout */
#define SYSUPDATED_MANAGER_LIST_TARGET_TIMEOUT_MS (1000) /* parse local configuration files timeout msec */
#define SYSUPDATED_TARGET_CHECK_NEW_TIMEOUT_MS (10000) /* download indexes from server timeout msec */
#define SYSUPDATED_TARGET_GET_PROPERTIES_TIMEOUT_MS (1000) /* returns properties including current version timeout msec */
#define SYSUPDATED_TARGET_UPDATE_TIMEOUT_MS (-1) /* download files from server and deploys timeout msec */
#define SYSUPDATED_JOB_CANCEL_TIMEOUT_MS (1000) /* cancel on-going job trigger only timeout msec */

/* Structure stores the `target` information reported by
 * `systemd-sysupdated` */
typedef struct {
	GsSystemdSysupdateTarget *proxy;
	gboolean is_valid;
	gchar *class; /* (owned) (not nullable) */
	gchar *name; /* (owned) (not nullable) */
	gchar *object_path; /* (owned) (not nullable) */
	gchar *current_version; /* (owned) (not nullable) */
	gchar *latest_version; /* (owned) (not nullable) */
} TargetItem;

static TargetItem *
target_item_new (const gchar *class, const gchar *name, const gchar *object_path)
{
	TargetItem *target = g_new0 (TargetItem, 1);
	g_clear_object (&target->proxy);
	target->is_valid = TRUE; /* default to true on creation */
	target->class = g_strdup (class);
	target->name = g_strdup (name);
	target->object_path = g_strdup (object_path);
	target->current_version = g_strdup ("");
	target->latest_version = g_strdup ("");
	return target;
}

static void
target_item_free (TargetItem *target)
{
	target->is_valid = FALSE;
	g_clear_pointer (&target->class, g_free);
	g_clear_pointer (&target->name, g_free);
	g_clear_pointer (&target->object_path, g_free);
	g_clear_pointer (&target->current_version, g_free);
	g_clear_pointer (&target->latest_version, g_free);
	g_free (target);
}

/* Structure stores the `targets` whose information to be updated in
 * queue and the current working `target` */
typedef struct {
	GQueue *queue; /* (owned) (not nullable) (element-type TargetItem) */
	TargetItem *target;  /* (not owned) (nullable) */
} GsPluginSystemdSysupdateRefreshMetadataData;

/* Takes ownership of @queue */
static GsPluginSystemdSysupdateRefreshMetadataData *
gs_plugin_systemd_sysupdate_refresh_metadata_data_new (GQueue *queue)
{
	GsPluginSystemdSysupdateRefreshMetadataData *data = g_new0 (GsPluginSystemdSysupdateRefreshMetadataData, 1);
	data->queue = g_steal_pointer (&queue);
	return data;
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_data_free (GsPluginSystemdSysupdateRefreshMetadataData *data)
{
	g_clear_pointer (&data->queue, g_queue_free);
	data->target = NULL;
	g_free (data);
}

/* Structure stores the `targets` whose information to be refined in
 * queue and the current working `target` */
typedef struct {
	GQueue *queue; /* (owned) (not nullable) (element-type TargetItem) */
	GsApp *app;  /* (nullable) (not owned) */
} GsPluginSystemdSysupdateRefineData;

static GsPluginSystemdSysupdateRefineData *
gs_plugin_systemd_sysupdate_refine_data_new (GQueue *queue)
{
	GsPluginSystemdSysupdateRefineData *data = g_new0 (GsPluginSystemdSysupdateRefineData, 1);
	data->queue = g_steal_pointer (&queue);
	return data;
}

static void
gs_plugin_systemd_sysupdate_refine_data_free (GsPluginSystemdSysupdateRefineData *data)
{
	g_clear_pointer (&data->queue, g_queue_free);
	data->app = NULL;
	g_free (data);
}

/* Structure stores the `apps` to be updated to newer version in
 * queue and the current working `app` */
typedef struct {
	GQueue *queue; /* (owned) (not nullable) (element-type GsApp) */
	GsApp *app;
	gulong cancelled_id;
} GsPluginSystemdSysupdateUpdateAppsData;

static GsPluginSystemdSysupdateUpdateAppsData *
gs_plugin_systemd_sysupdate_update_apps_data_new (GQueue *queue,
                                                  gulong  cancelled_id)
{
	GsPluginSystemdSysupdateUpdateAppsData *data = g_new0 (GsPluginSystemdSysupdateUpdateAppsData, 1);
	data->queue = g_steal_pointer (&queue);
	data->cancelled_id = cancelled_id;
	return data;
}

static void
gs_plugin_systemd_sysupdate_update_apps_data_free (GsPluginSystemdSysupdateUpdateAppsData *data)
{
	g_clear_pointer (&data->queue, g_queue_free);
	data->app = NULL;
	data->cancelled_id = 0;
	g_free (data);
}

/* Plugin object */
struct _GsPluginSystemdSysupdate {
	GsPlugin parent;

	gchar *os_pretty_name; /* (owned) (not nullable) */
	gchar *os_version; /* (owned) (not nullable) */

	GsSystemdSysupdateManager *manager_proxy; /* (owned) (nullable) */
	GHashTable *target_item_map; /* (owned) (not nullable) (element-type utf8 TargetItem) */
	GHashTable *job_task_map; /* (owned) (not nullable) (element-type utf8 GTask) */
	GHashTable *job_to_remove_status_map; /* (owned) (not nullable) (element-type utf8 int32) */
	GHashTable *job_to_cancel_task_map; /* (owned) (not nullable) (element-type utf8 GTask) */
	gboolean is_metadata_refresh_ongoing;
	guint64 last_refresh_usecs;
};

/* Plugin private methods, and their callbacks. */

static void
gs_plugin_systemd_sysupdate_remove_job_apply (GsPluginSystemdSysupdate *self,
                                              GTask                    *task,
                                              const gchar              *job_path,
                                              gint32                    job_status);

static void
gs_plugin_systemd_sysupdate_cancel_job_cancel_cb (GObject      *source_object,
                                                  GAsyncResult *result,
                                                  gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_cancel_job_cb (GObject      *source_object,
                                           GAsyncResult *result,
                                           gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_cancel_job_revoke (GsPluginSystemdSysupdate *self,
                                               const gchar              *job_path);

static void
gs_plugin_systemd_sysupdate_update_target_proxy_new_cb (GObject      *source_object,
                                                        GAsyncResult *result,
                                                        gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_update_target_update_cb (GObject      *source_object,
                                                     GAsyncResult *result,
                                                     gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_update_target_job_proxy_new_cb (GObject      *source_object,
                                                            GAsyncResult *result,
                                                            gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_update_target_notify_progress_cb (gpointer user_data);

/* Plugin overridden virtual methods, and their callbacks. */

static void
gs_plugin_systemd_sysupdate_setup_proxy_new_cb (GObject      *source_object,
                                                GAsyncResult *result,
                                                gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refine_iter (GsPluginSystemdSysupdate *self,
                                         GTask                    *task);

static void
gs_plugin_systemd_sysupdate_refine_proxy_new_cb (GObject      *source_object,
                                                 GAsyncResult *result,
                                                 gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refine_describe_cb (GObject      *source_object,
                                                GAsyncResult *result,
                                                gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refresh_metadata_list_targets_cb (GObject      *source_object,
                                                              GAsyncResult *result,
                                                              gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refresh_metadata_iter (GsPluginSystemdSysupdate *self,
                                                   GTask                    *task);

static void
gs_plugin_systemd_sysupdate_refresh_metadata_proxy_new_cb (GObject      *source_object,
                                                           GAsyncResult *result,
                                                           gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refresh_metadata_get_version_cb (GObject      *source_object,
                                                             GAsyncResult *result,
                                                             gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_refresh_metadata_check_new_cb (GObject      *source_object,
                                                           GAsyncResult *result,
                                                           gpointer      user_data);

static void
gs_plugin_systemd_sysupdate_update_apps_iter (GsPluginSystemdSysupdate *self,
                                              GTask                    *task);

static void
gs_plugin_systemd_sysupdate_update_apps_update_target_cb (GObject      *source_object,
                                                          GAsyncResult *result,
                                                          gpointer      user_data);

G_DEFINE_TYPE (GsPluginSystemdSysupdate, gs_plugin_systemd_sysupdate, GS_TYPE_PLUGIN)

static TargetItem *
lookup_target_by_app (GsPluginSystemdSysupdate *self,
                      GsApp                    *app)
{
	/* Helper to get the associated `target` of the given `app`
	 */
	return g_hash_table_lookup (self->target_item_map, gs_app_get_metadata_item (app, "SystemdSysupdated::Target"));
}

static GsApp *
create_app_for_target (GsPluginSystemdSysupdate *self,
                       TargetItem               *target)
{
	/* Create an app upgrade (os-upgrade) for the target `host` or an app
	 * update for the target `component`
	 */
	g_autoptr(GsApp) app = NULL;
	g_autofree gchar *app_id = NULL;
	g_autofree gchar *app_name = NULL;
	AsBundleKind bundle_kind = AS_BUNDLE_KIND_UNKNOWN;
	const gchar *app_summary = NULL;
	GsAppQuirk app_quirk = GS_APP_QUIRK_NEEDS_REBOOT
	                     | GS_APP_QUIRK_PROVENANCE
	                     | GS_APP_QUIRK_NOT_REVIEWABLE;

	if (g_strcmp0 (target->class, "host") == 0) {
		app_name = g_strdup (self->os_pretty_name);
		bundle_kind = AS_BUNDLE_KIND_PACKAGE;
		/* TRANSLATORS: this is the system OS upgrade */
		app_summary = _("System upgrade for the new features.");
	} else if (g_strcmp0 (target->class, "component") == 0) {
		app_name = g_strdup_printf ("%s-%s", "component", target->name);
		/* TRANSLATORS: this is the system component update */
		app_summary = _("System component with useful features");
		app_quirk |= GS_APP_QUIRK_COMPULSORY;
	} else {
		return NULL;
	}

	app_id = g_strdup_printf ("%s.%s", gs_plugin_get_name (GS_PLUGIN (self)), target->name);

	/* We explicitely don't set the license as we don't have it with the
	 * current version of the sysupdate D-Bus API.
	 */
	app = gs_app_new (app_id);
	gs_app_set_name (app, GS_APP_QUALITY_NORMAL, app_name);
	gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
	gs_app_set_kind (app, AS_COMPONENT_KIND_OPERATING_SYSTEM);
	gs_app_set_bundle_kind (app, bundle_kind);
	gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, app_summary);
	gs_app_set_version (app, "unknown");
	gs_app_set_size_installed (app, GS_SIZE_TYPE_UNKNOWABLE, 0);
	gs_app_set_size_download (app, GS_SIZE_TYPE_UNKNOWABLE, 0);
	gs_app_set_state (app, GS_APP_STATE_UNKNOWN);
	gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
	gs_app_set_allow_cancel (app, TRUE);

	/* store target name to look up target info. */
	gs_app_set_metadata (app, "SystemdSysupdated::Target", target->name);
	gs_app_set_metadata (app, "SystemdSysupdated::Class", target->class);

	gs_app_add_quirk (app, app_quirk);

	return g_steal_pointer (&app);
}

static GsApp *
get_or_create_app_for_target (GsPluginSystemdSysupdate *self,
                              TargetItem               *target)
{
	/* Get or create an app when there is no existing one in cache
	 * for the given target */
	const gchar *key = target->name;
	g_autoptr(GsApp) app = NULL;

	/* find in the per-plugin cache */
	app = gs_plugin_cache_lookup (GS_PLUGIN (self), key);
	if (app != NULL) {
		return g_steal_pointer (&app);
	}

	app = create_app_for_target (self, target);
	if (app == NULL) {
		g_debug ("not-supported target class: `%s`", target->class);
		return NULL;
	}

	/* own the app we created */
	gs_app_set_management_plugin (app, GS_PLUGIN (self));

	/* store app to the per-plugin cache */
	gs_plugin_cache_add (GS_PLUGIN (self), key, app);
	return g_steal_pointer (&app);
}

static void
update_app_for_target (GsPluginSystemdSysupdate *self,
                       GsApp                    *app,
                       TargetItem               *target)
{
	const gchar *app_version = NULL;
	GsAppState app_state = GS_APP_STATE_UNKNOWN;

	/* Update an existing app based on the given target item
	 */
	if (g_strcmp0 (target->class, "host") == 0) {
		gboolean is_available = (g_strcmp0 (target->latest_version, "") != 0);

		/* check `gs-upgrade-banner.c` for available os-upgrade state:
		 *  - GS_APP_STATE_AVAILABLE
		 *  - GS_APP_STATE_QUEUED_FOR_INSTALL
		 *  - GS_APP_STATE_INSTALLING
		 *  - GS_APP_STATE_DOWNLOADING
		 *  - GS_APP_STATE_UPDATABLE
		 *  - GS_APP_STATE_PENDING_INSTALL
		 */
		if (is_available) {
			app_version = target->latest_version;
			app_state = GS_APP_STATE_AVAILABLE;
		} else {
			app_version = self->os_version;
			app_state = GS_APP_STATE_UNKNOWN;
		}
	} else if (g_strcmp0 (target->class, "component") == 0) {
		gboolean is_available = (g_strcmp0 (target->latest_version, "") != 0);
		gboolean is_installed = (g_strcmp0 (target->current_version, "") != 0);

		/* if there is no latest version, it could be either the latest
		 * version has been installed already or no resource found in
		 * the server */
		if (is_available) {
			if (is_installed) {
				app_version = target->latest_version;
				app_state = GS_APP_STATE_UPDATABLE;
			} else {
				app_version = target->latest_version;
				app_state = GS_APP_STATE_AVAILABLE;
			}
		} else {
			if (is_installed) {
				app_version = target->current_version;
				app_state = GS_APP_STATE_INSTALLED;
			} else {
				app_version = "";
				app_state = GS_APP_STATE_UNKNOWN;
			}
		}
	} else {
		g_debug ("not-supported target class: `%s`", target->class);
		return;
	}

	gs_app_set_version (app, app_version);
	gs_app_set_state (app, app_state);
}

/* Wrapper methods for async. target update
 *
 * The goal of the method `gs_plugin_systemd_sysupdate_update_target_async()`
 * is to wrap the specific target update as a single async. call.
 * By design, there are two D-Bus method calls and two D-Bus signals
 * involved in one 'target update' progress:
 *  1) D-Bus method `Target.Update()`
 *  2) D-Bus method `Job.Cancel()`
 *  3) D-Bus signal `Job.PropertiesChanged()`
 *  4) D-Bus signal `Manager.JobRemoved()`
 *
 * Assumes there is only one job created dynamically in the runtime
 * by `systemd-sysupdated` is associated to the `Target.Update()`.
 * Here we create a subtask for each individual target update, and
 * hide the 'target to job' mapping from the caller by maintaining
 * the relationships internally within a look-up table. */
typedef struct {
	GsApp *app; /* (owned) (not nullable) */
	GsSystemdSysupdateJob *job_proxy; /* (owned) (nullable) */
	gchar *target_path; /* (owned) (not nullable) */
	gchar *job_path; /* (owned) (nullable) */
} GsPluginSystemdSysupdateUpdateTargetData;

static GsPluginSystemdSysupdateUpdateTargetData *
gs_plugin_systemd_sysupdate_update_target_data_new (GsApp       *app,
                                                    const gchar *target_path)
{
	GsPluginSystemdSysupdateUpdateTargetData *data = g_new0 (GsPluginSystemdSysupdateUpdateTargetData, 1);
	data->app = g_object_ref (app);
	data->target_path = g_strdup (target_path);
	return data;
}

static void
gs_plugin_systemd_sysupdate_update_target_data_free (GsPluginSystemdSysupdateUpdateTargetData *data)
{
	if (data != NULL) {
		g_clear_object (&data->app);
		g_clear_object (&data->job_proxy);
		g_clear_pointer (&data->target_path, g_free);
		g_clear_pointer (&data->job_path, g_free);
		g_free (data);
	}
}

/* Remove the given job. It is called when the server notified us a job
 * terminated.
 *
 * Because of the async nature of of the application, we can receive job removal
 * notifications from the server after we requested the update jobs but before
 * we finished preparing them. To handle job removal notifications correctly, we
 * may need to store them until we are ready. */
static void
gs_plugin_systemd_sysupdate_remove_job (GsPluginSystemdSysupdate *self,
                                        const gchar              *job_path,
                                        gint32                    job_status)
{
	GTask *task = NULL;

	if (g_hash_table_contains (self->job_to_remove_status_map, job_path)) {
		g_debug ("Job already filed for removal: %s", job_path);
		return;
	}

	/* filter out non-update jobs which we have no interest in, for
	 * example, from `Manager.ListTargets()` and from
	 * `Target.CheckNew()` */
	task = g_hash_table_lookup (self->job_task_map, job_path);
	if (task == NULL) {
		g_debug ("Couldn´t remove task for job `%s`, no task found, storing for later removal", job_path);
		g_hash_table_insert (self->job_to_remove_status_map, g_strdup (job_path), GINT_TO_POINTER (job_status));
		/* The job terminated, there is nothing to cancel anymore. */
		gs_plugin_systemd_sysupdate_cancel_job_revoke (self, job_path);
		return;
	}

	gs_plugin_systemd_sysupdate_remove_job_apply (self, task, job_path, job_status);
}

static void
gs_plugin_systemd_sysupdate_remove_job_apply (GsPluginSystemdSysupdate *self,
                                              GTask                    *task,
                                              const gchar              *job_path,
                                              gint32                    job_status)
{
	GsPluginSystemdSysupdateUpdateTargetData *data = NULL;
	const gchar *target_class = NULL;
	gboolean target_is_host = FALSE;

	g_debug ("Removing task found for job `%s`", job_path);
	/* pass the parameters to the callback */
	data = g_task_get_task_data (task);
	target_class = gs_app_get_metadata_item (data->app, "SystemdSysupdated::Class");
	target_is_host = g_strcmp0 (target_class, "host") == 0;

	/* `systemd-sysupdate` job returns `0` on success, otherwise
	 * returns the error status including user cancellation */
	if (job_status == 0) {
		gs_app_set_progress (data->app, GS_APP_PROGRESS_UNKNOWN);
		/* The `host` target should have its state left as `updatable`. */
		if (target_is_host) {
			gs_app_set_state (data->app, GS_APP_STATE_PENDING_INSTALL);
		} else {
			gs_app_set_state (data->app, GS_APP_STATE_INSTALLED);
		}
	} else {
		gs_app_set_progress (data->app, GS_APP_PROGRESS_UNKNOWN);
		/* The `host` target has the non-transient `updatable` state, so
		 * to recover back to the `available` state, we have to set it
		 * explicitely. */
		if (target_is_host) {
			gs_app_set_state (data->app, GS_APP_STATE_AVAILABLE);
		} else {
			gs_app_set_state_recover (data->app);
		}
	}

	/* remove task from the hashmap and return the job status */
	g_hash_table_remove (self->job_task_map, job_path);
	g_hash_table_remove (self->job_to_remove_status_map, job_path);
	/* The job terminated, there is nothing to cancel anymore. */
	gs_plugin_systemd_sysupdate_cancel_job_revoke (self, job_path);

	if (job_status == 0) {
		g_task_return_boolean (task, TRUE);
	} else {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Update failed with status = %i", job_status);
	}
}

static void
gs_plugin_systemd_sysupdate_remove_job_revoke (GsPluginSystemdSysupdate *self,
                                               const gchar              *job_path)
{
	g_hash_table_remove (self->job_to_remove_status_map, job_path);
}

/* Request systemd-sysupdate to cancel the given job. It is called when the
 * plugin's update job has been cancelled.
 *
 * Because of the async nature of the application, we can receive job
 * cancellation requests from the application after we requested the update jobs
 * but before we finished preparing them. To handle job cancellation requests
 * correctly, we may need to store them until we are ready. */
static void
gs_plugin_systemd_sysupdate_cancel_job (GsPluginSystemdSysupdate *self,
                                        GsApp                    *app)
{
	TargetItem *target = NULL;
	g_autofree gchar *job_path = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	g_autoptr(GCancellable) cancellable = NULL;
	g_autoptr(GTask) task = NULL;
	GTask *update_task = NULL;
	GsPluginSystemdSysupdateUpdateTargetData *update_data = NULL;

	target = lookup_target_by_app (self, app);
	if (target == NULL) {
		g_debug ("Couldn´t cancel the update: no target found");
		return;
	}

	/* iterate over the on-going tasks to find the job */
	g_hash_table_iter_init (&iter, self->job_task_map);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsPluginSystemdSysupdateUpdateTargetData *job_data = g_task_get_task_data (value);
		if (job_data != NULL &&
		    g_strcmp0 (job_data->target_path, target->object_path) == 0) {
			job_path = g_strdup (key);
			break;
		}
	}
	if (job_path == NULL) {
		g_debug ("Couldn´t cancel the update: no job found for target `%s`", target->object_path);
		return;
	}

	if (g_hash_table_contains (self->job_to_cancel_task_map, job_path)) {
		g_debug ("Job already filed for cancellation: %s", job_path);
		return;
	}

	if (g_hash_table_contains (self->job_to_remove_status_map, job_path)) {
		g_debug ("Job already filed for removal: %s", job_path);
		return;
	}

	cancellable = g_cancellable_new ();
	task = g_task_new (self, cancellable, gs_plugin_systemd_sysupdate_cancel_job_cb, NULL);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_cancel_job);
	g_task_set_task_data (task, g_strdup (job_path), (GDestroyNotify)g_free);

	update_task = G_TASK (g_hash_table_lookup (self->job_task_map, job_path));
	if (update_task == NULL) {
		g_debug ("Couldn´t cancel task for job `%s`, no task found, storing for later cancellation", job_path);
		g_hash_table_insert (self->job_to_cancel_task_map, g_strdup (job_path), g_steal_pointer (&task));
		return;
	}

	update_data = g_task_get_task_data (update_task);
	gs_systemd_sysupdate_job_call_cancel (update_data->job_proxy,
	                                      G_DBUS_CALL_FLAGS_NONE,
	                                      SYSUPDATED_JOB_CANCEL_TIMEOUT_MS,
	                                      cancellable,
	                                      gs_plugin_systemd_sysupdate_cancel_job_cancel_cb,
	                                      g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_cancel_job_cancel_cb (GObject      *source_object,
                                                  GAsyncResult *result,
                                                  gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;

	if (!gs_systemd_sysupdate_job_call_cancel_finish (GS_SYSTEMD_SYSUPDATE_JOB (source_object),
	                                                  result,
	                                                  &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	if (g_task_return_error_if_cancelled (task)) {
		return;
	}

	g_task_return_boolean (task, TRUE);
}

static void
gs_plugin_systemd_sysupdate_cancel_job_cb (GObject      *source_object,
                                           GAsyncResult *result,
                                           gpointer      user_data)
{
	GTask *task = G_TASK (result);
	const gchar *job_path = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autoptr(GError) local_error = NULL;

	job_path = g_task_get_task_data (task);
	g_hash_table_remove (self->job_to_cancel_task_map, job_path);

	if (!g_task_propagate_boolean (task, &local_error)) {
		g_debug ("Couldn´t cancel the update: %s", local_error->message);
		return;
	}

	g_debug ("Cancelled update job `%s` successfully", job_path);
}

static void
gs_plugin_systemd_sysupdate_cancel_job_revoke (GsPluginSystemdSysupdate *self,
                                               const gchar              *job_path)
{
	GTask *task = NULL;
	GCancellable *cancellable = NULL;

	task = G_TASK (g_hash_table_lookup (self->job_to_cancel_task_map, job_path));
	if (!task) {
		return;
	}

	cancellable = g_task_get_cancellable (task);
	if (!cancellable) {
		return;
	}

	g_cancellable_cancel (cancellable);
}

static void
gs_plugin_systemd_sysupdate_update_target_async (GsPluginSystemdSysupdate *self,
                                                 GsApp                    *app,
                                                 const gchar              *target_path,
                                                 const gchar              *target_version,
                                                 GCancellable             *cancellable,
                                                 GAsyncReadyCallback       callback,
                                                 gpointer                  user_data)
{
	GsPluginSystemdSysupdateUpdateTargetData *data = NULL;
	g_autoptr(GTask) task = NULL;
	TargetItem *target = NULL;
	GsPlugin *plugin = GS_PLUGIN (self);

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_update_target_async);

	data = gs_plugin_systemd_sysupdate_update_target_data_new (app,
	                                                           target_path);
	g_task_set_task_data (task, data, (GDestroyNotify)gs_plugin_systemd_sysupdate_update_target_data_free);

	target = lookup_target_by_app (self, data->app);
	if (target == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "cannot find target for app: %s", gs_app_get_name (data->app));
		return;
	}

	/* currently two actions `download file` and `deploy changes`
	 * are bound together as one method in `Target.Update()`.
	 * This method will trigger the update to start and return
	 * immediately. Results should be waited and handled within the
	 * signal `Manager.JobRemoved()` */
	gs_systemd_sysupdate_target_proxy_new (gs_plugin_get_system_bus_connection (plugin),
	                                       G_DBUS_PROXY_FLAGS_NONE,
	                                       "org.freedesktop.sysupdate1",
	                                       target_path,
	                                       cancellable,
	                                       gs_plugin_systemd_sysupdate_update_target_proxy_new_cb,
	                                       g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_update_target_proxy_new_cb (GObject      *source_object,
                                                        GAsyncResult *result,
                                                        gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GVariant) ret_val = NULL;
	g_autoptr(GsSystemdSysupdateTarget) proxy = NULL;
	g_autofree gchar *job_path = NULL;

	proxy = gs_systemd_sysupdate_target_proxy_new_finish (result, &local_error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	gs_systemd_sysupdate_target_call_update (proxy,
	                                         "", /* left empty as the latest version */
	                                         G_DBUS_CALL_FLAGS_NONE,
	                                         SYSUPDATED_TARGET_UPDATE_TIMEOUT_MS,
	                                         NULL, /* Makes the call explicitely non-cancellable so we can get the job path and cancel it correctly. */
	                                         gs_plugin_systemd_sysupdate_update_target_update_cb,
	                                         g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_update_target_update_cb (GObject      *source_object,
                                                     GAsyncResult *result,
                                                     gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GVariant) ret_val = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autofree gchar *job_path = NULL;
	GsPlugin *plugin = GS_PLUGIN (self);
	GsPluginSystemdSysupdateUpdateTargetData *data = NULL;

	if (!gs_systemd_sysupdate_target_call_update_finish (GS_SYSTEMD_SYSUPDATE_TARGET (source_object),
	                                                     NULL,
	                                                     NULL,
	                                                     &job_path,
	                                                     result,
	                                                     &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	data = g_task_get_task_data (task);
	g_set_str (&data->job_path, job_path);

	gs_systemd_sysupdate_job_proxy_new (gs_plugin_get_system_bus_connection (plugin),
	                                    G_DBUS_PROXY_FLAGS_NONE,
	                                    "org.freedesktop.sysupdate1",
	                                    job_path,
	                                    NULL, /* Makes the call explicitely non-cancellable so we can get the job path and cancel it correctly. */
	                                    gs_plugin_systemd_sysupdate_update_target_job_proxy_new_cb,
	                                    g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_update_target_job_proxy_new_cb (GObject      *source_object,
                                                            GAsyncResult *result,
                                                            gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GsSystemdSysupdateJob) proxy = NULL;
	GCancellable *cancellable = g_task_get_cancellable (task);
	GsPluginSystemdSysupdateUpdateTargetData *data = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);

	data = g_task_get_task_data (task);

	proxy = gs_systemd_sysupdate_job_proxy_new_finish (result, &local_error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		/* The job's preparation failed, we can't act on it, revoke any
		 * removal or cancellation request that we filed during its
		 * preparation. */
		gs_plugin_systemd_sysupdate_remove_job_revoke (self, data->job_path);
		gs_plugin_systemd_sysupdate_cancel_job_revoke (self, data->job_path);
		return;
	}

	g_set_object (&data->job_proxy, proxy);

	g_signal_connect_object (proxy, "notify::progress",
	                         G_CALLBACK (gs_plugin_systemd_sysupdate_update_target_notify_progress_cb),
	                         g_object_ref (task), G_CONNECT_SWAPPED);

	gs_plugin_systemd_sysupdate_update_target_notify_progress_cb (task);

	/* job path to task mapping, easier for the callbacks to use the
	 * object path to find it's related task */
	g_hash_table_insert (self->job_task_map,
	                     g_strdup (data->job_path),
	                     g_object_ref (task));

	/* We don't chain up or return here, the task will be terminated when
	 * systemd-sysupdate notifies us that the job is removed, or by
	 * cancelling the task. */

	/* If the update job has been filed for removal during its preparation,
	 * we need to resume the removal request. This will also revoke any
	 * cancellation request. */
	if (g_hash_table_contains (self->job_to_remove_status_map, data->job_path)) {
		gint32 job_status = GPOINTER_TO_INT (g_hash_table_lookup (self->job_to_remove_status_map, data->job_path));
		gs_plugin_systemd_sysupdate_remove_job_apply (self, task, data->job_path, job_status);
		return;
	}

	/* If the update job has been filed for cancellation during its
	 * preparation, we need to resume the cancellation request. */
	if (g_hash_table_contains (self->job_to_cancel_task_map, data->job_path)) {
		GTask *cancel_task = g_hash_table_lookup (self->job_to_cancel_task_map, data->job_path);
		gs_systemd_sysupdate_job_call_cancel (data->job_proxy,
		                                      G_DBUS_CALL_FLAGS_NONE,
		                                      SYSUPDATED_JOB_CANCEL_TIMEOUT_MS,
		                                      g_task_get_cancellable (cancel_task),
		                                      gs_plugin_systemd_sysupdate_cancel_job_cancel_cb,
		                                      g_steal_pointer (&cancel_task));
		return;
	}

	/* If the task has been cancelled during its preparation, we need to ask
	 * systemd-sysdupdate to cancel it. */
	if (g_cancellable_is_cancelled (cancellable)) {
		gs_plugin_systemd_sysupdate_cancel_job (self, data->app);
	}
}

static void
gs_plugin_systemd_sysupdate_update_target_notify_progress_cb (gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GsPluginSystemdSysupdateUpdateTargetData *data = g_task_get_task_data (task);
	guint progress = gs_systemd_sysupdate_job_get_progress (data->job_proxy);

	gs_app_set_state (data->app, GS_APP_STATE_DOWNLOADING);
	gs_app_set_progress (data->app, progress);
}

static gboolean
gs_plugin_systemd_sysupdate_update_target_finish (GsPluginSystemdSysupdate  *self,
                                                  GAsyncResult              *result,
                                                  GError                   **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

/* Plugin methods */
static void
gs_plugin_systemd_sysupdate_init (GsPluginSystemdSysupdate *self)
{
	/* Plugin constructor
	 */
}

static void
gs_plugin_systemd_sysupdate_dispose (GObject *object)
{
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (object);

	g_clear_object (&self->manager_proxy);

	G_OBJECT_CLASS (gs_plugin_systemd_sysupdate_parent_class)->dispose (object);
}

static void
gs_plugin_systemd_sysupdate_finalize (GObject *object)
{
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (object);

	g_clear_pointer (&self->os_pretty_name, g_free);
	g_clear_pointer (&self->os_version, g_free);
	g_clear_pointer (&self->target_item_map, g_hash_table_destroy);
	g_clear_pointer (&self->job_task_map, g_hash_table_destroy);
	g_clear_pointer (&self->job_to_remove_status_map, g_hash_table_destroy);
	g_clear_pointer (&self->job_to_cancel_task_map, g_hash_table_destroy);
	self->is_metadata_refresh_ongoing = FALSE;
	self->last_refresh_usecs = 0;

	G_OBJECT_CLASS (gs_plugin_systemd_sysupdate_parent_class)->finalize (object);
}

static void
gs_plugin_systemd_sysupdate_setup_async (GsPlugin            *plugin,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
	/* Plugin object init. before runtime operations
	 */
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GsOsRelease) os_release = NULL;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_setup_async);

	/* Check that the proxies exist (and are owned; they should auto-start)
	 * so we can disable the plugin for systems which don’t have
	 * systemd-sysupdate. */

	gs_systemd_sysupdate_manager_proxy_new (gs_plugin_get_system_bus_connection (plugin),
	                                        G_DBUS_PROXY_FLAGS_NONE,
	                                        "org.freedesktop.sysupdate1",
	                                        "/org/freedesktop/sysupdate1",
	                                        cancellable,
	                                        gs_plugin_systemd_sysupdate_setup_proxy_new_cb,
	                                        g_steal_pointer (&task));
}

static void
manager_proxy_job_removed_cb (GsPluginSystemdSysupdate       *self,
                              guint64                         job_id,
                              const gchar                    *job_path,
                              gint32                          job_status,
                              GsSystemdSysupdateManagerProxy  proxy)
{
	gs_plugin_systemd_sysupdate_remove_job (self, job_path, job_status);
}

static void
gs_plugin_systemd_sysupdate_setup_proxy_new_cb (GObject      *source_object,
                                                GAsyncResult *result,
                                                gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autoptr(GsOsRelease) os_release = NULL;
	const gchar *os_pretty_name = NULL;
	const gchar *os_version = NULL;

	self->manager_proxy = gs_systemd_sysupdate_manager_proxy_new_finish (result, &local_error);
	if (self->manager_proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* read os-release */
	os_release = gs_os_release_new (&local_error);
	if (local_error) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	os_pretty_name = gs_os_release_get_pretty_name (os_release);
	if (os_pretty_name == NULL) {
		os_pretty_name = "unknown";
	}

	os_version = gs_os_release_get_version (os_release);
	if (os_version == NULL) {
		os_version = "unknown";
	}

	/* `systemd-sysupdated` signal subscription */
	g_signal_connect_object (self->manager_proxy,
	                         "job-removed",
	                         G_CALLBACK (manager_proxy_job_removed_cb),
	                         self,
	                         G_CONNECT_SWAPPED);

	/* plugin object attributes init. */
	self->os_pretty_name = g_strdup (os_pretty_name);
	self->os_version = g_strdup (os_version);
	self->target_item_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)target_item_free);
	self->job_task_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)NULL);
	self->job_to_remove_status_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)NULL);
	self->job_to_cancel_task_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)NULL);
	self->last_refresh_usecs = 0;

	/* on success */
	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_systemd_sysupdate_setup_finish (GsPlugin      *plugin,
                                          GAsyncResult  *result,
                                          GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_systemd_sysupdate_refine_async (GsPlugin            *plugin,
                                          GsAppList           *list,
                                          GsPluginRefineFlags  flags,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
	GsPluginSystemdSysupdateRefineData *data = NULL;
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (plugin);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GQueue) queue = NULL;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_refine_async);

	/* put apps to be refined in queue */
	queue = g_queue_new ();
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);

		/* only process this app if was created by this plugin */
		if (!gs_app_has_management_plugin (app, plugin))
			continue;

		g_queue_push_tail (queue, app);
	}

	/* put apps in queue to task data */
	data = gs_plugin_systemd_sysupdate_refine_data_new (g_steal_pointer (&queue));
	g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify)gs_plugin_systemd_sysupdate_refine_data_free);

	/* invoke the first target */
	gs_plugin_systemd_sysupdate_refine_iter (self,
	                                         g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refine_iter (GsPluginSystemdSysupdate *self,
                                         GTask                    *owned_task)
{
	g_autoptr(GTask) task = g_steal_pointer (&owned_task);
	GsPluginSystemdSysupdateRefineData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	TargetItem *target = NULL;
	const gchar *target_path = NULL;
	GsPlugin *plugin = GS_PLUGIN (self);

	/* Helper method to check queue empty and invoke target describe
	 * one-by-one. If no latest version available, then try with the
	 * current version. */
	data->app = g_queue_pop_head (data->queue);
	if (data->app == NULL) {
		/* return on no next target */
		g_task_return_boolean (task, TRUE);
		return;
	}

	target = lookup_target_by_app (self, data->app);
	if (target == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "cannot find target for app: %s", gs_app_get_name (data->app));
		return;
	}

	target_path = target->object_path;

	gs_systemd_sysupdate_target_proxy_new (gs_plugin_get_system_bus_connection (plugin),
	                                       G_DBUS_PROXY_FLAGS_NONE,
	                                       "org.freedesktop.sysupdate1",
	                                       target_path,
	                                       cancellable,
	                                       gs_plugin_systemd_sysupdate_refine_proxy_new_cb,
	                                       g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refine_proxy_new_cb (GObject      *source_object,
                                                 GAsyncResult *result,
                                                 gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	GCancellable *cancellable = g_task_get_cancellable (task);
	g_autoptr(GVariant) ret_val = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autofree gchar *job_path = NULL;
	g_autoptr(GsSystemdSysupdateTarget) proxy = NULL;
	GsPluginSystemdSysupdateRefineData *data = g_task_get_task_data (task);
	TargetItem *target = NULL;
	const gchar *version = NULL;

	proxy = gs_systemd_sysupdate_target_proxy_new_finish (result, &local_error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	target = lookup_target_by_app (self, data->app);
	if (target == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "cannot find target for app: %s", gs_app_get_name (data->app));
		return;
	}

	version = (g_strcmp0 (target->latest_version, "") != 0) ? target->latest_version
	                                                        : target->current_version;

	/* if the version is not available, it will result an error
	 * later in the callback */
	gs_systemd_sysupdate_target_call_describe (proxy,
	                                           version,
	                                           FALSE,
	                                           G_DBUS_CALL_FLAGS_NONE,
	                                           SYSUPDATED_TARGET_GET_PROPERTIES_TIMEOUT_MS,
	                                           cancellable,
	                                           gs_plugin_systemd_sysupdate_refine_describe_cb,
	                                           g_steal_pointer (&task));
}

static void
refine_app_from_json (GsApp       *app,
                      const gchar *json)
{
	g_autofree gchar *msg = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonNode) root_node = NULL;
	JsonObject *root_object = NULL;
	JsonNode *contents_node;

	/* `systemd-sysupdated` will return a JSON document, it's format has
	 * been deducted by looking at parse_describe() in updatectl.c, and by
	 * looking at what the method returns in GNOME OS.
	 *
	 * The JSON document contains an object with the following fields:
	 * - version: string
	 * - newest: boolean
	 * - available: boolean
	 * - installed: boolean
	 * - obsolete: boolean
	 * - protected: boolean
	 * - changelog_urls: array of strings
	 * - contents: array of partition or regular file objects
	 *
	 * Partition objects have the following fields:
	 * - type: "partition" string
	 * - path: string
	 * - ptuuid: string
	 * - ptflags: number
	 * - mtime: null
	 * - mode: null
	 * - tries-done: null
	 * - tries-left: null
	 * - ro: boolean
	 *
	 * Regular file objects have the following fields:
	 * - type: "regular-file" string
	 * - path: string
	 * - ptuuid: null
	 * - ptflags: null
	 * - mtime: number
	 * - mode: number
	 * - tries-done: number
	 * - tries-left: number
	 * - ro: null
	 */

	root_node = json_from_string (json, &local_error);
	if (local_error != NULL) {
		g_debug ("couldn´t describe, JSON parsing failed: %s", local_error->message);
		return;
	}

	if (root_node != NULL && JSON_NODE_HOLDS_OBJECT (root_node))
		root_object = json_node_get_object (root_node);
	if (root_object == NULL) {
		g_debug ("couldn´t describe, unexpected JSON document format");
		return;
	}

	contents_node = json_object_get_member (root_object, "contents");
	if (contents_node == NULL) {
		g_debug ("couldn´t describe, unexpected JSON document format");
		return;
	}

	msg = json_to_string (contents_node, TRUE);
	gs_app_set_description (app, GS_APP_QUALITY_LOWEST, msg);
}

static void
gs_plugin_systemd_sysupdate_refine_describe_cb (GObject      *source_object,
                                                GAsyncResult *result,
                                                gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GVariant) ret_val = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autofree gchar *json = NULL;

	/* `systemd-sysupdated` also returns error when the given
	 * version is not available (case both no version installed and
	 * no available version in the server). we ignore the error here
	 * and always move on to the next target */
	if (!gs_systemd_sysupdate_target_call_describe_finish (GS_SYSTEMD_SYSUPDATE_TARGET (source_object),
	                                                       &json,
	                                                       result,
	                                                       &local_error)) {
		g_debug ("describe target error ignored, error = `%s`", local_error->message);
	} else {
		GsPluginSystemdSysupdateRefineData *data = g_task_get_task_data (task);
		refine_app_from_json (data->app, json);
	}

	/* move on to check new version */
	gs_plugin_systemd_sysupdate_refine_iter (self,
	                                         g_steal_pointer (&task));
}

static gboolean
gs_plugin_systemd_sysupdate_refine_finish (GsPlugin      *plugin,
                                           GAsyncResult  *result,
                                           GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_systemd_sysupdate_list_apps_async (GsPlugin              *plugin,
                                             GsAppQuery            *query,
                                             GsPluginListAppsFlags  flags,
                                             GCancellable          *cancellable,
                                             GAsyncReadyCallback    callback,
                                             gpointer               user_data)
{
	/* Return managed apps filtered by the given query
	 */
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (plugin);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GsAppList) list = gs_app_list_new ();
	GsAppQueryTristate is_installed = GS_APP_QUERY_TRISTATE_UNSET;
	GsAppQueryTristate is_for_update = GS_APP_QUERY_TRISTATE_UNSET;
	const gchar * const *keywords = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_list_apps_async);

	/* here we report the system updates as individual apps, so user
	 * can easily search and update a specific target */

	if (query != NULL) {
		is_installed = gs_app_query_get_is_installed (query);
		is_for_update = gs_app_query_get_is_for_update (query);
		keywords = gs_app_query_get_keywords (query);
	}

	/* currently only support a subset of query properties, and only
	 * one set at once */
	if ((is_installed == GS_APP_QUERY_TRISTATE_UNSET &&
	     is_for_update == GS_APP_QUERY_TRISTATE_UNSET &&
	     keywords == NULL) ||
	    gs_app_query_get_n_properties_set (query) != 1) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                         "Unsupported query");
		return;
	}

	/* iterate over our targets, after `refresh_metadata()` we
	 * should have target and its corresponding app created and
	 * stored in the per-plugin cache */
	g_hash_table_iter_init (&iter, self->target_item_map);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		TargetItem *target = (TargetItem *)value;
		g_autoptr(GsApp) app = NULL;

		/* get or create app for the target */
		app = get_or_create_app_for_target (self, target);
		if (app == NULL) {
			continue;
		}

		/* do not list the `OS upgrade` as an user app here since
		 * it's handle is customized in this plugin */
		if (g_strcmp0 (target->class, "host") == 0) {
			continue;
		}

		if ((keywords != NULL) &&
		    (g_strv_contains (keywords, "sysupdate") ||
		     g_strv_contains (keywords, target->class) ||
		     g_strv_contains (keywords, target->name))) {
			gs_app_list_add (list, app);
			continue;
		}

		if (is_for_update == GS_APP_QUERY_TRISTATE_TRUE) {
			gs_app_list_add (list, app);
			continue;
		}

		if ((is_installed != GS_APP_QUERY_TRISTATE_UNSET) &&
		    (((is_installed == GS_APP_QUERY_TRISTATE_FALSE) && (g_strcmp0 (target->current_version, "") == 0)) ||
		     ((is_installed == GS_APP_QUERY_TRISTATE_TRUE) && (g_strcmp0 (target->current_version, "") != 0)))) {
			gs_app_list_add (list, app);
			continue;
		}
	}

	g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static GsAppList *
gs_plugin_systemd_sysupdate_list_apps_finish (GsPlugin      *plugin,
                                              GAsyncResult  *result,
                                              GError       **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_async (GsPlugin                     *plugin,
                                                    guint64                       cache_age_secs,
                                                    GsPluginRefreshMetadataFlags  flags,
                                                    GCancellable                 *cancellable,
                                                    GAsyncReadyCallback           callback,
                                                    gpointer                      user_data)
{
	/* Periodically update the targets saved
	 */
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (plugin);
	g_autoptr(GTask) task = NULL;
	guint64 delta_usecs = 0;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_refresh_metadata_async);

	/* because currently we do not own any file by this plugin, we
	 * use the monotonic time saved last run to decide if we need to
	 * refresh the target list */
	delta_usecs = g_get_monotonic_time () - self->last_refresh_usecs; /* us */
	if (self->is_metadata_refresh_ongoing ||
	    (cache_age_secs > 0 && delta_usecs < cache_age_secs * G_USEC_PER_SEC)) {
		g_debug ("cache is only %" G_GUINT64_FORMAT " seconds old", (guint64) (delta_usecs / 10e6));
		g_task_return_boolean (task, TRUE);
		return;
	}
	self->is_metadata_refresh_ongoing = TRUE; /* update immediately to block continuous refreshes */
	self->last_refresh_usecs = g_get_monotonic_time ();

	/* here we ask `systemd-sysupdated` to list all available
	 * targets and enumerate the targets reported in the callback. */
	gs_systemd_sysupdate_manager_call_list_targets (self->manager_proxy,
	                                                G_DBUS_CALL_FLAGS_NONE,
	                                                SYSUPDATED_MANAGER_LIST_TARGET_TIMEOUT_MS,
	                                                cancellable,
	                                                gs_plugin_systemd_sysupdate_refresh_metadata_list_targets_cb,
	                                                g_steal_pointer (&task));
}

static gboolean
check_to_be_removed (gpointer key, gpointer value, gpointer user_data)
{
	TargetItem *target = (TargetItem *) value;
	return !target->is_valid;
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_list_targets_cb (GObject      *source_object,
                                                              GAsyncResult *result,
                                                              gpointer      user_data)
{
	GsPluginSystemdSysupdateRefreshMetadataData *data = NULL;
	TargetItem *target = NULL;
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GQueue) queue = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GVariant) ret_targets = NULL;
	g_autoptr(GVariantIter) variant_iter = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	const gchar *class = NULL;
	const gchar *name = NULL;
	const gchar *object_path = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (!gs_systemd_sysupdate_manager_call_list_targets_finish (GS_SYSTEMD_SYSUPDATE_MANAGER (source_object),
	                                                            &ret_targets,
	                                                            result,
	                                                            &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* mark all targets saved as invalid to detect removals */
	g_hash_table_iter_init (&iter, self->target_item_map);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		target = (TargetItem *)value;
		target->is_valid = FALSE;
	}

	/* iterate over targets and save to the target hashmap */
	g_variant_get (ret_targets, "a(sso)", &variant_iter);
	while (g_variant_iter_loop (variant_iter, "(&s&s&o)", &class, &name, &object_path)) {

		key = g_strdup (name);
		g_hash_table_insert (self->target_item_map, key, (gpointer)target_item_new (class, name, object_path)); /* overwrite value */
	}

	/* remove targets no-longer exist and their apps */
	g_hash_table_iter_init (&iter, self->target_item_map);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		target = (TargetItem *)value;
		if (!target->is_valid) {
			gs_plugin_cache_remove (GS_PLUGIN (self), key);
		}
	}
	g_hash_table_foreach_remove (self->target_item_map, (GHRFunc) check_to_be_removed, NULL);

	/* push all targets to queue. Make 'host' the first target if it
	 * exists, so other targets can point to it if it needs to */
	queue = g_queue_new ();
	g_hash_table_iter_init (&iter, self->target_item_map);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		target = (TargetItem *)value;
		if (g_strcmp0 (target->class, "host") == 0) {
			g_queue_push_head (queue, value);
		} else {
			g_queue_push_tail (queue, value);
		}
	}

	/* put apps in queue to task data */
	data = gs_plugin_systemd_sysupdate_refresh_metadata_data_new (g_steal_pointer (&queue));
	g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify)gs_plugin_systemd_sysupdate_refresh_metadata_data_free);

	/* invoke the first target */
	gs_plugin_systemd_sysupdate_refresh_metadata_iter (g_steal_pointer (&self),
	                                                   g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_iter (GsPluginSystemdSysupdate *self,
                                                   GTask                    *owned_task)
{
	g_autoptr(GTask) task = g_steal_pointer (&owned_task);
	GsPluginSystemdSysupdateRefreshMetadataData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	GsPlugin *plugin = GS_PLUGIN (self);

	/* Helper method to iterate over the elements of the queue one-by-one.
	 *
	 * While the typical use case is to have only a single update target,
	 * there could be multiple ones, so this could be improved in the future
	 * by applying the updates in parallel.
	 */
	data->target = g_queue_pop_head (data->queue);
	if (data->target == NULL) {
		/* We reached the end of the queue. */
		g_task_return_boolean (task, TRUE);
		return;
	}

	gs_systemd_sysupdate_target_proxy_new (gs_plugin_get_system_bus_connection (plugin),
	                                       G_DBUS_PROXY_FLAGS_NONE,
	                                       "org.freedesktop.sysupdate1",
	                                       data->target->object_path,
	                                       cancellable,
	                                       gs_plugin_systemd_sysupdate_refresh_metadata_proxy_new_cb,
	                                       g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_proxy_new_cb (GObject      *source_object,
                                                           GAsyncResult *result,
                                                           gpointer      user_data)
{
	GsPluginSystemdSysupdateRefreshMetadataData *data = NULL;
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GsSystemdSysupdateTarget) proxy = NULL;
	GCancellable *cancellable = g_task_get_cancellable (task);

	proxy = gs_systemd_sysupdate_target_proxy_new_finish (result, &local_error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	data = g_task_get_task_data (task);
	g_set_object (&data->target->proxy, proxy);

	gs_systemd_sysupdate_target_call_get_version (data->target->proxy,
	                                              G_DBUS_CALL_FLAGS_NONE,
	                                              SYSUPDATED_TARGET_GET_PROPERTIES_TIMEOUT_MS,
	                                              cancellable,
	                                              gs_plugin_systemd_sysupdate_refresh_metadata_get_version_cb,
	                                              g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_get_version_cb (GObject      *source_object,
                                                             GAsyncResult *result,
                                                             gpointer      user_data)
{
	GsPluginSystemdSysupdateRefreshMetadataData *data = NULL;
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *current_version = NULL;
	GCancellable *cancellable = g_task_get_cancellable (task);

	if (!gs_systemd_sysupdate_target_call_get_version_finish (GS_SYSTEMD_SYSUPDATE_TARGET (source_object),
	                                                          &current_version,
	                                                          result,
	                                                          &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	data = g_task_get_task_data (task);
	g_set_str (&data->target->current_version, current_version);

	/* move on to check new version */
	gs_systemd_sysupdate_target_call_check_new (data->target->proxy,
	                                            G_DBUS_CALL_FLAGS_NONE,
	                                            SYSUPDATED_TARGET_CHECK_NEW_TIMEOUT_MS,
	                                            cancellable,
	                                            gs_plugin_systemd_sysupdate_refresh_metadata_check_new_cb,
	                                            g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_refresh_metadata_check_new_cb (GObject      *source_object,
                                                           GAsyncResult *result,
                                                           gpointer      user_data)
{
	GsPluginSystemdSysupdateRefreshMetadataData *data = NULL;
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GVariant) ret_val = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);
	g_autoptr(GsApp) app = NULL;
	g_autofree gchar *latest_version = NULL;

	/* currently, the returned result contains only one string
	 * representing the latest version found in the server. However,
	 * it can possibly be an empty string representing no newer
	 * version available */
	if (!gs_systemd_sysupdate_target_call_check_new_finish (GS_SYSTEMD_SYSUPDATE_TARGET (source_object),
	                                                        &latest_version,
	                                                        result,
	                                                        &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* update target */
	data = g_task_get_task_data (task);
	g_set_str (&data->target->latest_version, latest_version);

	/* update app state base on the target's new version */
	app = get_or_create_app_for_target (self, data->target);
	update_app_for_target (self, app, data->target);

	/* move on to next target */
	gs_plugin_systemd_sysupdate_refresh_metadata_iter (g_steal_pointer (&self),
	                                                   g_steal_pointer (&task));
}

static gboolean
gs_plugin_systemd_sysupdate_refresh_metadata_finish (GsPlugin      *plugin,
                                                     GAsyncResult  *result,
                                                     GError       **error)
{
	GTask *task = G_TASK (result);
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);

	self->is_metadata_refresh_ongoing = FALSE;
	return g_task_propagate_boolean (task, error);
}

static void
gs_plugin_systemd_sysupdate_list_distro_upgrades_async (GsPlugin                        *plugin,
                                                        GsPluginListDistroUpgradesFlags  flags,
                                                        GCancellable                    *cancellable,
                                                        GAsyncReadyCallback              callback,
                                                        gpointer                         user_data)
{
	/* Report available distro upgrades
	 */
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (plugin);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GsAppList) list = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_list_distro_upgrades_async);

	/* report only the distro upgrade, and left all other targets be
	 * reported in `list_apps_async()` */
	list = gs_app_list_new ();
	g_hash_table_iter_init (&iter, self->target_item_map);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		TargetItem *target = (TargetItem *)value;
		g_autoptr(GsApp) app = NULL;

		/* ignore other than `host` targets */
		if (g_strcmp0 (target->class, "host") != 0) {
			continue;
		}

		/* by default, distro upgrade does not use state 'unknown'
		 * and 'installed'. Instead, just return an empty app list
		 * so there won't be anything displayed on the banner */
		if (g_strcmp0 (target->latest_version, "") == 0) {
			continue;
		}

		app = get_or_create_app_for_target (self, target);
		if (app != NULL) {
			gs_app_list_add (list, app);
		}
	}

	g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static GsAppList *
gs_plugin_systemd_sysupdate_list_distro_upgrades_finish (GsPlugin      *plugin,
                                                         GAsyncResult  *result,
                                                         GError       **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

static void
update_apps_cancelled_cb (GCancellable *cancellable,
                          gpointer      user_data)
{
	GTask *task = G_TASK (user_data);
	GsPluginSystemdSysupdate *self = NULL;
	GsPluginSystemdSysupdateUpdateAppsData *data = NULL;

	if (!g_cancellable_is_cancelled (cancellable)) {
		return;
	}

	self = g_task_get_source_object (task);
	data = g_task_get_task_data (task);
	gs_plugin_systemd_sysupdate_cancel_job (self, data->app);
}

static void
gs_plugin_systemd_sysupdate_update_apps_async (GsPlugin                           *plugin,
                                               GsAppList                          *apps,
                                               GsPluginUpdateAppsFlags             flags,
                                               GsPluginProgressCallback            progress_callback,
                                               gpointer                            progress_user_data,
                                               GsPluginAppNeedsUserActionCallback  app_needs_user_action_callback,
                                               gpointer                            app_needs_user_action_data,
                                               GCancellable                       *cancellable,
                                               GAsyncReadyCallback                 callback,
                                               gpointer                            user_data)
{
	/* Install the given system updates
	 */
	GsPluginSystemdSysupdateUpdateAppsData *data = NULL;
	GsPluginSystemdSysupdate *self = GS_PLUGIN_SYSTEMD_SYSUPDATE (plugin);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GQueue) queue = NULL;
	gulong cancelled_id = 0;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_update_apps_async);

	if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD &&
	    flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY) {
		g_task_return_boolean (task, TRUE);
		return;
	}

	/* The download and apply steps are merged into a single operation in
	 * systemd-sysupdate, meaning we can't download the update without
	 * downloading and vice versa. */
	/* TODO Split the download and apply steps once the systemd-sysupdate
	 * D-Bus API allows it. */
	if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD) {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Update failed: systemd-sysupdate can´t apply an update without downloading it");
		return;
	} else if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY) {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Update failed: systemd-sysupdate can´t download an update without applying it");
		return;
	}

	queue = g_queue_new ();
	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);

		/* only process this app if was created by this plugin */
		if (!gs_app_has_management_plugin (app, plugin)) {
			continue;
		}

		/* only update the app if it is source available */
		if (gs_app_get_state (app) != GS_APP_STATE_AVAILABLE &&
		    gs_app_get_state (app) != GS_APP_STATE_AVAILABLE_LOCAL &&
		    gs_app_get_state (app) != GS_APP_STATE_UPDATABLE &&
		    gs_app_get_state (app) != GS_APP_STATE_UPDATABLE_LIVE &&
		    gs_app_get_state (app) != GS_APP_STATE_QUEUED_FOR_INSTALL) {
			continue;
		}

		/* before we can update components individually, temporarily
		 * make the `devel` to be the last one to be updated for it
		 * is too big */
		if (g_strcmp0 (gs_app_get_name (app), "component-devel") == 0) {
			g_queue_push_tail (queue, app);
		} else {
			g_queue_push_head (queue, app);
		}
	}

	/* connect to cancellation signal */
	if (cancellable != NULL) {
		cancelled_id = g_cancellable_connect (cancellable,
		                                      G_CALLBACK (update_apps_cancelled_cb),
		                                      (gpointer)task,
		                                      (GDestroyNotify)NULL);
	}

	/* put apps in queue to task data */
	data = gs_plugin_systemd_sysupdate_update_apps_data_new (g_steal_pointer (&queue),
	                                                         cancelled_id);
	g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify)gs_plugin_systemd_sysupdate_update_apps_data_free);

	/* invoke the first target */
	gs_plugin_systemd_sysupdate_update_apps_iter (g_steal_pointer (&self),
	                                              g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_update_apps_iter (GsPluginSystemdSysupdate *self,
                                              GTask                    *task)
{
	GsPluginSystemdSysupdateUpdateAppsData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	TargetItem *target = NULL;

	/* Helper method to iterate over the elements of the queue one-by-one.
	 *
	 * While the typical use case is to have only a single update target,
	 * there could be multiple ones, so this could be improved in the future
	 * by applying the updates in parallel.
	 */
	data->app = g_queue_pop_head (data->queue);
	if (data->app == NULL) {
		/* We reached the end of the queue. */
		g_task_return_boolean (task, TRUE);
		return;
	}

	/* find the target associated to the app */
	target = lookup_target_by_app (self, data->app);
	if (target == NULL) {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Can´t find target for app: %s", gs_app_get_name (data->app));
		return;
	}

	/* update the 'target' to specific version */
	gs_plugin_systemd_sysupdate_update_target_async (self,
	                                                 data->app,
	                                                 target->object_path,
	                                                 gs_app_get_version (data->app),
	                                                 cancellable,
	                                                 gs_plugin_systemd_sysupdate_update_apps_update_target_cb,
	                                                 g_steal_pointer (&task));
}

static void
gs_plugin_systemd_sysupdate_update_apps_update_target_cb (GObject      *source_object,
                                                          GAsyncResult *result,
                                                          gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	g_autoptr(GError) local_error = NULL;
	GsPluginSystemdSysupdate *self = g_task_get_source_object (task);

	if (!gs_plugin_systemd_sysupdate_update_target_finish (self, result, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* move on to next app (target) */
	gs_plugin_systemd_sysupdate_update_apps_iter (g_steal_pointer (&self),
	                                              g_steal_pointer (&task));
}

static gboolean
gs_plugin_systemd_sysupdate_update_apps_finish (GsPlugin      *plugin,
                                                GAsyncResult  *result,
                                                GError       **error)
{
	GsPluginSystemdSysupdateUpdateAppsData *data = NULL;
	GTask *task = G_TASK (result);
	GCancellable *cancellable = g_task_get_cancellable (task);

	/* disconnect cancellation signal */
	data = g_task_get_task_data (task);
	if ((cancellable != NULL) &&
	    (data->cancelled_id != 0)) {
		g_cancellable_disconnect (cancellable, data->cancelled_id);
	}
	return g_task_propagate_boolean (g_steal_pointer (&task), error);
}

static void
gs_plugin_systemd_sysupdate_install_apps_async (GsPlugin                           *plugin,
                                                GsAppList                          *apps,
                                                GsPluginInstallAppsFlags            flags,
                                                GsPluginProgressCallback            progress_callback,
                                                gpointer                            progress_user_data,
                                                GsPluginAppNeedsUserActionCallback  app_needs_user_action_callback,
                                                gpointer                            app_needs_user_action_data,
                                                GCancellable                       *cancellable,
                                                GAsyncReadyCallback                 callback,
                                                gpointer                            user_data)
{
	GsPluginUpdateAppsFlags update_flags = GS_PLUGIN_UPDATE_APPS_FLAGS_NONE;

	if (flags & GS_PLUGIN_INSTALL_APPS_FLAGS_INTERACTIVE)
		update_flags |= GS_PLUGIN_UPDATE_APPS_FLAGS_INTERACTIVE;
	if (flags & GS_PLUGIN_INSTALL_APPS_FLAGS_NO_DOWNLOAD)
		update_flags |= GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD;
	if (flags & GS_PLUGIN_INSTALL_APPS_FLAGS_NO_APPLY)
		update_flags |= GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY;

	gs_plugin_systemd_sysupdate_update_apps_async (plugin,
	                                               apps,
	                                               update_flags,
	                                               progress_callback,
	                                               progress_user_data,
	                                               app_needs_user_action_callback,
	                                               app_needs_user_action_data,
	                                               cancellable,
	                                               callback,
	                                               user_data);
}

static gboolean
gs_plugin_systemd_sysupdate_install_apps_finish (GsPlugin      *plugin,
                                                 GAsyncResult  *result,
                                                 GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_systemd_sysupdate_download_upgrade_async (GsPlugin                     *plugin,
                                                    GsApp                        *app,
                                                    GsPluginDownloadUpgradeFlags  flags,
                                                    GCancellable                 *cancellable,
                                                    GAsyncReadyCallback           callback,
                                                    gpointer                      user_data)
{
	/* Flag specific distro upgrade as downloadable and installable
	 */
	g_autoptr(GTask) task = NULL;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_systemd_sysupdate_download_upgrade_async);

	/* only process this app if was created by this plugin */
	if (!gs_app_has_management_plugin (app, plugin)) {
		g_task_return_boolean (task, TRUE);
		return;
	}

	/* only update the app if it is source available */
	if (gs_app_get_state (app) != GS_APP_STATE_AVAILABLE &&
	    gs_app_get_state (app) != GS_APP_STATE_AVAILABLE_LOCAL) {
		g_task_return_boolean (task, TRUE);
		return;
	}

	gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_systemd_sysupdate_download_upgrade_finish (GsPlugin      *plugin,
                                                     GAsyncResult  *result,
                                                     GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_systemd_sysupdate_trigger_upgrade_async (GsPlugin                    *plugin,
                                                   GsApp                       *app,
                                                   GsPluginTriggerUpgradeFlags  flags,
                                                   GCancellable                *cancellable,
                                                   GAsyncReadyCallback          callback,
                                                   gpointer                     user_data)
{
	/* Download and install specific distro upgrade
	 */
	g_autoptr(GsAppList) apps = NULL;

	apps = gs_app_list_new ();
	gs_app_list_add (apps, app);

	gs_plugin_systemd_sysupdate_update_apps_async (plugin, apps,
	                                               GS_PLUGIN_UPDATE_APPS_FLAGS_NONE,
	                                               NULL, NULL,
	                                               NULL, NULL,
	                                               cancellable,
	                                               callback, user_data);
}

static gboolean
gs_plugin_systemd_sysupdate_trigger_upgrade_finish (GsPlugin      *plugin,
                                                    GAsyncResult  *result,
                                                    GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

void
gs_plugin_adopt_app (GsPlugin *plugin,
                     GsApp    *app)
{
	/* Adopt app originally discovered by other plugins
	 */

	// TODO: add bundle kind to libappstream and gs-plugin-appstream?
	// if (gs_app_get_bundle_kine (app) == AS_BUNDLE_KIND_SYSUPDATE)
	//     gs_app_set_management_plugin (app, plugin);
}

static void
gs_plugin_systemd_sysupdate_class_init (GsPluginSystemdSysupdateClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

	object_class->dispose = gs_plugin_systemd_sysupdate_dispose;
	object_class->finalize = gs_plugin_systemd_sysupdate_finalize;

	plugin_class->setup_async = gs_plugin_systemd_sysupdate_setup_async;
	plugin_class->setup_finish = gs_plugin_systemd_sysupdate_setup_finish;
	plugin_class->refine_async = gs_plugin_systemd_sysupdate_refine_async;
	plugin_class->refine_finish = gs_plugin_systemd_sysupdate_refine_finish;
	plugin_class->list_apps_async = gs_plugin_systemd_sysupdate_list_apps_async;
	plugin_class->list_apps_finish = gs_plugin_systemd_sysupdate_list_apps_finish;
	plugin_class->refresh_metadata_async = gs_plugin_systemd_sysupdate_refresh_metadata_async;
	plugin_class->refresh_metadata_finish = gs_plugin_systemd_sysupdate_refresh_metadata_finish;
	plugin_class->list_distro_upgrades_async = gs_plugin_systemd_sysupdate_list_distro_upgrades_async;
	plugin_class->list_distro_upgrades_finish = gs_plugin_systemd_sysupdate_list_distro_upgrades_finish;
	plugin_class->update_apps_async = gs_plugin_systemd_sysupdate_update_apps_async;
	plugin_class->update_apps_finish = gs_plugin_systemd_sysupdate_update_apps_finish;
	plugin_class->install_apps_async = gs_plugin_systemd_sysupdate_install_apps_async;
	plugin_class->install_apps_finish = gs_plugin_systemd_sysupdate_install_apps_finish;
	plugin_class->download_upgrade_async = gs_plugin_systemd_sysupdate_download_upgrade_async;
	plugin_class->download_upgrade_finish = gs_plugin_systemd_sysupdate_download_upgrade_finish;
	plugin_class->trigger_upgrade_async = gs_plugin_systemd_sysupdate_trigger_upgrade_async;
	plugin_class->trigger_upgrade_finish = gs_plugin_systemd_sysupdate_trigger_upgrade_finish;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_SYSTEMD_SYSUPDATE;
}


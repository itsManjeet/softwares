/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2021 Matthew Leeds <mwleeds@endlessos.org>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gs-app-version-history-dialog.h"

#include "gnome-software-private.h"
#include "gs-common.h"
#include "gs-app-version-history-row.h"
#include <glib/gi18n.h>

struct _GsAppVersionHistoryDialog
{
	AdwDialog	 parent_instance;
	GsApp		*app;
	GtkWidget	*listbox;
};

G_DEFINE_TYPE (GsAppVersionHistoryDialog, gs_app_version_history_dialog, ADW_TYPE_DIALOG)

static void
populate_version_history (GsAppVersionHistoryDialog *dialog,
			  GsApp			    *app)
{
	g_autoptr(GPtrArray) version_history = NULL;
	const gchar *app_version;
	gboolean can_check_whether_the_version_is_installed;

	/* remove previous */
	gs_widget_remove_all (dialog->listbox, (GsRemoveFunc) gtk_list_box_remove);

	/* grab focus so first row text is not selected by default */
	gtk_widget_grab_focus (dialog->listbox);

	version_history = gs_app_get_version_history (app);
	if (version_history == NULL || version_history->len == 0) {
		GtkWidget *row;
		row = gs_app_version_history_row_new ();
		gs_app_version_history_row_set_always_expanded (GS_APP_VERSION_HISTORY_ROW (row), TRUE);
		gs_app_version_history_row_set_info (GS_APP_VERSION_HISTORY_ROW (row),
						     gs_app_get_version (app),
						     gs_app_get_release_date (app), NULL, FALSE);
		gtk_list_box_append (GTK_LIST_BOX (dialog->listbox), row);
		gtk_widget_set_visible (row, TRUE);
		return;
	}

	app_version = gs_app_get_version_ui (app);
	can_check_whether_the_version_is_installed = app_version != NULL && version_history->len > 1 && gs_app_is_installed (app);

	/* add each */
	for (guint i = 0; i < version_history->len; i++) {
		GtkWidget *row;
		AsRelease *version = g_ptr_array_index (version_history, i);
		gboolean is_installed = FALSE;

		if (can_check_whether_the_version_is_installed)
			is_installed = g_strcmp0 (as_release_get_version (version), app_version) == 0;

		row = gs_app_version_history_row_new ();
		gs_app_version_history_row_set_always_expanded (GS_APP_VERSION_HISTORY_ROW (row), TRUE);
		gs_app_version_history_row_set_info (GS_APP_VERSION_HISTORY_ROW (row),
						     as_release_get_version (version),
						     as_release_get_timestamp (version),
						     as_release_get_description (version),
						     is_installed);

		gtk_list_box_append (GTK_LIST_BOX (dialog->listbox), row);
		gtk_widget_set_visible (row, TRUE);
	}
}

static void
gs_app_version_history_dialog_init (GsAppVersionHistoryDialog *dialog)
{
	gtk_widget_init_template (GTK_WIDGET (dialog));
}

static void
gs_app_version_history_dialog_class_init (GsAppVersionHistoryDialogClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-app-version-history-dialog.ui");

	gtk_widget_class_bind_template_child (widget_class, GsAppVersionHistoryDialog, listbox);
}

GtkWidget *
gs_app_version_history_dialog_new (GsApp *app)
{
	GsAppVersionHistoryDialog *dialog;

	dialog = g_object_new (GS_TYPE_APP_VERSION_HISTORY_DIALOG,
			       NULL);
	populate_version_history (dialog, app);

	return GTK_WIDGET (dialog);
}

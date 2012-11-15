/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* Postasa -- PicasaWeb Uploader plugin
 *
 * Copyright (C) 2009 The Free Software Foundation
 *
 * Author: Richard Schwarting <aquarichy@gmail.com>
 * Initially based on Postr code by: Lucas Rocha
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
 **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "eom-postasa-plugin.h"

#include <gmodule.h>
#include <glib/gi18n-lib.h>

#include <eom/eom-debug.h>
#include <eom/eom-thumb-view.h>
#include <eom/eom-image.h>

#include <gdata/gdata.h>

#define WINDOW_DATA_KEY "EomPostasaWindowData"

#define GTKBUILDER_CONFIG_FILE EOM_PLUGINDIR"/postasa/postasa-config.xml"
#define GTKBUILDER_UPLOAD_FILE EOM_PLUGINDIR"/postasa/postasa-uploads.xml"
#define DEFAULT_THUMBNAIL EOM_PLUGINDIR"/postasa/default.png"

EOM_PLUGIN_REGISTER_TYPE(EomPostasaPlugin, eom_postasa_plugin)

/**
 * _EomPostasaPluginPrivate:
 *
 * Private data for the Postasa EoM plugin
 **/
struct _EomPostasaPluginPrivate
{
	EomWindow    *eom_window;
	GDataClientLoginAuthorizer *authorizer;
	GDataPicasaWebService *service;
	GCancellable *login_cancellable;

	/* TODO: consider using GConf to store the username in; perhaps not the password */
	GtkDialog    *login_dialog;
	GtkEntry     *username_entry;
	GtkEntry     *password_entry;
	GtkLabel     *login_message;
	GtkButton    *login_button;
	GtkButton    *cancel_button;
	gboolean      uploads_pending;

	GtkWindow    *uploads_window;
	GtkTreeView  *uploads_view;
	GtkListStore *uploads_store;
};

/**
 * WindowData:
 *
 * Data we'll associate with the window, describing the action group
 * as well as the plugin, so we'll be able to access it even from the
 * #GtkActionEntry's callback.
 **/
typedef struct
{
	GtkActionGroup *ui_action_group;
	guint ui_id;
	EomPostasaPlugin *plugin;
} WindowData;

/**
 * PicasaWebUploadFileAsyncData:
 *
 * Small chunk of data for use by our asynchronous PicasaWeb upload
 * API.  It describes the position in the upload window's tree view, for
 * cancellation purposes, and the the image file that we want to
 * upload.
 *
 * TODO: remove this and the async API when we get a proper upload
 * async method into libgdata.
 **/
typedef struct
{
	GtkTreeIter *iter;
	GFile *imgfile;
} PicasaWebUploadFileAsyncData;

static void picasaweb_upload_cb (GtkAction *action, EomWindow *window);
static GtkWidget *login_get_dialog (EomPostasaPlugin *plugin);
static gboolean login_dialog_close (EomPostasaPlugin *plugin);

static const gchar * const ui_definition = 
	"<ui><menubar name=\"MainMenu\">"
	"<menu name=\"ToolsMenu\" action=\"Tools\"><separator/>"
	"<menuitem name=\"EomPluginPostasa\" action=\"EomPluginRunPostasa\"/>"
	"<separator/></menu></menubar></ui>";

/**
 * action_entries:
 *
 * Describes the #GtkActionEntry representing the Postasa upload menu
 * item and it's callback.
 **/
static const GtkActionEntry action_entries[] =
{
	{ "EomPluginRunPostasa",
	  "postasa",
	  N_("Upload to PicasaWeb"),
	  NULL,
	  N_("Upload your pictures to PicasaWeb"),
	  G_CALLBACK (picasaweb_upload_cb) }
};

/*** Uploads Dialog ***/

/**
 * uploads_cancel_row:
 *
 * Function on the upload list store to cancel a specified upload.
 * This is intended to be called by foreach functions.
 *
 * There's a small chance that the #GCancellable cancellation might not
 * occur before the upload is completed, in which case we do not
 * change the row's status to "Cancelled".
 **/
static gboolean
uploads_cancel_row (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, EomPostasaPlugin *plugin)
{
	GCancellable *cancellable;

	gtk_tree_model_get (model, iter, 4, &cancellable, -1);
	g_cancellable_cancel (cancellable);
	/* we let picasaweb_upload_async_cb() handle setting a Cancelled message */

	return FALSE;
}

/**
 * uploads_cancel_cb:
 *
 * Obtains the current selection and calls an API to cancel each one.
 **/
static void
uploads_cancel_cb (GtkWidget *cancel_button, EomPostasaPlugin *plugin)
{
	GtkTreeSelection *selection;
	selection = gtk_tree_view_get_selection (plugin->priv->uploads_view); /* don't unref */

	gtk_tree_selection_selected_foreach (selection, (GtkTreeSelectionForeachFunc)uploads_cancel_row, plugin);
}

/**
 * uploads_cancel_all_cb:
 *
 * Calls an API on every row to cancel its upload.
 **/
static void
uploads_cancel_all_cb (GtkWidget *cancel_all_button, EomPostasaPlugin *plugin)
{
	gtk_tree_model_foreach (GTK_TREE_MODEL (plugin->priv->uploads_store), (GtkTreeModelForeachFunc)uploads_cancel_row, plugin);
}

/**
 * uploads_get_dialog:
 *
 * Returns the a #GtkWindow representing the Uploads window.  If it
 * has not already been created, it creates it.  The Uploads window is
 * set to be hidden instead of destroyed when closed, to avoid having
 * to recreate it and re-parse the UI file, etc.
 **/
static GtkWindow *
uploads_get_dialog (EomPostasaPlugin *plugin)
{
	GtkBuilder *builder;
	GError *error = NULL;
	GtkButton *cancel_button;
	GtkButton *cancel_all_button;

	if (plugin->priv->uploads_window == NULL) {
		builder = gtk_builder_new ();
		gtk_builder_set_translation_domain (builder, GETTEXT_PACKAGE);
		gtk_builder_add_from_file (builder, GTKBUILDER_UPLOAD_FILE, &error);
		if (error != NULL) {
			g_warning ("Couldn't load Postasa uploads UI file:%d:%s", error->code, error->message);
			g_error_free (error);
			return NULL;
		}

		/* note: do not unref gtk_builder_get_object() returns */
		plugin->priv->uploads_window = GTK_WINDOW     (gtk_builder_get_object (builder, "uploads_window"));
		plugin->priv->uploads_view   = GTK_TREE_VIEW  (gtk_builder_get_object (builder, "uploads_view"));
		plugin->priv->uploads_store  = GTK_LIST_STORE (gtk_builder_get_object (builder, "uploads_store"));

		cancel_button     = GTK_BUTTON (gtk_builder_get_object (builder, "cancel_button"));
		cancel_all_button = GTK_BUTTON (gtk_builder_get_object (builder, "cancel_all_button"));

		/* TODO: can't set expand = TRUE when packing cells into columns via glade-3/GtkBuilder apparently?
		   bgo #602152  So for now, we take them, clear them out, and remap them.  Ugh.  Better solutions welcome.  */
		GtkTreeViewColumn *file_col       = GTK_TREE_VIEW_COLUMN (gtk_builder_get_object (builder, "file_col"));
		GtkCellRenderer   *thumbnail_cell = GTK_CELL_RENDERER    (gtk_builder_get_object (builder, "thumbnail_cell"));
		GtkCellRenderer   *filepath_cell  = GTK_CELL_RENDERER    (gtk_builder_get_object (builder, "filepath_cell"));
		gtk_tree_view_column_clear (file_col);
		gtk_tree_view_column_pack_start (file_col, thumbnail_cell, FALSE);
		gtk_tree_view_column_pack_end (file_col, filepath_cell, TRUE);
		gtk_tree_view_column_add_attribute (file_col, thumbnail_cell, "pixbuf", 0);
		gtk_tree_view_column_add_attribute (file_col, filepath_cell, "text", 1);
		GtkTreeViewColumn *progress_col   = GTK_TREE_VIEW_COLUMN (gtk_builder_get_object (builder, "progress_col"));
		GtkCellRenderer   *progress_cell  = GTK_CELL_RENDERER    (gtk_builder_get_object (builder, "progress_cell"));
		gtk_tree_view_column_clear (progress_col);
		gtk_tree_view_column_pack_end (progress_col, progress_cell, TRUE);
		gtk_tree_view_column_add_attribute (progress_col, progress_cell, "pulse", 3);
		gtk_tree_view_column_add_attribute (progress_col, progress_cell, "text", 5);

		g_object_unref (builder);

		g_signal_connect (G_OBJECT (cancel_button),     "clicked", G_CALLBACK (uploads_cancel_cb), plugin);
		g_signal_connect (G_OBJECT (cancel_all_button), "clicked", G_CALLBACK (uploads_cancel_all_cb), plugin);
		g_signal_connect (G_OBJECT (plugin->priv->uploads_window), "delete-event", G_CALLBACK (gtk_widget_hide_on_delete), plugin);
	}

	return plugin->priv->uploads_window;
}

typedef struct {
	EomPostasaPlugin *plugin;
	GtkTreeIter iter;
} PulseData;

static gboolean
pulse (PulseData *data)
{
	gint status;
	GCancellable *cancellable;

	gtk_tree_model_get (GTK_TREE_MODEL (data->plugin->priv->uploads_store), &(data->iter), 3, &status, 4, &cancellable, -1);

	if (0 <= status && status < G_MAXINT && g_cancellable_is_cancelled (cancellable) == FALSE) {
		/* TODO: consider potential for races and how g_timeout_add works wrt threading; none seen in testing, though */
		gtk_list_store_set (data->plugin->priv->uploads_store, &(data->iter), 3, status+1, -1);
		return TRUE;
	} else {
		/* either we've failed, <0, or we're done, G_MAX_INT */
		g_slice_free (PulseData, data);
		return FALSE;
	}
}

/**
 * uploads_add_entry:
 *
 * Adds a new row to the Uploads tree view for an #EomImage to upload.
 * The row stores the upload's #GCancellable and returns.
 *
 * Returns: a #GtkTreeIter that should be freed with g_slice_free()
 **/
static GtkTreeIter *
uploads_add_entry (EomPostasaPlugin *plugin, EomImage *image, GCancellable *cancellable)
{
	GtkWindow *uploads_window;
	GdkPixbuf *thumbnail_pixbuf;
	GdkPixbuf *scaled_pixbuf;
	gchar *size, *uri;
	GtkTreeIter *iter;

	/* display the Uploads window got from the plugin */
	uploads_window = uploads_get_dialog (plugin);
	gtk_widget_show_all (GTK_WIDGET (uploads_window));

	/* obtain the data describing the upload */
	/* TODO: submit patch with documentaiton for eom_image_get_*,
	   particularly what needs unrefing */
	uri = eom_image_get_uri_for_display (image);
	thumbnail_pixbuf = eom_image_get_thumbnail (image);
	if (thumbnail_pixbuf && GDK_IS_PIXBUF (thumbnail_pixbuf)) {
		scaled_pixbuf = gdk_pixbuf_scale_simple (thumbnail_pixbuf, 32, 32, GDK_INTERP_BILINEAR);
		g_object_unref (thumbnail_pixbuf);
	} else {
		/* This is currently a workaround due to limitations in
		 * eom's thumbnailing mechanism */
		GError *error = NULL;
		GtkIconTheme *icon_theme;

		icon_theme = gtk_icon_theme_get_default ();

		scaled_pixbuf = gtk_icon_theme_load_icon (icon_theme,
							  "image-x-generic",
							  32, 0, &error);

		if (!scaled_pixbuf) {
			g_warning ("Couldn't load icon: %s", error->message);
			g_error_free (error);
		}
	}
	size = g_strdup_printf ("%luKB", eom_image_get_bytes (image) / 1024);
	iter = g_slice_new0 (GtkTreeIter);

	/* insert the data into the upload's list store */
	gtk_list_store_insert_with_values (plugin->priv->uploads_store, iter, 0,
					   0, scaled_pixbuf,
					   1, uri,
					   2, size,
					   3, 50, /* upload status: set to G_MAXINT when done, to 0 to not start */
					   4, cancellable,
					   5, _("Uploading..."),
					   -1); /* TODO: where should cancellabe, scaled_pixbuf be unref'd? don't worry about it since
						   they'll exist until EoM exits anyway? or in eom_postasa_plugin_finalize()? */
	g_free (uri);
	g_free (size);
	g_object_unref (scaled_pixbuf);

	/* Set the progress bar to pulse every 50ms; freed in pulse() when upload is no longer in progress */
	PulseData *data; /* just needs to be freed with g_slice_free() */
	data = g_slice_new0 (PulseData);
	data->plugin = plugin;
	data->iter = *iter;
	g_timeout_add (50, (GSourceFunc)pulse, data);

	return iter;
}

static void
free_picasaweb_upload_file_async_data (PicasaWebUploadFileAsyncData *data)
{
	g_object_unref (data->imgfile);
	g_slice_free (GtkTreeIter, data->iter);
	g_slice_free (PicasaWebUploadFileAsyncData, data);
}

/**
 * picasaweb_upload_async_cb:
 *
 * Handles completion of the image's asynchronous upload to PicasaWeb.
 *
 * If the #GAsyncResults indicates success, we'll update the
 * treeview's row for the given upload indicating this.  Elsewise, if
 * it wasn't cancelled, then we report an error.
 *
 * NOTE: we also clean up the #PicasaWebUploadFileAsyncData here.
 *
 * TODO: we don't yet make the progress bar throb, how do we do that?
 *
 **/
static void
picasaweb_upload_async_cb (EomPostasaPlugin *plugin, GAsyncResult *res, PicasaWebUploadFileAsyncData *data)
{
	GCancellable* cancellable;
	GError *error = NULL; /* TODO: make sure to clear all set errors */

	if (g_simple_async_result_get_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res)) == TRUE) {
		gtk_list_store_set (plugin->priv->uploads_store, data->iter, 3, G_MAXINT, 5, _("Uploaded"), -1);
	} else {
		gtk_tree_model_get (GTK_TREE_MODEL (plugin->priv->uploads_store), data->iter, 4, &cancellable, -1);
		if (g_cancellable_is_cancelled (cancellable) == TRUE) {
			gtk_list_store_set (plugin->priv->uploads_store, data->iter, 3, -1, 5, _("Cancelled"), -1);
		} else {
			g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), &error);
			gtk_list_store_set (plugin->priv->uploads_store, data->iter, 3, -1, 5, error ? error->message : _("Failed"), -1);
			g_clear_error (&error);
		}
	}

	free_picasaweb_upload_file_async_data (data);
}

/*** PicasaWeb ***/

/**
 * tmp_picasaweb_upload_async:
 *
 * Temporary solution to provide asynchronous uploading and stop
 * blocking the UI until gdata_picasaweb_service_upload_file_async()
 * becomes available (bgo #600262).  This method does the synchronous
 * file upload, but is run asynchronously via
 * g_simple_async_result_run_in_thread().
 *
 * This sets up a minimal #GDataPicasaWebFile entry, using the
 * basename of the filepath for the file's title (which is not the
 * caption, but might be something we would consider doing).  The
 * image file and the minimal entry are then uploaded to PicasaWeb's
 * default album of "Drop Box".  In the future, we might consider
 * adding an Album Chooser to the Preferences/login window, but only
 * if there's demand.
 **/
static void
tmp_picasaweb_upload_async (GSimpleAsyncResult *result, GObject *source_object, GCancellable *cancellable)
{
	GDataPicasaWebFile *new_file = NULL;
	EomPostasaPlugin *plugin = EOM_POSTASA_PLUGIN (source_object);
	GDataPicasaWebService *service = plugin->priv->service;
	GDataPicasaWebFile *file_entry;
	PicasaWebUploadFileAsyncData *data;
	GDataUploadStream *upload_stream;
	GFileInputStream *in_stream;
	GFileInfo *file_info;
	gchar *filename;
	GError *error = NULL;

	data = (PicasaWebUploadFileAsyncData*)g_async_result_get_user_data (G_ASYNC_RESULT (result));

	/* get filename to set image title */
	file_entry = gdata_picasaweb_file_new (NULL);
	filename = g_file_get_basename (data->imgfile);
	gdata_entry_set_title (GDATA_ENTRY (file_entry), filename);
	g_free (filename);

	file_info = g_file_query_info (data->imgfile,
				      G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
				      G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
				      G_FILE_QUERY_INFO_NONE, cancellable,
				      &error);

	if (file_info == NULL)
		goto got_err;

	upload_stream = gdata_picasaweb_service_upload_file (service,
				      NULL /* Upload to Dropbox */, file_entry,
				      g_file_info_get_display_name (file_info),
				      g_file_info_get_content_type (file_info),
				      cancellable, &error);
	g_object_unref (file_info);

	if (upload_stream == NULL)
		goto got_err;

	in_stream = g_file_read (data->imgfile, cancellable, &error);

	if (in_stream == NULL) {
		g_object_unref (upload_stream);
		goto got_err;
	}

	if (g_output_stream_splice (G_OUTPUT_STREAM (upload_stream),
				    G_INPUT_STREAM (in_stream),
				    G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
				    G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
				    cancellable, &error) == -1)
	{
		g_object_unref (upload_stream);
		g_object_unref (in_stream);
		goto got_err;
	}


	new_file = gdata_picasaweb_service_finish_file_upload (service,
							       upload_stream,
							       &error);

	g_object_unref (upload_stream);
	g_object_unref (in_stream);
got_err:
	g_object_unref (file_entry);

	if (new_file == NULL || error) {
		if (g_cancellable_is_cancelled (cancellable) == FALSE) {
			g_simple_async_result_set_from_error (result, error);
			g_clear_error (&error); /* we can clear this, because set_from_error() does a g_error_copy() */
		}
	} else {
		g_simple_async_result_set_op_res_gboolean (result, TRUE);
	}

	if (new_file != NULL)
		g_object_unref (new_file);
}


/**
 * picasaweb_upload_files:
 *
 * This obtains the list of selected images in EoM (selected in the
 * thumbview), sets up asynchronous uploads through
 * tmp_picasaweb_upload_async() in their own thread and instigates
 * them.

 * This attempts to upload the selected files.  It provides a message
 * near the end indicating the number successfully uploaded, and any
 * error messages encountered along the way.
 *
 * TODO: once gdata_picasaweb_service_upload_file_async() is available
 * from libgdata, simplify this as possible.
 **/
static void
picasaweb_upload_files (EomPostasaPlugin *plugin)
{
	EomWindow *window;
	GtkWidget *thumbview;
	GList *images, *node;
	EomImage *image;
	GFile *imgfile;
	GCancellable *cancellable;
	GSimpleAsyncResult *result;
	PicasaWebUploadFileAsyncData *data;

	if (gdata_service_is_authorized (GDATA_SERVICE (plugin->priv->service)) == FALSE) {
		g_warning ("PicasaWeb could not be authenticated.  Aborting upload.");
		return;
	}

	window = plugin->priv->eom_window;
	thumbview = eom_window_get_thumb_view (window); /* do not unref */
	images = eom_thumb_view_get_selected_images (EOM_THUMB_VIEW (thumbview)); /* need to use g_list_free() */

	for (node = g_list_first (images); node != NULL; node = node->next) {
		image = (EomImage *) node->data;
		cancellable = g_cancellable_new (); /* TODO: this gets attached to the image's list row; free with row */

		imgfile = eom_image_get_file (image); /* unref this */

		data = g_slice_new0(PicasaWebUploadFileAsyncData); /* freed by picasaweb_upload_async_cb() or below */
		data->imgfile = g_file_dup (imgfile); /* unref'd in free_picasaweb_upload_file_async_data() */
		data->iter = uploads_add_entry (plugin, image, cancellable); /* freed with data */

		if (g_file_query_exists (imgfile, cancellable)) {
			/* TODO: want to replace much of this with gdata_picasaweb_service_upload_file_async when that's avail */
			result = g_simple_async_result_new (G_OBJECT (plugin), (GAsyncReadyCallback)picasaweb_upload_async_cb,
							    data, tmp_picasaweb_upload_async); /* TODO: should this be freed? where? */
			g_simple_async_result_run_in_thread (result, tmp_picasaweb_upload_async, 0, cancellable);
		} else {
			/* TODO: consider setting a proper error and passing it in the data through GSimpleAsyncResult's thread */
			gtk_list_store_set (plugin->priv->uploads_store, data->iter, 3, -1, 5, "File not found", -1);
			free_picasaweb_upload_file_async_data (data);
		}
		g_object_unref (imgfile);
	}
	g_list_free (images);
}

/**
 * picasaweb_login_async_cb:
 *
 * Handles the result of the asynchronous
 * gdata_service_authenticate_async() operation, called by our
 * picasaweb_login_cb().  Upon success, it switches "Cancel" to
 * "Close".  Regardless of the response, it re-enables the Login
 * button (which is desensitised during the login attempt).
 **/
static void
picasaweb_login_async_cb (GDataClientLoginAuthorizer *authorizer, GAsyncResult *result, EomPostasaPlugin *plugin)
{
	GError *error = NULL;
	gchar *message;
	gboolean success = FALSE;

	success = gdata_client_login_authorizer_authenticate_finish (authorizer,
								     result,
								     &error);

	gtk_widget_set_sensitive (GTK_WIDGET (plugin->priv->login_button), TRUE);
	gtk_widget_set_sensitive (GTK_WIDGET (plugin->priv->username_entry), TRUE);
	gtk_widget_set_sensitive (GTK_WIDGET (plugin->priv->password_entry), TRUE);

	if (success == FALSE || error != NULL) {
		message = g_strdup_printf (_("Login failed. %s"), error->message);
		gtk_label_set_text (plugin->priv->login_message, message);
		g_free (message);
	} else {
		gtk_label_set_text (plugin->priv->login_message, _("Logged in successully."));
		gtk_button_set_label (plugin->priv->cancel_button, _("Close"));
		login_dialog_close (plugin);
	}
}

/**
 * picasaweb_login_cb:
 *
 * Handles "clicked" for the Login button.  Attempts to use input from
 * the username and password entries to authenticate.  It does this
 * asynchronously, leaving it to @picasaweb_login_async_cb to handle
 * the result.  It disables the Login button while authenticating
 * (re-enabled when done) and ensures the Close/Cancel button says
 * Cancel (which, incidentally, will call g_cancellable_cancel on the
 * provided #GCancellable.
 **/
static void
picasaweb_login_cb (GtkWidget *login_button, gpointer _plugin)
{
	EomPostasaPlugin *plugin = EOM_POSTASA_PLUGIN (_plugin);

	gtk_button_set_label (plugin->priv->cancel_button, _("Cancel"));
	gtk_widget_set_sensitive (login_button, FALSE);
	gtk_widget_set_sensitive (GTK_WIDGET (plugin->priv->username_entry), FALSE);
	gtk_widget_set_sensitive (GTK_WIDGET (plugin->priv->password_entry), FALSE);

	/* TODO: want to handle passwords more securely */
	gtk_label_set_text (plugin->priv->login_message, _("Logging in..."));
	g_cancellable_reset (plugin->priv->login_cancellable);
	gdata_client_login_authorizer_authenticate_async (
					  plugin->priv->authorizer,
					  gtk_entry_get_text (plugin->priv->username_entry),
					  gtk_entry_get_text (plugin->priv->password_entry),
					  plugin->priv->login_cancellable, (GAsyncReadyCallback)picasaweb_login_async_cb, plugin);
}

/**
 * picasaweb_upload_cb:
 *
 * This checks that we are authenticated (popping up the login window
 * if we're not) and, if we are, moves on to upload the files.
 **/
static void
picasaweb_upload_cb (GtkAction	*action,
		     EomWindow *window)
{
	WindowData *data = g_object_get_data (G_OBJECT (window), WINDOW_DATA_KEY);
	EomPostasaPlugin *plugin = data->plugin;

	if (gdata_service_is_authorized (GDATA_SERVICE (plugin->priv->service)) == TRUE) {
		picasaweb_upload_files (plugin);
	} else {
		/* when the dialog closes, it checks if this is set to see if it should upload anything */
		plugin->priv->uploads_pending = TRUE;

		login_get_dialog (plugin);
		gtk_label_set_text (plugin->priv->login_message, _("Please log in to continue upload."));
		gtk_window_present (GTK_WINDOW (plugin->priv->login_dialog));
	}
}


/*** Login Dialog ***/

/**
 * login_dialog_close:
 *
 * Closes the login dialog, used for closing the window or cancelling
 * login.  This will also cancel any authentication in progress.  If
 * the login dialog was prompted by an upload attempt, it will resume
 * the upload attempt.
 **/
static gboolean
login_dialog_close (EomPostasaPlugin *plugin)
{
	/* abort the authentication attempt if in progress and we're cancelling */
	g_cancellable_cancel (plugin->priv->login_cancellable);
	gtk_widget_hide (GTK_WIDGET (plugin->priv->login_dialog));

	if (plugin->priv->uploads_pending == TRUE) {
		plugin->priv->uploads_pending = FALSE;
		picasaweb_upload_files (plugin);
	}

	return TRUE;
}

/**
 * login_dialog_cancel_button_cb:
 *
 * Handles clicks on the Cancel/Close button.
 **/
static gboolean
login_dialog_cancel_button_cb (GtkWidget *cancel_button, EomPostasaPlugin *plugin)
{
	return login_dialog_close (plugin);
}

/**
 * login_dialog_delete_event_cb:
 *
 * Handles other attempts to close the dialog. (e.g. window manager)
 **/
static gboolean
login_dialog_delete_event_cb (GtkWidget *widget, GdkEvent *event, gpointer *_plugin)
{
	return login_dialog_close (EOM_POSTASA_PLUGIN (_plugin));
}

/**
 * login_get_dialog:
 *
 * Retrieves the login dialog.  If it has not yet been constructed, it
 * does so.  If the user is already authenticated, it populates the
 * username and password boxes with the relevant values.
 **/
static GtkWidget *
login_get_dialog (EomPostasaPlugin *plugin)
{
	GtkBuilder *builder;
	GError *error = NULL;

	if (plugin->priv->login_dialog == NULL) {
		builder = gtk_builder_new ();
		gtk_builder_set_translation_domain (builder, GETTEXT_PACKAGE);
		gtk_builder_add_from_file (builder, GTKBUILDER_CONFIG_FILE, &error);
		if (error != NULL) {
			g_warning ("Couldn't load Postasa configuration UI file:%d:%s", error->code, error->message);
			g_error_free (error);
		}

		/* do not unref gtk_builder_get_object() returns */
		plugin->priv->username_entry = GTK_ENTRY  (gtk_builder_get_object (builder, "username_entry"));
		plugin->priv->password_entry = GTK_ENTRY  (gtk_builder_get_object (builder, "password_entry"));
		plugin->priv->login_dialog   = GTK_DIALOG (gtk_builder_get_object (builder, "postasa_login_dialog"));
		plugin->priv->cancel_button  = GTK_BUTTON (gtk_builder_get_object (builder, "cancel_button"));
		plugin->priv->login_button   = GTK_BUTTON (gtk_builder_get_object (builder, "login_button"));
		plugin->priv->login_message  = GTK_LABEL  (gtk_builder_get_object (builder, "login_message"));

		g_object_unref (builder);

		g_signal_connect (G_OBJECT (plugin->priv->login_button),  "clicked", G_CALLBACK (picasaweb_login_cb),     plugin);
		g_signal_connect (G_OBJECT (plugin->priv->cancel_button), "clicked", G_CALLBACK (login_dialog_cancel_button_cb), plugin);
		g_signal_connect (G_OBJECT (plugin->priv->login_dialog), "delete-event", G_CALLBACK (login_dialog_delete_event_cb), plugin);

		if (gdata_service_is_authorized (GDATA_SERVICE (plugin->priv->service))) {
			gtk_entry_set_text (plugin->priv->username_entry, gdata_client_login_authorizer_get_username (plugin->priv->authorizer));
			gtk_entry_set_text (plugin->priv->password_entry, gdata_client_login_authorizer_get_password (plugin->priv->authorizer));
		}
	}

	return GTK_WIDGET (plugin->priv->login_dialog);
}


/*** EomPlugin Functions ***/

/**
 * free_window_data:
 *
 * This handles destruction of the #WindowData we define for this plugin.
 **/
static void
free_window_data (WindowData *data)
{
	eom_debug (DEBUG_PLUGINS);

	g_return_if_fail (data != NULL);
	g_object_unref (data->ui_action_group);
	g_free (data);
}

/**
 * impl_create_config_dialog:
 *
 * Plugin hook for obtaining the configure/preferences dialog.  In
 * this case, it's just our Login dialog.  In the future, it might
 * include an album chooser, if there's demand.
 **/
static GtkWidget *
impl_create_config_dialog (EomPlugin *_plugin)
{
	EomPostasaPlugin *plugin = EOM_POSTASA_PLUGIN (_plugin);
	return GTK_WIDGET (login_get_dialog (plugin));
}

/**
 * impl_activate:
 *
 * Plugin hook for plugin activation.  Creates #WindowData for the
 * #EomPostasaPlugin that gets associated with the window and defines
 * some UI.
 **/
static void
impl_activate (EomPlugin *_plugin,
	       EomWindow *window)
{
	GtkUIManager *manager;
	WindowData *data;
	EomPostasaPlugin *plugin = EOM_POSTASA_PLUGIN (_plugin);

	eom_debug (DEBUG_PLUGINS);

	plugin->priv->eom_window = window;

	data = g_new (WindowData, 1); /* free'd by free_window_data() when window object is destroyed */
	data->plugin = EOM_POSTASA_PLUGIN (plugin); /* circular references, fun */
	data->ui_action_group = gtk_action_group_new ("EomPostasaPluginActions"); /* freed with WindowData in free_window_data() */
	gtk_action_group_set_translation_domain (data->ui_action_group, GETTEXT_PACKAGE);
	gtk_action_group_add_actions (data->ui_action_group, action_entries, G_N_ELEMENTS (action_entries), window);

	manager = eom_window_get_ui_manager (window); /* do not unref */
	gtk_ui_manager_insert_action_group (manager, data->ui_action_group, -1);
	data->ui_id = gtk_ui_manager_add_ui_from_string (manager,
							 ui_definition,
							 -1, NULL);
	g_warn_if_fail (data->ui_id != 0);

	g_object_set_data_full (G_OBJECT (window), WINDOW_DATA_KEY, data, (GDestroyNotify) free_window_data);
}

/**
 * impl_deactivate:
 *
 * Plugin hook for plugin deactivation. Removes UI and #WindowData
 **/
static void
impl_deactivate	(EomPlugin *plugin,
		 EomWindow *window)
{
	GtkUIManager *manager;
	WindowData *data;

	eom_debug (DEBUG_PLUGINS);

	manager = eom_window_get_ui_manager (window);

	data = (WindowData *) g_object_get_data (G_OBJECT (window), WINDOW_DATA_KEY);
	g_return_if_fail (data != NULL);

	gtk_ui_manager_remove_ui (manager, data->ui_id);
	gtk_ui_manager_remove_action_group (manager, data->ui_action_group);

	g_object_set_data (G_OBJECT (window), WINDOW_DATA_KEY, NULL); /* TODO: does doing this interfere with the GDestroyNotify
									 set in impl_activate()?  It's how it was done in Postr... */
}

/**
 * impl_update_ui:
 *
 * Plugin hook for updating the UI.  I don't think we do anything of use in here.
 **/
static void
impl_update_ui (EomPlugin *plugin,
		EomWindow *window)
{
}


/*** GObject Functions ***/

/**
 * eom_postasa_plugin_init:
 *
 * Object initialisation method.  Sets up the (unauthenticated)
 * PicasaWeb service, a #GCancellable for login, and sets the
 * uploads_pending flag to %FALSE.
 **/
static void
eom_postasa_plugin_init (EomPostasaPlugin *plugin)
{
	eom_debug_message (DEBUG_PLUGINS, "EomPostasaPlugin initializing");

	plugin->priv = G_TYPE_INSTANCE_GET_PRIVATE (plugin, EOM_TYPE_POSTASA_PLUGIN, EomPostasaPluginPrivate);
	plugin->priv->authorizer = gdata_client_login_authorizer_new ("EomPostasa", GDATA_TYPE_PICASAWEB_SERVICE);
	plugin->priv->service = gdata_picasaweb_service_new (GDATA_AUTHORIZER (plugin->priv->authorizer));
	plugin->priv->login_cancellable = g_cancellable_new (); /* unref'd in eom_postasa_plugin_finalize() */
	plugin->priv->uploads_pending = FALSE;
}

/**
 * eom_postasa_plugin_finalize:
 *
 * Cleans up the #EomPostasaPlugin object, unref'ing its #GDataPicasaWebService and #GCancellable.
 **/
static void
eom_postasa_plugin_finalize (GObject *_plugin)
{
	EomPostasaPlugin *plugin = EOM_POSTASA_PLUGIN (_plugin);

	eom_debug_message (DEBUG_PLUGINS, "EomPostasaPlugin finalizing");

	g_object_unref (plugin->priv->authorizer);
	g_object_unref (plugin->priv->service);
	g_object_unref (plugin->priv->login_cancellable);
	if (G_IS_OBJECT (plugin->priv->uploads_store))
		/* we check in case the upload window was never created */
		g_object_unref (plugin->priv->uploads_store);

	G_OBJECT_CLASS (eom_postasa_plugin_parent_class)->finalize (_plugin);
}

/**
 * eom_postasa_plugin_class_init:
 *
 * Plugin class initialisation.  Binds class hooks to actual implementations.
 **/
static void
eom_postasa_plugin_class_init (EomPostasaPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	EomPluginClass *plugin_class = EOM_PLUGIN_CLASS (klass);

	g_type_class_add_private (klass, sizeof (EomPostasaPluginPrivate));

	object_class->finalize = eom_postasa_plugin_finalize;

	plugin_class->activate = impl_activate;
	plugin_class->deactivate = impl_deactivate;
	plugin_class->update_ui = impl_update_ui;
	plugin_class->create_configure_dialog = impl_create_config_dialog;
}

/*
 * Copyright (c) 2014-2016 gnome-mpv
 *
 * This file is part of GNOME MPV.
 *
 * GNOME MPV is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNOME MPV is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNOME MPV.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

#include "mpv_obj.h"
#include "application.h"
#include "common.h"
#include "def.h"
#include "track.h"
#include "playlist.h"
#include "control_box.h"
#include "playlist_widget.h"

enum
{
	PROP_0,
	PROP_USE_OPENGL,
	PROP_WID,
	PROP_PLAYLIST,
	N_PROPERTIES
};

struct _MpvObjPrivate
{
	gboolean use_opengl;
	gint64 wid;
};

static void mpv_obj_set_inst_property(	GObject *object,
					guint property_id,
					const GValue *value,
					GParamSpec *pspec );
static void mpv_obj_get_inst_property(	GObject *object,
					guint property_id,
					GValue *value,
					GParamSpec *pspec );
static void parse_dim_string(	const gchar *mpv_geom_str,
				gint *width,
				gint *height );
static void handle_autofit_opt(MpvObj *mpv);
static void handle_msg_level_opt(MpvObj *mpv);
static void handle_property_change_event(MpvObj *mpv, mpv_event_property* prop);
static void mpv_obj_update_playlist(MpvObj *mpv);
static gint mpv_obj_apply_args(mpv_handle *mpv_ctx, gchar *args);
static void mpv_obj_log_handler(MpvObj *mpv, mpv_event_log_message* message);
static gboolean mpv_obj_handle_event(gpointer data);
static void opengl_callback(void *cb_ctx);
static void uninit_opengl_cb(Application *app);

G_DEFINE_TYPE_WITH_PRIVATE(MpvObj, mpv_obj, G_TYPE_OBJECT)

static void mpv_obj_set_inst_property(	GObject *object,
					guint property_id,
					const GValue *value,
					GParamSpec *pspec )
{	MpvObj *self = MPV_OBJ(object);

	if(property_id == PROP_USE_OPENGL)
	{
		self->priv->use_opengl = g_value_get_boolean(value);
	}
	else if(property_id == PROP_WID)
	{
		self->priv->wid = g_value_get_int64(value);
	}
	else if(property_id == PROP_PLAYLIST)
	{
		self->playlist = g_value_get_pointer(value);
	}
	else
	{
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void mpv_obj_get_inst_property(	GObject *object,
					guint property_id,
					GValue *value,
					GParamSpec *pspec )
{	MpvObj *self = MPV_OBJ(object);

	if(property_id == PROP_USE_OPENGL)
	{
		g_value_set_boolean(value, self->priv->use_opengl);
	}
	else if(property_id == PROP_WID)
	{
		g_value_set_int64(value, self->priv->wid);
	}
	else if(property_id == PROP_PLAYLIST)
	{
		g_value_set_pointer(value, self->playlist);
	}
	else
	{
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void parse_dim_string(	const gchar *mpv_geom_str,
				gint *width,
				gint *height )
{
	GdkScreen *screen = NULL;
	gint screen_width = -1;
	gint screen_height = -1;
	gchar **tokens = NULL;
	gint i = -1;

	screen = gdk_screen_get_default();
	screen_width = gdk_screen_get_width(screen);
	screen_height = gdk_screen_get_height(screen);
	tokens = g_strsplit(mpv_geom_str, "x", 2);

	*width = 0;
	*height = 0;

	while(tokens && tokens[++i] && i < 4)
	{
		gdouble multiplier = -1;
		gint value = -1;

		value = (gint)g_ascii_strtoll(tokens[i], NULL, 0);

		if(tokens[i][strnlen(tokens[i], 256)-1] == '%')
		{
			multiplier = value/100.0;
		}

		if(i == 0)
		{
			if(multiplier == -1)
			{
				*width = value;
			}
			else
			{
				*width = (gint)(multiplier*screen_width);
			}
		}
		else if(i == 1)
		{
			if(multiplier == -1)
			{
				*height = value;
			}
			else
			{
				*height = (gint)(multiplier*screen_height);
			}
		}
	}

	if(*width != 0 && *height == 0)
	{
		/* If height is not specified, set it to screen height. This
		 * should correctly emulate vanilla mpv's behavior since we
		 * always preserve aspect ratio when autofitting.
		 */
		*height = screen_height;
	}

	g_strfreev(tokens);
}

static void handle_autofit_opt(MpvObj *mpv)
{
	gchar *optbuf = NULL;
	gchar *geom = NULL;

	optbuf = mpv_get_property_string(mpv->mpv_ctx, "options/autofit");

	if(optbuf && strlen(optbuf) > 0)
	{
		gint autofit_width = 0;
		gint autofit_height = 0;
		gint64 vid_width = 0;
		gint64 vid_height = 0;
		gdouble width_ratio = -1;
		gdouble height_ratio = -1;
		gint rc = 0;

		rc |= mpv_get_property(	mpv->mpv_ctx,
					"dwidth",
					MPV_FORMAT_INT64,
					&vid_width );

		rc |= mpv_get_property(	mpv->mpv_ctx,
					"dheight",
					MPV_FORMAT_INT64,
					&vid_height );

		if(rc >= 0)
		{
			parse_dim_string(	optbuf,
						&autofit_width,
						&autofit_height );

			width_ratio =	(gdouble)autofit_width/
					(gdouble)vid_width;

			height_ratio =	(gdouble)autofit_height/
					(gdouble)vid_height;
		}

		if(rc >= 0 && width_ratio > 0 && height_ratio > 0)
		{
			if(width_ratio > 1 && height_ratio > 1)
			{
				mpv->autofit_ratio = 1;
			}
			else
			{
				/* Resize the window so that it is as big as
				 * possible within the limits imposed by
				 * 'autofit' while preseving the aspect ratio.
				 */
				mpv->autofit_ratio
					= (width_ratio < height_ratio)
					? width_ratio:height_ratio;
			}
		}
	}

	mpv_free(optbuf);
	g_free(geom);
}

static void handle_msg_level_opt(MpvObj *mpv)
{
	const struct
	{
		gchar *name;
		mpv_log_level level;
	}
	level_map[] = {	{"no", MPV_LOG_LEVEL_NONE},
			{"fatal", MPV_LOG_LEVEL_FATAL},
			{"error", MPV_LOG_LEVEL_ERROR},
			{"warn", MPV_LOG_LEVEL_WARN},
			{"info", MPV_LOG_LEVEL_INFO},
			{"v", MPV_LOG_LEVEL_V},
			{"debug", MPV_LOG_LEVEL_DEBUG},
			{"trace", MPV_LOG_LEVEL_TRACE},
			{NULL, MPV_LOG_LEVEL_NONE} };

	gchar *optbuf = NULL;
	gchar **tokens = NULL;
	mpv_log_level min_level = DEFAULT_LOG_LEVEL;
	gint i;

	optbuf = mpv_get_property_string(mpv->mpv_ctx, "options/msg-level");

	if(optbuf)
	{
		tokens = g_strsplit(optbuf, ",", 0);
	}

	if(mpv->log_level_list)
	{
		g_slist_free_full(mpv->log_level_list, g_free);

		mpv->log_level_list = NULL;
	}

	for(i = 0; tokens && tokens[i]; i++)
	{
		gchar **pair = g_strsplit(tokens[i], "=", 2);
		module_log_level *level = g_malloc(sizeof(module_log_level));
		gboolean found = FALSE;
		gint j;

		level->prefix = g_strdup(pair[0]);

		for(j = 0; level_map[j].name && !found; j++)
		{
			if(g_strcmp0(pair[1], level_map[j].name) == 0)
			{
				level->level = level_map[j].level;
				found = TRUE;
			}
		}


		/* Ignore if the given level is invalid */
		if(found)
		{
			/* Lower log levels have higher values */
			if(level->level > min_level)
			{
				min_level = level->level;
			}

			if(g_strcmp0(level->prefix, "all") != 0)
			{
				mpv->log_level_list
					= g_slist_append
						(mpv->log_level_list, level);
			}
		}

		g_strfreev(pair);
	}

	for(i = 0; level_map[i].level != min_level; i++);

	mpv_check_error
		(mpv_request_log_messages(mpv->mpv_ctx, level_map[i].name));

	mpv_free(optbuf);
	g_strfreev(tokens);
}

static void handle_property_change_event(MpvObj *mpv, mpv_event_property* prop)
{
	if(g_strcmp0(prop->name, "pause") == 0)
	{
		gboolean idle;

		mpv->state.paused = prop->data?*((int *)prop->data):TRUE;

		mpv_get_property(mpv->mpv_ctx, "idle", MPV_FORMAT_FLAG, &idle);

		if(idle && !mpv->state.paused)
		{
			mpv_obj_load(mpv, NULL, FALSE, TRUE);
		}
	}
	else if(g_strcmp0(prop->name, "eof-reached") == 0
	&& prop->data
	&& *((int *)prop->data) == 1)
	{
		mpv->state.paused = TRUE;
		mpv->state.loaded = FALSE;

		playlist_reset(mpv->playlist);
	}
}

static void mpv_obj_update_playlist(MpvObj *mpv)
{
	/* The length of "playlist//filename" including null-terminator (19)
	 * plus the number of digits in the maximum value of 64 bit int (19).
	 */
	const gsize filename_prop_str_size = 38;
	GtkListStore *store;
	GtkTreeIter iter;
	mpv_node mpv_playlist;
	gchar *filename_prop_str;
	gboolean iter_end;
	gint playlist_count;
	gint i;

	store = playlist_get_store(mpv->playlist);
	filename_prop_str = g_malloc(filename_prop_str_size);
	iter_end = FALSE;

	mpv_check_error(mpv_get_property(	mpv->mpv_ctx,
						"playlist",
						MPV_FORMAT_NODE,
						&mpv_playlist ));

	playlist_count = mpv_playlist.u.list->num;

	gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);

	for(i = 0; i < playlist_count; i++)
	{
		gint prop_count = 0;
		gchar *uri = NULL;
		gchar *title = NULL;
		gchar *name = NULL;
		gchar *old_name = NULL;
		gchar *old_uri = NULL;

		prop_count = mpv_playlist.u.list->values[i].u.list->num;

		/* The first entry must always exist */
		uri =	mpv_playlist.u.list
			->values[i].u.list
			->values[0].u.string;

		/* Try retrieving the title from mpv playlist */
		if(prop_count >= 4)
		{
			title = mpv_playlist.u.list
				->values[i].u.list
				->values[3].u.string;
		}

		name = title?title:get_name_from_path(uri);

		if(!iter_end)
		{
			gtk_tree_model_get
				(	GTK_TREE_MODEL(store), &iter,
					PLAYLIST_NAME_COLUMN, &old_name,
					PLAYLIST_URI_COLUMN, &old_uri, -1 );

			if(g_strcmp0(name, old_name) != 0)
			{
				gtk_list_store_set
					(	store, &iter,
						PLAYLIST_NAME_COLUMN, name, -1 );
			}

			if(g_strcmp0(uri, old_uri) != 0)
			{
				gtk_list_store_set
					(	store, &iter,
						PLAYLIST_URI_COLUMN, uri, -1 );
			}

			iter_end = !gtk_tree_model_iter_next
					(GTK_TREE_MODEL(store), &iter);

			g_free(old_name);
			g_free(old_uri);
		}
		/* Append entries to the playlist if there are fewer entries in
		 * the playlist widget than mpv's playlist.
		 */
		else
		{
			playlist_append(mpv->playlist, name, uri);
		}

		mpv_free(uri);
		g_free(name);
	}

	/* If there are more entries in the playlist widget than mpv's playlist,
	 * remove the excess entries from the playlist widget.
	 */
	if(!iter_end)
	{
		while(gtk_list_store_remove(store, &iter));
	}

	g_free(filename_prop_str);
	mpv_free_node_contents(&mpv_playlist);
}

static gint mpv_obj_apply_args(mpv_handle *mpv_ctx, gchar *args)
{
	gchar *opt_begin = args?strstr(args, "--"):NULL;
	gint fail_count = 0;

	while(opt_begin)
	{
		gchar *opt_end = strstr(opt_begin, " --");
		gchar *token;
		gchar *token_arg;
		gsize token_size;

		/* Point opt_end to the end of the input string if the current
		 * option is the last one.
		 */
		if(!opt_end)
		{
			opt_end = args+strlen(args);
		}

		/* Traverse the string backwards until non-space character is
		 * found. This removes spaces after the option token.
		 */
		while(	--opt_end != opt_begin
			&& (*opt_end == ' ' || *opt_end == '\n') );

		token_size = (gsize)(opt_end-opt_begin);
		token = g_strndup(opt_begin+2, token_size-1);
		token_arg = strpbrk(token, "= ");

		if(token_arg)
		{
			*token_arg = '\0';

			token_arg++;
		}
		else
		{
			/* Default to empty string if there is no explicit
			 * argument
			 */
			token_arg = "";
		}

		g_debug("Applying option --%s=%s", token, token_arg);

		if(mpv_set_option_string(mpv_ctx, token, token_arg) < 0)
		{
			fail_count++;

			g_warning(	"Failed to apply option: --%s=%s\n",
					token,
					token_arg );
		}

		opt_begin = strstr(opt_end, " --");

		if(opt_begin)
		{
			opt_begin++;
		}

		g_free(token);
	}

	return fail_count*(-1);
}

static gboolean mpv_obj_handle_event(gpointer data)
{
	Application *app = data;
	MpvObj *mpv = app->mpv;
	mpv_event *event = NULL;
	gboolean done = FALSE;

	while(!done)
	{
		event = mpv->mpv_ctx?mpv_wait_event(mpv->mpv_ctx, 0):NULL;

		if(!event)
		{
			done = TRUE;
		}
		else if(event->event_id == MPV_EVENT_PROPERTY_CHANGE)
		{
			mpv_event_property *prop = event->data;

			handle_property_change_event(mpv, prop);

			g_signal_emit_by_name(	mpv,
						"mpv-prop-change",
						g_strdup(prop->name) );
		}
		else if(event->event_id == MPV_EVENT_IDLE)
		{
			if(mpv->state.init_load)
			{
				mpv_obj_load(mpv, NULL, FALSE, FALSE);
			}
			else if(mpv->state.loaded)
			{
				gint rc;

				mpv->state.paused = TRUE;
				mpv->state.loaded = FALSE;

				rc = mpv_set_property(	mpv->mpv_ctx,
							"pause",
							MPV_FORMAT_FLAG,
							&mpv->state.paused );

				mpv_check_error(rc);
				playlist_reset(mpv->playlist);
			}

			mpv->state.init_load = FALSE;
		}
		else if(event->event_id == MPV_EVENT_FILE_LOADED)
		{
			mpv->state.loaded = TRUE;
			mpv->state.init_load = FALSE;

			mpv_obj_update_playlist(mpv);
		}
		else if(event->event_id == MPV_EVENT_END_FILE)
		{
			mpv_event_end_file *ef_event = event->data;

			mpv->state.init_load = FALSE;

			if(mpv->state.loaded)
			{
				mpv->state.new_file = FALSE;
			}

			if(ef_event->reason == MPV_END_FILE_REASON_ERROR)
			{
				const gchar *err;
				gchar *msg;

				err = mpv_error_string(ef_event->error);

				msg = g_strdup_printf
					(	_("Playback was terminated "
						"abnormally. Reason: %s."),
						err );

				mpv->state.paused = TRUE;

				mpv_set_property(	mpv->mpv_ctx,
							"pause",
							MPV_FORMAT_FLAG,
							&mpv->state.paused );

				g_signal_emit_by_name(mpv, "mpv-error", msg);
			}
		}
		else if(event->event_id == MPV_EVENT_VIDEO_RECONFIG)
		{
			if(mpv->state.new_file)
			{
				handle_autofit_opt(mpv);
			}
		}
		else if(event->event_id == MPV_EVENT_PLAYBACK_RESTART)
		{
			g_signal_emit_by_name(	mpv,
						"mpv-playback-restart" );
		}
		else if(event->event_id == MPV_EVENT_LOG_MESSAGE)
		{
			mpv_obj_log_handler(mpv, event->data);
		}
		else if(event->event_id == MPV_EVENT_SHUTDOWN
		|| event->event_id == MPV_EVENT_NONE)
		{
			done = TRUE;
		}

		if(event)
		{
			if(mpv->mpv_event_handler)
			{
				mpv->mpv_event_handler(event, data);
			}

			g_signal_emit_by_name
				(mpv, "mpv-event", event->event_id);
		}
	}

	return FALSE;
}

static void mpv_obj_log_handler(MpvObj *mpv, mpv_event_log_message* message)
{
	GSList *iter = mpv->log_level_list;
	module_log_level *level = NULL;
	gsize event_prefix_len = strlen(message->prefix);
	gboolean found = FALSE;

	if(iter)
	{
		level = iter->data;
	}

	while(iter && !found)
	{
		gsize prefix_len;
		gint cmp;

		prefix_len = strlen(level->prefix);

		cmp = strncmp(	level->prefix,
				message->prefix,
				(event_prefix_len < prefix_len)?
				event_prefix_len:prefix_len );

		/* Allow both exact match and prefix match */
		if(cmp == 0
		&& (prefix_len == event_prefix_len
		|| (prefix_len < event_prefix_len
		&& message->prefix[prefix_len] == '/')))
		{
			found = TRUE;
		}
		else
		{
			iter = g_slist_next(iter);
			level = iter?iter->data:NULL;
		}
	}

	if(!iter || (message->log_level <= level->level))
	{
		gchar *buf = g_strdup(message->text);
		gsize len = strlen(buf);

		if(len > 1)
		{
			/* g_message() automatically adds a newline
			 * character when using the default log handler,
			 * but log messages from mpv already come
			 * terminated with a newline character so we
			 * need to take it out.
			 */
			if(buf[len-1] == '\n')
			{
				buf[len-1] = '\0';
			}

			g_message("[%s] %s", message->prefix, buf);
		}

		g_free(buf);
	}
}

static void opengl_callback(void *cb_ctx)
{
#ifdef OPENGL_CB_ENABLED
	Application *app = cb_ctx;

	if(app->mpv->opengl_ctx)
	{
		gtk_gl_area_queue_render(GTK_GL_AREA(app->gui->vid_area));
	}
#endif
}

static void uninit_opengl_cb(Application *app)
{
#ifdef OPENGL_CB_ENABLED
	gtk_gl_area_make_current(GTK_GL_AREA(app->gui->vid_area));
	mpv_opengl_cb_uninit_gl(app->mpv->opengl_ctx);
#endif
}

static void mpv_obj_class_init(MpvObjClass* klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec = NULL;

	obj_class->set_property = mpv_obj_set_inst_property;
	obj_class->get_property = mpv_obj_get_inst_property;

	pspec = g_param_spec_boolean
		(	"use-opengl",
			"Use OpenGL",
			"Whether or not to use opengl-cb",
			FALSE,
			G_PARAM_CONSTRUCT_ONLY|G_PARAM_READWRITE );

	g_object_class_install_property(obj_class, PROP_USE_OPENGL, pspec);

	pspec = g_param_spec_int64
		(	"wid",
			"WID",
			"The ID of the window to attach to",
			G_MININT64,
			G_MAXINT64,
			-1,
			G_PARAM_CONSTRUCT_ONLY|G_PARAM_READWRITE );

	g_object_class_install_property(obj_class, PROP_WID, pspec);

	pspec = g_param_spec_pointer
		(	"playlist",
			"Playlist",
			"Playlist object to use for storage",
			G_PARAM_CONSTRUCT_ONLY|G_PARAM_READWRITE );

	g_object_class_install_property(obj_class, PROP_PLAYLIST, pspec);

	g_signal_new(	"mpv-init",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__VOID,
			G_TYPE_NONE,
			0 );

	g_signal_new(	"mpv-error",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__STRING,
			G_TYPE_NONE,
			1,
			G_TYPE_STRING );

	g_signal_new(	"mpv-playback-restart",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__VOID,
			G_TYPE_NONE,
			0 );

	g_signal_new(	"mpv-event",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__ENUM,
			G_TYPE_NONE,
			1,
			G_TYPE_INT );

	g_signal_new(	"mpv-prop-change",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__STRING,
			G_TYPE_NONE,
			1,
			G_TYPE_STRING );
}

static void mpv_obj_init(MpvObj *mpv)
{
	mpv->priv = mpv_obj_get_instance_private(mpv);
	mpv->mpv_ctx = mpv_create();
	mpv->opengl_ctx = NULL;
	mpv->playlist = NULL;
	mpv->log_level_list = NULL;
	mpv->autofit_ratio = 1;
	mpv->mpv_event_handler = NULL;
}

MpvObj *mpv_obj_new(gboolean use_opengl, gint64 wid, Playlist *playlist)
{
	return MPV_OBJ(g_object_new(	mpv_obj_get_type(),
					"use-opengl", use_opengl,
					"wid", wid,
					"playlist", playlist,
					NULL));
}

inline gint mpv_obj_command(MpvObj *mpv, const gchar **cmd)
{
	return mpv_command(mpv->mpv_ctx, cmd);
}

inline gint mpv_obj_command_string(MpvObj *mpv, const gchar *cmd)
{
	return mpv_command_string(mpv->mpv_ctx, cmd);
}

inline gint mpv_obj_set_property(	MpvObj *mpv,
					const gchar *name,
					mpv_format format,
					void *data )
{
	return mpv_set_property(mpv->mpv_ctx, name, format, data);
}

inline gint mpv_obj_set_property_string(	MpvObj *mpv,
						const gchar *name,
						const char *data )
{
	return mpv_set_property_string(mpv->mpv_ctx, name, data);
}

void mpv_obj_wakeup_callback(void *data)
{
	g_idle_add((GSourceFunc)mpv_obj_handle_event, data);
}

void mpv_check_error(int status)
{
	void *array[10];
	size_t size;

	if(status < 0)
	{
		size = (size_t)backtrace(array, 10);

		g_critical("MPV API error: %s\n", mpv_error_string(status));

		backtrace_symbols_fd(array, (int)size, STDERR_FILENO);

		exit(EXIT_FAILURE);
	}
}

void mpv_obj_initialize(Application *app)
{
	GSettings *main_settings = g_settings_new(CONFIG_ROOT);
	GSettings *win_settings = g_settings_new(CONFIG_WIN_STATE);
	gdouble volume = g_settings_get_double(win_settings, "volume")*100;
	gchar *config_dir = get_config_dir_path();
	gchar *mpvopt = NULL;
	MpvObj *mpv = app->mpv;

	const struct
	{
		const gchar *name;
		const gchar *value;
	}
	options[] = {	{"osd-level", "1"},
			{"softvol", "yes"},
			{"force-window", "yes"},
			{"audio-client-name", g_get_application_name()},
			{"title", "${media-title}"},
			{"pause", "yes"},
			{"ytdl", "yes"},
			{"input-cursor", "no"},
			{"cursor-autohide", "no"},
			{"softvol-max", "100"},
			{"config", "yes"},
			{"screenshot-template", "gnome-mpv-shot%n"},
			{"config-dir", config_dir},
			{NULL, NULL} };

	for(gint i = 0; options[i].name; i++)
	{
		g_debug(	"Applying default option --%s=%s",
				options[i].name,
				options[i].value );

		mpv_set_option_string(	mpv->mpv_ctx,
					options[i].name,
					options[i].value );
	}

	g_debug("Setting volume to %f", volume);
	mpv_set_option(mpv->mpv_ctx, "volume", MPV_FORMAT_DOUBLE, &volume);

	if(g_settings_get_boolean(main_settings, "mpv-config-enable"))
	{
		gchar *mpv_conf = g_settings_get_string
					(main_settings, "mpv-config-file");

		g_info("Loading config file: %s", mpv_conf);
		mpv_load_config_file(mpv->mpv_ctx, mpv_conf);

		g_free(mpv_conf);
	}

	mpvopt = g_settings_get_string(main_settings, "mpv-options");

	g_debug("Applying extra mpv options: %s", mpvopt);

	/* Apply extra options */
	if(mpv_obj_apply_args(mpv->mpv_ctx, mpvopt) < 0)
	{
		const gchar *msg
			= _("Failed to apply one or more MPV options.");

		g_signal_emit_by_name(mpv, "mpv-error", g_strdup(msg));
	}

	if(mpv->priv->use_opengl)
	{
		g_info("opengl-cb is enabled; forcing --vo=opengl-cb");
		mpv_set_option_string(mpv->mpv_ctx, "vo", "opengl-cb");

	}
	else
	{
		g_debug(	"Attaching mpv window to wid %#x",
				(guint)mpv->priv->wid );

		mpv_set_option(	mpv->mpv_ctx,
				"wid",
				MPV_FORMAT_INT64,
				&mpv->priv->wid );
	}

	mpv_observe_property(mpv->mpv_ctx, 0, "aid", MPV_FORMAT_INT64);
	mpv_observe_property(mpv->mpv_ctx, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv->mpv_ctx, 0, "eof-reached", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv->mpv_ctx, 0, "fullscreen", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv->mpv_ctx, 0, "track-list", MPV_FORMAT_NODE);
	mpv_observe_property(mpv->mpv_ctx, 0, "volume", MPV_FORMAT_DOUBLE);
	mpv_check_error(mpv_initialize(mpv->mpv_ctx));

	mpv->opengl_ctx = mpv_get_sub_api(mpv->mpv_ctx, MPV_SUB_API_OPENGL_CB);

	mpv_opengl_cb_set_update_callback(	mpv->opengl_ctx,
						opengl_callback,
						app );

	handle_msg_level_opt(mpv);
	g_signal_emit_by_name(mpv, "mpv-init");

	g_clear_object(&main_settings);
	g_clear_object(&win_settings);
	g_free(config_dir);
	g_free(mpvopt);
}

void mpv_obj_quit(Application *app)
{
	g_info("Terminating mpv");

	if(gtk_widget_get_realized(app->gui->vid_area)
	&& main_window_get_use_opengl(app->gui))
	{
		g_debug("Uninitializing opengl-cb");
		uninit_opengl_cb(app);

		app->opengl_ready = FALSE;
	}

	mpv_terminate_destroy(app->mpv->mpv_ctx);
}

void mpv_obj_load(	MpvObj *mpv,
			const gchar *uri,
			gboolean append,
			gboolean update )
{
	const gchar *load_cmd[] = {"loadfile", NULL, NULL, NULL};
	GtkListStore *playlist_store;
	GtkTreeIter iter;
	gboolean empty;

	g_info(	"Loading file (append=%s, update=%s): %s",
		append?"TRUE":"FALSE",
		update?"TRUE":"FALSE",
		uri?:"<PLAYLIST_ITEMS>" );

	playlist_store = playlist_get_store(mpv->playlist);

	empty = !gtk_tree_model_get_iter_first
			(GTK_TREE_MODEL(playlist_store), &iter);

	load_cmd[2] = (append && !empty)?"append":"replace";

	if(!append && uri && update)
	{
		playlist_clear(mpv->playlist);

		mpv->state.new_file = TRUE;
		mpv->state.loaded = FALSE;
	}

	if(!uri)
	{
		gboolean rc;
		gboolean append;

		rc = gtk_tree_model_get_iter_first
			(GTK_TREE_MODEL(playlist_store), &iter);

		append = FALSE;

		while(rc)
		{
			gchar *uri;

			gtk_tree_model_get(	GTK_TREE_MODEL(playlist_store),
						&iter,
						PLAYLIST_URI_COLUMN,
						&uri,
						-1 );

			/* append = FALSE only on first iteration */
			mpv_obj_load(mpv, uri, append, FALSE);

			append = TRUE;

			rc = gtk_tree_model_iter_next
				(GTK_TREE_MODEL(playlist_store), &iter);

			g_free(uri);
		}
	}

	if(uri && playlist_store)
	{
		gchar *path = get_path_from_uri(uri);

		load_cmd[1] = path;

		if(!append)
		{
			mpv->state.loaded = FALSE;
		}

		if(update)
		{
			gchar *name = get_name_from_path(path);

			playlist_append(mpv->playlist, name, uri);

			g_free(name);
		}

		mpv_check_error(mpv_request_event(	mpv->mpv_ctx,
							MPV_EVENT_END_FILE,
							0 ));

		mpv_check_error(mpv_command(mpv->mpv_ctx, load_cmd));

		mpv_check_error(mpv_set_property(	mpv->mpv_ctx,
							"pause",
							MPV_FORMAT_FLAG,
							&mpv->state.paused ));

		mpv_check_error(mpv_request_event(	mpv->mpv_ctx,
							MPV_EVENT_END_FILE,
							1 ));

		g_free(path);
	}
}
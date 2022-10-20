/* gst-pipewire
 *
 * Copyright © 2022 Carlos Rafael Giani
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <spa/utils/result.h>
#include "gstpipewirecore.h"

#pragma GCC diagnostic pop

GST_DEBUG_CATEGORY(pw_core_debug);
#define GST_CAT_DEFAULT pw_core_debug


G_DEFINE_TYPE(GstPipewireCore, gst_pipewire_core, GST_TYPE_OBJECT)


static void gst_pipewire_core_dispose(GObject *object);

static void gst_pipewire_core_sync_pw_core(GstPipewireCore *self);

static void gst_pipewire_core_on_core_done(void *object, uint32_t id, int sequence_number);
static void gst_pipewire_core_on_core_error(void *object, uint32_t id, int sequence_number, int res, const char *message);

static void gst_pipewire_core_shutdown(GstPipewireCore *self);


#define LOCK_CORE_LIST() g_mutex_lock(&core_list_mutex)
#define UNLOCK_CORE_LIST() g_mutex_unlock(&core_list_mutex)

static GMutex core_list_mutex;
static GList *core_list = NULL;


static const struct pw_core_events core_events =
{
	PW_VERSION_CORE_EVENTS,
	.done = gst_pipewire_core_on_core_done,
	.error = gst_pipewire_core_on_core_error,
};


static void gst_pipewire_core_class_init(GstPipewireCoreClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = GST_DEBUG_FUNCPTR(gst_pipewire_core_dispose);

	GST_DEBUG_CATEGORY_INIT(pw_core_debug, "pwcore", 0, "GStreamer PipeWire core");
}


static void gst_pipewire_core_init(GstPipewireCore *self)
{
	self->loop = NULL;
	self->context = NULL;
	self->core = NULL;
	self->core_done_seq_number = -1;
	self->last_error = 0;
	self->pending_seq_number = 0;
}


static void gst_pipewire_core_dispose(GObject *object)
{
	GstPipewireCore *self = GST_PIPEWIRE_CORE(object);
	gst_pipewire_core_shutdown(self);
	G_OBJECT_CLASS(gst_pipewire_core_parent_class)->dispose(object);
}


static void gst_pipewire_core_sync_pw_core(GstPipewireCore *self)
{
	/* Must be called with the pw_thread_loop lock held */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	self->pending_seq_number = pw_core_sync(self->core, 0, self->pending_seq_number);
#pragma GCC diagnostic pop

	while (true)
	{
		if (self->core_done_seq_number == self->pending_seq_number)
		{
			GST_DEBUG_OBJECT(self, "PipeWire core fully synced");
			break;
		}

		if (self->last_error < 0)
		{
			GST_ERROR_OBJECT(self, "stopping PipeWire mainloop due to error");
			break;
		}

		pw_thread_loop_wait(self->loop);
	}
}


static void gst_pipewire_core_on_core_done(void *object, uint32_t id, int sequence_number)
{
	GstPipewireCore *self = GST_PIPEWIRE_CORE_CAST(object);
	GST_TRACE_OBJECT(self, "id %" G_GUINT32_FORMAT " seqnum %d", (guint32)id, sequence_number);

	if (id == PW_ID_CORE)
	{
		GST_DEBUG_OBJECT(self, "PipeWire core done; sequence number: %d", sequence_number);
		self->core_done_seq_number = sequence_number;
		pw_thread_loop_signal(self->loop, FALSE);
	}
}


static void gst_pipewire_core_on_core_error(void *object, uint32_t id, int sequence_number, int res, const char *message)
{
	GstPipewireCore *self = GST_PIPEWIRE_CORE_CAST(object);

	/* When the graph found no node to link to, log this with DEBUG level. This can happen during probing,
	 * and missing nodes aren't an error then. And even when it _is_ an error, pw_stream already handles
	 * link failures on its own, so we do not need to unnecessarily add error lines to the log. */
	if (res == (-ENOENT))
	{
		GST_DEBUG_OBJECT(
			self,
			"PipeWire core got notified about a missing node error; most likely there is no node link the stream to;  id: %" PRIu32 "  sequence_number: %d  message: \"%s\"",
			id,
			sequence_number,
			message
		);
	}
	else
	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		GST_ERROR_OBJECT(
			self,
			"PipeWire core got notified about error:  id: %" PRIu32 "  sequence_number: %d  POSIX error: \"%s\" (%d)  message: \"%s\"",
			id,
			sequence_number,
			spa_strerror(res), res,
			message
		);
#pragma GCC diagnostic pop
	}

	if (id == PW_ID_CORE)
		self->last_error = res;

	pw_thread_loop_signal(self->loop, FALSE);
}


GstPipewireCore* gst_pipewire_core_get(int fd)
{
	GstPipewireCore *pipewire_core = NULL;
	GList *list_elem;

	LOCK_CORE_LIST();

	for (list_elem = core_list; list_elem != NULL; list_elem = list_elem->next)
	{
		GstPipewireCore *candidate_core = GST_PIPEWIRE_CORE_CAST(list_elem->data);
		if (candidate_core->requested_fd == fd)
		{
			pipewire_core = candidate_core;
			gst_object_ref(GST_OBJECT(pipewire_core));
			break;
		}
	}

	if (pipewire_core != NULL)
		goto finish;

	pipewire_core = GST_PIPEWIRE_CORE_CAST(g_object_new(gst_pipewire_core_get_type(), NULL));
	g_assert(pipewire_core != NULL);

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(pipewire_core));

	pipewire_core->requested_fd = fd;
	pipewire_core->core_done_seq_number = -1;
	pipewire_core->last_error = 0;
	pipewire_core->pending_seq_number = 0;

	pipewire_core->loop = pw_thread_loop_new("gstpipewire-main-loop", NULL);
	if (G_UNLIKELY(pipewire_core->loop == NULL))
	{
		GST_ERROR_OBJECT(pipewire_core, "could not create PipeWire mainloop");
		goto error;
	}

	pipewire_core->context = pw_context_new(pw_thread_loop_get_loop(pipewire_core->loop), NULL, 0);
	if (G_UNLIKELY(pipewire_core->context == NULL))
	{
		GST_ERROR_OBJECT(pipewire_core, "could not create PipeWire context");
		goto error;
	}

	if (pw_thread_loop_start(pipewire_core->loop) < 0)
	{
		GST_ERROR_OBJECT(pipewire_core, "could not start PipeWire mainloop");
		goto error;
	}

	pw_thread_loop_lock(pipewire_core->loop);

	if (fd < 0)
		pipewire_core->core = pw_context_connect(pipewire_core->context, NULL, 0);
	else
		pipewire_core->core = pw_context_connect_fd(pipewire_core->context, fcntl(fd, F_DUPFD_CLOEXEC, 3), NULL, 0);

	if (pipewire_core->core != NULL)
	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		pw_core_add_listener(
			pipewire_core->core,
			&(pipewire_core->core_listener),
			&core_events,
			pipewire_core
		);
#pragma GCC diagnostic pop
	}

	pw_thread_loop_unlock(pipewire_core->loop);

	if (G_UNLIKELY(pipewire_core->core == NULL))
	{
		GST_ERROR_OBJECT(pipewire_core, "could not create PipeWire core");
		goto error;
	}

	GST_DEBUG("adding core %" GST_PTR_FORMAT " to list", (gpointer)pipewire_core);
	core_list = g_list_prepend(core_list, pipewire_core);

finish:
	UNLOCK_CORE_LIST();
	return pipewire_core;

error:
	if (pipewire_core != NULL)
	{
		gst_object_unref(pipewire_core);
		pipewire_core = NULL;
	}

	goto finish;
}


void gst_pipewire_core_release(GstPipewireCore *core)
{
	LOCK_CORE_LIST();

	if (GST_OBJECT_REFCOUNT_VALUE(core) == 1)
	{
		GST_DEBUG("removing core %" GST_PTR_FORMAT " from list", (gpointer)core);
		core_list = g_list_remove(core_list, core);
	}

	gst_object_unref(GST_OBJECT(core));

	UNLOCK_CORE_LIST();
}


static void gst_pipewire_core_shutdown(GstPipewireCore *self)
{
	if (self->loop == NULL)
		return;

	if (self->core != NULL)
	{
		pw_thread_loop_lock(self->loop);

		gst_pipewire_core_sync_pw_core(self);
		pw_core_disconnect(self->core);

		self->core = NULL;

		pw_thread_loop_unlock(self->loop);
	}

	if (self->context != NULL)
	{
		pw_context_destroy(self->context);
		self->context = NULL;
	}

	pw_thread_loop_stop(self->loop);
	pw_thread_loop_destroy(self->loop);

	self->loop = NULL;
}

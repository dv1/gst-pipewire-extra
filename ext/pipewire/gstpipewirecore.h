/* gst-pipewire-extra
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

#ifndef __GST_PIPEWIRE_CORE_H__
#define __GST_PIPEWIRE_CORE_H__

#include <gst/gst.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <pipewire/pipewire.h>

#pragma GCC diagnostic pop


G_BEGIN_DECLS


typedef struct _GstPipewireCore GstPipewireCore;
typedef struct _GstPipewireCoreClass GstPipewireCoreClass;


#define GST_TYPE_PIPEWIRE_CORE             (gst_pipewire_core_get_type())
#define GST_PIPEWIRE_CORE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PIPEWIRE_CORE, GstPipewireCore))
#define GST_PIPEWIRE_CORE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PIPEWIRE_CORE, GstPipewireCoreClass))
#define GST_PIPEWIRE_CORE_CAST(obj)        ((GstPipewireCore *)(obj))
#define GST_IS_PIPEWIRE_CORE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PIPEWIRE_CORE))
#define GST_IS_PIPEWIRE_CORE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PIPEWIRE_CORE))


struct _GstPipewireCore
{
	GstObject parent;

	int requested_fd;
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	int core_done_seq_number;
	int last_error;
	int pending_seq_number;
};


struct _GstPipewireCoreClass
{
	GstObjectClass parent_class;
};


GType gst_pipewire_core_get_type(void);

GstPipewireCore* gst_pipewire_core_get(int fd);
void gst_pipewire_core_release(GstPipewireCore *core);


G_END_DECLS


#endif /* __GST_PIPEWIRE_CORE_H__ */

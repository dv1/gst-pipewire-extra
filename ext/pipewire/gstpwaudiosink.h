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

#ifndef __GST_PW_AUDIO_SINK_H__
#define __GST_PW_AUDIO_SINK_H__

#include <gst/gst.h>


G_BEGIN_DECLS


typedef struct _GstPwAudioSink GstPwAudioSink;
typedef struct _GstPwAudioSinkClass GstPwAudioSinkClass;


#define GST_TYPE_PW_AUDIO_SINK             (gst_pw_audio_sink_get_type())
#define GST_PW_AUDIO_SINK(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_AUDIO_SINK, GstPwAudioSink))
#define GST_PW_AUDIO_SINK_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_AUDIO_SINK, GstPwAudioSinkClass))
#define GST_PW_AUDIO_SINK_CAST(obj)        ((GstPwAudioSink *)(obj))
#define GST_IS_PW_AUDIO_SINK(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_AUDIO_SINK))
#define GST_IS_PW_AUDIO_SINK_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_AUDIO_SINK))


GType gst_pw_audio_sink_get_type(void);


G_END_DECLS


#endif /* __GST_PW_AUDIO_SINK_H__ */

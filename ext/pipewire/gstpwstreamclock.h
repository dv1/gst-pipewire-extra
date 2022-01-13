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

#ifndef __GST_PW_STREAM_CLOCK_H__
#define __GST_PW_STREAM_CLOCK_H__

#include <gst/gst.h>

#include <pipewire/pipewire.h>


G_BEGIN_DECLS


/**
 * GstPwStreamClock:
 *
 * Opaque #GstPwStreamClock structure.
 */
typedef struct _GstPwStreamClock GstPwStreamClock;

typedef struct _GstPwStreamClockClass GstPwStreamClockClass;


#define GST_TYPE_PW_STREAM_CLOCK             (gst_pw_stream_clock_get_type())
#define GST_PW_STREAM_CLOCK(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_STREAM_CLOCK, GstPwStreamClock))
#define GST_PW_STREAM_CLOCK_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_STREAM_CLOCK, GstPwStreamClockClass))
#define GST_PW_STREAM_CLOCK_CAST(obj)        ((GstPwStreamClock *)(obj))
#define GST_IS_PW_STREAM_CLOCK(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_STREAM_CLOCK))
#define GST_IS_PW_STREAM_CLOCK_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_STREAM_CLOCK))


GType gst_pw_stream_clock_get_type(void);

GstPwStreamClock* gst_pw_stream_clock_new(void);

void gst_pw_stream_clock_set_rate_diff(GstPwStreamClock *stream_clock, double rate_diff);
void gst_pw_stream_clock_reset_states(GstPwStreamClock *stream_clock);


G_END_DECLS


#endif /* __GST_PW_STREAM_CLOCK_H__ */

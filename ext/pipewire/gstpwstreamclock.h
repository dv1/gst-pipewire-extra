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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <pipewire/pipewire.h>

#pragma GCC diagnostic pop


G_BEGIN_DECLS


/**
 * GstPwStreamClock:
 *
 * Opaque #GstPwStreamClock structure.
 */
typedef struct _GstPwStreamClock GstPwStreamClock;

typedef struct _GstPwStreamClockClass GstPwStreamClockClass;

typedef GstClockTime (*GstPwStreamClockGetSysclockTimeFunc)(GstPwStreamClock *clock);


#define GST_TYPE_PW_STREAM_CLOCK             (gst_pw_stream_clock_get_type())
#define GST_PW_STREAM_CLOCK(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_STREAM_CLOCK, GstPwStreamClock))
#define GST_PW_STREAM_CLOCK_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_STREAM_CLOCK, GstPwStreamClockClass))
#define GST_PW_STREAM_CLOCK_CAST(obj)        ((GstPwStreamClock *)(obj))
#define GST_IS_PW_STREAM_CLOCK(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_STREAM_CLOCK))
#define GST_IS_PW_STREAM_CLOCK_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_STREAM_CLOCK))


GType gst_pw_stream_clock_get_type(void);

/**
 * gst_pw_stream_clock_new:
 * @get_sysclock_time_func: (allow-none): Callback for getting current system clock time.
 *
 * Creates a new #GstPwStreamClock.
 *
 * The clock uses the get_sysclock_time_func callback for its internal extrapolations. If
 * this argument is set to NULL, a default internal function is used. This argument is
 * mainly interesting for unit tests.
 *
 * Returns: (transfer full): new #GstPwStreamClock instance.
 */
GstPwStreamClock* gst_pw_stream_clock_new(GstPwStreamClockGetSysclockTimeFunc get_sysclock_time_func);

/**
 * gst_pw_stream_clock_reset:
 * @stream_clock The #GstPwStreamClock.
 *
 * Fully resets the internal states of the clock to their initial values.
 * This is useful for when a GStreamer element that uses this clock is set
 * to a READY or NULL state.
 */
void gst_pw_stream_clock_reset(GstPwStreamClock *stream_clock);

/**
 * gst_pw_stream_clock_reset:
 * @stream_clock The #GstPwStreamClock.
 *
 * "Freezes" the clock, causing it to return the last produced value constantly,
 * until gst_pw_stream_clock_add_observation() is called again, after which
 * the clock produces timestamps normally again (without a jump in the produced
 * timestamps). This is useful during pw_stream reconfigurations.
 *
 * Note that the gst_pw_stream_clock_reset() implies that the clock is frozen,
 * so calling this function directly after a reset is redundant.
 */
void gst_pw_stream_clock_freeze(GstPwStreamClock *stream_clock);

/**
 * gst_pw_stream_clock_add_observation:
 * @stream_clock The #GstPwStreamClock.
 * @observation Observation to add.
 *
 * Adds an "observation" to the clock. An "observation" is a timing update from
 * the pw_stream based on information from a pw_stream_get_time_n() call. That
 * call fills the fields of a pw_time struct. Such a struct is then passed to
 * this function as the observation.
 *
 * This function un-freezes a clock after it got frozen by a gst_pw_stream_clock_freeze()
 * or gst_pw_stream_clock_reset() call (the clock is also frozen right after
 * creating it, and that too is undone by this function).
 */
void gst_pw_stream_clock_add_observation(GstPwStreamClock *stream_clock, struct pw_time const *observation);


G_END_DECLS


#endif /* __GST_PW_STREAM_CLOCK_H__ */

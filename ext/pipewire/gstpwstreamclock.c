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

/**
 * SECTION:gstpwstreamclock
 * @title: GstPwStreamClock
 * @short_description: Clock based on PipeWire streams.
 * @see_also: #GstSystemClock, #GstPipeline
 *
 * #GstPwStreamClock implements a custom #GstSystemClock that is based on the
 * monotonic system clock, adjusted via rate difference quantities that come
 * from a <ulink url="https://docs.pipewire.org/group__pw__stream.html">PipeWire stream</ulink>.
 * Initially, the rate difference is set to 1.0, which produces timestamps
 * that are identical to that of the CLOCK_MONOTONIC system clock. The
 * gst_pw_stream_clock_set_rate_diff() is then called regularly whenever there
 * is information about the current rate difference between that system clock
 * and the PipeWire driver associated with the stream.
 *
 * This means that this clock can always be used, even if no stream is active.
 */

#include <time.h>
#include <gst/gst.h>
#include <gst/gstsystemclock.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include "gstpwstreamclock.h"

#pragma GCC diagnostic pop


GST_DEBUG_CATEGORY(pw_stream_clock_debug);
#define GST_CAT_DEFAULT pw_stream_clock_debug


struct _GstPwStreamClock
{
	GstSystemClock parent;

	/*< private >*/

	/* The object lock must be taken before accessing these states. */
	double rate_diff;
	GstClockTime internal_time_offset;
	GstClockTime system_clock_time_offset;
	GstClockTime last_timestamp;
};


struct _GstPwStreamClockClass
{
	GstSystemClockClass parent_class;
};


G_DEFINE_TYPE(GstPwStreamClock, gst_pw_stream_clock, GST_TYPE_SYSTEM_CLOCK)


static GstClockTime gst_pw_stream_clock_get_internal_time(GstClock *clock);

static GstClockTime gst_pw_stream_clock_get_current_monotonic_time(void);
static GstClockTime gst_pw_stream_clock_get_internal_time_unlocked(GstPwStreamClock *self, gboolean update_offsets);


static void gst_pw_stream_clock_class_init(GstPwStreamClockClass *klass)
{
	GstClockClass *clock_class;

	GST_DEBUG_CATEGORY_INIT(pw_stream_clock_debug, "pwstreamclock", 0, "PipeWire stream based clock");

	clock_class = GST_CLOCK_CLASS(klass);

	clock_class->get_internal_time = GST_DEBUG_FUNCPTR(gst_pw_stream_clock_get_internal_time);
}


static void gst_pw_stream_clock_init(GstPwStreamClock *self)
{
	gst_pw_stream_clock_reset_states(self);
}


static GstClockTime gst_pw_stream_clock_get_internal_time(GstClock *clock)
{
	GstClockTime gst_internal_time;
	GstPwStreamClock *self = GST_PW_STREAM_CLOCK(clock);

	GST_OBJECT_LOCK(self);
	gst_internal_time = gst_pw_stream_clock_get_internal_time_unlocked(self, FALSE);
	GST_OBJECT_UNLOCK(self);

	return gst_internal_time;
}


static GstClockTime gst_pw_stream_clock_get_current_monotonic_time(void)
{
	/* NOTE: Using CLOCK_MONOTONIC instead of CLOCK_MONOTONIC_RAW on purpose. See:
	 * https://stackoverflow.com/questions/47339326/measuring-elapsed-time-in-linux-clock-monotonic-vs-clock-monotonic-raw
	 *
	 * Also, PipeWire uses the former instead of the latter for its pw_time.now
	 * field values, so stick to the same to clock to ensure comparable timestamps.
	 */

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (GstClockTime)(ts.tv_sec) * GST_SECOND + (GstClockTime)(ts.tv_nsec);
}


static GstClockTime gst_pw_stream_clock_get_internal_time_unlocked(GstPwStreamClock *self, gboolean update_offsets)
{
	/* This must be called with the object lock taken. */

	/* system_clock_time_offset may be updated in this function.
	 * This can happen for two reasons:
	 *
	 * 1. In the very beginning, system_clock_time_offset is not
	 *    set to a valid value, and needs an initial timestamp.
	 * 2. When update_offsets is set to true.
	 *
	 * The first case is handled _before_ calculating internal_time,
	 * since that calculation requires a valid system_clock_time_offset.
	 *
	 * The second case is handled _after_ that calculation. That's
	 * because the current offset values are needed to compute the
	 * new offsets.
	 */

	gboolean offsets_not_initialized;
	GstClockTime sysclock_time, internal_time;

	offsets_not_initialized = !GST_CLOCK_TIME_IS_VALID(self->system_clock_time_offset);

	sysclock_time = gst_pw_stream_clock_get_current_monotonic_time();
	if (G_UNLIKELY(offsets_not_initialized))
	{
		self->system_clock_time_offset = sysclock_time;
		self->internal_time_offset = 0;
	}

	/* PipeWire informs us about the rate difference between the monotonic system clock
	 * and the clock of the driver that runs the PipeWire graph. Based on this, we can
	 * construct that clock by using linear extrapolation. PipeWire updates the rate_diff
	 * a couple of times, but mostly right after it started (its rate difference estimate
	 * stabilizes quickly, and then no longer changes). Every time the rate difference
	 * changes, the offsets are recomputed to produce a piecewise linear progress of the
	 * clock timestamps. That is, every time the rate difference is updated, a new linear
	 * segment starts. (See gst_pw_stream_clock_set_rate_diff().) The timestamps produced
	 * by the clock must be monotonically increasing, which is why the offsets are kept
	 * updated based on the rate_diff. */
	internal_time = (sysclock_time - self->system_clock_time_offset) * self->rate_diff + self->internal_time_offset;

	GST_LOG_OBJECT(
		self,
		"sysclock time %" G_GUINT64_FORMAT "; sysclock / internal time offsets: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; rate diff: %f; sysclock - internal diff relative to the offsets: %" G_GINT64_FORMAT " => internal time %" GST_TIME_FORMAT,
		sysclock_time,
		self->system_clock_time_offset, self->internal_time_offset,
		self->rate_diff,
		GST_CLOCK_DIFF((sysclock_time - self->system_clock_time_offset), (internal_time - self->internal_time_offset)),
		GST_TIME_ARGS(internal_time)
	);

	if (update_offsets)
	{
		self->system_clock_time_offset = sysclock_time;
		self->internal_time_offset = internal_time;
	}

	/* Catch cases when there's a jump backwards in the timestamps. This rarely happens,
	 * and then it usually does so because of a rate diff change. We must return timestamps
	 * that monotonically increase. In case of a backwards jump, the last timestamp we sent
	 * out will be higher than internal_time. Return last_timestamp until the internal
	 * time "catches up" and starts to exceed last_timestamp again. */
	if (GST_CLOCK_TIME_IS_VALID(self->last_timestamp) && G_UNLIKELY(self->last_timestamp > internal_time))
	{
		GST_LOG_OBJECT(self, "last timestamp %" GST_TIME_FORMAT " was higher than new internal time; returning last timestamp to ensure output timestamps remain monotonically increasing", GST_TIME_ARGS(self->last_timestamp));
		internal_time = self->last_timestamp;
	}
	else
		self->last_timestamp = internal_time;

	return internal_time;
}


/**
 * gst_pw_stream_clock_new:
 *
 * Create a new #GstPwStreamClock.
 *
 * Returns: (transfer full): new #GstPwStreamClock instance.
 */
GstPwStreamClock* gst_pw_stream_clock_new(void)
{
	GstPwStreamClock *stream_clock = GST_PW_STREAM_CLOCK_CAST(g_object_new(GST_TYPE_PW_STREAM_CLOCK, NULL));

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(stream_clock));

	return stream_clock;
}


/**
 * gst_pw_stream_clock_set_rate_diff:
 * @stream_clock: a #GstPwStreamClock
 * @rate_diff: Rate difference between stream driver and the monotonic system clock
 *
 * Sets the new rate difference. This is the rate difference that is reported
 * via <ulink url="https://docs.pipewire.org/structspa__io__position.html>spa_io_position</ulink>.
 * To get the current rate difference, store the spa_io_position pointer that is passed
 * via the <ulink url="https://docs.pipewire.org/structpw__stream__events.html">io_changed
 * stream event</ulink> (check for the SPA_IO_Position event ID). Then, in the
 * process stream event, the rate_diff value can be retrieved from the spa_io_position's
 * clock field. (Use the PW_STREAM_FLAG_RT_PROCESS when creating a stream to
 * prevent race conditions when accessing that field in the process stream event.)
 *
 * rate_diff must be greater than 0.
 *
 * NOTE: This takes the object lock of stream_clock.
 *
 * MT safe.
 */
void gst_pw_stream_clock_set_rate_diff(GstPwStreamClock *stream_clock, double rate_diff)
{
	g_assert(stream_clock != NULL);
	g_assert(rate_diff > 0.0);

	GST_OBJECT_LOCK(stream_clock);

	/* Filter out redundant calls when the rate difference did not change,
	 * or at least did not change enough. Use 1 PPM as epsilon. Clock drifts
	 * in the sub-PPM range are not easy to measure, and constantly readjusting
	 * the rate difference based on them is not useful. */
	if (G_APPROX_VALUE(stream_clock->rate_diff, rate_diff, 1e-6))
		goto finish;

	GST_LOG_OBJECT(stream_clock, "updating rate diff to %f", rate_diff);

	/* Update the offsets before setting a new rate to have valid offsets
	 * to continue from. */
	gst_pw_stream_clock_get_internal_time_unlocked(stream_clock, TRUE);

	stream_clock->rate_diff = rate_diff;

finish:
	GST_OBJECT_UNLOCK(stream_clock);
}


/**
 * gst_pw_stream_clock_reset_states:
 * @stream_clock: a #GstPwStreamClock
 *
 * Resets ínternal states, specifically the timestamp offsets and the rate
 * difference. The latter is set to 1.0. This is useful for when the GStreamer
 * pipeline switches back to the READY state.
 *
 * NOTE: This takes the object lock of stream_clock.
 *
 * MT safe.
 */
void gst_pw_stream_clock_reset_states(GstPwStreamClock *stream_clock)
{
	g_assert(stream_clock != NULL);

	GST_OBJECT_LOCK(stream_clock);

	stream_clock->rate_diff = 1.0;
	stream_clock->internal_time_offset = 0;
	stream_clock->system_clock_time_offset = GST_CLOCK_TIME_NONE;
	stream_clock->last_timestamp = GST_CLOCK_TIME_NONE;

	GST_OBJECT_UNLOCK(stream_clock);
}

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

/**
 * SECTION:gstpwstreamclock
 * @title: GstPwStreamClock
 * @short_description: Clock based on PipeWire streams.
 * @see_also: #GstSystemClock, #GstPipeline
 *
 * #GstPwStreamClock implements a custom #GstSystemClock that is based on the
 * monotonic system clock, adjusted via rate adjustments that come from a
 * <ulink url="https://docs.pipewire.org/group__pw__stream.html">PipeWire stream</ulink>.
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

	GstPwStreamClockGetSysclockTimeFunc get_sysclock_time_func;

	/* The object lock must be taken before accessing these fields
	 * unless it is guaranteed that there can be no concurrent call
	 * to gst_pw_stream_clock_get_internal_time(). */

	/* The driver clock rate as a fractional number. Used by
	 * get_internal_time_unlocked() to extrapolate a timestamp
	 * which increases at the pace of the driver clock. */
	guint64 driver_clock_rate_num;
	guint64 driver_clock_rate_denom;
	/* These are used by add_observation() to calculate the
	 * driver clock rate fields. */
	GstClockTime previous_driver_clock_time;
	GstClockTime previous_system_clock_time;

	/* Offsets for the piecewise linear reconstruction of the
	 * driver clock. driver_clock_time_offset and system_clock_time_offset
	 * are adjusted every time add_observation() is called.
	 * base_driver_clock_time_offset is adjusted only if can_extrapolate
	 * is FALSE. See the implementation of add_observation() for
	 * a more detailed explanation for what these offsets do. */
	GstClockTime driver_clock_time_offset;
	GstClockTime system_clock_time_offset;
	GstClockTimeDiff base_driver_clock_time_offset;

	/* This indicates to get_internal_time_unlocked() whether
	 * a timestamp can currently be extrapolated. This is only
	 * possible after an add_observation() call set valid values
	 * for the driver clock rate and the offsets. With these, it
	 * is possible to extrapolate a timestamp out of their values
	 * and the current system clock time. Otherwise, if no valid
	 * values are present, get_internal_time_unlocked() just returns
	 * the value of last_timestamp. This effectively implements a
	 * form of clock stretching that is useful for covering phases
	 * during which the pw_stream gets reconfigured for example.
	 * This field is set to TRUE by add_observation() and to FALSE
	 * by reset() and freeze(). */
	gboolean can_extrapolate;
	/* The timestamp that was produced by the last get_internal_time_unlocked()
	 * call. This is initially set to 0, meaning that the timestamps
	 * that are produced by that function always begin at 0. */
	GstClockTime last_timestamp;
};


struct _GstPwStreamClockClass
{
	GstSystemClockClass parent_class;
};


G_DEFINE_TYPE(GstPwStreamClock, gst_pw_stream_clock, GST_TYPE_SYSTEM_CLOCK)


static void gst_pw_stream_clock_dispose(GObject *object);

static GstClockTime gst_pw_stream_clock_get_internal_time(GstClock *clock);

static GstClockTime gst_pw_stream_clock_get_current_monotonic_time(GstPwStreamClock *self);
static GstClockTime gst_pw_stream_clock_get_internal_time_unlocked(GstPwStreamClock *self);


static void gst_pw_stream_clock_class_init(GstPwStreamClockClass *klass)
{
	GObjectClass *object_class;
	GstClockClass *clock_class;

	GST_DEBUG_CATEGORY_INIT(pw_stream_clock_debug, "pwstreamclock", 0, "PipeWire stream based clock");

	object_class = G_OBJECT_CLASS(klass);
	clock_class = GST_CLOCK_CLASS(klass);

	object_class->dispose          = GST_DEBUG_FUNCPTR(gst_pw_stream_clock_dispose);
	clock_class->get_internal_time = GST_DEBUG_FUNCPTR(gst_pw_stream_clock_get_internal_time);
}


static void gst_pw_stream_clock_init(GstPwStreamClock *self)
{
	self->driver_clock_rate_num = 1;
	self->driver_clock_rate_denom = 1;
	self->previous_driver_clock_time = GST_CLOCK_TIME_NONE;
	self->previous_system_clock_time = GST_CLOCK_TIME_NONE;

	self->driver_clock_time_offset = 0;
	self->system_clock_time_offset = 0;
	self->base_driver_clock_time_offset = 0;

	self->can_extrapolate = FALSE;
	self->last_timestamp = 0;
}


static void gst_pw_stream_clock_dispose(GObject *object)
{
	GST_DEBUG_OBJECT(object, "disposing of pwstreamclock %s", GST_OBJECT_NAME(object));
	G_OBJECT_CLASS(gst_pw_stream_clock_parent_class)->dispose(object);
}


static GstClockTime gst_pw_stream_clock_get_internal_time(GstClock *clock)
{
	GstClockTime gst_internal_time;
	GstPwStreamClock *self = GST_PW_STREAM_CLOCK(clock);

	GST_OBJECT_LOCK(self);
	gst_internal_time = gst_pw_stream_clock_get_internal_time_unlocked(self);
	GST_OBJECT_UNLOCK(self);

	return gst_internal_time;
}


static GstClockTime gst_pw_stream_clock_get_current_monotonic_time(G_GNUC_UNUSED GstPwStreamClock *self)
{
	/* This is a default GstPwStreamClockGetSysclockTimeFunc that is used
	 * if the caller sets the get_sysclock_time_func argument of
	 * gst_pw_stream_clock_new() to NULL.
	 *
	 * NOTE: Using CLOCK_MONOTONIC instead of CLOCK_MONOTONIC_RAW on purpose. See:
	 * https://stackoverflow.com/questions/47339326/measuring-elapsed-time-in-linux-clock-monotonic-vs-clock-monotonic-raw
	 *
	 * Also, PipeWire uses the former instead of the latter for its pw_time.now
	 * field values, so stick to the same to clock to ensure comparable timestamps.
	 */

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return GST_TIMESPEC_TO_TIME(ts);
}


static GstClockTime gst_pw_stream_clock_get_internal_time_unlocked(GstPwStreamClock *self)
{
	/* This must be called with the object lock taken. */

	GstClockTime sysclock_time, driver_clock_time;

	if (G_UNLIKELY(!self->can_extrapolate))
		return self->last_timestamp;

	g_assert(self->get_sysclock_time_func != NULL);
	sysclock_time = self->get_sysclock_time_func(self);

	/* Perform piecewise linear extrapolation to get the current driver clock time. */
	driver_clock_time = gst_util_uint64_scale_round(sysclock_time - self->system_clock_time_offset, self->driver_clock_rate_num, self->driver_clock_rate_denom) + self->driver_clock_time_offset;

	GST_LOG_OBJECT(
		self,
		"sysclock time %" G_GUINT64_FORMAT "; sysclock / driver-clock time offsets: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; rate: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; sysclock - driver-clock diff relative to the offsets: %" G_GINT64_FORMAT "  => driver-clock time %" GST_TIME_FORMAT,
		sysclock_time,
		self->system_clock_time_offset, self->driver_clock_time_offset,
		self->driver_clock_rate_num, self->driver_clock_rate_denom,
		GST_CLOCK_DIFF((sysclock_time - self->system_clock_time_offset), (driver_clock_time - self->driver_clock_time_offset)),
		GST_TIME_ARGS(driver_clock_time)
	);

	/* When new observations are made (by means of add_observation() calls),
	 * it may turn out that the extrapolations that were made so far actually
	 * overshot, meaning that some of the latest produced timestamps are then
	 * ahead of the driver clock time that was part of the add_observation()
	 * call. If we do not address this, we end up here with driver_clock_time
	 * values that suddenly go down again. This however is not acceptable;
	 * the driver_clock_time timestamps must be monotonically increasing.
	 * To fix this, keep returning last_timestamp until driver_clock_time
	 * "catches up" with the value of last_timestamp. */
	if (GST_CLOCK_TIME_IS_VALID(self->last_timestamp) && G_UNLIKELY(self->last_timestamp > driver_clock_time))
	{
		GST_LOG_OBJECT(self, "last timestamp %" GST_TIME_FORMAT " was higher than new driver-clock time; returning last timestamp to ensure output timestamps remain monotonically increasing", GST_TIME_ARGS(self->last_timestamp));
		driver_clock_time = self->last_timestamp;
	}
	else
		self->last_timestamp = driver_clock_time;

	return driver_clock_time;
}


GstPwStreamClock* gst_pw_stream_clock_new(GstPwStreamClockGetSysclockTimeFunc get_sysclock_time_func)
{
	GstPwStreamClock *stream_clock = GST_PW_STREAM_CLOCK_CAST(g_object_new(GST_TYPE_PW_STREAM_CLOCK, NULL));
	stream_clock->get_sysclock_time_func = (get_sysclock_time_func == NULL) ? &gst_pw_stream_clock_get_current_monotonic_time : get_sysclock_time_func;

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(stream_clock));

	GST_DEBUG_OBJECT(stream_clock, "created new pwstreamclock %s", GST_OBJECT_NAME(stream_clock));

	return stream_clock;
}


void gst_pw_stream_clock_freeze(GstPwStreamClock *stream_clock)
{
	g_assert(stream_clock != NULL);

	GST_OBJECT_LOCK(stream_clock);
	stream_clock->can_extrapolate = FALSE;
	stream_clock->previous_driver_clock_time = GST_CLOCK_TIME_NONE;
	stream_clock->previous_system_clock_time = GST_CLOCK_TIME_NONE;
	GST_OBJECT_UNLOCK(stream_clock);
}


void gst_pw_stream_clock_add_observation(GstPwStreamClock *stream_clock, struct pw_time const *observation)
{
	/* We do not get direct access to the "driver clock" in pipewire. This is the clock that
	 * is in the driver of the pipewire graph. That clock sets the pace of the graph. We do
	 * get periodic updates though; it is possible in the pw_stream process callback to
	 * call pw_stream_get_time_n(), which fills a pw_time struct with a snapshot that contains
	 * bits of timing information. In particular, it provides two timestamps: The monotonic
	 * system clock time when this snapshot was made, and the driver ticks, which essentially
	 * are like an "driver clock timestamp". We can interpret this as saying that at monotonic
	 * system clock time X the driver clock time was Y. This we refer to as an "observation".
	 * the monotonic system clock time is stored in the "now" field of the pw_time struct; the
	 * driver clock time is indirectly present as "ticks" in that same struct (it can be translated
	 * to a timestamp with the "rate" field of the pw_time struct).
	 *
	 * Using this information, we can reconstruct the driver clock in a piecewise linear fashion.
	 * In between observations, we extrapolate an driver clock timestamp based on the timestamps
	 * from the current and the last observations. The differences between their timestamps are
	 * used as the clock rate:
	 *
	 *   driver_clock_rate = (driver_clock_time - previous_driver_clock_time) / (system_clock_time - previous_driver_clock_time)
	 *
	 * With this, get_internal_time_unlocked() can then extrapolate:
	 *
	 *   extrapolated_driver_clock_timestamp = (current_sysclock_time - system_clock_time_offset) * driver_clock_rate + driver_clock_time_offset
	 *
	 * The offsets exist to translate the extrapolated timestamps to fit into the "pieces" that
	 * make up the piecewise linear extrapolation. driver_clock_time_offset and system_clock_time_offset
	 * applied such that the extrapolated timestamps begin at driver_clock_time_offset. In other words,
	 * if the current system clock time is the same as system_clock_time, we want the extrapolated
	 * result to be equal to driver_clock_time_offset.
	 *
	 * In addition, we have to take into account the situations when no extrapolation is possible.
	 * These are:
	 *
	 * 1. When freeze() was called. Callers do this when they for example reconfigure the pw_stream.
	 * 2. When the pwstreamclock is initially used. There is no "previous observation" present then;
	 *    no extrapolation can be made. We can interpret this as the clock being initially frozen.
	 *
	 * (A reset() call implies that the clock gets frozen.)
	 *
	 * In both cases, extrapolation will only be possible after a new observation has been made.
	 * Until then, a form of clock stretching is used by get_internal_time_unlocked() instead -
	 * it just returns the value of last_timestamp. Once a new observations is made, the
	 * clock is "unfrozen", that is, extrapolations are possible (again). We do not want to
	 * cause sudden big jumps in the timestamps though - instead, they shall continue at the
	 * timestamp value from before it was frozen (that is, getting a timestamp immediately
	 * after unfreezing should return the value of last_timestamp). For this purpose, the
	 * base_driver_clock_time_offset exists. It is updated only if extrapolation hadn't been
	 * possible so far and just became possible due to now having a new observation. Its
	 * function is to additionally shift driver_clock_time_offset to cover the timespan
	 * during which the clock was "frozen". That way, discontinuities are avoided.
	 *
	 * Note that while computing the driver clock rate requires *two* observations, for unfreezing,
	 * we start/resume extrapolation after just *one* observation. In case #1 above (= the freeze()
	 * function was called), we just reuse the last driver clock rate that was in effect before
	 * the clock was frozen. In case #2 above (= initial state), the rate is set to 1 (see reset()).
	 */

	GstClockTime system_clock_time, driver_clock_time;

	g_assert(stream_clock != NULL);
	g_assert(observation != NULL);

	system_clock_time = observation->now;
	driver_clock_time = gst_util_uint64_scale_int_round(
		(guint64)(observation->ticks) * observation->rate.num,
		GST_SECOND,
		observation->rate.denom
	);

	GST_LOG_OBJECT(
		stream_clock,
		"add observation: driver clock / system clock time %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT,
		GST_TIME_ARGS(driver_clock_time), GST_TIME_ARGS(system_clock_time)
	);

	GST_OBJECT_LOCK(stream_clock);

	/* Handle unlikely corner cases that would lead to incorrect behavior by early-exiting. */
	if (G_UNLIKELY(!GST_CLOCK_TIME_IS_VALID(driver_clock_time) || (stream_clock->previous_system_clock_time == system_clock_time)))
		goto finish;

	/* We can continue extrapolating after this observation. Update base_driver_clock_time_offset
	 * as described above to avoid discontinuities. */
	if (!stream_clock->can_extrapolate)
	{
		stream_clock->base_driver_clock_time_offset = GST_CLOCK_DIFF(driver_clock_time, stream_clock->last_timestamp);
		stream_clock->can_extrapolate = TRUE;
	}

	/* Update the driver clock rate if we have data about this current observation and a previous one.
	 * Note that a freeze() call also erases the previous observation, so while one single new
	 * observation can unfreeze the clock, until another observation is made, we have to reuse
	 * the rate that was last computed before the freeze. */
	if (G_LIKELY(GST_CLOCK_TIME_IS_VALID(stream_clock->previous_driver_clock_time)))
	{
		stream_clock->driver_clock_rate_num = driver_clock_time - stream_clock->previous_driver_clock_time;
		stream_clock->driver_clock_rate_denom = system_clock_time - stream_clock->previous_system_clock_time;
	}

	stream_clock->driver_clock_time_offset = ((GstClockTimeDiff)driver_clock_time) + stream_clock->base_driver_clock_time_offset;
	stream_clock->system_clock_time_offset = system_clock_time;

	stream_clock->previous_driver_clock_time = driver_clock_time;
	stream_clock->previous_system_clock_time = system_clock_time;

finish:
	GST_OBJECT_UNLOCK(stream_clock);
}

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

#include <gst/gst.h>
/* Turn off -Wdeprecated-declarations to mask the "g_memdup is deprecated"
 * warning (originating in gst/base/gstbytereader.h) that is present in
 * many GStreamer installations. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gst/base/base.h>
#pragma GCC diagnostic pop
#include <gst/audio/audio.h>
#include "gstpwaudioringbuffer.h"

GST_DEBUG_CATEGORY(pw_audio_ring_buffer_debug);
#define GST_CAT_DEFAULT pw_audio_ring_buffer_debug


G_DEFINE_TYPE(GstPwAudioRingBuffer, gst_pw_audio_ring_buffer, GST_TYPE_OBJECT)


static void gst_pw_audio_ring_buffer_dispose(GObject *object);


static void gst_pw_audio_ring_buffer_class_init(GstPwAudioRingBufferClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = GST_DEBUG_FUNCPTR(gst_pw_audio_ring_buffer_dispose);

	GST_DEBUG_CATEGORY_INIT(pw_audio_ring_buffer_debug, "pwaudioringbuffer", 0, "GStreamer PipeWire audio ring buffer");
}


static void gst_pw_audio_ring_buffer_init(GstPwAudioRingBuffer *self)
{
	self->stride = 0;

	self->buffered_frames = NULL;

	self->ring_buffer_length = 0;
	self->current_fill_level = 0;

	self->oldest_frame_pts = GST_CLOCK_TIME_NONE;

	self->num_pts_delta_history_entries = 0;
}


static void gst_pw_audio_ring_buffer_dispose(GObject *object)
{
	GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER(object);

	g_free(self->buffered_frames);

	G_OBJECT_CLASS(gst_pw_audio_ring_buffer_parent_class)->dispose(object);
}


GstPwAudioRingBuffer* gst_pw_audio_ring_buffer_new(GstPwAudioFormat *format, GstClockTime ring_buffer_length)
{
	GstPwAudioRingBuffer* ring_buffer;
	guint64 num_frames;

	g_assert(format != NULL);
	g_assert(GST_CLOCK_TIME_IS_VALID(ring_buffer_length) && (ring_buffer_length > 0));

	num_frames = gst_pw_audio_format_calculate_num_frames_from_duration(
		format,
		ring_buffer_length
	);

	ring_buffer = g_object_new(gst_pw_audio_ring_buffer_get_type(), NULL);
	g_assert(ring_buffer != NULL);

	ringbuffer_metrics_init(&(ring_buffer->metrics), num_frames);

	memcpy(&(ring_buffer->format), format, sizeof(GstPwAudioFormat));
	ring_buffer->stride = gst_pw_audio_format_get_stride(format);

	ring_buffer->buffered_frames = g_try_malloc(num_frames * ring_buffer->stride);
	if (G_UNLIKELY(ring_buffer->buffered_frames == NULL))
	{
		GST_ERROR_OBJECT(ring_buffer, "could not allocate buffer for frames");
		goto error;
	}

	ring_buffer->ring_buffer_length = ring_buffer_length;
	ringbuffer_metrics_init(&(ring_buffer->metrics), num_frames);

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(ring_buffer));

	return ring_buffer;

error:
	if (ring_buffer != NULL)
		gst_object_unref(GST_OBJECT(ring_buffer));

	return NULL;
}


void gst_pw_audio_ring_buffer_flush(GstPwAudioRingBuffer *ring_buffer)
{
	g_assert(ring_buffer != NULL);

	ringbuffer_metrics_reset(&(ring_buffer->metrics));
	ring_buffer->current_fill_level = 0;
	ring_buffer->oldest_frame_pts = GST_CLOCK_TIME_NONE;
	ring_buffer->num_pts_delta_history_entries = 0;
}


gsize gst_pw_audio_ring_buffer_push_frames(
	GstPwAudioRingBuffer *ring_buffer,
	gpointer frames,
	gsize num_frames,
	gsize *num_silence_frames_to_prepend,
	GstClockTime pts
)
{
	guint64 write_lengths[2];
	guint64 write_offset;
	guint64 num_frames_to_write;
	guint64 num_silence_frames_to_write = 0;

	g_assert(ring_buffer != NULL);
	g_assert(frames != NULL);
	g_assert(num_frames > 0);

	/* Prepending silence frames is required when there is a gap in the
	 * timestamped data, for example, when gstbuffer #1 comes into the
	 * sink with PTS 100000 duration 50000, and gstbuffer #2 comes in
	 * with PTS 170000, even though that second buffer's PTS should have
	 * been 100000+50000 = 150000. In such a case, a gap of 20000 ns would
	 * be inserted. (In practice, gaps are much larger than that, but we
	 * use small quantities for sake of clarity in this example.) However,
	 * when the ring buffer is empty, there is no such discontinuity,
	 * since there is no data to append new frames to. */
	if (ring_buffer->current_fill_level == 0)
	{
		GST_DEBUG_OBJECT(
			ring_buffer,
			"prepending %" G_GSIZE_FORMAT " frame(s) requested, but ring buffer is empty -"
			"no need to prepend silence; setting num_silence_frames_to_prepend to 0",
			*num_silence_frames_to_prepend
		);
		*num_silence_frames_to_prepend = 0;
	}

	if (G_UNLIKELY(*num_silence_frames_to_prepend > 0))
	{
		num_silence_frames_to_write = ringbuffer_metrics_write(&(ring_buffer->metrics), *num_silence_frames_to_prepend, &write_offset, write_lengths);
		g_assert(num_silence_frames_to_write <= *num_silence_frames_to_prepend);

		if (write_lengths[0] > 0)
		{
			gst_pw_audio_format_write_silence_frames(
				&(ring_buffer->format),
				ring_buffer->buffered_frames + write_offset * ring_buffer->stride,
				write_lengths[0]
			);
		}
		if (write_lengths[1] > 0)
		{
			gst_pw_audio_format_write_silence_frames(
				&(ring_buffer->format),
				ring_buffer->buffered_frames,
				write_lengths[1]
			);
		}

		*num_silence_frames_to_prepend -= num_silence_frames_to_write;

		ring_buffer->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(
			&(ring_buffer->format),
			ring_buffer->metrics.current_num_buffered_frames
		);

		GST_DEBUG_OBJECT(
			ring_buffer,
			"silence write lengths: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; fill level after prepending: %" GST_TIME_FORMAT,
			write_lengths[0], write_lengths[1],
			GST_TIME_ARGS(ring_buffer->current_fill_level)
		);
	}

	num_frames_to_write = ringbuffer_metrics_write(&(ring_buffer->metrics), num_frames, &write_offset, write_lengths);
	g_assert(num_frames_to_write <= num_frames);

	GST_LOG_OBJECT(
		ring_buffer,
		"pushed %" G_GSIZE_FORMAT " out of %" G_GUINT64_FORMAT " frame(s) "
		"(write lengths %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "); "
		"prepended %" G_GUINT64_FORMAT " silence frame(s), with %" G_GSIZE_FORMAT " remaining silence frame(s) to prepend; "
		"read / write positions: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; "
		"num buffered frames: %" G_GUINT64_FORMAT "; "
		"capacity: %" G_GUINT64_FORMAT,
		num_frames_to_write, num_frames,
		write_lengths[0], write_lengths[1],
		num_silence_frames_to_write, *num_silence_frames_to_prepend,
		ring_buffer->metrics.read_position, ring_buffer->metrics.write_position,
		ring_buffer->metrics.current_num_buffered_frames,
		ring_buffer->metrics.capacity
	);

	if (write_lengths[0] > 0)
	{
		memcpy(
			ring_buffer->buffered_frames + write_offset * ring_buffer->stride,
			frames,
			write_lengths[0] * ring_buffer->stride
		);
	}
	if (write_lengths[1] > 0)
	{
		memcpy(
			ring_buffer->buffered_frames,
			((guint8 *)frames) + write_lengths[0] * ring_buffer->stride,
			write_lengths[1] * ring_buffer->stride
		);
	}

	ring_buffer->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(
		&(ring_buffer->format),
		ring_buffer->metrics.current_num_buffered_frames
	);

	/* Update the oldest_frame_pts. To do this, calculate the PTS of the
	 * *newest* data - that is, the PTS that is right at the end of the buffer
	 * that was just supplied - and subtract the current fill level from it.
	 * Since the buffered data is made of a sequence of raw frames (there are
	 * no "holes" in the ring buffer), (newest_pts - current_fill_level) must
	 * equal the oldest frame PTS.
	 * Only do this if no oldest_frame_pts is set yet. This happens at the
	 * beginning, before the pw dataloop actually started. Once it is going,
	 * the code in gst_pw_audio_ring_buffer_retrieve_frames() will take care
	 * of keeping the oldest_frame_pts up to date. */
	if (GST_CLOCK_TIME_IS_VALID(pts) && !GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts))
	{
		GstClockTime newest_pts;
		GstClockTime oldest_frame_pts;
		GstClockTime duration;

		duration = gst_pw_audio_format_calculate_duration_from_num_frames(
			&(ring_buffer->format),
			num_frames_to_write
		);

		newest_pts = pts + duration;
		/* In some corner cases, newest_pts may be behind current_fill_level
		 * by just 1 nanosecond due to rounding errors in the conversion from
		 * frames to nanoseconds. Work around this by using MAX(). */
		newest_pts = MAX(ring_buffer->current_fill_level, newest_pts);
		oldest_frame_pts = newest_pts - ring_buffer->current_fill_level;

		/* If an oldest frame PTS is already known, check if the oldest PTS we just
		 * calculated differs significantly (by >= 1ms). Only log the update if it does. */
		if (GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts))
		{
			GstClockTimeDiff update_delta = GST_CLOCK_DIFF(ring_buffer->oldest_frame_pts, oldest_frame_pts);

			if (ABS(update_delta) > (1 * GST_MSECOND))
			{
				GST_INFO_OBJECT(
					ring_buffer,
					"updating oldest frame PTS from: %" GST_TIME_FORMAT " to: %" GST_TIME_FORMAT " (delta: %" GST_STIME_FORMAT ")",
					GST_TIME_ARGS(ring_buffer->oldest_frame_pts),
					GST_TIME_ARGS(oldest_frame_pts),
					GST_STIME_ARGS(update_delta)
				);
			}
		}

		ring_buffer->oldest_frame_pts = oldest_frame_pts;
	}

	return num_frames_to_write;
}


GstPwAudioRingBufferRetrievalResult gst_pw_audio_ring_buffer_retrieve_frames(
	GstPwAudioRingBuffer *ring_buffer,
	gpointer destination,
	gsize num_frames_to_retrieve,
	GstClockTime retrieval_pts,
	GstClockTime ring_buffer_data_pts_shift,
	GstClockTimeDiff skew_threshold,
	GstClockTimeDiff *buffered_frames_to_retrieval_pts_delta
)
{
	GstPwAudioRingBufferRetrievalResult retval = GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK;
	guint64 read_lengths[2];
	guint64 actual_num_frames_to_retrieve;
	GstClockTime expected_retrieval_duration;
	GstClockTime actual_retrieval_duration;

	g_assert(ring_buffer != NULL);
	g_assert(destination != NULL);
	g_assert(num_frames_to_retrieve > 0);
	g_assert(GST_CLOCK_TIME_IS_VALID(ring_buffer_data_pts_shift));
	g_assert(skew_threshold >= 0);
	g_assert(buffered_frames_to_retrieval_pts_delta != NULL);

	*buffered_frames_to_retrieval_pts_delta = 0;

	if (G_UNLIKELY(ring_buffer->metrics.current_num_buffered_frames == 0))
	{
		g_assert(ring_buffer->current_fill_level == 0);
		retval = GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_RING_BUFFER_IS_EMPTY;
		goto finish;
	}

	expected_retrieval_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(ring_buffer->format), num_frames_to_retrieve);

	actual_num_frames_to_retrieve = MIN(num_frames_to_retrieve, ring_buffer->metrics.current_num_buffered_frames);
	actual_retrieval_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(ring_buffer->format), actual_num_frames_to_retrieve);

	if (GST_CLOCK_TIME_IS_VALID(retrieval_pts) && GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts))
	{
		/* All required timestamps are valid. We can synchronize
		 * the retrieval against the oldest_frame_pts. */

		/* These PTS define two "windows":
		 *
		 * - The "retrieval window" - the timespan that starts at
		 *   retrieval_window_start_pts and ends at retrieval_window_end_pts.
		 *   The user is interested in the frames that lie in this timespan.
		 *   If there aren't sufficient frames inside this timespan to
		 *   completely fill it, silence frames are added to pad the output.
		 * - The "buffered frames window" - the timespan that starts at
		 *   buffered_frames_start_pts and ends at buffered_frames_end_pts.
		 *   This is the timespan that covers all buffered frames.
		 *
		 * The goal here is to (a) determine the intersection between
		 * these two windows and (b) see if any silence frames need
		 * to be added. The intersection defines the block of frames
		 * that has to be retrieved from the ring buffer.
		 */
		GstClockTime retrieval_window_start_pts = retrieval_pts;
		GstClockTime retrieval_window_end_pts = retrieval_window_start_pts + expected_retrieval_duration;
		GstClockTime buffered_frames_start_pts = ring_buffer->oldest_frame_pts + ring_buffer_data_pts_shift;
		GstClockTime buffered_frames_end_pts = buffered_frames_start_pts + ring_buffer->current_fill_level;

		GST_LOG_OBJECT(
			ring_buffer,
			"retrieval window: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT "  "
			"buffered frames window: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT "  "
			"stride: %" G_GSIZE_FORMAT "  "
			"num buffered frames: %" G_GUINT64_FORMAT "  "
			"fill level: %" GST_TIME_FORMAT "  "
			"expected / actual num frames to retrieve: %" G_GSIZE_FORMAT " / %" G_GSIZE_FORMAT "  "
			"expected / actual retrieval duration: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT "  "
			"ring buffer data PTS shift: %" GST_TIME_FORMAT,
			GST_TIME_ARGS(retrieval_window_start_pts), GST_TIME_ARGS(retrieval_window_end_pts),
			GST_TIME_ARGS(buffered_frames_start_pts), GST_TIME_ARGS(buffered_frames_end_pts),
			ring_buffer->stride,
			ring_buffer->metrics.current_num_buffered_frames,
			GST_TIME_ARGS(ring_buffer->current_fill_level),
			num_frames_to_retrieve,
			actual_num_frames_to_retrieve,
			GST_TIME_ARGS(expected_retrieval_duration),
			GST_TIME_ARGS(actual_retrieval_duration),
			GST_TIME_ARGS(ring_buffer_data_pts_shift)
		);

		if (retrieval_window_end_pts < buffered_frames_start_pts)
		{
			/* Simplest case: None of the data can be played yet, since the
			 * buffered frames window is entirely in the future relative to the
			 * retrieval window - there is no intersection between these two.
			 * The buffered frames are fully valid, but can't be used yet. */

			GST_DEBUG_OBJECT(
				ring_buffer,
				"buffered frames window is entirely in the future - cannot retrieve any frames yet"
			);

			gst_pw_audio_format_write_silence_frames(
				&(ring_buffer->format),
				((guint8 *)destination),
				num_frames_to_retrieve
			);

			retval = GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE;
			goto finish;
		}
		else if (retrieval_window_start_pts >= buffered_frames_end_pts)
		{
			/* The entire content of the ring buffer is invalid since it expired.
			 * The buffered frames window is entirely in the past relative to the
			 * retrieval window - there is no intersection between these two.
			 * Since the buffered frames are all expired, they will never be played,
			 * and have to be flushed. Do this by resetting the ring buffer to
			 * the empty state (which is its initial state). */

			GST_DEBUG_OBJECT(
				ring_buffer,
				"buffered frames window is entirely in the past - all frames have expired"
			);

			gst_pw_audio_format_write_silence_frames(
				&(ring_buffer->format),
				((guint8 *)destination),
				num_frames_to_retrieve
			);

			retval = GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST;
			goto reset_to_empty_state;
		}
		else
		{
			GstClockTime silence_length = 0;
			GstClockTime duration_of_expired_buffered_frames = 0;
			GstClockTimeDiff pts_delta, median_pts_delta;
			gsize num_frames_with_extra_padding;
			guint64 read_offset;
			guint64 total_lengths;
			gsize num_silence_frames_to_prepend = 0;
			gsize num_silence_frames_to_append = 0;
			guint8 *dest_ptr = destination;

			/* At this point it is clear that at least *some* of the buffered frames lie
			 * within the retrieval window. In here, the code takes care of calculating
			 * the delta between the PTS of the oldest buffered frame and the retrieval
			 * PTS. This is useful for clock drift compensation; if the clock of the
			 * PipeWire graph driver and the pipeline clock drift apart, then so will
			 * the distance between these two PTS. If for example the driver's clock is
			 * slower, then fewer frames will be consumed per second, frames stay for
			 * longer in the ring buffer, and the oldest framé PTS is incremented
			 * less often. If the driver's clock is instead faster, then the oldest frame
			 * PTS is incremented faster etc. We also apply a small 3-number median
			 * filter on the raw PTS delta to weed out occasional  outliers that could
			 * mislead the code further below into skipping franes / insert silence when
			 * it isn't actually needed. */

			pts_delta = GST_CLOCK_DIFF(buffered_frames_start_pts, retrieval_window_start_pts);

			/* Apply the median filter. Special cases:
			 *
			 * - PTS delta history is empty, and will get its first value: Just use that
			 *   one as the "filtered" value. This is actually not filtered at all, but
			 *   it is useful to have a quantity right at the very beginning.
			 *
			 * - PTS delta history has 1 value, now gets a second one: Calculate their
			 *   average, then return that as the filtered value.
			 *
			 * Once the history has 2 values already, a 3-value median can be computed.
			 */
			switch (ring_buffer->num_pts_delta_history_entries)
			{
				case 0:
					ring_buffer->pts_delta_history[0] = pts_delta;
					ring_buffer->num_pts_delta_history_entries++;
					median_pts_delta = pts_delta;
					break;

				case 1:
					ring_buffer->pts_delta_history[1] = pts_delta;
					ring_buffer->num_pts_delta_history_entries++;
					median_pts_delta = (pts_delta + ring_buffer->pts_delta_history[0]) / 2;
					break;

				case 2:
					ring_buffer->pts_delta_history[2] = pts_delta;
					ring_buffer->num_pts_delta_history_entries++;
					median_pts_delta = calculate_3_value_median(ring_buffer->pts_delta_history);
					break;

				default:
					memmove(&(ring_buffer->pts_delta_history[0]), &(ring_buffer->pts_delta_history[1]), sizeof(GstClockTimeDiff) * (GST_PW_AUDIO_RING_BUFFER_PTS_DELTA_HISTORY_SIZE - 1));
					ring_buffer->pts_delta_history[GST_PW_AUDIO_RING_BUFFER_PTS_DELTA_HISTORY_SIZE - 1] = pts_delta;
					median_pts_delta = calculate_3_value_median(ring_buffer->pts_delta_history);
			}

			/* We need to distinguish between two cases:
			 *
			 * - Buffered frames window is partially in the past relative to the
			 *   retrieval window. The oldest N frames in the ring buffer have
			 *   expired and must be discarded. The buffered frames window's start
			 *   PTS will be older than the retrieval window's PTS in this case.
			 * - Buffered frames window is partially in the future relative to the
			 *   retrieval window. The first N frames in the retrieval window need
			 *   to be filled with silence to cover the time until the actual
			 *   frames that are about to be played. The buffered frames window's
			 *   start PTS will be newer than the retrieval window's PTS in this case.
			 *
			 * (While we do insert silence frames and/or throw away the oldest N
			 * frames, the calculation intiially is based on nanoseconds, since
			 * the initial comparison is done with the PTS.)
			 *
			 * These two cases modify the contents of the ring buffer by adding
			 * and removing samples. This is referred to as "skewing", and only
			 * happens if the absolute value of median_pts_delta exceeds the
			 * skew_threshold. This is very important, since PTS can (and usually
			 * do) have a degree of jitter that mostly cancels itself out over time.
			 * Without the threshold, we'd be skewing the signal all the time
			 * unnecessarily. As a side effect, if there really is a big drift,
			 * skewing corrects it rapidly.
			 *
			 * We use the median-filtered PTS delta, not the raw PTS delta.
			 * The raw one occasionally can have big outliers that must be
			 * ignored, otherwise they cause glitches. But if the _filtered_
			 * PTS delta lies beyond the skew threshold, also reset the PTS
			 * delta history, since otherwise, now-stale values would be used.
			 */
			if (median_pts_delta < (-skew_threshold))
			{
				silence_length = -median_pts_delta;
				ring_buffer->num_pts_delta_history_entries = 0;
			}
			else if (median_pts_delta > (+skew_threshold))
			{
				duration_of_expired_buffered_frames = median_pts_delta;
				ring_buffer->num_pts_delta_history_entries = 0;
			}
			else
			{
				/* We set this quantity only if no skewing was performed; otherwise, the
				 * delta may mistakenly get factored in twice (once by the skewing, another
				 * time by the caller, who for example feeds the delta into a PID controller). */
				*buffered_frames_to_retrieval_pts_delta = median_pts_delta;
			}

			/* Silence needs to be prepended if the frames lie in the future
			 * but are still within the retrieval window. */
			if (silence_length > 0)
			{
				guint64 num_frames_with_silence_prepended;

				num_silence_frames_to_prepend = gst_pw_audio_format_calculate_num_frames_from_duration(&(ring_buffer->format), silence_length);

				GST_DEBUG_OBJECT(
					ring_buffer,
					"prepending %f ms (=%" G_GSIZE_FORMAT " frame(s)) of silence",
					silence_length / ((gdouble)GST_MSECOND),
					num_silence_frames_to_prepend
				);

				/* Check if prepending silence frames would result in a sum total
				 * of frames that exceeds the requested amount. If so, we have to
				 * reduce the number of frames we are going to retrieve from the
				 * ring buffer. */
				num_frames_with_silence_prepended = actual_num_frames_to_retrieve + num_silence_frames_to_prepend;
				if (num_frames_with_silence_prepended > num_frames_to_retrieve)
				{
					guint64 num_excess_frames = num_frames_with_silence_prepended - num_frames_to_retrieve;
					g_assert(num_excess_frames <= actual_num_frames_to_retrieve);
					actual_num_frames_to_retrieve -= num_excess_frames;
					actual_retrieval_duration -= silence_length;
				}
			}

			/* Expired frames must be thrown away. We do that by flushing those
			 * from the ring buffer. Also adjust actual_retrieval_duration,
			 * actual_retrieval_duration, and oldest_frame_pts to account for
			 * the length of the discarded frames. */
			if (duration_of_expired_buffered_frames > 0)
			{
				gsize advance_amount;
				gsize num_frames_to_flush = gst_pw_audio_format_calculate_num_frames_from_duration(&(ring_buffer->format), duration_of_expired_buffered_frames);

				g_assert(num_frames_to_flush <= ring_buffer->metrics.current_num_buffered_frames);

				GST_DEBUG_OBJECT(
					ring_buffer,
					"the first %f ms (=%" G_GSIZE_FORMAT " frame(s)) in the ring buffer are expired; skipping",
					duration_of_expired_buffered_frames / ((gdouble)GST_MSECOND),
					num_frames_to_flush
				);

				num_frames_to_flush = MIN(num_frames_to_flush, actual_num_frames_to_retrieve);

				/* "Flush" by advancing the read pointer. */
				advance_amount = ringbuffer_metrics_flush(&(ring_buffer->metrics), num_frames_to_flush);
				g_assert(advance_amount == num_frames_to_flush);

				if (GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts))
				{
					/* The oldest_frame_pts must be updated by the duration that is _actually_
					 * flushed. This can be less than the originally requested duration, which
					 * is duration_of_expired_buffered_frames. see the MIN() macro call above.
					 * In cases where silence was prepended, this can happen. If we do not
					 * take this into account, oldest_frame_pts is advanced by a too high
					 * duration, and thus causes a significant sudden drift. */
					GstClockTime flushed_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(ring_buffer->format), num_frames_to_flush);
					GstClockTime updated_pts = ring_buffer->oldest_frame_pts + flushed_duration;

					GST_DEBUG_OBJECT(
						ring_buffer,
						"updating oldest queued data PTS: %" GST_TIME_FORMAT " -> %" GST_TIME_FORMAT " (flushed duration: %" GST_TIME_FORMAT ")",
						GST_TIME_ARGS(ring_buffer->oldest_frame_pts),
						GST_TIME_ARGS(updated_pts),
						GST_TIME_ARGS(flushed_duration)
					);

					ring_buffer->oldest_frame_pts = updated_pts;
				}

				/* Update these quantities since they were calculated with the now-flushed frames included. */
				actual_num_frames_to_retrieve = MIN(num_frames_to_retrieve, ring_buffer->metrics.current_num_buffered_frames);
				actual_retrieval_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(ring_buffer->format), actual_num_frames_to_retrieve);
			}

			if (G_UNLIKELY(actual_num_frames_to_retrieve == 0))
			{
				GST_DEBUG_OBJECT(
					ring_buffer,
					"buffered frames window is (partially) in the present, but all data"
					" that could be put in retrieved buffer was clipped"
				);

				gst_pw_audio_format_write_silence_frames(
					&(ring_buffer->format),
					((guint8 *)destination),
					num_frames_to_retrieve
				);

				/* Note that we do _not_ jump to reset_to_empty_state, since the ring
				 * buffer might still have valid content, we just clipped the frames
				 * that originally were slated to be extracted. */
				retval = GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_ALL_DATA_FOR_BUFFER_CLIPPED;
				goto finish;
			}

			/* If after all of the processing earlier the remaining number of frames
			 * is less than the required amount, we append silence frames to ensure
			 * that the output has that the requested number of frames. */
			num_frames_with_extra_padding = num_silence_frames_to_prepend + actual_num_frames_to_retrieve;
			num_silence_frames_to_append = (num_frames_to_retrieve > num_frames_with_extra_padding) ? (num_frames_to_retrieve - num_frames_with_extra_padding) : 0;

			GST_LOG_OBJECT(
				ring_buffer,
				"buffered frames window is (partially) in the present; buffered frames to retrieval PTS delta: %" G_GINT64_FORMAT,
				*buffered_frames_to_retrieval_pts_delta
			);

			/* Finally, get the read positions and actually extract frames
			 * from the ring buffer. */ 

			total_lengths = ringbuffer_metrics_read(&(ring_buffer->metrics), actual_num_frames_to_retrieve, &read_offset, read_lengths);
			g_assert(total_lengths == actual_num_frames_to_retrieve);

			if (num_silence_frames_to_prepend > 0)
			{
				gst_pw_audio_format_write_silence_frames(
					&(ring_buffer->format),
					dest_ptr,
					num_silence_frames_to_prepend
				);

				dest_ptr += num_silence_frames_to_prepend * ring_buffer->stride;
			}

			if (read_lengths[0] > 0)
			{
				memcpy(
					dest_ptr,
					ring_buffer->buffered_frames + read_offset * ring_buffer->stride,
					read_lengths[0] * ring_buffer->stride
				);

				dest_ptr += read_lengths[0] * ring_buffer->stride;
			}
			if (read_lengths[1] > 0)
			{
				memcpy(
					dest_ptr,
					ring_buffer->buffered_frames,
					read_lengths[1] * ring_buffer->stride
				);

				dest_ptr += read_lengths[1] * ring_buffer->stride;
			}

			/* Append the silence frames if necessary. */
			if (num_silence_frames_to_append > 0)
			{
				gst_pw_audio_format_write_silence_frames(
					&(ring_buffer->format),
					dest_ptr,
					num_silence_frames_to_append
				);
			}
		}
	}
	else
	{
		/* Either, the oldest frame PTS is not set, or the user does not want
		 * synchronized retrieval (indicated by an invalid retrieval_pts value).
		 * The latter case typically happens because the output is already synced,
		 * or because synced output is turned off. Behave like a simple buffer in
		 * these cases. */

		guint64 read_offset;
		guint64 total_lengths;

		total_lengths = ringbuffer_metrics_read(&(ring_buffer->metrics), actual_num_frames_to_retrieve, &read_offset, read_lengths);
		g_assert(total_lengths == actual_num_frames_to_retrieve);

		if (read_lengths[0] > 0)
		{
			memcpy(
				destination,
				ring_buffer->buffered_frames + read_offset * ring_buffer->stride,
				read_lengths[0] * ring_buffer->stride
			);
		}
		if (read_lengths[1] > 0)
		{
			memcpy(
				((guint8 *)destination) + read_lengths[0] * ring_buffer->stride,
				ring_buffer->buffered_frames,
				read_lengths[1] * ring_buffer->stride
			);
		}

		if (actual_num_frames_to_retrieve < num_frames_to_retrieve)
		{
			gst_pw_audio_format_write_silence_frames(
				&(ring_buffer->format),
				((guint8 *)destination) + actual_num_frames_to_retrieve * ring_buffer->stride,
				num_frames_to_retrieve - actual_num_frames_to_retrieve
			);
		}

		GST_LOG_OBJECT(
			ring_buffer,
			"retrieving frames without sync;  "
			"stride: %" G_GSIZE_FORMAT "  "
			"read / write positions: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "  "
			"num buffered frames: %" G_GUINT64_FORMAT "  "
			"fill level: %" GST_TIME_FORMAT "  "
			"expected / actual num frames to retrieve: %" G_GSIZE_FORMAT " / %" G_GSIZE_FORMAT "  "
			"expected / actual retrieval duration: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT,
			ring_buffer->stride,
			ring_buffer->metrics.read_position, ring_buffer->metrics.write_position,
			ring_buffer->metrics.current_num_buffered_frames,
			GST_TIME_ARGS(ring_buffer->current_fill_level),
			num_frames_to_retrieve,
			actual_num_frames_to_retrieve,
			GST_TIME_ARGS(expected_retrieval_duration),
			GST_TIME_ARGS(actual_retrieval_duration)
		);
	}

	if (GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts))
	{
		/* Increment the oldest PTS since we just retrieved the oldest frame(s).
		 * That way, this timestamp remains valid for future retrievals. */
		ring_buffer->oldest_frame_pts += actual_retrieval_duration;
	}

	ring_buffer->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(
		&(ring_buffer->format),
		ring_buffer->metrics.current_num_buffered_frames
	);

finish:
	return retval;

reset_to_empty_state:
	gst_pw_audio_ring_buffer_flush(ring_buffer);
	goto finish;
}

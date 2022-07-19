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
 * SECTION:gstpwaudioqueue
 * @title: GstPwAudioQueue
 * @short_description: Queue for audio data, used by GStreamer pipewire audio elements.
 *
 * #GstPwAudioQueue provides a queue that stores audio data and allows for retrieving data
 * at the right moment (depending on input buffer timestamps, current time, and audio type).
 *
 * The underlying data structure can either be a #GstAdapter (for contiguous audio types)
 * or a #GstQueueArray (for packetized audio types). See #GstPwAudioFormat for details
 * about contiguous and packetized audio data types.
 *
 * The behavior of the queue with respects to the timestamps depends on whether the audio
 * type is contiguous or packetized. With contiguous types, it is possible to retrieve
 * partial bits of data and synchronize the output of this queue against the current time
 * of a clock. For example, if there's 400ms worth of queued data in the internal #GstAdapter,
 * the user wants to retrieve 50ms worth of data, and the timestamps indicate that the queued
 * data is 20ms into the future, then gst_pw_audio_queue_retrieve_buffer() will produce a
 * #GstBuffer with 30ms of silence, followed by the oldest 20ms of the queued data. If
 * instead the queue is filled with data now, but the timestamps indicate that the queued data
 * needs to be played 1 second from now, gst_pw_audio_queue_retrieve_buffer() will not retrieve
 * data, and instead inform the user that the data fully lies in the future. If according to
 * the timestamps the queued data is already fully expired, the queue is purged, and the user
 * is told that all of that data was too old and got discarded.
 *
 * With packetized types, this is not possible, and timestamps are ignored. This structure
 * then behaves just like an ordinary #GstBuffer queue. This is because silence cannot be
 * inserted in compressed audio, and compressed frames cannot be subdivided to provide
 * partial data.
 *
 * Access is not inherently MT safe. Using synchronization primitives is advised.
 */

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/audio/audio.h>

#include "gstpwaudioqueue.h"

GST_DEBUG_CATEGORY(pw_audio_queue_debug);
#define GST_CAT_DEFAULT pw_audio_queue_debug


/* How many frames the silence_frames_buffer contains. */
#define SILENCE_FRAMES_BUFFER_LENGTH 256


struct _GstPwAudioQueuePrivate
{
	/* PTS of the oldest data in the queue. This is initially invalid, and
	 * set to the PTS of the first buffer that is pushed into the queue.
	 * Later, when data is retrieved from the queue, this is updated, since
	 * the oldest data is retrieved first.
	 * This timestamp is needed when the stream has not started yet and needs
	 * to start at the right moment (= when the clock reaches this timestamp).
	 * The queued data may lie entirely in the future when this timestamp is
	 * queried, or entirely in the past etc. gst_pw_audio_queue_retrieve_buffer()
	 * checks for these cases and behaves depending on the value of this timestamp.
	 * Only used with contiguous data. */
	GstClockTime oldest_queued_data_pts;

	/* Currently set audio format. Only valid if format_initialized is set to TRUE. */
	GstPwAudioFormat format;
	/* TRUE if the format field was set to a valid value. Currently only used for assertions. */
	gboolean format_initialized;

	gboolean is_contiguous;

	GstQueueArray *packetized_audio_buffer_queue;
	GstAdapter *contiguous_audio_buffer_queue;

	GstBuffer *silence_frames_buffer;
};


G_DEFINE_TYPE_WITH_PRIVATE(GstPwAudioQueue, gst_pw_audio_queue, GST_TYPE_OBJECT)


static void gst_pw_audio_queue_dispose(GObject *object);

static void gst_pw_audio_queue_clear_packetized_queue(GstPwAudioQueue *queue);


static void gst_pw_audio_queue_class_init(GstPwAudioQueueClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = GST_DEBUG_FUNCPTR(gst_pw_audio_queue_dispose);

	GST_DEBUG_CATEGORY_INIT(pw_audio_queue_debug, "pwaudioqueue", 0, "GStreamer PipeWire audio queue");
}


static void gst_pw_audio_queue_init(GstPwAudioQueue *self)
{
	self->current_fill_level = 0;

	self->priv = gst_pw_audio_queue_get_instance_private(self);

	self->priv->oldest_queued_data_pts = GST_CLOCK_TIME_NONE;

	self->priv->format_initialized = FALSE;

	self->priv->is_contiguous = FALSE;

	self->priv->packetized_audio_buffer_queue = gst_queue_array_new(0);
	g_assert(self->priv->packetized_audio_buffer_queue != NULL);
#if GST_CHECK_VERSION(1, 16, 0)
	gst_queue_array_set_clear_func(self->priv->packetized_audio_buffer_queue, (GDestroyNotify)gst_buffer_unref);
#endif

	self->priv->contiguous_audio_buffer_queue = gst_adapter_new();

	self->priv->silence_frames_buffer = NULL;
}


static void gst_pw_audio_queue_dispose(GObject *object)
{
	GstPwAudioQueue *self = GST_PW_AUDIO_QUEUE(object);

	if (self->priv->packetized_audio_buffer_queue != NULL)
	{
		/* In GStreamer versions older than 1.16, GstQueueArray does not support
		 * a "clear function" that could be used by it for deallocating items
		 * in the queue prior to freeing the queue's memory, so we have to
		 * manually unref all the buffers before freeing the queue. */
#if !GST_CHECK_VERSION(1, 16, 0)
		gst_pw_audio_queue_clear_packetized_queue(self);
#endif
		gst_queue_array_free(self->priv->packetized_audio_buffer_queue);
		self->priv->packetized_audio_buffer_queue = NULL;
	}

	if (self->priv->contiguous_audio_buffer_queue != NULL)
	{
		gst_object_unref(G_OBJECT(self->priv->contiguous_audio_buffer_queue));
		self->priv->contiguous_audio_buffer_queue = NULL;
	}

	gst_buffer_replace(&(self->priv->silence_frames_buffer), NULL);

	G_OBJECT_CLASS(gst_pw_audio_queue_parent_class)->dispose(object);
}


static void gst_pw_audio_queue_clear_packetized_queue(GstPwAudioQueue *queue)
{
	/* gst_queue_array_clear() was introduced in GStreamer 1.16. For older
	 * GStreamer versions, we have to replicate that function's behavior
	 * by retrieving each gstbuffer manually from the queue. */
#if GST_CHECK_VERSION(1, 16, 0)
	gst_queue_array_clear(queue->priv->packetized_audio_buffer_queue);
#else
	while (!gst_queue_array_is_empty(queue->priv->packetized_audio_buffer_queue))
	{
		GstBuffer *gstbuffer = (GstBuffer *)gst_queue_array_pop_head(queue->priv->packetized_audio_buffer_queue);
		gst_buffer_unref(gstbuffer);
	}
#endif
}


/**
 * gst_pw_audio_queue_new:
 *
 * Create a new #GstPwAudioQueue.
 *
 * Note that you must set a valid format with gst_pw_audio_queue_set_format()
 * before this queue can actually be used.
 *
 * Returns: (transfer full): new #GstPwAudioQueue instance.
 */
GstPwAudioQueue* gst_pw_audio_queue_new(void)
{
	GstPwAudioQueue* queue = g_object_new(gst_pw_audio_queue_get_type(), NULL);

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(queue));

	return queue;
}


/**
 * gst_pw_audio_queue_set_format:
 * @queue: a #GstPwAudioQueue
 * @format: #GstPwAudioFormat to use
 *
 * Sets a new format for this queue.
 * This must be called before any data is pushed or retrieved.
 *
 * This call implicitly flushes the queue before setting the new format,
 * since any old queued data is unusable due to format mismatch.
 *
 * An internal copy of the format structure is made; the supplied #GstPwAudioFormat
 * pointer does not have to stay valid after this call.
 */
void gst_pw_audio_queue_set_format(GstPwAudioQueue *queue, GstPwAudioFormat *format)
{
	gchar *format_as_string;

	g_assert(queue != NULL);
	g_assert(format != NULL);

	format_as_string = gst_pw_audio_format_to_string(format);
	GST_DEBUG_OBJECT(queue, "setting queue format to:  %s", format_as_string);
	g_free(format_as_string);

	gst_pw_audio_queue_flush(queue);

	memcpy(&(queue->priv->format), format, sizeof(GstPwAudioFormat));

	queue->priv->is_contiguous = gst_pw_audio_format_data_is_contiguous(format->audio_type);
	queue->priv->format_initialized = TRUE;

	if (queue->priv->is_contiguous)
	{
		GstMapInfo map_info;

		gst_buffer_replace(&(queue->priv->silence_frames_buffer), NULL);

		queue->priv->silence_frames_buffer = gst_buffer_new_allocate(
			NULL,
			SILENCE_FRAMES_BUFFER_LENGTH * gst_pw_audio_format_get_stride(&(queue->priv->format)),
			NULL
		);

		gst_buffer_map(queue->priv->silence_frames_buffer, &map_info, GST_MAP_WRITE);
		gst_pw_audio_format_write_silence_frames(&(queue->priv->format), map_info.data, SILENCE_FRAMES_BUFFER_LENGTH);
		gst_buffer_unmap(queue->priv->silence_frames_buffer, &map_info);
	}
}


/**
 * gst_pw_audio_queue_flush:
 * @queue: a #GstPwAudioQueue
 *
 * Flushes the entire content of this queue.
 */
void gst_pw_audio_queue_flush(GstPwAudioQueue *queue)
{
	g_assert(queue != NULL);

	if (queue->priv->is_contiguous)
	{
		if (gst_adapter_available(queue->priv->contiguous_audio_buffer_queue) > 0)
		{
			GST_DEBUG_OBJECT(queue, "flushing non-empty contiguous audio buffer queue");
			gst_adapter_clear(queue->priv->contiguous_audio_buffer_queue);
		}
	}
	else
	{
		if (!gst_queue_array_is_empty(queue->priv->packetized_audio_buffer_queue))
		{
			GST_DEBUG_OBJECT(queue, "flushing non-empty packetized audio buffer queue");
			gst_pw_audio_queue_clear_packetized_queue(queue);
		}
	}

	queue->current_fill_level = 0;
	queue->priv->oldest_queued_data_pts = GST_CLOCK_TIME_NONE;
}


/**
 * gst_pw_audio_queue_push_silence_frames:
 * @queue: a #GstPwAudioQueue
 * @num_silence_frames: How many silence frames to push
 *
 * Adds (pushes) silence frames to the end of the queue.
 * 
 * This function can only be used if the audio type of the queued data is contiguous.
 */
void gst_pw_audio_queue_push_silence_frames(GstPwAudioQueue *queue, gsize num_silence_frames)
{
	gsize num_written_frames = 0;
	gsize stride;

	g_assert(queue != NULL);

	if (G_UNLIKELY(!queue->priv->is_contiguous))
	{
		GST_ERROR_OBJECT(queue, "cannot insert silence frames into non-contiguous data");
		return;
	}

	stride = gst_pw_audio_format_get_stride(&(queue->priv->format));

	while (num_written_frames < num_silence_frames)
	{
		gsize num_remaining_frames_to_write = num_silence_frames - num_written_frames;

		if (num_remaining_frames_to_write >= SILENCE_FRAMES_BUFFER_LENGTH)
		{
			gst_adapter_push(queue->priv->contiguous_audio_buffer_queue, gst_buffer_ref(queue->priv->silence_frames_buffer));
			num_written_frames += SILENCE_FRAMES_BUFFER_LENGTH;
			GST_LOG_OBJECT(
				queue,
				"inserted %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT " silence frames into queue",
				(gsize)SILENCE_FRAMES_BUFFER_LENGTH,
				num_silence_frames
			);
		}
		else
		{
			gst_adapter_push(
				queue->priv->contiguous_audio_buffer_queue,
				gst_buffer_copy_region(
					queue->priv->silence_frames_buffer,
					GST_BUFFER_COPY_MEMORY,
					0,
					num_remaining_frames_to_write * stride
				)
			);
			num_written_frames += num_remaining_frames_to_write;
			GST_LOG_OBJECT(
				queue,
				"inserted %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT " silence frames into queue",
				num_remaining_frames_to_write,
				num_silence_frames
			);
		}
	}

	queue->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(
		&(queue->priv->format),
		gst_adapter_available(queue->priv->contiguous_audio_buffer_queue) / stride
	);
}


/**
 * gst_pw_audio_queue_push_buffer:
 * @queue: a #GstPwAudioQueue
 * @buffer: (transfer full): a #GstBuffer whose data shall be pushed to the end of the queue
 *
 * Adds (pushes) data to the end of the queue.
 * If the buffer has a valid PTS, and no other buffer with valid PTS was pushed
 * in yet,  or if the last gst_pw_audio_queue_retrieve_buffer() call returned
 * GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_QUEUE_IS_EMPTY, then the buffer's PTS
 * will be used as the PTS for syncing the start of playback. That is, the next
 * gst_pw_audio_queue_retrieve_buffer() will use this PTS for synced output.
 * If the buffer's PTS is not valid, none of this is done.
 */
void gst_pw_audio_queue_push_buffer(GstPwAudioQueue *queue, GstBuffer *buffer)
{
	GstClockTime buffer_pts;

	g_assert(queue != NULL);
	g_assert(buffer != NULL);
	g_assert(queue->priv->format_initialized);

	buffer_pts = GST_BUFFER_PTS(buffer);

	if (queue->priv->is_contiguous)
	{
		GST_LOG_OBJECT(queue, "pushing gstbuffer into contiguous queue: %" GST_PTR_FORMAT, (gpointer)buffer);
		gst_adapter_push(queue->priv->contiguous_audio_buffer_queue, buffer);
		queue->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(
			&(queue->priv->format),
			gst_adapter_available(queue->priv->contiguous_audio_buffer_queue) / gst_pw_audio_format_get_stride(&(queue->priv->format))
		);
	}
	else
	{
		GST_LOG_OBJECT(queue, "pushing gstbuffer into packetized queue: %" GST_PTR_FORMAT, (gpointer)buffer);
		queue->current_fill_level += GST_BUFFER_DURATION(buffer);
		gst_queue_array_push_tail(queue->priv->packetized_audio_buffer_queue, buffer);
	}

	/* If the oldest PTS is not set yet, do so now. We'll use that PTS in
	 * gst_pw_audio_queue_retrieve_buffer() to know if, when, and how much
	 * of the queued data we need to access. (If the data isn't contiguous
	 * though this timestamp is not used by that function.) */
	if (!GST_CLOCK_TIME_IS_VALID(queue->priv->oldest_queued_data_pts))
		queue->priv->oldest_queued_data_pts = buffer_pts;
}


/**
 * gst_pw_audio_queue_retrieve_buffer:
 * @queue: a #GstPwAudioQueue
 * @min_num_output_frames: Minimum number of output frames required.
 * @ideal_num_output_frames: How many frames the output should ideally contain.
 * @retrieval_pts: Timestamp of the data to retrieve. Can be #GST_CLOCK_TIME_NONE
 *    to indicate that no synchronized retrieval is needed.
 * @queued_data_pts_shift: By how much to shift the internal PTS that is
 *    associated with the queued data while computing which frames to retrieve.
 * @retrieval_details: #GstPwAudioQueueRetrievalDetails structure to fill
 *    with details about the retrieved data.
 *
 * Retrieves audio data from this queue. At least min_num_output_frames frames
 * are retrieved. (See #GstPwAudioFormat for an explanation about what frames are.)
 * The function tries to retrieve ideal_num_output_frames frames, but if fewer
 * are available, it will get those. If there are fewer frames available than
 * min_num_output_frames requires, this function computes a number of silence
 * frames to append to reach that minimum frame count. (More on that below.)
 *
 * The retrieved bytes are available as a #GstBuffer that is passed to
 * the retrieval_details' retrieved_buffer field.
 *
 * Furthermore, this function is capable of determining if and how much queued
 * data can be retrieved based on the current time and the PTS from buffers that
 * were passed to gst_pw_audio_queue_push_buffer() earlier. The queue keeps track
 * of the PTS that is associated with the oldest queued data. When retrieving,
 * this oldest data is the one that is accessed first. Consequently, the retrieved
 * data is associated with that oldest PTS. After the retrieval, the internal
 * stored oldest PTS is updated to account for the retrieved data.
 *
 * This oldest PTS together with the total amount of queued data form the
 * "queued data window". That's the window when data is available for retrieval.
 * retrieval_pts and ideal_num_output_frames form "output window". That's the
 * time window of the data the caller is interested in. Actual data retrieval
 * happens when these two windows intersect. This function computes that intersection,
 * and if there is one, performs the retrieval of that intersection.
 * 
 * The window intersection may reveal that only a subset of the output window
 * can get actual data from the queue. There are two possible types of partial
 * window intersections:
 *
 * 1. Queued data window lies in the future relative to the output window.
 *    In other words, the queued data lies in the future, but the first N
 *    queued bytes can be retrieved, because they still lie within the output
 *    window. However, the duration between the start of the output window and
 *    the start of the queued data window needs to be filled with silence.
 *    That silence must be prepended to the retrieved data.
 * 2. Queued data window lies in the past relative to the output window.
 *    In other words, the queued data lies in the past, but the last N
 *    queued bytes can be retrieved, because they still lie within the output
 *    window. However, the other, older queued bytes are all expired since
 *    they fall outside of the output window, so this function needs to flush
 *    those bytes.
 *
 * Additionally, there are two possible cases when no data can be retrieved
 * even if the queue isn't empty because the windows do _not_ intersect:
 *
 * 3. Queued data window is completely in the future relative to the output window.
 *    This means that all of the queued data is too far in the future and none
 *    of it can be retrieved yet.
 * 4. Queued data window is completely in the past relative to the output window.
 *    All of the data is expired and unusuable. This function then flushes all
 *    of the queued data.
 *
 * As mentioned above, silence is prepended in case #1 above. Additionally, if
 * the retrieved data is less than what is requested by min_num_output_frames,
 * silence is _appended_ to the retrieved data to cover the remaining amount
 * to get min_num_output_frames worth of data.
 *
 * For efficiency reasons, prepending and appending silence is not done via
 * reallocations, writing silence samples into memory regions etc. Instead, the
 * #GstPwAudioQueueRetrievalDetails contains the num_silence_frames_to_prepend
 * and num_silence_frames_to_append fields, which are set to the number of
 * silence frames the caller shall prepend/append. This is more efficient in
 * this case, since PipeWire's pw_stream regularly invokes the on_process_stream()
 * callback, and that callback is given memory blocks to write audio into.
 * Prepending silence is accomplished by filling the first N bytes of those
 * memory blocks with silence (if num_silence_frames_to_prepend is nonzero),
 * then writing the contents of retrieved_buffer into those blocks, and finally,
 * writing silence after the retrieved_buffer data
 * (if num_silence_frames_to_append is nonzero).
 *
 * queued_data_pts_shift shifts the queued data window into the future.
 * This is needed if there are additional latencies that aren't known until
 * right before this function is called. pw_stream delay is one example.
 * Such latencies cannot be baked into the PTS - they have to be applied
 * later. This PTS shift accomplishes that.
 */
GstPwAudioQueueRetrievalResult gst_pw_audio_queue_retrieve_buffer(
	GstPwAudioQueue *queue,
	gsize min_num_output_frames,
	gsize ideal_num_output_frames,
	GstClockTime retrieval_pts,
	GstClockTime queued_data_pts_shift,
	GstPwAudioQueueRetrievalDetails *retrieval_details
)
{
	gsize stride;

	g_assert(queue != NULL);
	g_assert(ideal_num_output_frames > 0);
	g_assert(GST_CLOCK_TIME_IS_VALID(queued_data_pts_shift));
	g_assert(retrieval_details != NULL);
	g_assert(queue->priv->format_initialized);

	stride = gst_pw_audio_format_get_stride(&(queue->priv->format));

	if (queue->priv->is_contiguous)
	{
		gsize num_queued_frames;
		gsize actual_num_output_frames;
		GstClockTime queued_duration;
		GstClockTime actual_output_duration;
		GstBuffer *retrieved_buffer;

		/* Figure out how much data we can take from the adapter. */

		num_queued_frames = gst_adapter_available(queue->priv->contiguous_audio_buffer_queue) / stride;
		if (G_UNLIKELY(num_queued_frames == 0))
			goto queue_is_empty;
		actual_num_output_frames = MIN(ideal_num_output_frames, num_queued_frames);

		queued_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(queue->priv->format), num_queued_frames);
		actual_output_duration = gst_pw_audio_format_calculate_duration_from_num_frames(&(queue->priv->format), actual_num_output_frames);

		if (GST_CLOCK_TIME_IS_VALID(retrieval_pts) && GST_CLOCK_TIME_IS_VALID(queue->priv->oldest_queued_data_pts))
		{
			/* All required timestamps are valid. We can synchronize
			 * the output against the supplied retrieval_pts. */

			/* As mentioned in the documentatio, these PTS define two "windows":
			 *
			 * - The "output window" - the timespan that starts at
			 *   actual_output_buffer_start_pts and ends at actual_output_buffer_end_pts.
			 * - The "queued data window" - the timespan that starts at
			 *   queued_data_start_pts and ends at queued_data_end_pts.
			 *
			 * The goal here is to (a) determine the intersection between
			 * these two windows and (b) see if any silence frames need
			 * to be added. The intersection defines the block of data
			 * that has to be retrieved from the contiguous queue.
			 */
			GstClockTime actual_output_buffer_start_pts = retrieval_pts;
			GstClockTime actual_output_buffer_end_pts = actual_output_buffer_start_pts + actual_output_duration;
			GstClockTime queued_data_start_pts = queue->priv->oldest_queued_data_pts + queued_data_pts_shift;
			GstClockTime queued_data_end_pts = queued_data_start_pts + queued_duration;

			GST_DEBUG_OBJECT(
				queue,
				"queued data window: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT "  "
				"actual output buffer window: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT "  "
				"num queued frames: %" G_GSIZE_FORMAT "  "
				"queued duration: %" GST_TIME_FORMAT "  "
				"ideal num output frames: %" G_GSIZE_FORMAT "  "
				"min num output frames: %" G_GSIZE_FORMAT "  "
				"actual num output frames: %" G_GSIZE_FORMAT "  "
				"actual output duration: %" GST_TIME_FORMAT "  "
				"queued data PTS shift: %" GST_TIME_FORMAT "  ",
				GST_TIME_ARGS(queued_data_start_pts), GST_TIME_ARGS(queued_data_end_pts),
				GST_TIME_ARGS(actual_output_buffer_start_pts), GST_TIME_ARGS(actual_output_buffer_end_pts),
				num_queued_frames,
				GST_TIME_ARGS(queued_duration),
				ideal_num_output_frames,
				min_num_output_frames,
				actual_num_output_frames,
				GST_TIME_ARGS(actual_output_duration),
				GST_TIME_ARGS(queued_data_pts_shift)
			);

			if (actual_output_buffer_end_pts < queued_data_start_pts)
			{
				/* Simplest case: None of the data can be played yet, since the
				 * queued data window is entirely in the future relative to the
				 * output window - there is no intersection between these two.
				 * The queued data is fully valid, but can't be used yet. */

				GST_DEBUG_OBJECT(
					queue,
					"queued data window is entirely in the future - cannot retrieve any queued data yet"
				);

				retrieval_details->retrieved_buffer = NULL;

				return GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE;
			}
			else if (actual_output_buffer_start_pts >= queued_data_end_pts)
			{
				/* The entire contents of the queue is invalid since it expired.
				 * The queued data window is entirely in the past relative to the
				 * output window - there is no intersection between these two.
				 * Since the queued data is expired, it will never be played, and
				 * has to be flushed. */

				GST_DEBUG_OBJECT(
					queue,
					"queued data window is entirely in the past - all queued data has expired"
				);

				gst_pw_audio_queue_flush(queue);
				retrieval_details->retrieved_buffer = NULL;

				return GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST;
			}
			else
			{
				GstClockTime silence_length;
				GstClockTime duration_of_expired_queued_data;
				gsize num_frames_with_silence;

				/* At this point it is clear that at least *some* of the queued
				 * data lies within the output window. */

				/* We need to distinguish between two cases:
				 *
				 * - Queued data window is partially in the past relative to the
				 *   output window. The oldest N frames in the contiguous queue have
				 *   expired and must be discarded. The queued data window's start
				 *   PTS will be older than the output window's PTS in this case.
				 * - Queued data window is partially in the future relative to the
				 *   output window. The first N frames in the output window need
				 *   to be filled with silence to cover the time until the actual
				 *   data is to be played. The queued data window's start PTS
				 *   will be newer than the output window's PTS in this case.
				 *
				 * (We don't directly compute frames, but rather nanosecond durations,
				 * since we are using PTS here. But conceptually we do throw away
				 * expired frames and/or prepend silence frames.)
				 */
				if (queued_data_start_pts > actual_output_buffer_start_pts)
				{
					silence_length = queued_data_start_pts - actual_output_buffer_start_pts;
					duration_of_expired_queued_data = 0;
				}
				else
				{
					silence_length = 0;
					duration_of_expired_queued_data = actual_output_buffer_start_pts - queued_data_start_pts;
				}

				/* Silence needs to be prepended if the data lies in the future but is still
				 * within the output window. We subtract the number of silence frames from
				 * actual_num_output_frames and the silence duration from output_duration to
				 * account for the room within the output window that will be taken up by
				 * the silence frames. */
				if (silence_length > 0)
				{
					retrieval_details->num_silence_frames_to_prepend = gst_pw_audio_format_calculate_num_frames_from_duration(&(queue->priv->format), silence_length);
					actual_num_output_frames -= retrieval_details->num_silence_frames_to_prepend;
					actual_output_duration -= silence_length;
				}
				else
					retrieval_details->num_silence_frames_to_prepend = 0;

				/* Expired data must be thrown away. We do that by flushing the expired frames
				 * from the contiguous queue. Also adjust actual_num_output_frames and
				 * output_duration to account for the length of the discarded frames. */
				if (duration_of_expired_queued_data > 0)
				{
					gsize num_frames_to_flush = gst_pw_audio_format_calculate_num_frames_from_duration(&(queue->priv->format), duration_of_expired_queued_data);
					num_frames_to_flush = MIN(num_frames_to_flush, actual_num_output_frames);
					gst_adapter_flush(queue->priv->contiguous_audio_buffer_queue, num_frames_to_flush * stride);
					g_assert(actual_num_output_frames >= num_frames_to_flush);
					actual_num_output_frames -= num_frames_to_flush;
					actual_output_duration -= duration_of_expired_queued_data;
				}

				if (G_UNLIKELY(actual_num_output_frames == 0))
				{
					GST_DEBUG_OBJECT(
						queue,
						"queued data window is (partially) in the present, but all data"
						" that could be put in retrieved buffer was clipped"
					);
					retrieval_details->retrieved_buffer = NULL;
					return GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_ALL_DATA_FOR_BUFFER_CLIPPED;
				}

				/* If after all of the processing earlier the remaining number of frames
				 * is less than the required minimum, we append silence frames to ensure
				 * that the output has that minimum number of frames. */
				num_frames_with_silence = retrieval_details->num_silence_frames_to_prepend + actual_num_output_frames;
				retrieval_details->num_silence_frames_to_append = (min_num_output_frames > num_frames_with_silence) ? (min_num_output_frames - num_frames_with_silence) : 0;

				GST_DEBUG_OBJECT(
					queue,
					"queued data window is (partially) in the present"
				);

				retrieved_buffer = gst_adapter_take_buffer(queue->priv->contiguous_audio_buffer_queue, actual_num_output_frames * stride);
			}
		}
		else
		{
			/* Either, the queued data did not contain valid timestamps, or the
			 * user does not want synchronized retrieval (indicated by an invalid
			 * retrieval_pts value). The latter case typically happens because the
			 * output is already synced, or because synced output is turned off.
			 * Behave like a simple queue in these cases. */
			retrieved_buffer = gst_adapter_take_buffer(queue->priv->contiguous_audio_buffer_queue, actual_num_output_frames * stride);
			retrieval_details->num_silence_frames_to_prepend = 0;
			retrieval_details->num_silence_frames_to_append = 0;

			GST_LOG_OBJECT(
				queue,
				"retrieving frames without sync;  "
				"stride: %" G_GSIZE_FORMAT "  "
				"num queued frames: %" G_GSIZE_FORMAT "  "
				"queued duration: %" GST_TIME_FORMAT "  "
				"ideal num output frames: %" G_GSIZE_FORMAT "  "
				"min num output frames: %" G_GSIZE_FORMAT "  "
				"actual num output frames: %" G_GSIZE_FORMAT "  "
				"actual output duration: %" GST_TIME_FORMAT,
				stride,
				num_queued_frames,
				GST_TIME_ARGS(queued_duration),
				ideal_num_output_frames,
				min_num_output_frames,
				actual_num_output_frames,
				GST_TIME_ARGS(actual_output_duration)
			);
		}

		/* Ensure that the buffer is writable to safely set its duration and PTS. */
		retrieved_buffer = gst_buffer_make_writable(retrieved_buffer);

		GST_BUFFER_DURATION(retrieved_buffer) = actual_output_duration;

		if (GST_CLOCK_TIME_IS_VALID(queue->priv->oldest_queued_data_pts))
		{
			/* We took the oldest (actual_num_output_frames) frames from the queue.
			 * Consequently, the PTS of the resulting gstbuffer must equal
			 * oldest_queued_data_pts (with the queued data PTS shift applied). */
			GST_BUFFER_PTS(retrieved_buffer) = queue->priv->oldest_queued_data_pts + queued_data_pts_shift;
			/* Increment the oldest PTS since we just retrieved the oldest data.
			 * That way, this timestamp remains valid for future retrievals. */
			queue->priv->oldest_queued_data_pts += GST_BUFFER_DURATION(retrieved_buffer);
		}

		/* Update the fill level after retrieving data from the queue. */
		queue->current_fill_level = gst_pw_audio_format_calculate_duration_from_num_frames(&(queue->priv->format), num_queued_frames - actual_num_output_frames);

		retrieval_details->retrieved_buffer = retrieved_buffer;
	}
	else
	{
		/* Packetized audio types only allow for regular queue behavíor.
		 * The timestamps are ignored when the data is of such a type.
		 * Also, it is not possible to retrieve an exact amount of data
		 * (the amount being ideal_output_buffer_duration), since the
		 * packets (MPEG frames for example) cannot be subdivided. */

		if (gst_queue_array_is_empty(queue->priv->packetized_audio_buffer_queue))
			goto queue_is_empty;

		retrieval_details->retrieved_buffer = (GstBuffer *)gst_queue_array_pop_head(queue->priv->packetized_audio_buffer_queue);
		retrieval_details->num_silence_frames_to_prepend = 0;
		retrieval_details->num_silence_frames_to_append = 0;

		GST_LOG_OBJECT(queue, "retrieved queued gstbuffer: %" GST_PTR_FORMAT, (gpointer)(retrieval_details->retrieved_buffer));

		/* This assertion is made because current_fill_level is the sum of all
		 * individual buffer durations, so if this assertion fails, something
		 * is wrong with the current_fill_level value or the queued buffers. */
		g_assert(queue->current_fill_level >= GST_BUFFER_DURATION(retrieval_details->retrieved_buffer));
		queue->current_fill_level -= GST_BUFFER_DURATION(retrieval_details->retrieved_buffer);
	}

	return GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_OK;

queue_is_empty:
	g_assert(queue->current_fill_level == 0);
	retrieval_details->retrieved_buffer = NULL;
	queue->priv->oldest_queued_data_pts = GST_CLOCK_TIME_NONE;
	return GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_QUEUE_IS_EMPTY;
}

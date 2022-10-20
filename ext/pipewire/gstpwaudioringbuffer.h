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
 * SECTION:gstpwaudioringbuffer
 * @title: GstPwAudioRingBuffer
 * @short_description: Ring buffer for audio data, used by GStreamer pipewire audio elements.
 *
 * #GstPwAudioRingBuffer provides a queue that stores audio data and allows for retrieving data
 * at the right moment (depending on input buffer timestamps, current time, and audio type).
 * It only supports PCM and DSD audio types, since those allow for inserting silence frames
 * and removing frames for purposes of synchronization; such modifications are generally not
 * possible with encoded/compressed audio formats like AC3, DTS, MPEG-H.
 *
 * When given timestamps (PTS), the ringbuffer can keep track of those to synchronize
 * frame retrieval. It stores the PTS of the oldest buffered frame and uses that along
 * with the fill level to synchronize output. Initially, that "oldest frame PTS" is not
 * initialized; the first gst_pw_audio_ring_buffer_retrieve_frames() call initializes it
 * based on the retrieval PTS argument it is given. Followup calls to this function that
 * have valid retrieval PTS are compared against the oldest frame PTS and the fill level.
 * The function then retrieves frames and inserts silence frames / skips frames as needed
 * since gst_pw_audio_ring_buffer_retrieve_frames() is also given a number of frames to
 * retrieve, and if there are insufficient buffered frames, or not all frames can be used
 * (see below), then the missing frames have to be replaced by silence frames.
 *
 * The following cases can be distinguished (assuming R = number of frames to retrieve):
 *
 * 1. All frames in the ring buffer area "expired", that is, they lie fully in the past relative
 *    to the retrieval PTS. The buffered data is no longer usable, and gets flushed.
 *    gst_pw_audio_ring_buffer_retrieve_frames() produces R silence frames only, since there
 *    is no valid data available.
 * 2. All frames in the ring buffer lie in the future relative to the retrieval PTS. Nothing
 *    gets flushed, since the data is valid, just can't be used *yet*. Just like case #1,
 *    gst_pw_audio_ring_buffer_retrieve_frames() produces R silence frames; there isn't any
 *    data that can be used ríght now.
 * 3. The newest N buffered frames lie within the retrieval window; the rest are expired.
 *    These newest N frames are retrieved. Since the retrieval window was only partially
 *    filled, R-N silence frames are appended to the retrieved frames.
 * 4. The oldest N buffered frames lie within the retrieval window; the rest are in the future.
 *    These oldest N frames are retrieved. Since the retrieval window was only partially
 *    filled, R-N silence frames are prepended to the retrieved frames.
 *
 * A variant of cases #3 and #4 is when the entire retrieval window can be filled. For example,
 * if some frames lie in the past (like in case #3), but the ring buffer is filled enough to
 * provide all requested R frames regardless, then the expired frames are flushed, but no
 * silence frames are appended. Same applies to case #4.
 *
 * Access is not inherently MT safe. Using synchronization primitives is advised.
 */

#ifndef __GST_PW_AUDIO_RING_BUFFER_H__
#define __GST_PW_AUDIO_RING_BUFFER_H__

#include <gst/gst.h>
#include "gstpwaudioformat.h"
#include "utils.h"


G_BEGIN_DECLS


/**
 * GstPwAudioRingBuffer:
 *
 * Opaque #GstPwAudioRingBuffer structure.
 */
typedef struct _GstPwAudioRingBuffer GstPwAudioRingBuffer;
typedef struct _GstPwAudioRingBufferClass GstPwAudioRingBufferClass;


#define GST_PW_AUDIO_RING_BUFFER_PTS_DELTA_HISTORY_SIZE 3


#define GST_TYPE_PW_AUDIO_RING_BUFFER            (gst_pw_audio_ring_buffer_get_type())
#define GST_PW_AUDIO_RING_BUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_AUDIO_RING_BUFFER, GstPwAudioRingBuffer))
#define GST_PW_AUDIO_RING_BUFFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_AUDIO_RING_BUFFER, GstPwAudioRingBufferClass))
#define GST_PW_AUDIO_RING_BUFFER_CAST(obj)       ((GstPwAudioRingBuffer *)(obj))
#define GST_IS_PW_AUDIO_RING_BUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_AUDIO_RING_BUFFER))
#define GST_IS_PW_AUDIO_RING_BUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_AUDIO_RING_BUFFER))


typedef enum
{
	GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK,
	GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_RING_BUFFER_IS_EMPTY,
	GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE,
	GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST,
	GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_ALL_DATA_FOR_BUFFER_CLIPPED
}
GstPwAudioRingBufferRetrievalResult;


struct _GstPwAudioRingBuffer
{
	GstObject parent;

	/*< private >*/

	GstPwAudioFormat format;
	/* Stride in bytes of one frame. This is the result
	 * of gst_pw_audio_format_get_stride(format). */
	gsize stride;

	guint8 *buffered_frames;

	ringbuffer_metrics metrics;
	GstClockTime ring_buffer_length;
	GstClockTime current_fill_level;

	/* PTS of the oldest frame in the ring buffer. This is initially invalid,
	 * and set to the PTS of the first frame that is pushed into the ring buffer.
	 * Later, when frames are retrieved from the buffer, this is updated, since
	 * the oldest frames are retrieved first. This timestamp is needed when the
	 * stream has not started yet and needs to start at the right moment
	 * (= when the clock reaches this timestamp). The buffered frames may lie
	 * entirely in the future when this timestamp is queried, or entirely in
	 * the past etc. gst_pw_audio_ring_buffer_retrieve_buffer() checks for
	 * these cases and acts depending on the value of this timestamp.*/
	GstClockTime oldest_frame_pts;

	/* Small PTS delta history used for computing a short 3-number median. */
	GstClockTimeDiff pts_delta_history[GST_PW_AUDIO_RING_BUFFER_PTS_DELTA_HISTORY_SIZE];
	gint num_pts_delta_history_entries;
};


struct _GstPwAudioRingBufferClass
{
	GstObjectClass parent_class;
};


GType gst_pw_audio_ring_buffer_get_type(void);

GstPwAudioRingBuffer* gst_pw_audio_ring_buffer_new(GstPwAudioFormat *format, GstClockTime ring_buffer_length);

void gst_pw_audio_ring_buffer_flush(GstPwAudioRingBuffer *ring_buffer);

gsize gst_pw_audio_ring_buffer_push_frames(
	GstPwAudioRingBuffer *ring_buffer,
	gpointer frames,
	gsize num_frames,
	gsize num_silence_frames_to_prepend,
	GstClockTime pts,
	GstClockTime duration
);

GstPwAudioRingBufferRetrievalResult gst_pw_audio_ring_buffer_retrieve_frames(
	GstPwAudioRingBuffer *ring_buffer,
	gpointer destination,
	gsize num_frames_to_retrieve,
	GstClockTime retrieval_pts,
	GstClockTime ring_buffer_data_pts_shift,
	GstClockTimeDiff skew_threshold,
	GstClockTimeDiff *buffered_frames_to_retrieval_pts_delta
);

static inline GstClockTime gst_pw_audio_ring_buffer_get_oldest_frame_pts(GstPwAudioRingBuffer *ring_buffer)
{
	g_assert(ring_buffer != NULL);
	return ring_buffer->oldest_frame_pts;
}

static inline GstClockTime gst_pw_audio_ring_buffer_get_current_fill_level(GstPwAudioRingBuffer *ring_buffer)
{
	g_assert(ring_buffer != NULL);
	return ring_buffer->current_fill_level;
}


G_END_DECLS


#endif /* __GST_PW_AUDIO_RING_BUFFER_H__ */

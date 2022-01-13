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

#ifndef __GST_PW_AUDIO_QUEUE_H__
#define __GST_PW_AUDIO_QUEUE_H__

#include <gst/gst.h>
#include "gstpwaudioformat.h"


G_BEGIN_DECLS


/**
 * GstPwAudioQueue:
 *
 * Opaque #GstPwAudioQueue structure.
 */
typedef struct _GstPwAudioQueue GstPwAudioQueue;
typedef struct _GstPwAudioQueuePrivate GstPwAudioQueuePrivate;
typedef struct _GstPwAudioQueueRetrievalDetails GstPwAudioQueueRetrievalDetails;
typedef struct _GstPwAudioQueueClass GstPwAudioQueueClass;


#define GST_TYPE_PW_AUDIO_QUEUE             (gst_pw_audio_queue_get_type())
#define GST_PW_AUDIO_QUEUE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_AUDIO_QUEUE, GstPwAudioQueue))
#define GST_PW_AUDIO_QUEUE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_AUDIO_QUEUE, GstPwAudioQueueClass))
#define GST_PW_AUDIO_QUEUE_CAST(obj)        ((GstPwAudioQueue *)(obj))
#define GST_IS_PW_AUDIO_QUEUE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_AUDIO_QUEUE))
#define GST_IS_PW_AUDIO_QUEUE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_AUDIO_QUEUE))


typedef enum
{
	GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_OK,
	GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_QUEUE_IS_EMPTY,
	GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE,
	GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST,
	GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_ALL_DATA_FOR_BUFFER_CLIPPED
}
GstPwAudioQueueRetrievalResult;


struct _GstPwAudioQueueRetrievalDetails
{
	GstBuffer *retrieved_buffer;
	gsize num_silence_frames_to_prepend;
	gsize num_silence_frames_to_append;
};


struct _GstPwAudioQueue
{
	GstObject parent;

	/*< private >*/

	GstClockTime current_fill_level;
	GstPwAudioQueuePrivate *priv;
};


struct _GstPwAudioQueueClass
{
	GstObjectClass parent_class;
};


GType gst_pw_audio_queue_get_type(void);

GstPwAudioQueue* gst_pw_audio_queue_new(void);

void gst_pw_audio_queue_set_format(GstPwAudioQueue *queue, GstPwAudioFormat *format);
void gst_pw_audio_queue_flush(GstPwAudioQueue *queue);
void gst_pw_audio_queue_push_silence_frames(GstPwAudioQueue *queue, gsize num_silence_frames);
void gst_pw_audio_queue_push_buffer(GstPwAudioQueue *queue, GstBuffer *buffer);
GstPwAudioQueueRetrievalResult gst_pw_audio_queue_retrieve_buffer(
	GstPwAudioQueue *queue,
	gsize min_num_output_frames,
	gsize ideal_num_output_frames,
	GstClockTime current_time,
	GstClockTime buffer_pts_shift,
	GstPwAudioQueueRetrievalDetails *retrieval_details
);

static inline GstClockTime gst_pw_audio_queue_get_fill_level(GstPwAudioQueue *queue)
{
	g_assert(queue != NULL);
	return queue->current_fill_level;
}


G_END_DECLS


#endif // __GST_PW_AUDIO_QUEUE_H__

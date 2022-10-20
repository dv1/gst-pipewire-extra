#ifndef __GST_PIPEWIRE_UTILS_H__
#define __GST_PIPEWIRE_UTILS_H__

#include <string.h>
#include <gst/gst.h>


static inline GstClockTimeDiff calculate_3_value_median(GstClockTimeDiff const *pts_delta_history)
{
	/* Store temporaries in variables since MIN and MAX are macros, not functions */
	GstClockTimeDiff a = pts_delta_history[0];
	GstClockTimeDiff b = pts_delta_history[1];
	GstClockTimeDiff c = pts_delta_history[2];
	GstClockTimeDiff min_ab = MIN(a, b);
	GstClockTimeDiff max_ab = MAX(a, b);
	GstClockTimeDiff max_ab_min_c = MIN(max_ab, c);
	return MAX(min_ab, max_ab_min_c);
}


typedef struct
{
	guint64 current_num_buffered_frames;
	guint64 capacity;
	guint64 read_position;
	guint64 write_position;
}
ringbuffer_metrics;


static inline void ringbuffer_metrics_init(ringbuffer_metrics *metrics, guint64 capacity)
{
	g_assert(metrics != NULL);
	g_assert(capacity > 0);

	memset(metrics, 0, sizeof(ringbuffer_metrics));
	metrics->capacity = capacity;
}


static inline void ringbuffer_metrics_reset(ringbuffer_metrics *metrics)
{
	g_assert(metrics != NULL);

	metrics->current_num_buffered_frames = 0;
	metrics->read_position = 0;
	metrics->write_position = 0;
}


static inline guint64 ringbuffer_metrics_flush(ringbuffer_metrics *metrics, guint64 num_frames_to_flush)
{
	g_assert(metrics != NULL);

	num_frames_to_flush = MIN(num_frames_to_flush, metrics->current_num_buffered_frames);
	if (G_UNLIKELY(num_frames_to_flush == 0))
		return 0;

	metrics->read_position = (metrics->read_position + num_frames_to_flush) % metrics->capacity;

	metrics->current_num_buffered_frames -= num_frames_to_flush;

	return num_frames_to_flush;
}


static inline guint64 ringbuffer_metrics_read(ringbuffer_metrics *metrics, guint64 num_frames_to_read, guint64 *read_offset, guint64 *read_lengths)
{
	g_assert(metrics != NULL);
	g_assert(read_offset != NULL);
	g_assert(read_lengths != NULL);

	num_frames_to_read = MIN(num_frames_to_read, metrics->current_num_buffered_frames);
	if (G_UNLIKELY(num_frames_to_read == 0))
	{
		read_lengths[0] = read_lengths[1] = 0;
		return 0;
	}

	read_lengths[0] = metrics->capacity - metrics->read_position;
	read_lengths[0] = MIN(read_lengths[0], num_frames_to_read);
	read_lengths[1] = num_frames_to_read - read_lengths[0];

	*read_offset = metrics->read_position;

	metrics->read_position = (metrics->read_position + num_frames_to_read) % metrics->capacity;

	metrics->current_num_buffered_frames -= num_frames_to_read;

	return num_frames_to_read;
}


static inline guint64 ringbuffer_metrics_write(ringbuffer_metrics *metrics, guint64 num_frames_to_write, guint64 *write_offset, guint64 *write_lengths)
{
	guint64 available_space;

	g_assert(metrics != NULL);
	g_assert(write_offset != NULL);
	g_assert(write_lengths != NULL);

	available_space = metrics->capacity - metrics->current_num_buffered_frames;

	num_frames_to_write = MIN(num_frames_to_write, available_space);
	if (G_UNLIKELY(num_frames_to_write == 0))
	{
		write_lengths[0] = write_lengths[1] = 0;
		return 0;
	}

	write_lengths[0] = metrics->capacity - metrics->write_position;
	write_lengths[0] = MIN(write_lengths[0], num_frames_to_write);
	write_lengths[1] = num_frames_to_write - write_lengths[0];

	*write_offset = metrics->write_position;

	metrics->write_position = (metrics->write_position + num_frames_to_write) % metrics->capacity;

	metrics->current_num_buffered_frames += num_frames_to_write;

	return num_frames_to_write;
}


#endif /* __GST_PIPEWIRE_UTILS_H__ */

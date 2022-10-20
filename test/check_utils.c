#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include "utils.h"


GST_START_TEST(basic_read_operations)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 read_offset;
	guint64 read_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 0;
	m.write_position = 0;
	m.current_num_buffered_frames = 0;
	result = ringbuffer_metrics_read(&m, 10, &read_offset, read_lengths);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 0);
	assert_equals_uint64(read_lengths[0], 0);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 0);

	m.read_position = 0;
	m.write_position = 0;
	m.current_num_buffered_frames = 1000;
	result = ringbuffer_metrics_read(&m, 100, &read_offset, read_lengths);
	assert_equals_uint64(read_offset, 0);
	assert_equals_uint64(m.read_position, 100);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 900);
	assert_equals_uint64(read_lengths[0], 100);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 100);

	result = ringbuffer_metrics_read(&m, 900, &read_offset, read_lengths);
	assert_equals_uint64(read_offset, 100);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 0);
	assert_equals_uint64(read_lengths[0], 900);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 900);

	result = ringbuffer_metrics_read(&m, 10, &read_offset, read_lengths);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 0);
	assert_equals_uint64(read_lengths[0], 0);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 0);
}
GST_END_TEST;


GST_START_TEST(wrap_around_read)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 read_offset;
	guint64 read_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 800;
	m.write_position = 100;
	m.current_num_buffered_frames = 300;
	result = ringbuffer_metrics_read(&m, 300, &read_offset, read_lengths);
	assert_equals_uint64(read_offset, 800);
	assert_equals_uint64(m.read_position, 100);
	assert_equals_uint64(m.write_position, 100);
	assert_equals_uint64(m.current_num_buffered_frames, 0);
	assert_equals_uint64(read_lengths[0], 200);
	assert_equals_uint64(read_lengths[1], 100);
	assert_equals_uint64(result, 300);
}
GST_END_TEST;


GST_START_TEST(read_to_end_then_wrap_around)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 read_offset;
	guint64 read_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 200;
	m.write_position = 100;
	m.current_num_buffered_frames = 900;
	result = ringbuffer_metrics_read(&m, 800, &read_offset, read_lengths);
	assert_equals_uint64(read_offset, 200);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 100);
	assert_equals_uint64(m.current_num_buffered_frames, 100);
	assert_equals_uint64(read_lengths[0], 800);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 800);

	result = ringbuffer_metrics_read(&m, 30, &read_offset, read_lengths);
	assert_equals_uint64(read_offset, 0);
	assert_equals_uint64(m.read_position, 30);
	assert_equals_uint64(m.write_position, 100);
	assert_equals_uint64(m.current_num_buffered_frames, 70);
	assert_equals_uint64(read_lengths[0], 30);
	assert_equals_uint64(read_lengths[1], 0);
	assert_equals_uint64(result, 30);
}
GST_END_TEST;


GST_START_TEST(basic_write_operations)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 write_offset;
	guint64 write_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 0;
	m.write_position = 0;
	m.current_num_buffered_frames = 1000;
	result = ringbuffer_metrics_write(&m, 10, &write_offset, write_lengths);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 1000);
	assert_equals_uint64(write_lengths[0], 0);
	assert_equals_uint64(write_lengths[1], 0);
	assert_equals_uint64(result, 0);

	m.read_position = 0;
	m.write_position = 0;
	m.current_num_buffered_frames = 0;
	result = ringbuffer_metrics_write(&m, 100, &write_offset, write_lengths);
	assert_equals_uint64(write_offset, 0);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 100);
	assert_equals_uint64(m.current_num_buffered_frames, 100);
	assert_equals_uint64(write_lengths[0], 100);
	assert_equals_uint64(write_lengths[1], 0);
	assert_equals_uint64(result, 100);

	result = ringbuffer_metrics_write(&m, 900, &write_offset, write_lengths);
	assert_equals_uint64(write_offset, 100);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 1000);
	assert_equals_uint64(write_lengths[0], 900);
	assert_equals_uint64(write_lengths[1], 0);
	assert_equals_uint64(result, 900);

	result = ringbuffer_metrics_write(&m, 10, &write_offset, write_lengths);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 1000);
	assert_equals_uint64(write_lengths[0], 0);
	assert_equals_uint64(write_lengths[1], 0);
	assert_equals_uint64(result, 0);
}
GST_END_TEST;


GST_START_TEST(wrap_around_write)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 write_offset;
	guint64 write_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 100;
	m.write_position = 800;
	m.current_num_buffered_frames = 700;
	result = ringbuffer_metrics_write(&m, 300, &write_offset, write_lengths);
	assert_equals_uint64(write_offset, 800);
	assert_equals_uint64(m.read_position, 100);
	assert_equals_uint64(m.write_position, 100);
	assert_equals_uint64(m.current_num_buffered_frames, 1000);
	assert_equals_uint64(write_lengths[0], 200);
	assert_equals_uint64(write_lengths[1], 100);
	assert_equals_uint64(result, 300);
}
GST_END_TEST;


GST_START_TEST(combined_wrapped_read_and_write)
{
	ringbuffer_metrics m;
	guint64 result;
	guint64 readwrite_offset;
	guint64 readwrite_lengths[2];

	ringbuffer_metrics_init(&m, 1000);

	m.read_position = 700;
	m.write_position = 0;
	m.current_num_buffered_frames = 300;
	result = ringbuffer_metrics_read(&m, 300, &readwrite_offset, readwrite_lengths);
	assert_equals_uint64(readwrite_offset, 700);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 0);
	assert_equals_uint64(m.current_num_buffered_frames, 0);
	assert_equals_uint64(readwrite_lengths[0], 300);
	assert_equals_uint64(readwrite_lengths[1], 0);
	assert_equals_uint64(result, 300);

	result = ringbuffer_metrics_write(&m, 400, &readwrite_offset, readwrite_lengths);
	assert_equals_uint64(readwrite_offset, 0);
	assert_equals_uint64(m.read_position, 0);
	assert_equals_uint64(m.write_position, 400);
	assert_equals_uint64(m.current_num_buffered_frames, 400);
	assert_equals_uint64(readwrite_lengths[0], 400);
	assert_equals_uint64(readwrite_lengths[1], 0);
	assert_equals_uint64(result, 400);

	result = ringbuffer_metrics_read(&m, 150, &readwrite_offset, readwrite_lengths);
	assert_equals_uint64(readwrite_offset, 0);
	assert_equals_uint64(m.read_position, 150);
	assert_equals_uint64(m.write_position, 400);
	assert_equals_uint64(m.current_num_buffered_frames, 250);
	assert_equals_uint64(readwrite_lengths[0], 150);
	assert_equals_uint64(readwrite_lengths[1], 0);
	assert_equals_uint64(result, 150);
}
GST_END_TEST;


static Suite * gst_pw_utils_suite(void)
{
	Suite *s = suite_create("GstPwUtils");
	TCase *tc = tcase_create("general");

	suite_add_tcase(s, tc);
	tcase_add_test(tc, basic_read_operations);
	tcase_add_test(tc, wrap_around_read);
	tcase_add_test(tc, read_to_end_then_wrap_around);
	tcase_add_test(tc, basic_write_operations);
	tcase_add_test(tc, wrap_around_write);
	tcase_add_test(tc, combined_wrapped_read_and_write);

	return s;
}


GST_CHECK_MAIN(gst_pw_utils);

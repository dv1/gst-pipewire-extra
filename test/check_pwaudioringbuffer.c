#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include <gst/audio/audio.h>
#include "gstpwaudioringbuffer.h"
#include "gstpwaudioformat.h"


#define PCM_SAMPLE_RATE 48000
#define NUM_CHANNELS 1
#define PCM_SAMPLE_FORMAT GST_AUDIO_FORMAT_S16LE

#define CALC_NUM_FRAMES_FOR_MSECS(MSECS) (PCM_SAMPLE_RATE * (MSECS) / 1000)


/* NOTE: By default, tests use input buffers with a length of 10 ms
 * (that is, their num_frames constant is initialized to CALC_NUM_FRAMES_FOR_MSECS(10)).
 * Some tests may use a different length if needed. In particular, the timestamped
 * tests use a length of 20 ms to make room for silence frames. */


GST_START_TEST(creation_and_initial_states)
{
	/* Check the state of the ringbuffer right after creating it. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);

	/* We expect the ringbuffer to be empty, and the states to reflect that.
	 * (Format related states are exempt from this - they aren't involved
	 * in the buffer fill level.) */
	assert_equals_uint64(ring_buffer->stride, GST_AUDIO_INFO_BPF(&(format.info.pcm_audio_info)));
	/* buffered_frames is preallocated by gst_pw_audio_ring_buffer_new()
	 * and remains allocated even if the ring buffer is empty. */
	fail_if(ring_buffer->buffered_frames == NULL);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 0);
	/* The ring buffer length of 1 second equals PCM_SAMPLE_RATE number of frames. */
	assert_equals_uint64(ring_buffer->metrics.capacity, PCM_SAMPLE_RATE);
	assert_equals_uint64(ring_buffer->metrics.read_position, 0);
	assert_equals_uint64(ring_buffer->metrics.write_position, 0);
	assert_equals_uint64(ring_buffer->current_fill_level, 0);
	fail_if(GST_CLOCK_TIME_IS_VALID(ring_buffer->oldest_frame_pts));
	assert_equals_int(ring_buffer->num_pts_delta_history_entries, 0);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(basic_io)
{
	/* Test basic, non-timestamped IO operations. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(10) };
	gint16 frames[num_frames * NUM_CHANNELS];
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);

	/* First, push some frames into the ring buffer. */

	/* In production, the ring buffer's memory block does not need to be set to 0,
	 * since areas that contain no actual data aren't looked anyway. For this test,
	 * we do set the block's contents to 0 to be able to test it. */
	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Prepare our test content that we want to push. The PCM frames start at 10
	 * (not 0 - this helps with checking the ring buffer memory block later) and
	 * increase monotonically. */
	for (i = 0; i < num_frames; ++i)
		frames[i] = i + 10;

	/* Perform the actual push. */
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames,
		0,
		GST_CLOCK_TIME_NONE
	);
	/* We expect all test frames to be pushed since there's enough room in the ring buffer. */
	assert_equals_uint64(push_result, num_frames);

	/* Check that the frames were pushed at the right location in the ring buffer and that
	 * their values weren't corrupted somehow. */
	for (i = 0; i < num_frames; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Prepare the second push, 20 frames this time. We expect all of those 20 frames to be
	 * fully pushed as well. In addition, we insert 10 silence frames in between the already
	 * pushed frames and the new ones to test this silence frame insertion feature. */
	for (i = 0; i < 20; ++i)
		frames[i] = 5;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		20,
		10,
		GST_CLOCK_TIME_NONE
	);
	assert_equals_uint64(push_result, 20);

	/* Check that silence frames were inserted. */
	for (i = 0; i < 10; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i + num_frames], 0);
	/* Now check for the actual 20 frames that were pushed. */
	for (i = 0; i < 20; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i + num_frames + 10], 5);

	/* After testing frame push, we test frame retrieval. */

	/* Clear our input buffer before testing the frame retrieval. */
	memset(frames, 0, sizeof(frames));

	/* The previous push operations should have filled the ring buffer to this extent.
	 * We need this for successful retrieval. */
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 510);

	/* Try retrieving the oldest 300 frames. We expect this to succeed since the
	 * ring buffer has sufficient data for this. Note that all PTS specific args
	 * are set to 0 or GST_CLOCK_TIME_NONE - we do timestamp-less tests here. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		300,
		GST_CLOCK_TIME_NONE,
		0,
		0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	for (i = 0; i < 300; ++i)
		assert_equals_int(frames[i], i + 10);

	/* Check that the fill level has been reduced accordingly. */
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 210);

	/* Try to retrieve 210 more frames. After this, the ring buffer should be empty. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		210,
		GST_CLOCK_TIME_NONE,
		0,
		0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	for (i = 0; i < 180; ++i)
		assert_equals_int(frames[i], i + 300 + 10);
	for (i = 0; i < 10; ++i)
		assert_equals_int(frames[i + 180], 0);
	for (i = 0; i < 20; ++i)
		assert_equals_int(frames[i + 190], 5);

	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 0);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(attempt_to_retrieve_more_frames_than_available)
{
	/* Test attempts to retrieve more frames than available
	 * in the ring buffer. We expect the retrieve_frames
	 * call to return all data from the ring buffer and pad
	 * the result with silence frames to sum up to a total
	 * number of frames that matches the requested amount. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(10) };
	gint16 frames[num_frames * NUM_CHANNELS];
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Insert 10 frames as preparation. */
	for (i = 0; i < 10; ++i)
		frames[i] = i + 10;

	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		10,
		0,
		GST_CLOCK_TIME_NONE
	);

	assert_equals_uint64(push_result, 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 10);

	memset(frames, 0, sizeof(frames));

	/* Now attempt to retrieve 100 frames. There are only 10 frames in the
	 * ring buffer, so the function should append 100-10 = 90 silence frames
	 * to the output. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		100,
		GST_CLOCK_TIME_NONE,
		0,
		0,
		/* This is a dummy variable since this argument can't be NULL; we do not actually use this */
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	/* Test that the first 10 frames contain the actual (former) content of the ring buffer. */ 
	for (i = 0; i < 10; ++i)
		assert_equals_int(frames[i], i + 10);
	/* Check that 90 silence frames were appended. */
	for (i = 10; i < 100; ++i)
		assert_equals_int(frames[i], 0);

	/* The ring buffer should be empty now. */
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 0);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(attempt_to_push_frames_when_full)
{
	/* Test attempts to push frames into a ring buffer that
	 * is full. We expect the push_frames call to fail. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(100) };
	gint16 frames[num_frames * NUM_CHANNELS];
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass 100 milliseconds as ring buffer length to match the
	 * ring buffer length to that of the input buffer. This
	 * makes it simpler to fully fill the ring buffer in one go. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_MSECOND * 100);
	fail_if(ring_buffer == NULL);

	/* Fill the ring buffer as preparation. */
	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);
	for (i = 0; i < num_frames; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames,
		0,
		GST_CLOCK_TIME_NONE
	);
	assert_equals_uint64(push_result, num_frames);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames);

	/* Now try to push 10 frames into the ring buffer. We expect this to return 0,
	 * indicating that 0 frames were pushed (because the ring buffer is already full). */
	memset(frames, 0, sizeof(frames));
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		10,
		0,
		GST_CLOCK_TIME_NONE
	);
	assert_equals_uint64(push_result, 0);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(attempt_to_push_frames_when_almost_full)
{
	/* Test attempts to push frames into a ring buffer that
	 * is full. We expect the push_frames call to only
	 * push the first N frames. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(100) - 4 };
	gint16 frames[num_frames * NUM_CHANNELS];
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass 100 milliseconds as ring buffer length to match the
	 * ring buffer length to that of the input buffer. This
	 * makes it simpler to fully fill the ring buffer in one go. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_MSECOND * 100);
	fail_if(ring_buffer == NULL);

	/* Fill the ring buffer as preparation. */
	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);
	for (i = 0; i < num_frames; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames,
		0,
		GST_CLOCK_TIME_NONE
	);
	assert_equals_uint64(push_result, num_frames);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames);

	/* Now try to push 10 frames into the ring buffer. We expect this to return 4,
	 * indicating that 4 frames were pushed (because the ring buffer is almost full). */
	memset(frames, 0, sizeof(frames));
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		10,
		0,
		GST_CLOCK_TIME_NONE
	);
	assert_equals_uint64(push_result, 4);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, CALC_NUM_FRAMES_FOR_MSECS(100));

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(basic_timestamped_io)
{
	/* Test basic, timestamped IO operations. This test works by pushing
	 * 1 ms worth of frames in several steps and timestamping those pushes. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 1 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_1ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 1);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Push another 1ms worth of data. We expect this data to be placed
	 * immediately after the already pushed frames. oldest_frame_pts is
	 * not supposed to be changed, since the newly pushed data is not
	 * the "oldest" one in the buffer. */
	for (i = 0; i < num_frames_for_1ms; ++i)
		frames[i] = i + 100;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms,
		0,
		GST_MSECOND * 11
	);
	assert_equals_uint64(push_result, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_1ms * 2);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 2);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i + num_frames_for_1ms], i + 100);

	// TODO: The tests below do not actually use an oldest_frame_pts timestamp
	// of 10ms, but rather a 13ms one. This is caused by the code in
	// gst_pw_audio_ring_buffer_push_frames() that updates oldest_frame_pts
	// even after it was initialized the first time. See the TODO comment
	// there for details. It should be 10ms. The 13ms need to be investigated.
	//
	/* Push another 1ms worth of data. This time we leave a "hole" of 3ms
	 * in between the timestamps. (That is, previous timestamp of 11ms + the
	 * duration of 1ms = 12ms, but we actually pass a timestamp of 15 ms,
	 * and 15-12 = 3.) This "hole" should *not* affect placement - the
	 * frames are still expected to be placed right after the already buffered
	 * ones. push_frames() only looks at the PTS in the very beginning, when
	 * oldest_frame_pts is set to GST_CLOCK_TIME_NONE. Afterwards it assumes
	 * that the PCM stream has no "holes". Any such holes need to be dealt with
	 * by the caller prior to calling push_frames() (for example by inserting
	 * silence frames). */
	for (i = 0; i < num_frames_for_1ms; ++i)
		frames[i] = i + 200;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms,
		0,
		GST_MSECOND * 15
	);
	assert_equals_uint64(push_result, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 13);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_1ms * 3);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 3);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i + num_frames_for_1ms * 2], i + 200);

	/* Try to retrieve 2.5 ms worth of frames at timestamp 10ms. That equals
	 * the value of oldest_frame_pts, so we expect this call to return oldest
	 * 2.5 ms from the ring buffer. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 25 / 10,
		GST_MSECOND * 13,
		0,
		0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	assert_equals_int(buffered_frames_to_retrieval_pts_delta, 0);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(frames[i + num_frames_for_1ms * 0], i + 10);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(frames[i + num_frames_for_1ms * 1], i + 100);
	for (i = 0; i < num_frames_for_1ms / 2; ++i)
		assert_equals_int(frames[i + num_frames_for_1ms * 2], i + 200);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(buffered_frames_fully_in_the_future)
{
	/* Test what happens when trying to retrieve frames when the buffered
	 * frames lie fully in the future relative to the retrieval PTS. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 1 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_1ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 1);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Try to retrieve 2ms worth of frames at timestamp 1ms. Since oldest_frame_pts
	 * is set to 10ms, there is no data within the 1ms .. 3ms timestamp window to
	 * be retrieved. Consequently, we expect retrieve_frames to return
	 * GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 2,
		GST_MSECOND * 1,
		0,
		0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(buffered_frames_fully_in_the_past)
{
	/* Test what happens when trying to retrieve frames when the buffered
	 * frames lie fully in the past relative to the retrieval PTS. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 1 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_1ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_1ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 1);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Try to retrieve 2ms worth of frames at timestamp 100ms. Since oldest_frame_pts
	 * is set to 10ms, there is no data within the 100ms .. 120ms timestamp window to
	 * be retrieved. Consequently, we expect retrieve_frames to return
	 * GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST. */
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 2,
		GST_MSECOND * 100,
		0,
		0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, 0);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(buffered_frames_partially_in_the_future)
{
	/* Test what happens when trying to retrieve frames when the buffered
	 * frames lie partially in the future relative to the retrieval PTS. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	enum { num_frames_for_10ms = num_frames_for_1ms * 10 };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 10 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_10ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_10ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 10);
	for (i = 0; i < 48; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Try to retrieve 3ms worth of frames at timestamp 9ms. Since oldest_frame_pts
	 * is set to 10ms, we can't produce data for the first one out of the 3 requested
	 * milliseconds. Consequently, we prepend the output with 1ms worth of silence
	 * frames and then output the oldest 2 ms from the ring buffer. */
	memset(frames, 0, sizeof(frames));
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 3,
		GST_MSECOND * 9,
		0,
		GST_MSECOND * 0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	assert_equals_int(buffered_frames_to_retrieval_pts_delta, 0);
	for (i = 0; i < num_frames_for_1ms; ++i)
		assert_equals_int(frames[i], 0);
	for (i = num_frames_for_1ms; i < (num_frames_for_1ms * 3); ++i)
		assert_equals_int(frames[i], i - num_frames_for_1ms + 10);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(buffered_frames_partially_in_the_past)
{
	/* Test what happens when trying to retrieve frames when the buffered
	 * frames lie partially in the past relative to the retrieval PTS. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	enum { num_frames_for_10ms = num_frames_for_1ms * 10 };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 10 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_10ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_10ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 10);
	for (i = 0; i < 48; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Try to retrieve 3ms worth of frames at timestamp 9ms. Since oldest_frame_pts
	 * is set to 10ms, we can't produce data for the first one out of the 3 requested
	 * milliseconds. Consequently, we prepend the output with 1ms worth of silence
	 * frames and then output the oldest 2 ms from the ring buffer. */
	memset(frames, 0, sizeof(frames));
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 3,
		GST_MSECOND * 11,
		0,
		GST_MSECOND * 0,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	assert_equals_int(buffered_frames_to_retrieval_pts_delta, 0);
	/* The " + num_frames_for_1ms" part is there because the oldest 1ms of frames was skipped. */
	for (i = 0; i < (num_frames_for_1ms * 3); ++i)
		assert_equals_int(frames[i], i + 10 + num_frames_for_1ms);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


GST_START_TEST(buffered_frames_partially_in_the_future_within_skew_threshold)
{
	/* Test the behavior of the skew threshold when retrieving
	 * frames with a timestamp that exhibits some jitter. */

	GstPwAudioFormat format = {
		.audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM,
	};
	GstPwAudioRingBuffer *ring_buffer;
	gsize push_result;
	enum { num_frames = CALC_NUM_FRAMES_FOR_MSECS(20) };
	enum { num_frames_for_1ms = CALC_NUM_FRAMES_FOR_MSECS(1) };
	enum { num_frames_for_10ms = num_frames_for_1ms * 10 };
	gint16 frames[num_frames * NUM_CHANNELS];
	GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
	GstPwAudioRingBufferRetrievalResult retrieval_result;
	guint i;

	gst_audio_info_set_format(
		&(format.info.pcm_audio_info),
		PCM_SAMPLE_FORMAT,
		PCM_SAMPLE_RATE,
		NUM_CHANNELS,
		NULL
	);

	/* Pass GST_SECOND as ring buffer length to create one with
	 * as many PCM frames as PCM_SAMPLE_RATE indicates. */
	ring_buffer = gst_pw_audio_ring_buffer_new(&format, GST_SECOND);
	fail_if(ring_buffer == NULL);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_CLOCK_TIME_NONE);

	memset(ring_buffer->buffered_frames, 0, ring_buffer->stride * ring_buffer->metrics.capacity);

	/* Push 10 ms of PCM data at timestamp 10ms. We expect these frames
	 * to be placed at the beginning of the ring buffer's memory block.
	 * This timestamp is also expected to be set as oldest_frame_pts. */
	for (i = 0; i < num_frames_for_10ms; ++i)
		frames[i] = i + 10;
	push_result = gst_pw_audio_ring_buffer_push_frames(
		ring_buffer,
		frames,
		num_frames_for_10ms,
		0,
		GST_MSECOND * 10
	);
	assert_equals_uint64(push_result, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->oldest_frame_pts, GST_MSECOND * 10);
	assert_equals_uint64(ring_buffer->metrics.current_num_buffered_frames, num_frames_for_10ms);
	assert_equals_uint64(ring_buffer->current_fill_level, GST_MSECOND * 10);
	for (i = 0; i < 48; ++i)
		assert_equals_int(((gint16 *)(ring_buffer->buffered_frames))[i], i + 10);

	/* Try to retrieve 3ms worth of frames at timestamp 9ms, but with a skew
	 * threshold of 100ms. The 9ms timestamp deviates from the oldest_frame_pts
	 * value, which is set to 10ms. But this deviation of 1ms is less than
	 * the skew threshold of 100ms, so we expect retrieve_frames() to _not_
	 * skip frames or insert silence frames; instead, we expect the
	 * buffered_frames_to_retrieval_pts_delta value to be set to -1ms, and the
	 * returned frames to match the oldest 1ms frames from the ring bufer.
	 * (buffered_frames_to_retrieval_pts_delta is expected to be -1ms, not
	 * 1ms, since the direction matters - it is the delta _from_ the retrieval
	 * PTS _to_ the value of oldest_frame_pts.) */
	memset(frames, 0, sizeof(frames));
	retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
		ring_buffer,
		frames,
		num_frames_for_1ms * 3,
		GST_MSECOND * 9,
		0,
		GST_MSECOND * 100,
		&buffered_frames_to_retrieval_pts_delta
	);
	assert_equals_int(retrieval_result, GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK);
	assert_equals_int(buffered_frames_to_retrieval_pts_delta, GST_MSECOND * (-1));
	for (i = 0; i < (num_frames_for_1ms * 3); ++i)
		assert_equals_int(frames[i], i + 10);

	gst_object_unref(GST_OBJECT(ring_buffer));
}
GST_END_TEST


static Suite * gst_pw_audio_ring_buffer_suite(void)
{
	Suite *s = suite_create("gst_pipewire_dsd_convert");
	TCase *tc = tcase_create("general");

	suite_add_tcase(s, tc);
	tcase_add_test(tc, creation_and_initial_states);
	tcase_add_test(tc, basic_io);
	tcase_add_test(tc, attempt_to_retrieve_more_frames_than_available);
	tcase_add_test(tc, attempt_to_push_frames_when_full);
	tcase_add_test(tc, attempt_to_push_frames_when_almost_full);
	tcase_add_test(tc, basic_timestamped_io);
	tcase_add_test(tc, buffered_frames_fully_in_the_future);
	tcase_add_test(tc, buffered_frames_fully_in_the_past);
	tcase_add_test(tc, buffered_frames_partially_in_the_future);
	tcase_add_test(tc, buffered_frames_partially_in_the_past);
	tcase_add_test(tc, buffered_frames_partially_in_the_future_within_skew_threshold);

	return s;
}

GST_CHECK_MAIN(gst_pw_audio_ring_buffer)

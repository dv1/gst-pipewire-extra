#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include "gstpwstreamclock.h"


static GstClockTime test_sysclock_time = 0;

static GstClockTime get_test_sysclock_time(G_GNUC_UNUSED GstPwStreamClock *clock)
{
	return test_sysclock_time;
}


#define ADD_OBSERVATION(clock, driver_clock_time, system_clock_time) \
	G_STMT_START { \
		struct pw_time t = { \
			.now = (system_clock_time), \
			.ticks = (driver_clock_time), \
			.rate = { .num = 1, .denom = GST_SECOND } \
		}; \
		gst_pw_stream_clock_add_observation(clock, &t); \
	} G_STMT_END


GST_START_TEST(initial_behavior)
{
	/* Here we test the initial behavior of the clock, the timestamps
	 * it produces before any observations are added, and how the first
	 * few observations affect its behavior. */

	GstPwStreamClock *clock;
	GstClockTime t;

	/* Create our pwstreamclock when the simulated sysclock is at timestamp 1000. */
	test_sysclock_time = 1000;
	clock = gst_pw_stream_clock_new(get_test_sysclock_time);

	/* We expect the pwstreamclock to always start at timestamp 0 even if the
	 * sysclock isn't at timestamp 0. */
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Initially, the clock is in a "frozen" state, so we expect it to keep
	 * returning timestamp 0, even after 10 wall-clock milliseconds passed. */
	g_usleep(10000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Test the frozen state further by advancing the simulated clock.
	 * It should still return 0 because it is "frozen". */
	test_sysclock_time = 2000000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Add an observation. This un-freezes the clock. However, the very first
	 * timestamp is expected to still return 0 since we advance the simulated
	 * sysclock to be at the exact same timestamp as the observation. */
	ADD_OBSERVATION(clock, 4000, 10000);
	test_sysclock_time = 10000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Advance the simulated sysclock a little. We expect a timestamp of 100,
	 * since the pwstreamclock only saw 1 observation so far, so it currently
	 * is calculated with a driver clock rate of 1.0, meaning that a sysclock
	 * delta of 100 equals a driver clock delta of 100. */
	test_sysclock_time = 10100;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 100);

	/* Add a second observation. Now the pwstreamclock can actually calculate
	 * a driver clock rate. We expect a clock rate of:
	 *  (5000 - 4000) / (12000 - 1000) = 1000 / 2000 = 1/2
	 * Thus, a sysclock advance of 4000 results in a driver clock advance of
	 * 2000 for example. We expect the pwstreamclock to return a timestamp
	 * of 1000, since the first observation contained a driver clock time
	 * of 4000, and this one has a timestamp of 5000. */
	ADD_OBSERVATION(clock, 5000, 12000);
	test_sysclock_time = 12000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 1000);

	/* We advance the sysclock by a delta of 2000. Since the second observation
	 * previously added above caused the pwstreamclock to calculate a rate of
	 * 1/2, we expect a driver clock time advance of half of that (= 1000). */
	test_sysclock_time = 14000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);
}
GST_END_TEST;


GST_START_TEST(frozen_clock)
{
	GstPwStreamClock *clock;
	GstClockTime t;

	/* Create our pwstreamclock when the simulated sysclock is at timestamp 1000
	 * and immediately add an observation to start off in an unfrozen state. */
	test_sysclock_time = 1000;
	clock = gst_pw_stream_clock_new(get_test_sysclock_time);
	ADD_OBSERVATION(clock, 500, 1000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Advance the simulated sysclock by a value of 100. Since we only added
	 * one observation to the clock so far, the pwstreamclock currently
	 * uses a default internal rate of 1.0, the sysclock advance of 100 is
	 * translated to a driver clock advance of 100. */
	test_sysclock_time = 1100;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 100);

	/* Add another observation to cause the pwstreamclock to calculate
	 * an internal driver clock rate of 1/2. Due to that rate, we expect a
	 * sysclock advance of 1000 to result in a driver clock advance of 500. */
	test_sysclock_time = 2000;
	ADD_OBSERVATION(clock, 1000, 2000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 500);

	/* Freeze the clock. After this call, we expect the clock to keep
	 * returning the last produced timestamp (which was 500). */
	gst_pw_stream_clock_freeze(clock);

	/* Advance the sysclock and check that the pwstreamclock still
	 * returns the same timestamp 500 in its frozen state. */
	test_sysclock_time = 2500;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 500);

	/* Now add an observation at sysclock time 5000 and driver clock 2200.
	 * We still expect a returned timestamp 500 since we also advance
	 * the sysclock to 5000. But from now on, sysclock advances should
	 * cause the pwstreamclock to advance its timestamps. Also, unfreezing
	 * retains the driver rate that was used prior to the freeze. That
	 * rate was 1/2, so we expect that to be used. */
	test_sysclock_time = 5000;
	ADD_OBSERVATION(clock, 2200, 5000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 500);

	/* Advance the sysclock by a value of 100. We expect the driver
	 * clock timestamps to advance by 50 due to the 1/2 rate. */
	test_sysclock_time = 5100;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 550);
}
GST_END_TEST;


GST_START_TEST(extrapolation_overshoot)
{
	GstPwStreamClock *clock;
	GstClockTime t;

	/* Create our pwstreamclock when the simulated sysclock is at timestamp 1000
	 * and immediately add an observation to start off in an unfrozen state. */
	test_sysclock_time = 1000;
	clock = gst_pw_stream_clock_new(get_test_sysclock_time);
	ADD_OBSERVATION(clock, 500, 1000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 0);

	/* Advance the simulated sysclock to let the pwstreamclock propduce
	 * the timestamp 2000. */
	test_sysclock_time = 3000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);

	/* Add an observation whose driver clock timestamp is smaller than the one
	 * we produced above (we produced timestamp 2000, observation has timestamp 1500).
	 * This simulates an extrapolation overshoot - the timestamp 2000 from earlier
	 * overshot, since the observation tells us that the driver clock time is actually
	 * at 1500, not 2000. We expect the pwstreamclock to still return 2000 to ensure
	 * monotonically increasing output, and to keep doing so until the internal
	 * extrapolation yields a timestamp that exceeds 2000. */
	ADD_OBSERVATION(clock, 1500, 3000);
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);

	/* At sysclock time 3100, the pwstreamclock would normally return
	 * (3100 - 3000) * 0.5 + (1500 -500) = 1050. (0.5 is the clock rate, and
	 * the -500 comes from the initial driver clock timestamp in the very
	 * first observation.)
	 * But since 1050 < 2000, the clock still returns 2000, again to
	 * ensure monotonically increasing timestamp behavior. */
	test_sysclock_time = 3100;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);

	/* Same as before: (3500 - 3000) * 0.5 + (1500 -500) = 1250, but 1250 < 2000. */
	test_sysclock_time = 3500;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);

	/* Here the pwstreamclock "catches up" with the previously highest timestamp
	 * 2000, because: (5000 - 3000) * 0.5 + (1500 -500) = 2000. From now on,
	 * any further advancing of the simulated sysclock will yield advances in
	 * the driver clock timestamps, that is, we no longer expect the clock to
	 * return 2000 after this call. */
	test_sysclock_time = 5000;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2000);

	/* We expect 2100 because: (5200 - 3000) * 0.5 + (1500 -500) = 2100
	 * and 2100 > 2000. */
	test_sysclock_time = 5200;
	t = gst_clock_get_internal_time(GST_CLOCK(clock));
	assert_equals_uint64(t, 2100);
}
GST_END_TEST;


static Suite * gst_pw_stream_clock_suite(void)
{
	Suite *s = suite_create("GstPwStreamClock");
	TCase *tc = tcase_create("general");

	suite_add_tcase(s, tc);
	tcase_add_test(tc, initial_behavior);
	tcase_add_test(tc, frozen_clock);
	tcase_add_test(tc, extrapolation_overshoot);

	return s;
}

GST_CHECK_MAIN(gst_pw_stream_clock);

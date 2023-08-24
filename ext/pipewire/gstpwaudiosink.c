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

/* Basic idea of this sink is to do most of the heavy lifting in render(),
 * and avoid any unnecessary work in on_process_stream(), especially work
 * that has very variable and/or unpredictable runtime complexity. For
 * example, translating timestamps is done in render(). Blocking wait
 * during pause is done in render() etc. */

#include <gst/gst.h>
/* Turn off -Wdeprecated-declarations to mask the "g_memdup is deprecated"
 * warning (originating in gst/base/gstbytereader.h) that is present in
 * many GStreamer installations. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gst/base/base.h>
#pragma GCC diagnostic pop
#include <gst/audio/audio.h>

#include <stdint.h>
#include <string.h>

/* Turn off -pedantic to mask the "ISO C forbids braced-groups within expressions"
 * warnings that occur because PipeWire uses such braced-groups extensively. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pipewire/pipewire.h>
#include <spa/node/io.h>
#include <spa/utils/result.h>
#pragma GCC diagnostic pop

#include "gstpipewirecore.h"
#include "gstpwstreamclock.h"
#include "gstpwaudioformat.h"
#include "gstpwaudiosink.h"
#include "gstpwaudioringbuffer.h"
#include "pi_controller.h"


GST_DEBUG_CATEGORY(pw_audio_sink_debug);
#define GST_CAT_DEFAULT pw_audio_sink_debug


#define COLOR_GREEN "\033[32m"
#define COLOR_DEFAULT "\033[0m"


enum
{
	PROP_0,

	PROP_PROVIDE_CLOCK,
	PROP_ALIGNMENT_THRESHOLD,
	PROP_SKEW_THRESHOLD,
	PROP_TARGET_OBJECT_ID,
	PROP_STREAM_PROPERTIES,
	PROP_SOCKET_FD,
	PROP_RING_BUFFER_LENGTH,
	PROP_APP_NAME,
	PROP_NODE_NAME,
	PROP_NODE_DESCRIPTION,
	PROP_CACHE_PROBED_CAPS,

	PROP_LAST
};


#define DEFAULT_PROVIDE_CLOCK TRUE
#define DEFAULT_ALIGNMENT_THRESHOLD (GST_MSECOND * 40)
#define DEFAULT_SKEW_THRESHOLD (GST_MSECOND * 1)
#define DEFAULT_TARGET_OBJECT_ID PW_ID_ANY
#define DEFAULT_STREAM_PROPERTIES NULL
#define DEFAULT_SOCKET_FD (-1)
#define DEFAULT_RING_BUFFER_LENGTH 100
#define DEFAULT_APP_NAME NULL
#define DEFAULT_NODE_NAME NULL
#define DEFAULT_NODE_DESCRIPTION NULL
#define DEFAULT_CACHE_PROBED_CAPS TRUE

#define LOCK_AUDIO_DATA_BUFFER_MUTEX(pw_audio_sink) g_mutex_lock(&((pw_audio_sink)->audio_data_buffer_mutex))
#define UNLOCK_AUDIO_DATA_BUFFER_MUTEX(pw_audio_sink) g_mutex_unlock(&((pw_audio_sink)->audio_data_buffer_mutex))

#define LOCK_LATENCY_MUTEX(pw_audio_sink) g_mutex_lock(&((pw_audio_sink)->latency_mutex))
#define UNLOCK_LATENCY_MUTEX(pw_audio_sink) g_mutex_unlock(&((pw_audio_sink)->latency_mutex))

/* Factors for the PI controller. Empirically picked. */
#define PI_CONTROLLER_KI_FACTOR 0.01
#define PI_CONTROLLER_KP_FACTOR 0.15

/* Factors for converting PTS deltas into PPM quantities for the PI controller. */
#define MAX_DRIFT_PTS_DELTA (5 * GST_MSECOND)
#define MAX_DRIFT_PPM 10000


struct _GstPwAudioSink
{
	GstBaseSink parent;

	/*< private >*/

	/** Object properties **/

	GstClockTimeDiff alignment_threshold;
	GstClockTimeDiff skew_threshold;
	uint32_t target_object_id;
	GstStructure *stream_properties;
	int socket_fd;
	guint ring_buffer_length_in_ms;
	gchar *app_name;
	gchar *node_name;
	gchar *node_description;
	gboolean cache_probed_caps;

	/** Playback format **/

	GstCaps *sink_caps;
	GstPwAudioFormat pw_audio_format;
	GstPwAudioFormatProbe *format_probe;
	GstPipewireDsdFormat actual_dsd_format;
	gsize stride;
	guint dsd_data_rate_multiplier;
	guint dsd_buffer_size_multiplier;
	GMutex probe_process_mutex;
	GstCaps *cached_probed_caps;

	/** Buffers for audio data **/

	/* NOTE: Access to the these data structures and PTS & current fill level states
	 * requires the audio_data_buffer_mutex to be locked if the pw_stream is connected.
	 *
	 * The ring_buffer is used for raw audio, the encoded_data_queue for encoded audio. */
	GstPwAudioRingBuffer *ring_buffer;
	GMutex audio_data_buffer_mutex;
	GCond audio_data_buffer_cond;
	GstQueueArray *encoded_data_queue;
	GstClockTime total_queued_encoded_data_duration;
	gsize dsd_conversion_buffer_size;
	guint8 *dsd_conversion_buffer;

	/** PCM clock drift compensation states **/

	/* PI controller for filtering the PTS delta that comes from the ring buffer. */
	PIController pi_controller;
	/* Timestamp of previous tick to calculate the time_scale that gets
	 * passed to the pi_controller_compute() function. */
	GstClockTime previous_time;

	/** Misc playback states **/

	/* Set to 1 during the flush-start event, and back to 0 during the flush-stop one.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint flushing;
	/* Set to 1 during the PLAYING->PAUSED state change (not during READY->PAUSED!).
	 * Set back to 0 during PAUSED->PLAYING, and also during PAUSED->READY in case
	 * the pipeline is paused and gets shut down.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint paused;
	/* Set to 1 in the process callback if the stream delay has changed.
	 * Read in render(). If it is set to 1, the code in render() will
	 * post a latency message to inform the pipeline about the stream
	 * delay (as latency). This is not done directly in the process callback
	 * because that function must finish as quickly as possible, and
	 * posting a gstmessage could potentially block that function for
	 * an unpredictable amount of time.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint notify_upstream_about_stream_delay;
	/* The process callback will pass the current pipeline clock time to
	 * gst_pw_audio_ring_buffer_retrieve_frames(). If synced_playback_started is FALSE,
	 * that function will be given a skew threshold of 0, forcing the ring buffer to
	 * resynchronize itself against that current time. This ensures that this function
	 * call syncs its output exactly at the beginning of the synchronization, which is
	 * very important to avoid heavily unsynced output in the beginning. Once this call
	 * finishes, synced_playback_started is set to TRUE, and the normal skew threshold
	 * is used, since the skew threshold applies to playback that is already in sync.
	 * synced_playback_started is set back to FALSE in case of underruns, pw_stream
	 * output discontinuities (see the pw_time checks in the process callback),
	 * flush events, and when the ring buffer's data is fully expired.
	 * Access to this field requires the audio_data_buffer_mutex to be locked
	 * if the pw_stream is connected. */
	gboolean synced_playback_started;
	/* Used for checking the buffer PTS for discontinuities. */
	GstClockTime expected_next_running_time_pts;
	/* Pipeline latency in nanoseconds. Set when the latency event
	 * is processed in send_event(). */
	GstClockTime latency;
	/* The latency_mutex synchronizes access to latency and stream_delay_in_ns. */
	GMutex latency_mutex;
	/* Set to true in the on_stream_drained() callback. Used for waiting until the
	 * pw_stream itself is drained. */
	gboolean stream_drained;
	/* Relevant for encoded audio. If the encoded audio frames are larger than the
	 * requested audio length during a cycle, then this counter keeps track of the
	 * excess playtime that is sent into the graph. It is not possible to subdivide
	 * an encoded frame, so if it is longer than the quantum, it still has to be sent
	 * as-is. By keeping track of the excess, the gst_pw_audio_sink_render_encoded()
	 * function can produce "null frames" at appropriate times to compensate for the
	 * excess playtime, avoiding an overflow in the PipeWire sink. */
	GstClockTime accum_excess_encaudio_playtime;
	/* This is used for determining when the pw_stream's latency property needs an
	 * update. The unit is _not_ nanoseconds; rather, it uses rate ticks (rate as in
	 * the rate field in pw_audio_format.info.encoded_audio_info.rate). */
	guint64 last_encoded_frame_length;

	/** Element clock **/

	/* Element clock based on the pw_stream. Always available, since it gets timestamps
	 * from the monotonic system clock and adjusts them according to the pw_stream
	 * feedback (see the io_changed() callback). */
	GstPwStreamClock *stream_clock;
	/* True if the stream_clock is set as the pipeline clock, or in other words,
	 * is GST_ELEMENT_CLOCK(sink) == stream_clock .
	 * Access to this field requires the audio_data_buffer_mutex to be locked
	 * if the pw_stream is connected. */
	gboolean stream_clock_is_pipeline_clock;

	/** PipeWire specifics **/

	GstPipewireCore *pipewire_core;
	struct pw_stream *stream;
	gboolean stream_is_connected;
	gboolean stream_is_active;
	struct spa_hook stream_listener;
	gboolean stream_listener_added;
	/* The pointer to the SPA IO position is received in the
	 * io_changed stream event and accessed in the process event. */
	struct spa_io_position *spa_position;
	/* The pointer to the SPA IO RateMatch is received in the
	 * io_changed stream event and accessed in the process event. */
	struct spa_io_rate_match *spa_rate_match;
	/* The stream delay is originally given in ticks by PipeWire.
	 * We retain that original quantity to be able to later detect
	 * changes in the stream delay. */
	gint64 stream_delay_in_ticks;
	/* Stream delay in nanoseconds. Access to this quantity
	 * requires the latency_mutex lock to be taken
	 * if the pw_stream is connected. */
	gint64 stream_delay_in_ns;
	/* Quantum size in driver ticks. Set in the io_changed callback
	 * when it is passed SPA_IO_Position information. */
	guint64 quantum_size_in_ticks;
	/* Quantum size in nanoseconds. */
	guint64 quantum_size_in_ns;
	/* The value of the pw_time.ticks result of the last pw_stream_get_time_n()
	 * call in the process callback. The difference between this and the current
	 * pw_time.ticks result must be <= quantum_size_in_ticks. Otherwise, a
	 * discontinuity happened (ALSA buffer underrun for example). This allows us
	 * to detect these discontinuities and resynchronize playback when they happen. */
	guint64 last_pw_time_ticks;
	gboolean last_pw_time_ticks_set;
	/* Snapshot of GObject property values, done in gst_pw_audio_sink_start().
	 * This is done to prevent potential race conditions if the user changes
	 * these properties while they are being read. Making these copies
	 * eliminates the need for a mutex lock. */
	GstClockTimeDiff skew_threshold_snapshot;
	GstClockTime ring_buffer_length_snapshot;
};


struct _GstPwAudioSinkClass
{
	GstBaseSinkClass parent_class;
};


G_DEFINE_TYPE(GstPwAudioSink, gst_pw_audio_sink, GST_TYPE_BASE_SINK)


static void gst_pw_audio_sink_dispose(GObject *object);
static void gst_pw_audio_sink_finalize(GObject *object);
static void gst_pw_audio_sink_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_pw_audio_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static GstStateChangeReturn gst_pw_audio_sink_change_state(GstElement *element, GstStateChange transition);
static GstClock* gst_pw_audio_sink_provide_clock(GstElement *element);
static gboolean gst_pw_audio_sink_set_clock(GstElement *element, GstClock *clock);
static gboolean gst_pw_audio_sink_send_event(GstElement *element, GstEvent *event);
static gboolean gst_pw_audio_sink_query(GstElement *element, GstQuery *query);

static gboolean gst_pw_audio_sink_set_caps(GstBaseSink *basesink, GstCaps *caps);
static GstCaps* gst_pw_audio_sink_get_caps(GstBaseSink *basesink, GstCaps *filter);
static GstCaps* gst_pw_audio_sink_fixate(GstBaseSink *basesink, GstCaps *caps);

static void gst_pw_audio_sink_get_times(GstBaseSink *basesink, GstBuffer *buffer, GstClockTime *start, GstClockTime *end);

static gboolean gst_pw_audio_sink_start(GstBaseSink *basesink);
static gboolean gst_pw_audio_sink_stop(GstBaseSink *basesink);

static gboolean gst_pw_audio_sink_query_pad(GstBaseSink *basesink, GstQuery *query);

static gboolean gst_pw_audio_sink_event(GstBaseSink *basesink, GstEvent *event);
static GstFlowReturn gst_pw_audio_sink_wait_event(GstBaseSink *basesink, GstEvent *event);

static GstFlowReturn gst_pw_audio_sink_preroll(GstBaseSink *basesink, GstBuffer *incoming_buffer);
static GstFlowReturn gst_pw_audio_sink_render(GstBaseSink *basesink, GstBuffer *incoming_buffer);

static GstFlowReturn gst_pw_audio_sink_render_raw(GstPwAudioSink *self, GstBuffer *original_incoming_buffer);
static GstFlowReturn gst_pw_audio_sink_render_encoded(GstPwAudioSink *self, GstBuffer *original_incoming_buffer);

static gboolean gst_pw_audio_sink_handle_convert_query(GstPwAudioSink *self, GstQuery *query);

static void gst_pw_audio_sink_set_provide_clock_flag(GstPwAudioSink *self, gboolean flag);
static gboolean gst_pw_audio_sink_get_provide_clock_flag(GstPwAudioSink *self);

static void gst_pw_audio_sink_activate_stream_unlocked(GstPwAudioSink *self, gboolean activate);
static void gst_pw_audio_sink_setup_audio_data_buffer(GstPwAudioSink *self);
static void gst_pw_audio_sink_teardown_audio_data_buffer(GstPwAudioSink *self);
static void gst_pw_audio_sink_reset_audio_data_buffer_unlocked(GstPwAudioSink *self);
static void gst_pw_audio_sink_reset_drift_compensation_states(GstPwAudioSink *self);
static void gst_pw_audio_sink_drain_stream_unlocked(GstPwAudioSink *self);
static void gst_pw_audio_sink_drain_stream_and_audio_data_buffer(GstPwAudioSink *self);
static void gst_pw_audio_sink_disconnect_stream(GstPwAudioSink *self);


/* pw_stream callbacks for both raw and encoded data. */

static void gst_pw_audio_sink_pw_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error);
static void gst_pw_audio_sink_param_changed(void *data, uint32_t id, const struct spa_pod *param);
static void gst_pw_audio_sink_io_changed(void *data, uint32_t id, void *area, uint32_t size);
static void gst_pw_audio_sink_on_stream_drained(void *data);


/* pw_stream callbacks for raw data. */

static void gst_pw_audio_sink_raw_on_process_stream(void *data);

static const struct pw_stream_events raw_stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = gst_pw_audio_sink_pw_state_changed,
	.param_changed = gst_pw_audio_sink_param_changed,
	.io_changed = gst_pw_audio_sink_io_changed,
	.drained = gst_pw_audio_sink_on_stream_drained,
	.process = gst_pw_audio_sink_raw_on_process_stream,
};


/* pw_stream callbacks for encoded data. */

static void gst_pw_audio_sink_encoded_on_process_stream(void *data);

static const struct pw_stream_events encoded_stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = gst_pw_audio_sink_pw_state_changed,
	.param_changed = gst_pw_audio_sink_param_changed,
	.io_changed = gst_pw_audio_sink_io_changed,
	.drained = gst_pw_audio_sink_on_stream_drained,
	.process = gst_pw_audio_sink_encoded_on_process_stream,
};




static void gst_pw_audio_sink_class_init(GstPwAudioSinkClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstBaseSinkClass *base_sink_class;
	GstCaps *template_caps;

	GST_DEBUG_CATEGORY_INIT(pw_audio_sink_debug, "pwaudiosink", 0, "PipeWire audio sink");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	base_sink_class = GST_BASE_SINK_CLASS(klass);

	template_caps = gst_pw_audio_format_get_template_caps();
	gst_element_class_add_pad_template(
		element_class,
		gst_pad_template_new(
			"sink",
			GST_PAD_SINK,
			GST_PAD_ALWAYS,
			template_caps
		)
	);
	gst_caps_unref(template_caps);

	object_class->dispose      = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_dispose);
	object_class->finalize     = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_finalize);
	object_class->set_property = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_set_property);
	object_class->get_property = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_get_property);

	element_class->change_state  = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_change_state);
	element_class->provide_clock = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_provide_clock);
	element_class->set_clock     = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_set_clock);
	element_class->send_event    = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_send_event);
	element_class->query         = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_query);

	base_sink_class->set_caps    = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_set_caps);
	base_sink_class->get_caps    = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_get_caps);
	base_sink_class->fixate      = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_fixate);
	base_sink_class->get_times   = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_get_times);
	base_sink_class->start       = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_start);
	base_sink_class->stop        = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_stop);
	base_sink_class->query       = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_query_pad);
	base_sink_class->event       = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_event);
	base_sink_class->wait_event  = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_wait_event);
	base_sink_class->preroll     = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_preroll);
	base_sink_class->render      = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_render);

	g_object_class_install_property(
		object_class,
		PROP_PROVIDE_CLOCK,
		g_param_spec_boolean(
			"provide-clock",
			"Provide Clock",
			"Provide a clock to be used as the global pipeline clock",
			DEFAULT_PROVIDE_CLOCK,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_ALIGNMENT_THRESHOLD,
		g_param_spec_int64(
			"alignment-threshold",
			"Alignment threshold",
			"How far apart buffers can maximally be to still be considered continuous, in nanoseconds",
			0, G_MAXINT64,
			DEFAULT_ALIGNMENT_THRESHOLD,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_SKEW_THRESHOLD,
		g_param_spec_int64(
			"skew-threshold",
			"Skew threshold",
			"How far apart current pipeline clock time can be from the timestamp of buffered "
			"data before skewing is performed to compensate the drift, in nanoseconds",
			0, G_MAXINT64,
			DEFAULT_SKEW_THRESHOLD,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_TARGET_OBJECT_ID,
		g_param_spec_uint(
			"target-object-id",
			"Target object ID",
			"PipeWire target object id to connect to (default = let the PipeWire manager select a target)",
			0, G_MAXUINT,
			DEFAULT_TARGET_OBJECT_ID,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_STREAM_PROPERTIES,
		g_param_spec_boxed(
			"stream-properties",
			"Stream properties",
			"List of PipeWire stream properties to add to this sink's client PipeWire node",
			GST_TYPE_STRUCTURE,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_SOCKET_FD,
		g_param_spec_int(
			"socket-fd",
			"Socket file descriptor",
			"File descriptor of connected socket to use for communicating with the PipeWire daemon (-1 = open custom internal socket)",
			-1, G_MAXINT,
			DEFAULT_SOCKET_FD,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_RING_BUFFER_LENGTH,
		g_param_spec_uint(
			"ring-buffer-length",
			"Ring buffer length",
			"The length of the ring buffer that is used with continuous data, in milliseconds (if filled to this capacity, sink will block until there's room in the buffer)",
			1, G_MAXUINT,
			DEFAULT_RING_BUFFER_LENGTH,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_APP_NAME,
		g_param_spec_string(
			"app-name",
			"App name",
			"Name of the application that uses this sink; example: \"Totem Media Player\" (NULL = default)",
			DEFAULT_APP_NAME,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_NODE_NAME,
		g_param_spec_string(
			"node-name",
			"Node name",
			"Name to use for this sink's client PipeWire node (NULL = default)",
			DEFAULT_NODE_NAME,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_NODE_DESCRIPTION,
		g_param_spec_string(
			"node-description",
			"Node description",
			"One-line human readable description of this sink's client PipeWire node; example: \"Bluetooth headset\" (NULL = default)",
			DEFAULT_NODE_DESCRIPTION,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_CACHE_PROBED_CAPS,
		g_param_spec_boolean(
			"cache-probed-caps",
			"Cache proped caps",
			"Cache the caps that get probed during the first caps query after the element started",
			DEFAULT_CACHE_PROBED_CAPS,
			(GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
		)
	);

	gst_element_class_set_static_metadata(
		element_class,
		"pwaudiosink",
		"Sink/Audio",
		"Sink for sending audio data to a PipeWire graph",
		"Carlos Rafael Giani <crg7475@mailbox.org>"
	);
}


static void gst_pw_audio_sink_init(GstPwAudioSink *self)
{
	self->alignment_threshold = DEFAULT_ALIGNMENT_THRESHOLD;
	self->skew_threshold = DEFAULT_SKEW_THRESHOLD;
	self->target_object_id = DEFAULT_TARGET_OBJECT_ID;
	self->stream_properties = DEFAULT_STREAM_PROPERTIES;
	self->socket_fd = DEFAULT_SOCKET_FD;
	self->ring_buffer_length_in_ms = DEFAULT_RING_BUFFER_LENGTH;
	self->app_name = g_strdup(DEFAULT_APP_NAME);
	self->node_name = g_strdup(DEFAULT_NODE_NAME);
	self->node_description = g_strdup(DEFAULT_NODE_DESCRIPTION);
	self->cache_probed_caps = DEFAULT_CACHE_PROBED_CAPS;

	self->sink_caps = NULL;
	memset(&(self->pw_audio_format), 0, sizeof(self->pw_audio_format));
	self->format_probe = NULL;
	g_mutex_init(&(self->probe_process_mutex));
	self->cached_probed_caps = NULL;

	self->ring_buffer = NULL;
	g_mutex_init(&(self->audio_data_buffer_mutex));
	g_cond_init(&(self->audio_data_buffer_cond));
	self->encoded_data_queue = NULL;
	self->total_queued_encoded_data_duration = 0;
	self->dsd_conversion_buffer_size = 0;
	self->dsd_conversion_buffer = NULL;

	pi_controller_init(&(self->pi_controller), PI_CONTROLLER_KI_FACTOR, PI_CONTROLLER_KP_FACTOR);
	gst_pw_audio_sink_reset_drift_compensation_states(self);

	self->flushing = 0;
	self->paused = 0;
	self->notify_upstream_about_stream_delay = 0;
	self->expected_next_running_time_pts = GST_CLOCK_TIME_NONE;
	self->latency = 0;
	g_mutex_init(&(self->latency_mutex));
	self->stream_drained = FALSE;
	self->accum_excess_encaudio_playtime = 0;

	self->stream_clock = gst_pw_stream_clock_new(NULL);
	g_assert(self->stream_clock != NULL);
	self->stream_clock_is_pipeline_clock = FALSE;

	self->pipewire_core = NULL;
	self->stream = NULL;
	self->stream_is_connected = FALSE;
	self->stream_is_active = FALSE;
	self->stream_listener_added = FALSE;
	self->spa_position = NULL;
	self->spa_rate_match = NULL;
	self->stream_delay_in_ticks = 0;
	self->stream_delay_in_ns = 0;
	self->quantum_size_in_ticks = 0;
	self->quantum_size_in_ns = 0;
	self->last_pw_time_ticks = 0;
	self->last_pw_time_ticks_set = FALSE;

	gst_pw_audio_sink_set_provide_clock_flag(self, DEFAULT_PROVIDE_CLOCK);
}

static void gst_pw_audio_sink_dispose(GObject *object)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(object);

	gst_pw_audio_sink_teardown_audio_data_buffer(self);

	if (self->stream_clock != NULL)
	{
		gst_object_unref(GST_OBJECT(self->stream_clock));
		self->stream_clock = NULL;
	}

	G_OBJECT_CLASS(gst_pw_audio_sink_parent_class)->dispose(object);
}


static void gst_pw_audio_sink_finalize(GObject *object)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(object);

	g_cond_clear(&(self->audio_data_buffer_cond));
	g_mutex_clear(&(self->audio_data_buffer_mutex));

	g_free(self->node_description);
	g_free(self->node_name);
	g_free(self->app_name);
	if (self->stream_properties != NULL)
		gst_structure_free(self->stream_properties);

	G_OBJECT_CLASS(gst_pw_audio_sink_parent_class)->finalize(object);
}


static void gst_pw_audio_sink_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(object);

	switch (prop_id)
	{
		case PROP_PROVIDE_CLOCK:
			gst_pw_audio_sink_set_provide_clock_flag(self, g_value_get_boolean(value));
			break;

		case PROP_ALIGNMENT_THRESHOLD:
			GST_OBJECT_LOCK(self);
			self->alignment_threshold = g_value_get_int64(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_SKEW_THRESHOLD:
			GST_OBJECT_LOCK(self);
			self->skew_threshold = g_value_get_int64(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_TARGET_OBJECT_ID:
			GST_OBJECT_LOCK(self);
			self->target_object_id = g_value_get_uint(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_STREAM_PROPERTIES:
		{
			GstStructure const *new_structure;

			GST_OBJECT_LOCK(self);

			if (self->stream_properties != NULL)
				gst_structure_free(self->stream_properties);

			new_structure = gst_value_get_structure(value);

			if (new_structure != NULL)
				self->stream_properties = gst_structure_copy(new_structure);
			else
				self->stream_properties = NULL;

			GST_OBJECT_UNLOCK(self);

			break;
		}

		case PROP_SOCKET_FD:
			GST_OBJECT_LOCK(self);
			self->socket_fd = g_value_get_int(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_RING_BUFFER_LENGTH:
			GST_OBJECT_LOCK(self);
			self->ring_buffer_length_in_ms = g_value_get_uint(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_APP_NAME:
			GST_OBJECT_LOCK(self);
			g_free(self->app_name);
			self->app_name = g_value_dup_string(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_NODE_NAME:
			GST_OBJECT_LOCK(self);
			g_free(self->node_name);
			self->node_name = g_value_dup_string(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_NODE_DESCRIPTION:
			GST_OBJECT_LOCK(self);
			g_free(self->node_description);
			self->node_description = g_value_dup_string(value);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_CACHE_PROBED_CAPS:
			GST_OBJECT_LOCK(self);
			self->cache_probed_caps = g_value_get_boolean(value);
			GST_OBJECT_UNLOCK(self);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_pw_audio_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(object);

	switch (prop_id)
	{
		case PROP_PROVIDE_CLOCK:
			g_value_set_boolean(value, gst_pw_audio_sink_get_provide_clock_flag(self));
			break;

		case PROP_ALIGNMENT_THRESHOLD:
			GST_OBJECT_LOCK(self);
			g_value_set_int64(value, self->alignment_threshold);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_SKEW_THRESHOLD:
			GST_OBJECT_LOCK(self);
			g_value_set_int64(value, self->skew_threshold);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_TARGET_OBJECT_ID:
			GST_OBJECT_LOCK(self);
			g_value_set_uint(value, self->target_object_id);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_STREAM_PROPERTIES:
			GST_OBJECT_LOCK(self);
			gst_value_set_structure(value, self->stream_properties);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_SOCKET_FD:
			GST_OBJECT_LOCK(self);
			g_value_set_int(value, self->socket_fd);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_RING_BUFFER_LENGTH:
			GST_OBJECT_LOCK(self);
			g_value_set_uint(value, self->ring_buffer_length_in_ms);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_APP_NAME:
			GST_OBJECT_LOCK(self);
			g_value_set_string(value, self->app_name);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_NODE_NAME:
			GST_OBJECT_LOCK(self);
			g_value_set_string(value, self->node_name);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_NODE_DESCRIPTION:
			GST_OBJECT_LOCK(self);
			g_value_set_string(value, self->node_description);
			GST_OBJECT_UNLOCK(self);
			break;

		case PROP_CACHE_PROBED_CAPS:
			GST_OBJECT_LOCK(self);
			g_value_set_boolean(value, self->cache_probed_caps);
			GST_OBJECT_UNLOCK(self);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static GstStateChangeReturn gst_pw_audio_sink_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn result;
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(element);

	switch (transition)
	{
		/* Pausing/resuming requires (de)activating the stream. Stream
		 * activation is also known as "corking" (PulseAudio terminology).
		 * It avoids unnecessary on_process_stream() calls while paused.
		 * Also, the "paused" flag is used in render() to enter/exit
		 * a wait loop. */

		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			GST_DEBUG_OBJECT(self, "setting paused flag and deactivating stream (if not already inactive) before PLAYING->PAUSED state change");

			pw_thread_loop_lock(self->pipewire_core->loop);
			gst_pw_audio_sink_drain_stream_unlocked(self);
			gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			g_atomic_int_set(&(self->paused), 1);
			g_cond_signal(&(self->audio_data_buffer_cond));

			break;

		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_DEBUG_OBJECT(self, "clearing paused flag and activating stream (if not already active) before PAUSED->PLAYING state change");

			pw_thread_loop_lock(self->pipewire_core->loop);
			gst_pw_audio_sink_activate_stream_unlocked(self, TRUE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			g_atomic_int_set(&(self->paused), 0);

			break;

		/* We also set paused to 0 during the PAUSED->READY state change
		 * in case the state was changed from PLAYING to PAUSED earlier,
		 * because then, paused will be set to 1. By setting it to 0 here,
		 * we ensure that the code in render() does not stay indefinitely
		 * in its pause wait loop. */
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			GST_DEBUG_OBJECT(self, "clearing paused flag before PAUSED->READY state change");
			g_atomic_int_set(&(self->paused), 0);
			/* Cancel any gst_pw_audio_format_probe_probe_audio_type()
			 * call that is running inside get_caps(). */
			gst_pw_audio_format_probe_cancel(self->format_probe);
			break;

		default:
			break;
	}

	result = GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->change_state(element, transition);

	GST_DEBUG_OBJECT(
		self,
		"state change %s result: %s",
		gst_state_change_get_name(transition),
		gst_element_state_change_return_get_name(result)
	);

	switch (transition)
	{
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
		{
			GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT_CAST(self));
			GstClockTime current_time = gst_clock_get_time(GST_ELEMENT_CLOCK(self));

			GST_DEBUG_OBJECT(
				self,
				"base-time is now: %" GST_TIME_FORMAT " current time: %" GST_TIME_FORMAT,
				GST_TIME_ARGS(base_time),
				GST_TIME_ARGS(current_time)
			);

			break;
		}

		default:
			break;
	}

	return result;
}


static GstClock* gst_pw_audio_sink_provide_clock(GstElement *element)
{
	GstClock *clock = NULL;
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(element);

	GST_OBJECT_LOCK(self);
	clock = GST_CLOCK_CAST(gst_object_ref(self->stream_clock));
	GST_OBJECT_UNLOCK(self);

	return clock;
}


static gboolean gst_pw_audio_sink_set_clock(GstElement *element, GstClock *clock)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(element);

	LOCK_AUDIO_DATA_BUFFER_MUTEX(self);
	self->stream_clock_is_pipeline_clock = (clock == GST_CLOCK_CAST(self->stream_clock));
	UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	GST_DEBUG_OBJECT(
		self,
		"pipeline is setting clock %" GST_PTR_FORMAT " as the element's clock; is the PW stream clock: %d",
		(gpointer)clock,
		self->stream_clock_is_pipeline_clock
	);

	return GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->set_clock(element, clock);
}


static gboolean gst_pw_audio_sink_send_event(GstElement *element, GstEvent *event)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(element);

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_LATENCY:
			/* Store the latency from the event here. This is an alternative way to get the
			 * base sink latency, which otherwise would require a gst_base_sink_get_latency()
			 * call, and that call locks the basesink's object mutex. As it turns out, that
			 * function's value only ever updates when this event is received, so we just do
			 * that manually here and store the latency in a value that is surrounded by the
			 * latency mutex. We anyway need that mutex already for other values, so not
			 * having to rely on gst_base_sink_get_latency saves a few basesink mutex
			 * lock/unlock operations. */
			LOCK_LATENCY_MUTEX(self);
			gst_event_parse_latency(event, &(self->latency));
			UNLOCK_LATENCY_MUTEX(self);

			GST_DEBUG_OBJECT(self, "got base sink latency: %" GST_TIME_FORMAT, GST_TIME_ARGS(self->latency));

			break;

		default:
			break;
	}

	return GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->send_event(element, event);
}


static gboolean gst_pw_audio_sink_query(GstElement *element, GstQuery *query)
{
	gboolean ret = TRUE;
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(element);

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_LATENCY:
		{
			gboolean sink_is_live, upstream_is_live;
			GstClockTime min_latency, max_latency;
			GstClockTime stream_delay_in_ns;

			/* There is no special logic for latency calculations
			 * when playing encoded audio, so just do the default
			 * handling and exit. */
			if (!gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
			{
				GST_DEBUG_OBJECT(element, "using default latency query logic for encoded audio");
				ret = GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->query(element, query);
				break;
			}

			ret = gst_base_sink_query_latency(
				GST_BASE_SINK_CAST(element),
				&sink_is_live, &upstream_is_live,
				&min_latency, &max_latency
			);
			if (!ret)
				break;

			GST_DEBUG_OBJECT(
				element,
				"sink is live: %d  upstream is live: %u  min/max latency from basesink: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT,
				sink_is_live, upstream_is_live,
				GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency)
			);

			/* Only adjust latency if both of these flags are true. Reasons:
			 *
			 * - Latency is only meaningful if upstream is live and if the
			     sink is configured to synchronize against the clock.
			 * - If the sink itself is not "live" (meaning: it is not set to
			 *   synchronize against the clock), it will output data as soon
			 *   as it arrives, and not necessarily with the delay that is
			 *   given by latency figures. Therefore, latency also makes no
			 *   sense if sink_is_live is FALSE.
			 */
			if (sink_is_live && upstream_is_live)
			{
				/* The extra latency from this sink is the pw_stream delay. This
				 * one is received in on_process_stream(), which will cause render()
				 * to post a LATENCY message, and that will re-query all elements
				 * (including this sink itself) for their current latency. That
				 * way, the pw_stream delay will be correctly factored into the
				 * sink's latency figures. */

				/* Synchronize access since the stream delay is set by
				 * the on_process_stream() function. */
				LOCK_LATENCY_MUTEX(self);
				stream_delay_in_ns = self->stream_delay_in_ns;
				UNLOCK_LATENCY_MUTEX(self);

				min_latency += stream_delay_in_ns;
				if (GST_CLOCK_TIME_IS_VALID(max_latency))
					max_latency += stream_delay_in_ns;

				GST_DEBUG_OBJECT(
					element,
					"PW stream delay: %" GST_TIME_FORMAT
					"  => adjusted min/max latency: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT,
					GST_TIME_ARGS(stream_delay_in_ns),
					GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency)
				);
			}

			/* NOTE: This is not optional, and must be done even if sink_is_live
			 * or upstream_is_live are FALSE, otherwise the query is not answered
			 * correctly, potentially leading to subtle playback bugs. */
			gst_query_set_latency(query, sink_is_live, min_latency, max_latency);

			break;
		}

		case GST_QUERY_CONVERT:
			ret = gst_pw_audio_sink_handle_convert_query(self, query);
			break;

		default:
			ret = GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->query(element, query);
			break;
	}

	return ret;
}


static gboolean gst_pw_audio_sink_set_caps(GstBaseSink *basesink, GstCaps *caps)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);
	gboolean ret = TRUE;
	struct spa_pod const *params[1];
	enum pw_stream_state state;
	char const *error_str = NULL;
	enum pw_stream_flags flags = 0;
	uint32_t target_object_id;
	gboolean pw_thread_loop_locked = FALSE;
	guint8 builder_buffer[1024];

	GST_DEBUG_OBJECT(self, "got new sink caps %" GST_PTR_FORMAT, (gpointer)caps);

	gst_pw_stream_clock_freeze(self->stream_clock);

	/* Wait until any remaining audio data that uses the old caps is played.
	 * Then we can safely disconnect the stream and don't lose any audio data. */
	gst_pw_audio_sink_drain_stream_and_audio_data_buffer(self);
	gst_pw_audio_sink_disconnect_stream(self);

	/* After disconnecting we remove the listener if it was previously added.
	 * This is important, otherwise the stream accumulates listeners -
	 * we only want one to be in use. */
	if (self->stream_listener_added)
	{
		spa_hook_remove(&(self->stream_listener));
		self->stream_listener_added = FALSE;
	}

	/* Get rid of the old caps here. That way, should an error occur below,
	 * we won't be left with the old, obsolete caps. */
	gst_caps_replace(&(self->sink_caps), NULL);
	/* Get rid of our current audio data buffer before creating a new one. */
	gst_pw_audio_sink_teardown_audio_data_buffer(self);

	/* Get a PW audio format out of the caps and initialize the POD
	 * that is then passed to pw_stream_connect() to specify the
	 * audio format params to the new PW stream. */
	if (!gst_pw_audio_format_from_caps(&(self->pw_audio_format), GST_OBJECT_CAST(self), caps))
		goto error;

	/* Edit the DSD info to set UNKNOWN as the DSD format. This is necessary since
	 * the input data may be using a different format than what the PipeWire graph
	 * expects. That graph format is stored in gst_pw_audio_sink_param_changed.
	 * By setting the DSD format in the info as UNKNOWN, we essentially tell the
	 * graph that we don't care about the format. Then, if there is a mismatch
	 * between the input format and the graph format, we convert on the fly using
	 * gst_pipewire_dsd_convert(). */
	{
		GstPipewireDsdFormat original_dsd_format = self->pw_audio_format.info.dsd_audio_info.format;

		if (self->pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_DSD)
			self->pw_audio_format.info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_UNKNOWN;

		if (!gst_pw_audio_format_to_spa_pod(&(self->pw_audio_format), GST_OBJECT_CAST(self), builder_buffer, sizeof(builder_buffer), params))
			goto error;

		if (self->pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_DSD)
			self->pw_audio_format.info.dsd_audio_info.format = original_dsd_format;
	}

	self->stride = gst_pw_audio_format_get_stride(&(self->pw_audio_format));

	/* Pick the stream connection flags.
	 *
	 * - PW_STREAM_FLAG_AUTOCONNECT to tell the session manager to link this client to a consumer.
	 * - PW_STREAM_FLAG_MAP_BUFFERS to not have to memory-map PW buffers manually.
	 * - PW_STREAM_FLAG_INACTIVE since we want to decide explicitly when the stream starts.
	 * - PW_STREAM_FLAG_RT_PROCESS to force the process stream event to be called in the same thread
	 *   that does the processing in the PipeWire graph. Necessary to safely fetch the rate_diff value
	 *   from the SPA IO position.
	 */
	flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_RT_PROCESS;

	/* Get GObject property values. */
	GST_OBJECT_LOCK(self);
	target_object_id = self->target_object_id;
	GST_OBJECT_UNLOCK(self);

	/* Set up the new stream connection. We must do this with a locked
	 * PW threaded loop to prevent race conditions from happening
	 * while the connection is established. */

	pw_thread_loop_lock(self->pipewire_core->loop);
	pw_thread_loop_locked = TRUE;

	state = pw_stream_get_state(self->stream, &error_str);
	if (state == PW_STREAM_STATE_ERROR)
	{
		GST_ERROR_OBJECT(self, "cannot start stream - PW stream is in an error state: %s", error_str);
		goto error;
	}

	pw_stream_add_listener(
		self->stream,
		&(self->stream_listener),
		gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type) ? &raw_stream_events : &encoded_stream_events,
		self
	);
	self->stream_listener_added = TRUE;

	pw_stream_connect(
		self->stream,
		PW_DIRECTION_OUTPUT,
		target_object_id,
		flags,
		params, 1
	);

	state = pw_stream_get_state(self->stream, &error_str);
	if (state == PW_STREAM_STATE_ERROR)
	{
		GST_ERROR_OBJECT(self, "cannot start stream - PW stream is in an error state: %s", error_str);
		goto error;
	}

	self->stream_is_connected = TRUE;
	self->sink_caps = gst_caps_ref(caps);

	self->last_encoded_frame_length = 0;

	/* Set the audio's rate as the pw rate property if this is an encoded stream.
	 * This can help with tuning the quantum to better fit encoded frame lengths.
	 * (Raw audio can be subdivided freely to perfectly fit any quantum length,
	 * so this logic isn't really necessary for those audio types.)
	 * Also, remove the latency property in case there's one left over from
	 * a previous stream. */
	{
		gchar *rate_str = NULL;
		struct spa_dict_item items[2];

		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, NULL);

		if (gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
		{
			items[1] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_RATE, NULL);
		}
		else
		{
			guint rate = self->pw_audio_format.info.encoded_audio_info.rate;
			rate_str = g_strdup_printf("1/%u", rate);
			items[1] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_RATE, rate_str);
		}

		pw_stream_update_properties(self->stream, &SPA_DICT_INIT(items, 2));
		g_free(rate_str);
	}

	gst_pw_audio_sink_setup_audio_data_buffer(self);

	gst_pw_audio_sink_activate_stream_unlocked(self, TRUE);

finish:
	if (pw_thread_loop_locked)
		pw_thread_loop_unlock(self->pipewire_core->loop);
	return ret;

error:
	ret = FALSE;
	goto finish;
}


static GstCaps* gst_pw_audio_sink_get_caps(GstBaseSink *basesink, GstCaps *filter)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);
	GstCaps *available_sinkcaps = NULL;
	gboolean cache_probed_caps;
	uint32_t target_object_id;
	gboolean cancelled = FALSE;

	GST_DEBUG_OBJECT(self, "new get-caps query");

	GST_OBJECT_LOCK(self);
	cache_probed_caps = self->cache_probed_caps;
	target_object_id = self->target_object_id;
	GST_OBJECT_UNLOCK(self);

	/* get_caps() may get called simultaneously by different threads.
	 * It is not a good idea to let the probing happen concurrently.
	 * Use a mutex to prevent probing attempts from ever running at the
	 * same time. (The GstPwAudioFormatProbe's setup, teardown, and
	 * probe calls themselves are MT safe, but this does not help here. */
	g_mutex_lock(&(self->probe_process_mutex));

	if (self->cached_probed_caps != NULL)
	{
		available_sinkcaps = gst_caps_ref(self->cached_probed_caps);
		GST_DEBUG_OBJECT(self, "using cached probed caps as available caps: %" GST_PTR_FORMAT, (gpointer)available_sinkcaps);
		goto finish;
	}

	if (self->pipewire_core != NULL)
	{
		gint audio_type;

		GST_DEBUG_OBJECT(self, "probing PipeWire graph for available caps");

		available_sinkcaps = gst_caps_new_empty();

		gst_pw_audio_format_probe_setup(self->format_probe);

		for (audio_type = 0; (audio_type < GST_NUM_PIPEWIRE_AUDIO_TYPES) && !cancelled; ++audio_type)
		{
			GstPwAudioFormatProbeResult probing_result;
			GstPwAudioFormat *probed_details = NULL;

			probing_result = gst_pw_audio_format_probe_probe_audio_type(
				self->format_probe,
				audio_type,
				target_object_id,
				&probed_details
			);

			switch (probing_result)
			{
				case GST_PW_AUDIO_FORMAT_PROBE_RESULT_SUPPORTED:
				{
					switch (audio_type)
					{
						case GST_PIPEWIRE_AUDIO_TYPE_DSD:
						{
							/* In here, place the probed DSD format as the first one
							 * in the format list. This ensures that upstream prefers
							 * this format and only uses others if necessary, which
							 * helps, since those others require conversion, while
							 * the probed format doesn't. */

							GstCaps *caps = gst_pw_audio_format_get_template_caps_for_type(audio_type);
							GstStructure *s = gst_caps_get_structure(caps, 0);
							GValue list_value = G_VALUE_INIT;
							GValue string_value = G_VALUE_INIT;
							gint format_idx;

							g_value_init(&list_value, GST_TYPE_LIST);
							g_value_init(&string_value, G_TYPE_STRING);

							/* First add the probed format. */
							g_value_set_static_string(&string_value, gst_pipewire_dsd_format_to_string(probed_details->info.dsd_audio_info.format));
							gst_value_list_append_value(&list_value, &string_value);

							/* Now add the rest. */
							for (format_idx = GST_PIPEWIRE_DSD_FIRST_VALID_FORMAT; format_idx < GST_NUM_PIPEWIRE_DSD_FORMATS; ++format_idx)
							{
								GstPipewireDsdFormat dsd_format = (GstPipewireDsdFormat)format_idx;

								if (dsd_format == probed_details->info.dsd_audio_info.format)
									continue;

								g_value_set_static_string(&string_value, gst_pipewire_dsd_format_to_string(dsd_format));
								gst_value_list_append_value(&list_value, &string_value);
							}

							gst_structure_set_value(s, "format", &list_value);

							g_value_unset(&list_value);
							g_value_unset(&string_value);

							gst_caps_append(available_sinkcaps, caps);
							break;
						}

						default:
							gst_caps_append(available_sinkcaps, gst_pw_audio_format_get_template_caps_for_type(audio_type));
					}

					break;
				}

				case GST_PW_AUDIO_FORMAT_PROBE_RESULT_CANCELLED:
					cancelled = TRUE;
					break;

				default:
					break;
			}
		}

#if !PW_CHECK_VERSION(0, 3, 57)
		// This is a workaround. Without this, any DSD playback other than DSD64 fails.
		// It seems that the ALSA SPA sink node is not correctly reinitialized, and "lingers"
		// in its DSD64 setup (which is used during probing). This leads to this error in the log:
		//
		//   pw.context   | [       context.c:  737 pw_context_debug_port_params()] params Spa:Enum:ParamId:EnumFormat: 0:0 Invalid argument (input format (no more input formats))
		//
		// By dummy-probing PCM again, the node is forced to reinitialize.
		//
		// Reported as: https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/2625
		// Fixed in version 0.3.57.
		gst_pw_audio_format_probe_probe_audio_type(self->format_probe, GST_PIPEWIRE_AUDIO_TYPE_PCM, target_object_id, NULL);
#endif

		gst_pw_audio_format_probe_teardown(self->format_probe);

		if (cache_probed_caps)
			gst_caps_replace(&(self->cached_probed_caps), available_sinkcaps);
	}
	else
	{
		available_sinkcaps = gst_pw_audio_format_get_template_caps();
		GST_DEBUG_OBJECT(self, "using template caps as available caps");
	}

finish:
	g_mutex_unlock(&(self->probe_process_mutex));

	if (cancelled)
	{
		/* In case of cancellation discard the partial caps result and just return
		 * the template caps. We aren't going to play anything anyway, since
		 * cancellation happens during the PAUSED->READY state change. By returning
		 * the template caps we at least remain deterministic in what we return
		 * in the cancellation case. */
		GST_DEBUG_OBJECT(self, "returning template caps after pw format probing got cancelled");
		gst_caps_unref(available_sinkcaps);
		return gst_pw_audio_format_get_template_caps();
	}

	if (filter != NULL)
	{
		GstCaps *unfiltered_available_sinkcaps = available_sinkcaps;

		/* Intersect with the GST_CAPS_INTERSECT_FIRST mode. This allows
		 * for the filter caps to also determine the order of caps. For
		 * example, if the list of formats in the available sinkcaps is
		 * "U8, S16LE, S32LE", and the filter caps contain "S16LE, S24LE, U8",
		 * then the resulted intersection is "S16LE, U8". */
		available_sinkcaps = gst_caps_intersect_full(filter, available_sinkcaps, GST_CAPS_INTERSECT_FIRST);
		GST_DEBUG_OBJECT(self, "responding to caps query (query has filter caps):");
		GST_DEBUG_OBJECT(self, "  unfiltered available sink caps: %" GST_PTR_FORMAT, (gpointer)unfiltered_available_sinkcaps);
		GST_DEBUG_OBJECT(self, "  caps filter:                    %" GST_PTR_FORMAT, (gpointer)filter);
		GST_DEBUG_OBJECT(self, "  final filtered caps for query:  %" GST_PTR_FORMAT, (gpointer)available_sinkcaps);
		gst_caps_unref(unfiltered_available_sinkcaps);
	}
	else
	{
		GST_DEBUG_OBJECT(self, "responding to caps query (query has no filter caps):");
		GST_DEBUG_OBJECT(self, "  final caps for query:           %" GST_PTR_FORMAT, (gpointer)available_sinkcaps);
	}

	return available_sinkcaps;
}


static GstCaps* gst_pw_audio_sink_fixate(GstBaseSink *basesink, GstCaps *caps)
{
	caps = gst_pw_audio_format_fixate_caps(caps);
	return GST_BASE_SINK_CLASS(gst_pw_audio_sink_parent_class)->fixate(basesink, caps);
}


static void gst_pw_audio_sink_get_times(GstBaseSink *basesink, GstBuffer *buffer, GstClockTime *start, GstClockTime *end)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(basesink);

	if (gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
	{
		/* This sink handles the clock synchronization by itself when playing raw
		 * audio. Setting *start and *end to GST_CLOCK_TIME_NONE informs the base
		 * class that it must not handle the synchronization on its own. */

		*start = GST_CLOCK_TIME_NONE;
		*end = GST_CLOCK_TIME_NONE;
	}
	else
	{
		/* For encoded audio, the basesink's default synchronization is
		 * good enough, so just use that; don't bother using a custom one. */
		GST_BASE_SINK_CLASS(gst_pw_audio_sink_parent_class)->get_times(basesink, buffer, start, end);
	}
}


static gboolean copy_stream_properties_to_pw_props(GQuark field_id, GValue const *value, gpointer data)
{
	struct pw_properties *pw_props = (struct pw_properties *)data;
	GValue stringified_gvalue = G_VALUE_INIT;

	if (g_value_type_transformable(G_VALUE_TYPE(value), G_TYPE_STRING))
	{
		g_value_init(&stringified_gvalue, G_TYPE_STRING);

		if (g_value_transform(value, &stringified_gvalue))
			pw_properties_set(pw_props, g_quark_to_string(field_id), g_value_get_string(&stringified_gvalue));

		g_value_unset(&stringified_gvalue);
	}

	return TRUE;
}


static gboolean gst_pw_audio_sink_start(GstBaseSink *basesink)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);
	gboolean retval = TRUE;
	int socket_fd;
	struct pw_properties *pw_props;
	gchar *stream_media_name = NULL;

	GST_OBJECT_LOCK(self);

	/* Get GObject property values. */
	socket_fd = self->socket_fd;
	self->skew_threshold_snapshot = self->skew_threshold;
	self->ring_buffer_length_snapshot = self->ring_buffer_length_in_ms * GST_MSECOND;

	self->pipewire_core = gst_pipewire_core_get(socket_fd);

	GST_OBJECT_UNLOCK(self);

	if (G_UNLIKELY(self->pipewire_core == NULL))
	{
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE, ("Could not get PipeWire core"), (NULL));
		goto error;
	}

	self->format_probe = gst_pw_audio_format_probe_new(self->pipewire_core);

	GST_DEBUG_OBJECT(self, "creating new PipeWire stream");

	pw_props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		NULL
	);

	/* Unlike the other properties, we don't create local copies of the
	 * name/description strings and stream properties, since we only need
	 * them during these pw_properties_set() calls, which don't block
	 * for very long. */
	GST_OBJECT_LOCK(self);

	if (self->app_name != NULL)
	{
		pw_properties_set(pw_props, PW_KEY_APP_NAME, self->app_name);
		GST_DEBUG_OBJECT(self, "app name for the new PipeWire stream: %s", self->app_name);
	}

	if (self->node_name != NULL)
	{
		pw_properties_set(pw_props, PW_KEY_NODE_NAME, self->node_name);
		GST_DEBUG_OBJECT(self, "node name for the new PipeWire stream: %s", self->node_name);
	}

	if (self->node_description != NULL)
	{
		pw_properties_set(pw_props, PW_KEY_NODE_DESCRIPTION, self->node_description);
		GST_DEBUG_OBJECT(self, "node description for the new PipeWire stream: %s", self->node_description);
	}

	if (self->stream_properties != NULL)
	{
		gst_structure_foreach(self->stream_properties, copy_stream_properties_to_pw_props, pw_props);
		GST_DEBUG_OBJECT(self, "extra properties for the new PipeWire stream: %" GST_PTR_FORMAT, (gpointer)(self->stream_properties));
	}

	/* Reuse the node name as the stream name. We copy the string here
	 * to prevent potential race conditions if the user assigns a new
	 * name string to the node-name property while this code runs. */
	stream_media_name = g_strdup(self->node_name);

	GST_OBJECT_UNLOCK(self);

	pw_thread_loop_lock(self->pipewire_core->loop);
	self->stream = pw_stream_new(self->pipewire_core->core, stream_media_name, pw_props);
	pw_thread_loop_unlock(self->pipewire_core->loop);
	if (G_UNLIKELY(self->stream == NULL))
	{
		GST_ERROR_OBJECT(self, "could not create PipeWire stream");
		goto error;
	}

	GST_DEBUG_OBJECT(self, "PipeWire stream successfully created");

finish:
	g_free(stream_media_name);
	return retval;

error:
	gst_pw_audio_sink_stop(basesink);
	retval = FALSE;
	goto finish;
}


static gboolean gst_pw_audio_sink_stop(GstBaseSink *basesink)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);

	if (self->stream != NULL)
	{
		GST_DEBUG_OBJECT(self, "disconnecting and destroying PipeWire stream");
		gst_pw_audio_sink_disconnect_stream(self);

		pw_thread_loop_lock(self->pipewire_core->loop);
		pw_stream_destroy(self->stream);
		pw_thread_loop_unlock(self->pipewire_core->loop);

		self->stream = NULL;
	}

	/* Perform these teardown steps with the probe_process_mutex
	 * locked, since caps queries can happen simultaneously,
	 * and those trigger a get_caps() call. get_caps() accesses
	 * these same resources we are tearing down here. */
	{
		g_mutex_lock(&(self->probe_process_mutex));

		if (self->format_probe != NULL)
		{
			gst_pw_audio_format_probe_teardown(self->format_probe);
			gst_object_unref(GST_OBJECT(self->format_probe));
			self->format_probe = NULL;
		}

		gst_caps_replace(&(self->cached_probed_caps), NULL);

		if (self->pipewire_core != NULL)
		{
			GST_DEBUG_OBJECT(self, "releasing PipeWire core");
			gst_pipewire_core_release(self->pipewire_core);
			self->pipewire_core = NULL;
		}

		g_mutex_unlock(&(self->probe_process_mutex));
	}

	/* Recreate the stream clock. This is the only way
	 * to fully reset _all_ internal states, including
	 * the states of the clock base classes. */
	if (self->stream_clock != NULL)
	{
		gst_object_unref(GST_OBJECT(self->stream_clock));
		self->stream_clock = gst_pw_stream_clock_new(NULL);
		g_assert(self->stream_clock != NULL);
	}

	gst_caps_replace(&(self->sink_caps), NULL);

	gst_pw_audio_sink_reset_drift_compensation_states(self);

	gst_pw_audio_sink_reset_audio_data_buffer_unlocked(self);
	gst_pw_audio_sink_teardown_audio_data_buffer(self);

	self->flushing = 0;
	self->paused = 0;
	self->latency = 0;
	self->accum_excess_encaudio_playtime = 0;
	self->stream_clock_is_pipeline_clock = FALSE;
	self->stream_listener_added = FALSE;
	self->spa_position = NULL;
	self->spa_rate_match = NULL;
	self->stream_delay_in_ticks = 0;
	self->stream_delay_in_ns = 0;
	self->quantum_size_in_ticks = 0;
	self->quantum_size_in_ns = 0;
	self->last_pw_time_ticks = 0;
	self->last_pw_time_ticks_set = FALSE;
	self->skew_threshold_snapshot = 0;

	return TRUE;
}


static gboolean gst_pw_audio_sink_query_pad(GstBaseSink *basesink, GstQuery *query)
{
	/* This is not simply a duplicate of the code from gst_pw_audio_sink_query().
	 * gst_pw_audio_sink_query() is called by gst_element_query(), while this
	 * is called whenever the sink's pad receives a query to respond to. */

	gboolean ret;
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_CONVERT:
			ret = gst_pw_audio_sink_handle_convert_query(self, query);
			break;

		default:
			ret = GST_BASE_SINK_CLASS(gst_pw_audio_sink_parent_class)->query(basesink, query);
			break;
	}

	return ret;
}


static gboolean gst_pw_audio_sink_event(GstBaseSink *basesink, GstEvent *event)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_FLUSH_START:
		{
			GST_DEBUG_OBJECT(self, "flushing started; setting flushing flag and resetting audio data buffer");

			gst_pw_stream_clock_freeze(self->stream_clock);

			g_atomic_int_set(&(self->flushing), 1);
			g_cond_signal(&(self->audio_data_buffer_cond));

			/* Deactivate the stream since we won't be producing data during flush. */
			pw_thread_loop_lock(self->pipewire_core->loop);
			pw_stream_flush(self->stream, FALSE);
			gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			/* Get rid of all buffered data during flush. */
			LOCK_AUDIO_DATA_BUFFER_MUTEX(self);
			gst_pw_audio_sink_reset_audio_data_buffer_unlocked(self);
			UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);

			break;
		}

		case GST_EVENT_FLUSH_STOP:
		{
			GST_DEBUG_OBJECT(self, "flushing stopped; clearing flushing flag");

			g_atomic_int_set(&(self->flushing), 0);

			/* Flush is over, we produce data again. Reactivate the stream. */
			pw_thread_loop_lock(self->pipewire_core->loop);
			gst_pw_audio_sink_activate_stream_unlocked(self, TRUE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			break;
		}

		default:
			break;
	}

	return GST_BASE_SINK_CLASS(gst_pw_audio_sink_parent_class)->event(basesink, event);
}


static GstFlowReturn gst_pw_audio_sink_wait_event(GstBaseSink *basesink, GstEvent *event)
{
	GstFlowReturn flow_ret = GST_FLOW_OK;
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_GAP:
		{
			GstClockTime timestamp, duration;

			/* These events are not explicitly handled, since gaps are already
			 * compensated for in render() by inserting nullsamples (via the
			 * alignment threshold check). We just log gaps here and do nothing
			 * further. */
			// TODO: Change this: Record the gap event, and next time the
			// render() function is called, insert the exact amount of nullsamples.
			// That's because the alignment threshold check won't work properly
			// for small gaps that are below that threshold.

			gst_event_parse_gap(event, &timestamp, &duration);

			GST_DEBUG_OBJECT(
				self,
				"gap event received; timestamp: %" GST_TIME_FORMAT " duration: %" GST_TIME_FORMAT,
				GST_TIME_ARGS(timestamp),
				GST_TIME_ARGS(duration)
			);

			break;
		}

		case GST_EVENT_EOS:
		{
			/* After EOS, no more data will arrive until another stream-start
			 * event is produced. Drain any buffered data, then deactivate
			 * the pw_stream to prevent unnecessary PipeWire xruns and
			 * on_process_stream() calls.
			 * More data can follow in two cases:
			 *
			 * 1. A new stream is produced. A stream-start event is produced, along
			 *    with a caps event and a segment event. This means that in case of
			 *    a new stream, set_caps will be called, which reconnects and then
			 *    reactivates the stream.
			 * 2. flush-stop event is received. No new caps event is required by
			 *    GStreamer after flush-stop, but our flush-stop handle calls
			 *    gst_pw_audio_sink_activate_stream_unlocked() to reactivate the stream.
			 *
			 * In both cases, the stream resumes, so there's no danger of having
			 * a stream that won't do anything after EOS.
			 */

			GST_DEBUG_OBJECT(self, "EOS received; draining audio data and deactivating stream");

			gst_pw_audio_sink_drain_stream_and_audio_data_buffer(self);

			pw_thread_loop_lock(self->pipewire_core->loop);
			gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			break;
		}

		default:
			break;
	}

	return flow_ret;
}


static GstFlowReturn gst_pw_audio_sink_preroll(G_GNUC_UNUSED GstBaseSink *basesink, G_GNUC_UNUSED GstBuffer *incoming_buffer)
{
	/* Don't preroll anything. Prerolling is not useful for PipeWire audio. */
	return GST_FLOW_OK;
}


static GstFlowReturn gst_pw_audio_sink_render(GstBaseSink *basesink, GstBuffer *incoming_buffer)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);

	if (gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
		return gst_pw_audio_sink_render_raw(self, incoming_buffer);
	else
		return gst_pw_audio_sink_render_encoded(self, incoming_buffer);
}


static GstFlowReturn gst_pw_audio_sink_render_raw(GstPwAudioSink *self, GstBuffer *original_incoming_buffer)
{
	GstFlowReturn flow_ret = GST_FLOW_OK;
	GstBaseSink *basesink = GST_BASE_SINK_CAST(self);
	GstSegment *segment = &(basesink->segment);
	GstBuffer *incoming_buffer_copy = NULL;
	gboolean sync_enabled;
	gboolean force_discontinuity_handling = FALSE;
	gsize num_silence_frames_to_insert = 0;
	GstClockTime computed_original_buffer_duration;
	gsize num_frames;
	gsize num_remaining_frames_to_push;
	gsize incoming_buffer_frame_offset;

	num_frames = gst_buffer_get_size(original_incoming_buffer) / self->stride;

	/* For PCM/DSD audio data, it is better to not rely on values from GST_BUFFER_DURATION.
	 * These can be invalid, completely absent, or differ in length from the playtime of
	 * the actual data. For example, with some older WMA files, buffers could have a duration
	 * value of 2 ms more than the playtime duration of the actual decoded PCM data. */
	computed_original_buffer_duration = gst_pw_audio_format_calculate_duration_from_num_frames(
		&(self->pw_audio_format),
		num_frames
	);

	GST_LOG_OBJECT(
		self,
		"original incoming buffer: %" GST_PTR_FORMAT "; num frames: %" G_GSIZE_FORMAT "; calculated duration %" GST_TIME_FORMAT " based on number of frames",
		(gpointer)original_incoming_buffer,
		num_frames,
		GST_TIME_ARGS(computed_original_buffer_duration)
	);

	if (G_UNLIKELY(GST_BUFFER_FLAG_IS_SET(original_incoming_buffer, GST_BUFFER_FLAG_DISCONT)))
	{
		GST_DEBUG_OBJECT(self, "discont flag set - resetting alignment check");
		/* Forget about the last expected_next_running_time_pts, since between it and
		 * the current running_time_pts value is an expected discontinuity. That's what
		 * the DISCONT buffer flag is for - to announce *expected* discontinuities.
		 * It doesn't mean "handle a discontinuity now", quite the opposite, it essentially
		 * means "ignore this discontinuity". */
		self->expected_next_running_time_pts = GST_CLOCK_TIME_NONE;
	}

	if (G_UNLIKELY(GST_BUFFER_FLAG_IS_SET(original_incoming_buffer, GST_BUFFER_FLAG_RESYNC)))
	{
		GST_DEBUG_OBJECT(self, "resync flag set; forcing discontinuity handling");
		/* The resync flag means that we must resynchronize _now_ against the current timestamp.
		 * Such a resynchronization already occurs when a big enough discontinuity (one that
		 * is not marked with the DISCONT flag) is observed. We therefore simply force that
		 * same mechanism to kick in now. This is how the resync flag induced resynchronization
		 * is implemented here. */
		force_discontinuity_handling = TRUE;
	}

	sync_enabled = gst_base_sink_get_sync(basesink);

	/* If the sync property is set to TRUE, and the incoming data is in a TIME
	 * segment & contains timestamped buffers, create a sub-buffer out of
	 * original_incoming_buffer and store that sub-buffer as the
	 * incoming_buffer_copy. This is done that way because sub-buffers
	 * can be set to cover only a portion of the original buffer's memory.
	 * That is how buffers are clipped (if they need to be clipped); sub-
	 * buffers are created that share the same underlying datablock as the
	 * original buffer, thus avoiding unnecessary memory copies.
	 * The sub-buffer is given original_incoming_buffer's clipped timestamp
	 * (or the original timestamp if no clipping occurred), as well as its
	 * duration (also adjusted for clipping if necessary).
	 *
	 * Note that the incoming_buffer_copy's timestamp is translated to
	 * clock-time. This is done to allow the code in on_process_stream to
	 * immediately compare the buffer's timestamp against the stream_clock
	 * without first having to do the translation. That function must finish
	 * its execution ASAP, so offloading computation from it helps.
	 *
	 * First, do a number of checks to filter out cases where
	 * synchronization cannot be done. */

	if (!sync_enabled)
	{
		GST_LOG_OBJECT(self, "synced playback disabled; not adjusting buffer timestamp and duration");
	}
	else if (G_UNLIKELY(segment->format != GST_FORMAT_TIME))
	{
		GST_LOG_OBJECT(
			self,
			"synced playback not possible with non-TIME segment; segment details: %" GST_SEGMENT_FORMAT,
			(gpointer)segment
		);
		sync_enabled = FALSE;
	}
	else if (G_UNLIKELY(!GST_BUFFER_PTS_IS_VALID(original_incoming_buffer)))
	{
		GST_LOG_OBJECT(
			self,
			"synced playback not possible; segment is in TIME format, but incoming buffer is not timestamped"
		);
		sync_enabled = FALSE;
	}

	if (sync_enabled)
	{
		GstClockTimeDiff ts_offset;
		GstClockTime render_delay;
		GstClockTime pts_begin, pts_end;
		GstClockTime clipped_pts_begin, clipped_pts_end;
		GstClockTimeDiff sync_offset;
		GstSegment pts_clipping_segment;

		pts_begin = GST_BUFFER_PTS(original_incoming_buffer);
		pts_end = GST_BUFFER_PTS(original_incoming_buffer) + computed_original_buffer_duration;

		gst_segment_init(&pts_clipping_segment, GST_FORMAT_TIME);
		pts_clipping_segment.start = segment->start;
		pts_clipping_segment.stop = segment->stop;
		pts_clipping_segment.duration = -1;

		ts_offset = gst_base_sink_get_ts_offset(basesink);
		render_delay = gst_base_sink_get_render_delay(basesink);
		sync_offset = ts_offset - render_delay;

		GST_LOG_OBJECT(
			self,
			"ts-offset: %" G_GINT64_FORMAT " render delay: %" GST_TIME_FORMAT " => sync offset: %" G_GINT64_FORMAT,
			ts_offset,
			GST_TIME_ARGS(render_delay),
			sync_offset
		);

		if (G_UNLIKELY(sync_offset < 0))
		{
			pts_clipping_segment.start += -sync_offset;
			if (pts_clipping_segment.stop != (guint64)(-1))
				pts_clipping_segment.stop += -sync_offset;

			sync_offset = 0;
		}

		if (!gst_segment_clip(&pts_clipping_segment, GST_FORMAT_TIME, pts_begin, pts_end, &clipped_pts_begin, &clipped_pts_end))
		{
			GST_DEBUG_OBJECT(self, "incoming buffer is fully outside of the current segment; dropping buffer");
			goto finish;
		}

		GST_LOG_OBJECT(
			self, "original buffer begin/end PTS: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT "  clipped begin/end PTS: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT,
			GST_TIME_ARGS(pts_begin), GST_TIME_ARGS(pts_end),
			GST_TIME_ARGS(clipped_pts_begin), GST_TIME_ARGS(clipped_pts_end)
		);

		if (GST_CLOCK_TIME_IS_VALID(clipped_pts_begin) && GST_CLOCK_TIME_IS_VALID(clipped_pts_end))
		{
			GstClockTime running_time_pts;
			gsize clipped_begin_frames = 0, clipped_end_frames = 0;
			gsize original_num_frames;
			GstClockTime begin_clip_duration, end_clip_duration;
			GstClockTime pw_base_time;

			running_time_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, clipped_pts_begin);

			pw_base_time = GST_ELEMENT_CAST(self)->base_time;

			if (GST_CLOCK_TIME_IS_VALID(self->expected_next_running_time_pts))
			{
				GstClockTimeDiff discontinuity = GST_CLOCK_DIFF(self->expected_next_running_time_pts, running_time_pts);

				// TODO: Accumulate discontinuity, and only perform compensation
				// if the accumulated discontinuity is nonzero after a while.
				// This filters out cases of alternating discontinuities, like
				// +1ms now -1ms next +1ms next -1ms next etc.

				if (G_UNLIKELY((ABS(discontinuity) > self->alignment_threshold) || ((discontinuity != 0) && force_discontinuity_handling)))
				{
					/* A positive discontinuity value means that there is a gap between
					 * this buffer and the last one. If we are playing contiguous
					 * audio data, we can fill the gap with silence frames.
					 * A negative discontinuity value means that the last N nanoseconds
					 * of the last buffer overlap with the first N nanoseconds of this
					 * buffer. We have to throw away the first N nanoseconds of the
					 * new buffer in that case. (N = ABS(discontinuity)) */

					if (discontinuity > 0)
					{
						/* Shift the running-time PTS to make room for the extra silence frames. */
						running_time_pts += discontinuity;
						num_silence_frames_to_insert = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), discontinuity);
						GST_DEBUG_OBJECT(
							self,
							"discontinuity detected (%" GST_TIME_FORMAT "); need to insert %" G_GSIZE_FORMAT " silence frame(s) to compensate",
							GST_TIME_ARGS(discontinuity),
							num_silence_frames_to_insert
						);
					}
					else
					{
						/* We need to clip the first N nanoseconds (N = -discontinuity), since these
						 * overlap with already played data. The start of the remaining data needs to
						 * be shifted into the future by (-discontinuity) nanoseconds to align it with
						 * the previous data and to account for the clipped amount. clipped_pts_end
						 * is not modified, however, since the overall duration of the data to play
						 * is reduced by (-discontinuity) nanoseconds by the clipping. */
						running_time_pts += (-discontinuity);
						clipped_pts_begin += (-discontinuity);
						GST_DEBUG_OBJECT(
							self,
							"discontinuity detected (-%" GST_TIME_FORMAT "); need to clip this (positive) amount of nanoseconds from the beginning of the gstbuffer",
							GST_TIME_ARGS(-discontinuity)
						);
					}
				}
			}

			g_assert(GST_CLOCK_TIME_IS_VALID(running_time_pts));

			begin_clip_duration = clipped_pts_begin - pts_begin;
			end_clip_duration = pts_end - clipped_pts_end;

			clipped_begin_frames = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), begin_clip_duration);
			clipped_end_frames = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), end_clip_duration);

			original_num_frames = gst_buffer_get_size(original_incoming_buffer) / self->stride;

			GST_LOG_OBJECT(
				self,
				"clip begin/end duration: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT "  clipped begin/end frames: %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT " original num frames: %" G_GSIZE_FORMAT,
				GST_TIME_ARGS(begin_clip_duration), GST_TIME_ARGS(end_clip_duration),
				clipped_begin_frames, clipped_end_frames,
				original_num_frames
			);

			/* Fringe case: The buffer is completely clipped, so we just drop it. */
			if (G_UNLIKELY((clipped_begin_frames >= original_num_frames) || (clipped_end_frames >= original_num_frames)))
			{
				GST_DEBUG_OBJECT(self, "clipped begin/end frames fully clip the buffer; dropping buffer");
				goto finish;
			}

			incoming_buffer_copy = gst_buffer_copy_region(
				original_incoming_buffer,
				GST_BUFFER_COPY_MEMORY,
				clipped_begin_frames * self->stride,
				gst_buffer_get_size(original_incoming_buffer) - (clipped_begin_frames + clipped_end_frames) * self->stride
			);

			/* Set the incoming_buffer_copy's timestamp, translated to
			 * clock-time by adding pw_base_time to running_time_pts. */
			GST_BUFFER_PTS(incoming_buffer_copy) = pw_base_time + running_time_pts;
			GST_BUFFER_DURATION(incoming_buffer_copy) = clipped_pts_end - clipped_pts_begin;

			/* Estimate the next PTS. If the stream PTS are properly aligned, then the next
			 * running-time PTS will match this estimate. Otherwise, there is a misalignment,
			 * and we have to compensate. */
			self->expected_next_running_time_pts = running_time_pts + GST_BUFFER_DURATION(incoming_buffer_copy);

			GST_LOG_OBJECT(
				self,
				"current and next expected running time: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT,
				GST_TIME_ARGS(running_time_pts), GST_TIME_ARGS(self->expected_next_running_time_pts)
			);

			GST_LOG_OBJECT(
				self,
				"base-time: %" GST_TIME_FORMAT "  clock-time clipped buffer PTS: %" GST_TIME_FORMAT "  clipped buffer duration: %" GST_TIME_FORMAT,
				GST_TIME_ARGS(pw_base_time),
				GST_TIME_ARGS(GST_BUFFER_PTS(incoming_buffer_copy)),
				GST_TIME_ARGS(GST_BUFFER_DURATION(incoming_buffer_copy))
			);
		}
		else
		{
			GST_LOG_OBJECT(self, "clipped begin/end PTS invalid after clipping; not adjusting buffer timestamp and duration, not playing in sync");
			sync_enabled = FALSE;
		}
	}

	if (!sync_enabled)
	{
		/* This location is reached in these cases:
		 *
		 * - "sync" GObject property is set to FALSE.
		 * - Incoming buffer has no valid PTS.
		 * - Clipping the buffer PTS against the segment produced invalid PTS.
		 * - Current segment is not a TIME segment.
		 *
		 * If at least one of these applies, then the code above will not create a sub-buffer
		 * and store that sub-buffer as incoming_buffer_copy. We have to do that here
		 * instead. Nothing is clipped, since there is no information about what needs
		 * to be clipped (if anything needs clipping at all), so just copy the entire
		 * buffer. (GstMemory blocks are ref'd, not deep-copied; see the gst_buffer_copy()
		 * documentation). We invalidate the copy's PTS to signal to the rest of the
		 * code that this buffer is not to be played in sync and is instead to be played
		 * as soon as it is dequeued by the on_process_stream() callback.
		 */
		incoming_buffer_copy = gst_buffer_copy(original_incoming_buffer);
		GST_BUFFER_PTS(incoming_buffer_copy) = GST_CLOCK_TIME_NONE;
		/* Also discard the expected_next_running_time_pts to avoid
		 * incorrect discontinuity calculations. */
		self->expected_next_running_time_pts = GST_CLOCK_TIME_NONE;
	}

	num_remaining_frames_to_push = num_frames;
	incoming_buffer_frame_offset = 0;

	while (TRUE)
	{
		GstMapInfo map_info;
		gboolean map_ret;
		gsize num_pushed_frames;
		GstClockTime pts_offset;
		GstClockTime push_pts;

		if (g_atomic_int_get(&(self->flushing)))
		{
			GST_DEBUG_OBJECT(self, "exiting loop in render function since we are flushing");
			flow_ret = GST_FLOW_FLUSHING;
			goto finish;
		}

		if (g_atomic_int_get(&(self->paused)))
		{
			GST_DEBUG_OBJECT(self, "sink is paused; waiting for preroll, flushing, or a state change to READY");

			flow_ret = gst_base_sink_wait_preroll(basesink);

			if (flow_ret != GST_FLOW_OK)
				goto finish;
		}

		if (g_atomic_int_get(&(self->notify_upstream_about_stream_delay)))
		{
			GST_DEBUG_OBJECT(self, "posting message to bus to inform about latency change");
			gst_element_post_message(GST_ELEMENT_CAST(self), gst_message_new_latency(GST_OBJECT(self)));
			g_atomic_int_set(&(self->notify_upstream_about_stream_delay), 0);
		}

		map_ret = gst_buffer_map(incoming_buffer_copy, &map_info, GST_MAP_READ);
		if (G_UNLIKELY(!map_ret))
		{
			GST_ERROR_OBJECT(self, "could not map incoming buffer; buffer details: %" GST_PTR_FORMAT, (gpointer)incoming_buffer_copy);
			flow_ret = GST_FLOW_ERROR;
			goto finish;
		}

		LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

		pts_offset = gst_pw_audio_format_calculate_duration_from_num_frames(
			&(self->ring_buffer->format),
			incoming_buffer_frame_offset
		);

		push_pts = GST_BUFFER_PTS_IS_VALID(incoming_buffer_copy) ? (GST_BUFFER_PTS(incoming_buffer_copy) + pts_offset) : GST_CLOCK_TIME_NONE;

		num_pushed_frames = gst_pw_audio_ring_buffer_push_frames(
			self->ring_buffer,
			map_info.data + incoming_buffer_frame_offset * self->stride,
			num_remaining_frames_to_push,
			&num_silence_frames_to_insert,
			push_pts
		);

		gst_buffer_unmap(incoming_buffer_copy, &map_info);

		g_assert(num_pushed_frames <= num_remaining_frames_to_push);

		if (num_pushed_frames == num_remaining_frames_to_push)
		{
			UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
			GST_LOG_OBJECT(self, "all (remaining) %" G_GSIZE_FORMAT " frames pushed", num_remaining_frames_to_push);
			break;
		}
		else
		{
			GST_LOG_OBJECT(
				self,
				"attempted to push %" G_GSIZE_FORMAT " frame(s), actually pushed %" G_GSIZE_FORMAT "; waiting until there is more room",
				num_remaining_frames_to_push,
				num_pushed_frames
			);

			g_cond_wait(&(self->audio_data_buffer_cond), &(self->audio_data_buffer_mutex));

			UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
		}

		num_remaining_frames_to_push -= num_pushed_frames;
		incoming_buffer_frame_offset += num_pushed_frames;
	}

finish:
	if (incoming_buffer_copy != NULL)
		gst_buffer_unref(incoming_buffer_copy);
	return flow_ret;
}


static GstFlowReturn gst_pw_audio_sink_render_encoded(GstPwAudioSink *self, GstBuffer *original_incoming_buffer)
{
	GstFlowReturn flow_ret = GST_FLOW_OK;
	GstBaseSink *basesink = GST_BASE_SINK_CAST(self);
	guint64 quantum_size_in_ns;
	GstClockTime frame_duration;
	guint rate;
	guint frame_length;

	GST_LOG_OBJECT(self, "incoming buffer: %" GST_PTR_FORMAT, (gpointer)original_incoming_buffer);

	if (G_UNLIKELY(!GST_BUFFER_DURATION_IS_VALID(original_incoming_buffer)))
	{
		GST_ERROR_OBJECT(self, "incoming buffer has no valid duration");
		return GST_FLOW_ERROR;
	}
	frame_duration = GST_BUFFER_DURATION(original_incoming_buffer);

	pw_thread_loop_lock(self->pipewire_core->loop);
	quantum_size_in_ns = self->quantum_size_in_ns;
	pw_thread_loop_unlock(self->pipewire_core->loop);

	rate = self->pw_audio_format.info.encoded_audio_info.rate;
	frame_length = gst_util_uint64_scale_round(frame_duration, rate, GST_SECOND);

	if (self->last_encoded_frame_length != frame_length)
	{
		struct spa_dict_item items[1];
		gchar *latency_str;

		latency_str = g_strdup_printf("%u/%u", frame_length, rate);

		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency_str);
		pw_thread_loop_lock(self->pipewire_core->loop);
		pw_stream_update_properties(self->stream, &SPA_DICT_INIT(items, 1));
		pw_thread_loop_unlock(self->pipewire_core->loop);

		GST_INFO_OBJECT(self, "updating pw stream latency to %s", latency_str);

		g_free(latency_str);

		self->last_encoded_frame_length = frame_length;
	}

	LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	while (TRUE)
	{
		if (g_atomic_int_get(&(self->flushing)))
		{
			GST_DEBUG_OBJECT(self, "exiting loop in render function since we are flushing");
			flow_ret = GST_FLOW_FLUSHING;
			goto finish;
		}

		if (g_atomic_int_get(&(self->paused)))
		{
			GST_DEBUG_OBJECT(self, "sink is paused; waiting for preroll, flushing, or a state change to READY");

			UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
			flow_ret = gst_base_sink_wait_preroll(basesink);
			LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

			if (flow_ret != GST_FLOW_OK)
				goto finish;
		}

		/* In some cases, we can reach this point _before_ the quantum size is known.
		 * Just push data then. This state won't last very long, so as soon as the
		 * quantum_size_in_ns is nonzero, we actually limit the max queue size based
		 * on that nonzero value. */
		if ((quantum_size_in_ns == 0) || (self->total_queued_encoded_data_duration < quantum_size_in_ns))
		{
			GST_LOG_OBJECT(
				self,
				"encoded data queue has room for more data (duration of queued data; %" GST_TIME_FORMAT " - less than one quantum); pushing",
				GST_TIME_ARGS(self->total_queued_encoded_data_duration)
			);
			gst_queue_array_push_tail(self->encoded_data_queue, gst_buffer_ref(original_incoming_buffer));
			self->total_queued_encoded_data_duration += frame_duration;
			goto finish;
		}
		else
		{
			GST_LOG_OBJECT(
				self,
				"encoded data queue has no room for more data (duration of queued data; %" GST_TIME_FORMAT " - >= one quantum); waiting",
				GST_TIME_ARGS(self->total_queued_encoded_data_duration)
			);
			g_cond_wait(&(self->audio_data_buffer_cond), &(self->audio_data_buffer_mutex));
		}
	}

finish:
	UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
	return flow_ret;
}


static gboolean gst_pw_audio_sink_handle_convert_query(GstPwAudioSink *self, GstQuery *query)
{
	GstFormat source_gstformat, dest_gstformat;
	gint64 source_quantity, dest_quantity;

	gst_query_parse_convert(query, &source_gstformat, &source_quantity, &dest_gstformat, NULL);

	GST_LOG_OBJECT(
		self,
		"handle convert query: source/dest format: %s/%s source quantity: %" G_GINT64_FORMAT,
		gst_format_get_name(source_gstformat),
		gst_format_get_name(dest_gstformat),
		source_quantity
	);

	if (G_UNLIKELY(source_gstformat == dest_gstformat))
	{
		GST_DEBUG_OBJECT(self, "not actually converting anything since source and dest format are the same");
		dest_quantity = source_quantity;
	}
	else
	{
		gint64 source_quantity_in_bytes;
		gsize stride;

		if (source_quantity < 0)
		{
			/* TODO */
			GST_FIXME_OBJECT(self, "converting negative quantities is not yet supported");
			goto cannot_convert;
		}

		if ((source_gstformat != GST_FORMAT_BYTES) || (dest_gstformat != GST_FORMAT_BYTES))
		{
			if (self->sink_caps == NULL)
			{
				/* If sink caps are not set, it means that neither is pw_audio_format. Hence the check above. */
				GST_DEBUG_OBJECT(self, "cannot respond to convert query (yet) because pw_audio_format is not initialized");
				goto cannot_convert;
			}
		}

		stride = gst_pw_audio_format_get_stride(&(self->pw_audio_format));

		switch (source_gstformat)
		{
			case GST_FORMAT_BYTES:
				source_quantity_in_bytes = source_quantity;
				break;

			case GST_FORMAT_DEFAULT:
				source_quantity_in_bytes = source_quantity * stride;
				break;

			case GST_FORMAT_TIME:
				source_quantity_in_bytes = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), source_quantity) * stride;
				break;

			default:
				GST_DEBUG_OBJECT(self, "cannot handle source format %s in convert query", gst_format_get_name(source_gstformat));
				goto cannot_convert;
		}

		switch (dest_gstformat)
		{
			case GST_FORMAT_BYTES:
				dest_quantity = source_quantity_in_bytes;
				break;

			case GST_FORMAT_DEFAULT:
				dest_quantity = source_quantity_in_bytes / stride;
				break;

			case GST_FORMAT_TIME:
				dest_quantity = gst_pw_audio_format_calculate_duration_from_num_frames(&(self->pw_audio_format), source_quantity_in_bytes / stride);
				break;

			default:
				GST_DEBUG_OBJECT(self, "cannot handle dest format %s in convert query", gst_format_get_name(dest_gstformat));
				goto cannot_convert;
		}
	}

	GST_LOG_OBJECT(self, "conversion result: %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT, source_quantity, dest_quantity);

	gst_query_set_convert(query, source_gstformat, source_quantity, dest_gstformat, dest_quantity);

	return TRUE;

cannot_convert:
	return FALSE;
}


static void gst_pw_audio_sink_set_provide_clock_flag(GstPwAudioSink *self, gboolean flag)
{
	GST_DEBUG_OBJECT(self, "setting provide-clock to %d", flag);
	GST_OBJECT_LOCK(self);
	if (flag)
		GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
	else
		GST_OBJECT_FLAG_UNSET(self, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
	GST_OBJECT_UNLOCK(self);
}


static gboolean gst_pw_audio_sink_get_provide_clock_flag(GstPwAudioSink *self)
{
	gboolean flag;

	GST_OBJECT_LOCK(self);
	flag = GST_OBJECT_FLAG_IS_SET(self, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
	GST_OBJECT_UNLOCK(self);

	return flag;
}


static void gst_pw_audio_sink_activate_stream_unlocked(GstPwAudioSink *self, gboolean activate)
{
	/* This must be called with the pw_thread_loop_lock taken. */

	if (self->stream_is_active == activate)
		return;

	/* Reset drift compensation states if we are about to activate
	 * the stream. We do this _only_ in the inactive -> active
	 * case, and only _before_ activating. This prevents race
	 * conditions where the process callback accesses these states
	 * while we are resetting them here. Also reset last_pw_time_ticks,
	 * since we use that for detecting discontinuities within the
	 * flow of a pw_stream itself. Since we are just starting the
	 * stream, we have no "last" pw_time ticks recorded yet. */
	if (activate)
	{
		gst_pw_audio_sink_reset_drift_compensation_states(self);
		self->last_pw_time_ticks = 0;
		self->last_pw_time_ticks_set = FALSE;
	}

	pw_stream_set_active(self->stream, activate);
	GST_DEBUG_OBJECT(self, "%s PipeWire stream", activate ? "activating" : "deactivating");

	self->stream_is_active = activate;

	if (!activate)
		self->stream_drained = FALSE;
}


static void gst_pw_audio_sink_setup_audio_data_buffer(GstPwAudioSink *self)
{
	if (gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
	{
		self->ring_buffer = gst_pw_audio_ring_buffer_new(&(self->pw_audio_format), self->ring_buffer_length_snapshot);

		if (self->pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_DSD)
		{
			/* Allocate a DSD conversion buffer that is big enough for 1 second
			 * worth of DSD data with DSDU32BE/LE grouping formats (the biggest
			 * ones available). This is far bigger that what's needed in the vast
			 * majority of cases, since in PipeWire, a quantum length of more than
			 * maybe 50-100ms is unusual, so 1s gives us plenty of headroom. */

			self->dsd_conversion_buffer_size = gst_pw_audio_format_calculate_num_frames_from_duration(
				&(self->pw_audio_format),
				GST_SECOND * 1
			) * self->pw_audio_format.info.dsd_audio_info.channels * 4; /* "*4" for the DSDU32 format */

			GST_DEBUG_OBJECT(
				self,
				"allocating DSD conversion buffer with %" G_GSIZE_FORMAT " byte(s)",
				self->dsd_conversion_buffer_size
			);

			self->dsd_conversion_buffer = g_malloc(self->dsd_conversion_buffer_size);
		}
	}
	else
	{
		self->encoded_data_queue = gst_queue_array_new(0);
		gst_queue_array_set_clear_func(self->encoded_data_queue, (GDestroyNotify)gst_buffer_unref);
		self->total_queued_encoded_data_duration = 0;
	}
}


static void gst_pw_audio_sink_teardown_audio_data_buffer(GstPwAudioSink *self)
{
	if (self->ring_buffer != NULL)
	{
		gst_object_unref(GST_OBJECT(self->ring_buffer));
		self->ring_buffer = NULL;
	}

	if (self->encoded_data_queue != NULL)
	{
		gst_queue_array_free(self->encoded_data_queue);
		self->encoded_data_queue = NULL;
	}

	g_free(self->dsd_conversion_buffer);
	self->dsd_conversion_buffer = NULL;
}


static void gst_pw_audio_sink_reset_audio_data_buffer_unlocked(GstPwAudioSink *self)
{
	/* This must be called with the audio data buffer mutex locked. */

	if (self->ring_buffer != NULL)
		gst_pw_audio_ring_buffer_flush(self->ring_buffer);

	self->accum_excess_encaudio_playtime = 0;

	/* Also reset these states, since a queue reset effectively ends
	 * any synchronized playback of the stream that was going on earlier,
	 * and there's no more old data to check for alignment with new data. */
	self->synced_playback_started = FALSE;
	self->expected_next_running_time_pts = GST_CLOCK_TIME_NONE;
}


static void gst_pw_audio_sink_reset_drift_compensation_states(GstPwAudioSink *self)
{
	pi_controller_reset(&(self->pi_controller));
	self->previous_time = GST_CLOCK_TIME_NONE;
}


static void gst_pw_audio_sink_drain_stream_unlocked(GstPwAudioSink *self)
{
	/* This must be called with the pw_thread_loop_lock taken. */

	/* pw_stream_flush with drain = TRUE will block permanently if the
	 * stream is not active, so the stream_is_active check is essential.
	 * Also check stream_drained to avoid redundant calls. */
	if (!self->stream_is_active || self->stream_drained)
		return;

	GST_DEBUG_OBJECT(self, "pw stream drain initiated");
	pw_stream_flush(self->stream, TRUE);
	while (!self->stream_drained)
		pw_thread_loop_wait(self->pipewire_core->loop);
}


static void gst_pw_audio_sink_drain_stream_and_audio_data_buffer(GstPwAudioSink *self)
{
	if (self->ring_buffer == NULL)
		return;

	/* locking the audio data buffer mutex to wait until the process
	 * callback signals that the fill level / queue length can be
	 * (re-)checked. We do that until the fill level is zero
	 * (= buffer is empty) in raw audio and until the queue is
	 * empty in encoded audio. */
	LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	if (gst_pw_audio_format_data_is_raw(self->pw_audio_format.audio_type))
	{
		while (TRUE)
		{
			GstClockTime current_fill_level;

			if (g_atomic_int_get(&(self->flushing)))
			{
				GST_DEBUG_OBJECT(self, "aborting drain since we are flushing");
				break;
			}

			current_fill_level = gst_pw_audio_ring_buffer_get_current_fill_level(self->ring_buffer);

			if (current_fill_level == 0)
			{
				GST_DEBUG_OBJECT(self, "audio data buffer is fully drained");
				break;
			}
			else
			{
				GST_DEBUG_OBJECT(self, "audio data buffer still contains data; current audio data buffer fill level: %" GST_TIME_FORMAT, GST_TIME_ARGS(current_fill_level));
				g_cond_wait(&(self->audio_data_buffer_cond), &(self->audio_data_buffer_mutex));
			}
		}
	}
	else
	{
		while (TRUE)
		{
			guint num_queued_frames;

			if (g_atomic_int_get(&(self->flushing)))
			{
				GST_DEBUG_OBJECT(self, "aborting drain since we are flushing");
				break;
			}

			num_queued_frames = gst_queue_array_get_length(self->encoded_data_queue);

			if (num_queued_frames == 0)
			{
				GST_DEBUG_OBJECT(self, "encoded data queue is fully drained");
				break;
			}
			else
			{
				GST_DEBUG_OBJECT(self, "encoded data queue still contains data; number of queued frames: %u", num_queued_frames);
				g_cond_wait(&(self->audio_data_buffer_cond), &(self->audio_data_buffer_mutex));
			}
		}
	}

	UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	/* Now that the audio data buffer is empty, the next
	 * step is to drain the PipeWire stream itself. */

	pw_thread_loop_lock(self->pipewire_core->loop);
	gst_pw_audio_sink_drain_stream_unlocked(self);
	pw_thread_loop_unlock(self->pipewire_core->loop);

	/* NOTE: Stream is drained at this point and must be reactivated
	 * by calling gst_pw_audio_sink_activate_stream_unlocked(). */
}


static void gst_pw_audio_sink_disconnect_stream(GstPwAudioSink *self)
{
	if (!self->stream_is_connected)
		return;

	pw_thread_loop_lock(self->pipewire_core->loop);
	gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
	pw_stream_disconnect(self->stream);
	pw_thread_loop_unlock(self->pipewire_core->loop);

	self->stream_is_connected = FALSE;
}


static gchar const * spa_io_position_state_to_string(enum spa_io_position_state const state)
{
	switch (state)
	{
		case SPA_IO_POSITION_STATE_STOPPED: return "stopped";
		case SPA_IO_POSITION_STATE_STARTING: return "starting";
		case SPA_IO_POSITION_STATE_RUNNING: return "running";
		default: return "<unknown>";
	}
}


static void gst_pw_audio_sink_pw_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);

	GST_DEBUG_OBJECT(
		self,
		"PipeWire state changed:  old: %s  new: %s  error: \"%s\"",
		pw_stream_state_as_string(old_state),
		pw_stream_state_as_string(new_state),
		(error == NULL) ? "<none>" : error
	);

	switch (new_state) {
		case PW_STREAM_STATE_ERROR:
		case PW_STREAM_STATE_UNCONNECTED:
			/* Make sure the stream is now considered drained. This is important if
			 * the pipewire server went away, because then, the sink will attempt
			 * to drain the stream, and that request will never be answered, because
			 * the response would come from the (now gone) server. */
			GST_DEBUG_OBJECT(
				self,
				"marking stream as drained after reaching the %s state; if stream wasn't drained before, its data for sure is gone by now",
				pw_stream_state_as_string(new_state)
			);
			self->stream_drained = TRUE;
			pw_thread_loop_signal(self->pipewire_core->loop, FALSE);
			break;
		default:
			break;
	}
}


static void gst_pw_audio_sink_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);
	GstPwAudioFormat changed_pw_audio_format;

	if ((id != SPA_PARAM_Format) || (param == NULL))
		return;

	/* In theory, the format param may change at any moment. In practice, this is
	 * not expected to happen, since we set up the pw_stream with an EnumFormat
	 * param that contains exactly one set of fixed parameters. The only exception
	 * is the DSD audio format, which can change depending on what the graph needs.
	 * This means that we can expect all parameters except the DSD audio format
	 * to stay the same. For this reason, we look at only that one here. */

	if (!gst_pw_audio_format_from_spa_pod_with_format_param(
		&changed_pw_audio_format,
		GST_OBJECT_CAST(self),
		param
	))
		return;

	{
		gchar *format_str = gst_pw_audio_format_to_string(&changed_pw_audio_format);
		GST_DEBUG_OBJECT(self, "format param changed;  audio format details: %s", format_str);
		g_free(format_str);
	}

	/* See the gst_pw_audio_format_get_stride() documentation for an
	 * explanation why dsd_buffer_size_multiplier is needed. */
	if (changed_pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_DSD)
	{
		GstPipewireDsdFormat input_dsd_format = self->pw_audio_format.info.dsd_audio_info.format;
		GstPipewireDsdFormat graph_dsd_format = changed_pw_audio_format.info.dsd_audio_info.format;

		guint input_dsd_format_width = gst_pipewire_dsd_format_get_width(input_dsd_format);
		guint graph_dsd_format_width = gst_pipewire_dsd_format_get_width(graph_dsd_format);

		self->actual_dsd_format = graph_dsd_format;

		/* dsd_data_rate_multiplier is needed because the minimum necessary amount
		 * of data that needs to be produced in the process callback depends on
		 * dsd_buffer_size_multiplier *and* on the DSD rate. Any rate higher than
		 * DSD64 needs an integer multiple of the indicated quantum size. */
		self->dsd_data_rate_multiplier = self->pw_audio_format.info.dsd_audio_info.rate / GST_PIPEWIRE_DSD_DSD64_BYTE_RATE;

		if (graph_dsd_format_width > input_dsd_format_width)
			self->dsd_buffer_size_multiplier = graph_dsd_format_width / input_dsd_format_width;
		else
			self->dsd_buffer_size_multiplier = 1;

		GST_DEBUG_OBJECT(
			self,
			"additional DSD information:  input/graph DSD format: %s/%s  input/graph DSD format width: %u/%u  buffer size multiplier: %u  data rate multiplier: %u",
			gst_pipewire_dsd_format_to_string(input_dsd_format), gst_pipewire_dsd_format_to_string(graph_dsd_format),
			input_dsd_format_width, graph_dsd_format_width,
			self->dsd_buffer_size_multiplier,
			self->dsd_data_rate_multiplier
		);
	}
}


static void gst_pw_audio_sink_io_changed(void *data, uint32_t id, void *area, G_GNUC_UNUSED uint32_t size)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);

	switch (id)
	{
		case SPA_IO_Position:
		{
			/* Retrieve SPA IO position pointer for keeping track of the rate_diff.
			 * rate_diff is then accessed in gst_pw_audio_sink_on_process_stream(). */

			self->spa_position = (struct spa_io_position *)area;
			if (self->spa_position != NULL)
			{
				self->quantum_size_in_ticks = self->spa_position->clock.duration;
				self->quantum_size_in_ns = gst_util_uint64_scale_int(
					self->quantum_size_in_ticks * self->spa_position->clock.rate.num,
					GST_SECOND,
					self->spa_position->clock.rate.denom
				);

				GST_DEBUG_OBJECT(
					self,
					"got new SPA IO position:  offset: %" G_GINT64_FORMAT "  state: %s  num segments: %" G_GUINT32_FORMAT "  "
					"quantum size in ticks: %" G_GUINT64_FORMAT "  rate: %" G_GUINT32_FORMAT "/%" G_GUINT32_FORMAT " "
					" => quantum size in ns: %" GST_TIME_FORMAT,
					(gint64)(self->spa_position->offset),
					spa_io_position_state_to_string((enum spa_io_position_state)(self->spa_position->state)),
					(guint32)(self->spa_position->n_segments),
					self->quantum_size_in_ticks,
					self->spa_position->clock.rate.num, self->spa_position->clock.rate.denom,
					GST_TIME_ARGS(self->quantum_size_in_ns)
				);
			}
			else
			{
				GST_DEBUG_OBJECT(self, "got NULL SPA IO position; our PW client node got removed from the driver");
			}

			break;
		}

		case SPA_IO_RateMatch:
		{
			/* Retrieve SPA IO RateMatch pointer. The referred rate_match structure does not
			 * yet contain valid values at this point. But when gst_pw_audio_sink_on_process_stream()
			 * is called, it does, so it is usable there.
			 * The rate matching ASRC is disabled by default, and is enabled explicitly by setting
			 * the SPA_IO_RATE_MATCH_FLAG_ACTIVE flag. Once enabled, the "rate" field tweaks the ASRC
			 * ratio. The ASRC's ratio is multiplied by this field if the flag is active. This means
			 * that if for example rate is 1.1, then 110% of the normal amount of data is generated.
			 * So, if the audio output is falling behind the reference signal, rate needs to be >1.0,
			 * and if it is ahead of the reference signal, it needs to be <1.0. The rate field can be
			 * set in the process callback (see gst_pw_audio_sink_raw_on_process_stream).
			 * The rate match pointer can be NULL if no rate matching is available, for example,
			 * because this is a passthrough stream. */

			self->spa_rate_match = (struct spa_io_rate_match *)area;

			if (self->spa_rate_match != NULL)
			{
				if (self->stream_clock_is_pipeline_clock)
				{
					GST_INFO_OBJECT(self, "stream clock is the pipeline clock; not enabling rate match");
					self->spa_rate_match->flags &= ~SPA_IO_RATE_MATCH_FLAG_ACTIVE;
				}
				else
				{
					/* Make sure that the starting state is one without any actual sample rate conversion. */
					self->spa_rate_match->rate = 1.0;

					GST_INFO_OBJECT(self, "stream clock is not the pipeline clock; enabling rate match");
					self->spa_rate_match->flags |= SPA_IO_RATE_MATCH_FLAG_ACTIVE;
				}
			}
			else
			{
				GST_DEBUG_OBJECT(self, "got NULL SPA IO rate match");
			}

			break;
		}

		default:
			break;
	}
}


static void gst_pw_audio_sink_on_stream_drained(void *data)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);
	GST_DEBUG_OBJECT(self, "pw stream fully drained");
	self->stream_drained = TRUE;
	pw_thread_loop_signal(self->pipewire_core->loop, FALSE);
}


static void gst_pw_audio_sink_raw_on_process_stream(void *data)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);
	struct pw_time stream_time;
	struct pw_buffer *pw_buf;
	struct spa_data *inner_spa_data;
	GstClockTime upstream_pipeline_latency;
	gint64 stream_delay_in_ns;
	guint64 num_frames_to_produce;
	gint64 time_since_delay_measurement;
	guint64 min_num_required_ticks;
	gboolean produce_silence_quantum = TRUE;

	GST_LOG_OBJECT(self, COLOR_GREEN "new PipeWire graph tick" COLOR_DEFAULT);

	/* pw_stream_get_time() is deprecated since version 0.3.50. */
#if PW_CHECK_VERSION(0, 3, 50)
	pw_stream_get_time_n(self->stream, &stream_time, sizeof(stream_time));
#else
	pw_stream_get_time(self->stream, &stream_time);
#endif

	gst_pw_stream_clock_add_observation(self->stream_clock, &stream_time);

	if (self->last_pw_time_ticks_set)
	{
		uint64_t tick_delta = stream_time.ticks - self->last_pw_time_ticks;

		if (G_UNLIKELY(tick_delta > self->quantum_size_in_ticks))
		{
			GST_INFO_OBJECT(self, "tick delta is %" G_GUINT64_FORMAT ", which is greater than expected %" G_GUINT64_FORMAT "; discontinuity in pw stream detected; resynchronizing", (guint64)tick_delta, (guint64)(self->quantum_size_in_ticks));
			self->synced_playback_started = FALSE;
		}
		else if (G_UNLIKELY(tick_delta < self->quantum_size_in_ticks))
		{
			/* TODO: It is currently unclear what this case means, but it does not seem that we have to resynchronize then. */
			GST_INFO_OBJECT(self, "tick delta is %" G_GUINT64_FORMAT ", which is lesser than expected %" G_GUINT64_FORMAT, (guint64)tick_delta, (guint64)(self->quantum_size_in_ticks));
		}

		/* If tick_delta == self->quantum_size_in_ticks, then no discontinuity happens, everything is OK. */
	}
	else
		self->last_pw_time_ticks_set = TRUE;

	self->last_pw_time_ticks = stream_time.ticks;

	/* We set the stream_delay_in_ns value here and access the latency value,
	 * so the latency mutex must be locked. (stream_delay_in_ticks is only
	 * ever used in here.) */
	LOCK_LATENCY_MUTEX(self);

	if ((stream_time.rate.denom != 0) && (self->stream_delay_in_ticks != stream_time.delay))
	{
		gint64 new_delay_in_ns;

		new_delay_in_ns = gst_util_uint64_scale_int(
			stream_time.delay * stream_time.rate.num,
			GST_SECOND,
			stream_time.rate.denom
		);

		GST_DEBUG_OBJECT(
			self,
			"stream delay updated from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT " (old -> new delay in ticks: %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT "; PW rate: %" G_GUINT32_FORMAT "/%" G_GUINT32_FORMAT ")",
			GST_TIME_ARGS(self->stream_delay_in_ns),
			GST_TIME_ARGS(new_delay_in_ns),
			self->stream_delay_in_ticks, (gint64)(stream_time.delay),
			(guint32)(stream_time.rate.num), (guint32)(stream_time.rate.denom)
		);

		self->stream_delay_in_ticks = stream_time.delay;
		self->stream_delay_in_ns = stream_delay_in_ns = new_delay_in_ns;
		g_atomic_int_set(&(self->notify_upstream_about_stream_delay), 1);
	}
	else
		stream_delay_in_ns = self->stream_delay_in_ns;

	/* In live pipelines, the pipeline has a defined latency. This sink element
	 * gets the latency in a latency event (see gst_pw_audio_sink_send_event())
	 * and is stored there in self->latency. That latency quantity includes our
	 * own latency, that is, the value of stream_delay_in_ns. Thus, if the
	 * pipeline is live, then self->latency must include the value of stream_delay_in_ns,
	 * and therefore, self->latency >= stream_delay_in_ns must hold. We are
	 * interested in the latency upstream elements add; we already know our own
	 * latency (it is defined by stream_delay_in_ns). Subtract stream_delay_in_ns
	 * to get the upstream latency _without_ our own. But if this is not a live
	 * pipeline, then self->latency >= stream_delay_in_ns won't hold, and we use
	 * 0 as the upstream latency (since there is none in non-live pipelines). */
	upstream_pipeline_latency = ((gint64)(self->latency) >= stream_delay_in_ns) ? (self->latency - stream_delay_in_ns) : 0;

	UNLOCK_LATENCY_MUTEX(self);

	/* stream_time.delay was measured at the stream_time.now timestamp.
	 * That timestamp was recorded using the monotonic system clock.
	 * To further refine the frame retrieval, calculate how much time has
	 * elapsed since stream_time.delay was sampled. */
	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		time_since_delay_measurement = SPA_TIMESPEC_TO_NSEC(&ts) - stream_time.now;

		if (G_LIKELY(stream_delay_in_ns >= time_since_delay_measurement))
		{
			GST_LOG_OBJECT(self, "nanoseconds since delay measurement: %" G_GINT64_FORMAT, time_since_delay_measurement);
		}
		else
		{
			GST_WARNING_OBJECT(
				self,
				"nanoseconds since delay measurement (%" G_GINT64_FORMAT ") exceed stream delay (%" G_GINT64_FORMAT "); underrun is likely to have occurred; resynchronizing",
				time_since_delay_measurement,
				stream_delay_in_ns
			);
			self->synced_playback_started = FALSE;
		}
	}

	pw_buf = pw_stream_dequeue_buffer(self->stream);
	if (G_UNLIKELY(pw_buf == NULL))
	{
		GST_WARNING_OBJECT(self, "there are no PipeWire buffers to dequeue; cannot process anything");
		return;
	}

	g_assert(pw_buf->buffer != NULL);

	if (G_UNLIKELY(pw_buf->buffer->n_datas == 0))
	{
		GST_WARNING_OBJECT(self, "dequeued PipeWire buffer has no data");
		goto finish;
	}

	inner_spa_data = &(pw_buf->buffer->datas[0]);
	if (G_UNLIKELY(inner_spa_data->data == NULL))
	{
		GST_WARNING_OBJECT(self, "dequeued PipeWire buffer has no mapped data pointer");
		goto finish;
	}

	min_num_required_ticks = (self->spa_rate_match != NULL) ? self->spa_rate_match->size : self->quantum_size_in_ticks;

	/* We are about to retrieve data from the ring buffer and
	 * also are about to access synced_playback_started, so synchronize
	 * access by locking the audio buffer's gstobject mutex. It is unlocked
	 * immediately once it is no longer needed to minimize chances of
	 * thread starvation (in the render() function) and similar. */
	LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	switch (self->pw_audio_format.audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
			num_frames_to_produce = min_num_required_ticks;
			break;

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
			// TODO: It is currently unclear why the *2 multiplication is necessary.
			// It is known that commit c48a4bc166bfb5827ecea1195a8435458a3d8501 in
			// pipewire-git ("pw-cat: fix DSF playback again") applies frame scaling,
			// so perhaps something related needs to be done here. Alternatively,
			// the "2" may be related to the number of channels.
			num_frames_to_produce = min_num_required_ticks
			                      * self->dsd_data_rate_multiplier
			                      * self->dsd_buffer_size_multiplier
			                      * 2;
			break;

		default:
			g_assert_not_reached();
	}

	if (G_UNLIKELY(gst_pw_audio_ring_buffer_get_current_fill_level(self->ring_buffer) == 0))
	{
		GST_DEBUG_OBJECT(self, "ring buffer empty/underrun; producing silence quantum");
		/* In case of an underrun we have to re-sync the output. */
		self->synced_playback_started = FALSE;
		UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
	}
	else if (G_UNLIKELY(num_frames_to_produce == 0))
	{
		inner_spa_data->chunk->offset = 0;
		inner_spa_data->chunk->size = 0;
		inner_spa_data->chunk->stride = self->stride;
		UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
	}
	else
	{
		produce_silence_quantum = FALSE;

		switch (self->pw_audio_format.audio_type)
		{
			case GST_PIPEWIRE_AUDIO_TYPE_PCM:
			case GST_PIPEWIRE_AUDIO_TYPE_DSD:
			{
				GstPwAudioRingBufferRetrievalResult retrieval_result;
				GstClockTime current_time = GST_CLOCK_TIME_NONE;
				GstClockTimeDiff buffered_frames_to_retrieval_pts_delta;
				gboolean early_exit = FALSE;

				GstClockTimeDiff effective_skew_threshold = self->synced_playback_started ? self->skew_threshold_snapshot : 0;

				current_time = gst_clock_get_time(GST_ELEMENT_CLOCK(self));
				GST_LOG_OBJECT(
					self,
					"current time: %" GST_TIME_FORMAT "  "
					"num frames to produce: %" G_GUINT64_FORMAT "  "
					"upstream pipeline latency: %" GST_TIME_FORMAT,
					GST_TIME_ARGS(current_time),
					num_frames_to_produce,
					GST_TIME_ARGS(upstream_pipeline_latency)
				);

				if ((self->pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_DSD)
				 && (self->pw_audio_format.info.dsd_audio_info.format != self->actual_dsd_format))
				{
					/* If we reach this point, it means we have incoming DSD data
					 * that can't directly be passed to the graph, because the latter
					 * uses a different grouping format. We have to converr the data
					 * first. Retrieve incoming DSD frames from the ring buffer and
					 * store them in the intermediate "DSD conversion buffer". Then,
					 * use that buffer as the source and the SPA data chunk as the
					 * destination for the conversion.
					 *
					 * The frame counts can be confusing, because they depend on the
					 * DSD format. The same playtime that is covered by 20 DSDU8
					 * frames is also coverd by 10 DSDU16LE frames for example.
					 * Here, we deal with _input_ format widths and strides, because
					 * that's what is passed around until the very end, when the
					 * actual conversion takes place. */

					GstPipewireDsdFormat input_dsd_format = self->pw_audio_format.info.dsd_audio_info.format;
					GstPipewireDsdFormat graph_dsd_format = self->actual_dsd_format;
					guint input_dsd_format_width = gst_pipewire_dsd_format_get_width(input_dsd_format);
					guint input_dsd_format_stride = input_dsd_format_width * self->pw_audio_format.info.dsd_audio_info.channels;

					/* To avoid buffer overflows, check how many input DSD frames can
					 * fit in the DSD conversion buffer. */
					guint64 max_num_input_frames_in_conv_buffer = self->dsd_conversion_buffer_size / input_dsd_format_stride;
					/* Counter for many DSD frames were retrieved and converted in the loop. */
					guint64 num_produced_frames;

					guint8 *dest_start_ptr = (guint8 *)(inner_spa_data->data);
					guint8 *dest_ptr = (guint8 *)(inner_spa_data->data);

					for (num_produced_frames = 0;  num_produced_frames < num_frames_to_produce;)
					{
						guint64 num_frames_left_to_produce = num_frames_to_produce - num_produced_frames;

						/* Apply limit in case there are more frames to retrieve
						 * than what the DSD conversion buffer can handle. */
						guint64 num_frames_to_convert = MIN(max_num_input_frames_in_conv_buffer, num_frames_left_to_produce);

						gsize num_conv_output_bytes = num_frames_to_convert * input_dsd_format_stride;

						GST_LOG_OBJECT(
							self,
							"converting DSD frames: num produced / num to produce: %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "; "
							"now converting %" G_GUINT64_FORMAT,
							num_produced_frames, num_frames_to_produce,
							num_frames_to_convert
						);

						g_assert((dest_ptr - dest_start_ptr) <= inner_spa_data->maxsize);
						g_assert(num_conv_output_bytes <= self->dsd_conversion_buffer_size);

						retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
							self->ring_buffer,
							self->dsd_conversion_buffer,
							num_frames_to_convert,
							current_time,
							upstream_pipeline_latency + time_since_delay_measurement,
							effective_skew_threshold,
							&buffered_frames_to_retrieval_pts_delta
						);

						gst_pipewire_dsd_convert(
							self->dsd_conversion_buffer,
							dest_ptr,
							input_dsd_format,
							graph_dsd_format,
							num_conv_output_bytes,
							self->pw_audio_format.info.dsd_audio_info.channels
						);

						dest_ptr += num_conv_output_bytes;
						num_produced_frames += num_frames_to_convert;
					}
				}
				else
				{
					/* We use both upstream_pipeline_latency and time_since_delay_measurement
					 * for the PTS shift quantity. The former is necessary to compensate for
					 * the upstream pipeline latency. The latter is necessary to retrieve data
					 * from a moment that corresponds to the scheduled beginning of this
					 * pipewire graph tick. */
					retrieval_result = gst_pw_audio_ring_buffer_retrieve_frames(
						self->ring_buffer,
						inner_spa_data->data,
						num_frames_to_produce,
						current_time,
						upstream_pipeline_latency + time_since_delay_measurement,
						effective_skew_threshold,
						&buffered_frames_to_retrieval_pts_delta
					);
				}

				inner_spa_data->chunk->offset = 0;
				inner_spa_data->chunk->size = num_frames_to_produce * self->stride;
				inner_spa_data->chunk->stride = self->stride;

				switch (retrieval_result)
				{
					case GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_OK:
						self->synced_playback_started = TRUE;
						UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
						break;

					case GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_RING_BUFFER_IS_EMPTY:
					{
						self->synced_playback_started = FALSE;
						UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
						early_exit = TRUE;
						GST_DEBUG_OBJECT(self, "ring buffer is empty; could not retrieve frames and need to resynchronize playback");
						break;
					}

					case GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE:
					{
						UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
						early_exit = TRUE;
						break;
					}

					case GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST:
					{
						self->synced_playback_started = FALSE;
						UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
						early_exit = TRUE;
						GST_DEBUG_OBJECT(self, "the ring buffer's frames lie entirely in the past; need to flush those and then resynchronize playback");
						break;
					}

					case GST_PW_AUDIO_RING_BUFFER_RETRIEVAL_RESULT_ALL_DATA_FOR_BUFFER_CLIPPED:
					{
						UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);
						early_exit = TRUE;
						break;
					}
				}

				if (early_exit)
					break;

				/* If the pipeline clock and our PW stream clock are not the same, we must compensate
				 * for a clock drift. Calculate it and a factor for compensating this drift with the
				 * ASRC of the pw_stream. We use the PI controller for this. */
				if ((self->pw_audio_format.audio_type == GST_PIPEWIRE_AUDIO_TYPE_PCM) && !(self->stream_clock_is_pipeline_clock) && (self->spa_rate_match != NULL))
				{
					double input_ppm, filtered_ppm;
					double rate, time_scale;
					GstClockTimeDiff drift_pts_delta, clamped_drift_pts_delta;

					/* Get the PTS delta from the queue. This delta already comes median-filtered,
					 * so we don't bother pre-filtering it further here. However, we do clamp it
					 * to limit the impact it can have on the PI controller. */
					drift_pts_delta = buffered_frames_to_retrieval_pts_delta;
					clamped_drift_pts_delta = CLAMP(drift_pts_delta, -MAX_DRIFT_PTS_DELTA, +MAX_DRIFT_PTS_DELTA);

					/* Do a simple linear transform to convert the PTS delta to a PPM value. PPM
					 * is a relative quantity, and we use MAX_DRIFT_PTS_DELTA as the reference. */
					input_ppm = MAX_DRIFT_PPM * ((double)clamped_drift_pts_delta) / MAX_DRIFT_PTS_DELTA;

					/* Calculate the time_scale to correctly factor in the elapsed time between
					 * ticks when the PI controller calculates the filtered PPM quantity. As for
					 * the situation in the beginning, we don't do any clock drift compensation
					 * from the get-go anyway, so it is fine to use a time_scale of 0, which
					 * effectively amounts to a no-op in pi_controller_compute(). */
					time_scale = GST_CLOCK_TIME_IS_VALID(self->previous_time)
						? (((double)GST_CLOCK_DIFF(self->previous_time, current_time)) / GST_SECOND)
						: 0.0;

					/* Perform the filtering using the PI controller. */
					filtered_ppm = pi_controller_compute(&(self->pi_controller), input_ppm, time_scale);

					/* Using the PPM, adjust the ASRC. */
					rate = 1.0 - filtered_ppm / 1000000.0;
					self->spa_rate_match->rate = rate;

					GST_LOG_OBJECT(
						self,
						"drift adjustment: original / clamped PTS delta: %"
						G_GINT64_FORMAT " / %" G_GINT64_FORMAT
						" time scale: %f input / filtered PPM: %f / %f rate: %f",
						drift_pts_delta, clamped_drift_pts_delta,
						time_scale, input_ppm, filtered_ppm, rate
					);

					/* Store the current time for the next iteration so we can compute the next time_scale. */
					self->previous_time = current_time;
				}

				break;
			}

			default:
				/* Raise an assertion here. If we reach this point, then it means that
				 * either, this raw process callback is being used for non-raw
				 * data, or some extra raw audio type got introduced, but code for
				 * processing it wasn't added here yet. */
				g_assert_not_reached();
		}

		g_cond_signal(&(self->audio_data_buffer_cond));
	}

	if (produce_silence_quantum)
	{
		guint64 num_silence_frames = num_frames_to_produce;
		guint64 num_silence_bytes = num_silence_frames * self->stride;

		g_assert(num_silence_frames <= (inner_spa_data->maxsize / self->stride));

		GST_LOG_OBJECT(self, "producing %" G_GUINT64_FORMAT " frame(s) of silence for silent quantum", num_silence_frames);

		inner_spa_data->chunk->offset = 0;
		inner_spa_data->chunk->size = num_silence_bytes;
		inner_spa_data->chunk->stride = self->stride;

		gst_pw_audio_format_write_silence_frames(&(self->pw_audio_format), inner_spa_data->data, num_silence_frames);

		g_cond_signal(&(self->audio_data_buffer_cond));
	}

finish:
	pw_stream_queue_buffer(self->stream, pw_buf);
}


static void gst_pw_audio_sink_encoded_on_process_stream(void *data)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);
	struct pw_time stream_time;
	struct pw_buffer *pw_buf;
	struct spa_data *inner_spa_data;
	GstBuffer *frame;

	GST_LOG_OBJECT(self, COLOR_GREEN "new PipeWire graph tick" COLOR_DEFAULT);

	pw_stream_get_time_n(self->stream, &stream_time, sizeof(stream_time));

	gst_pw_stream_clock_add_observation(self->stream_clock, &stream_time);

	pw_buf = pw_stream_dequeue_buffer(self->stream);
	if (G_UNLIKELY(pw_buf == NULL))
	{
		GST_WARNING_OBJECT(self, "there are no PipeWire buffers to dequeue; cannot process anything");
		return;
	}

	g_assert(pw_buf->buffer != NULL);

	if (G_UNLIKELY(pw_buf->buffer->n_datas == 0))
	{
		GST_WARNING_OBJECT(self, "dequeued PipeWire buffer has no data");
		goto finish;
	}

	inner_spa_data = &(pw_buf->buffer->datas[0]);
	if (G_UNLIKELY(inner_spa_data->data == NULL))
	{
		GST_WARNING_OBJECT(self, "dequeued PipeWire buffer has no mapped data pointer");
		goto finish;
	}

	LOCK_AUDIO_DATA_BUFFER_MUTEX(self);

	/* Here, we want to get a quantum's worth of data. Also, if we already send an excess amount
	 * of data, and that excess amount reached a quantum's worth, we skip this cycle to prevent
	 * an overflow from happening. */
	if (self->accum_excess_encaudio_playtime >= self->quantum_size_in_ns)
	{
		GST_LOG_OBJECT(self, "producing null frame to compensate for excess playtime");
		frame = NULL;
		self->accum_excess_encaudio_playtime -= self->quantum_size_in_ns;
	}
	else if (self->total_queued_encoded_data_duration < self->quantum_size_in_ns)
	{
		GST_LOG_OBJECT(
			self,
			"insufficient data queued (need at least 1 quantum's worth of queued data; queued: %" GST_TIME_FORMAT " ns); producing null frame",
			GST_TIME_ARGS(self->total_queued_encoded_data_duration)
		);
		frame = NULL;
	}
	else
	{
		GstMapInfo map_info;
		GstClockTime accumulated_duration = 0;

		inner_spa_data->chunk->offset = 0;
		inner_spa_data->chunk->stride = 1;
		inner_spa_data->chunk->size = 0;

		/* At this point, we know that (a) we can send data to the graph without
		 * causing an overflow and (b) we've got enough data queued for one quantum.
		 * Go through the encoded data queue and gather frames until a quantum's
		 * worth of data is accumulated. This is particularly important if the
		 * individual frames are smaller than the quantum; if we always sent
		 * single frames instead of accumulating them to match a quantum's size,
		 * underflows could constantly happen in the graph's sink (because we'd
		 * send insufficient data for covering the quantum's duration).
		 * To be safe, this loop also checks that the queue isn't empty.
		 * It should not be empty, but better safe than sorry. */
		while ((gst_queue_array_get_length(self->encoded_data_queue) > 0) && (accumulated_duration < self->quantum_size_in_ns))
		{
			frame = gst_queue_array_pop_head(self->encoded_data_queue);

			gst_buffer_map(frame, &map_info, GST_MAP_READ);
			memcpy((guint8 *)(inner_spa_data->data) + inner_spa_data->chunk->size, map_info.data, map_info.size);
			inner_spa_data->chunk->size += map_info.size;
			gst_buffer_unmap(frame, &map_info);

			accumulated_duration += GST_BUFFER_DURATION(frame);
			gst_buffer_unref(frame);

			GST_LOG_OBJECT(
				self,
				"got frame from encoded data queue with duration %" GST_TIME_FORMAT "; accumulated duration: %" GST_TIME_FORMAT,
				GST_TIME_ARGS(GST_BUFFER_DURATION(frame)),
				GST_TIME_ARGS(accumulated_duration)
			);
		}
		GST_LOG_OBJECT(self, "got enough data for one quantum");

		g_assert(self->total_queued_encoded_data_duration >= accumulated_duration);
		self->total_queued_encoded_data_duration -= accumulated_duration;

		if (accumulated_duration > self->quantum_size_in_ns)
		{
			GstClockTime excess_playtime = accumulated_duration - self->quantum_size_in_ns;
			self->accum_excess_encaudio_playtime += excess_playtime;
		}

		pw_buf->size = inner_spa_data->chunk->size;

		/* Signal that there is now room in the queue for new data.
		 * Potentially needed if the g_cond_wait() call in
		 * gst_pw_audio_sink_render_encoded() is blocking. */
		g_cond_signal(&(self->audio_data_buffer_cond));
	}

	if (frame == NULL)
	{
		inner_spa_data->chunk->offset = 0;
		inner_spa_data->chunk->stride = 0;
		inner_spa_data->chunk->size = 0;
		pw_buf->size = 0;
	}

	UNLOCK_AUDIO_DATA_BUFFER_MUTEX(self);

finish:
	pw_stream_queue_buffer(self->stream, pw_buf);
}

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

/* Basic idea of this sink is to do most of the heavy lifting in render(),
 * and avoid any unnecessary work in on_process_stream(), especially work
 * that has very variable and/or unpredictable runtime complexity. For
 * example, translating timestamps is done in render(). Blocking wait
 * during pause is done in render() etc. */

/* Major TODOs:
 *
 * - Check preroll behavior
 * - Clock drift compensation (possibly by using spa_io_rate_match)
 * - Support for non-PCM formats (infrastructure for this is in place already)
 * - Trick modes
 * - Playback speed other than 1.0
 * - Reverse playback
 */

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/audio/audio.h>

#include <stdint.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <pipewire/pipewire.h>
#include <spa/node/io.h>
#include <spa/utils/result.h>

#pragma GCC diagnostic pop

#include "gstpipewirecore.h"
#include "gstpwstreamclock.h"
#include "gstpwaudioformat.h"
#include "gstpwaudioqueue.h"
#include "gstpwaudiosink.h"


GST_DEBUG_CATEGORY(pw_audio_sink_debug);
#define GST_CAT_DEFAULT pw_audio_sink_debug


#define COLOR_GREEN "\033[32m"
#define COLOR_DEFAULT "\033[0m"


enum
{
	PROP_0,

	PROP_PROVIDE_CLOCK,
	PROP_ALIGNMENT_THRESHOLD,
	PROP_TARGET_OBJECT_ID,
	PROP_STREAM_PROPERTIES,
	PROP_SOCKET_FD,
	PROP_AUDIO_BUFFER_QUEUE_LENGTH,
	PROP_APP_NAME,
	PROP_NODE_NAME,
	PROP_NODE_DESCRIPTION,

	PROP_LAST
};


#define DEFAULT_PROVIDE_CLOCK TRUE
#define DEFAULT_ALIGNMENT_THRESHOLD (GST_MSECOND * 40)
#define DEFAULT_TARGET_OBJECT_ID PW_ID_ANY
#define DEFAULT_STREAM_PROPERTIES NULL
#define DEFAULT_SOCKET_FD (-1)
#define DEFAULT_AUDIO_BUFFER_QUEUE_LENGTH 100
#define DEFAULT_APP_NAME NULL
#define DEFAULT_NODE_NAME NULL
#define DEFAULT_NODE_DESCRIPTION NULL


#define LOCK_AUDIO_BUFFER_QUEUE(pw_audio_sink) GST_OBJECT_LOCK((pw_audio_sink)->audio_buffer_queue)
#define UNLOCK_AUDIO_BUFFER_QUEUE(pw_audio_sink) GST_OBJECT_UNLOCK((pw_audio_sink)->audio_buffer_queue)
#define GET_AUDIO_BUFFER_QUEUE_MUTEX(pw_audio_sink) GST_OBJECT_GET_LOCK((pw_audio_sink)->audio_buffer_queue)

#define LOCK_LATENCY_MUTEX(pw_audio_sink) g_mutex_lock(&((pw_audio_sink)->latency_mutex))
#define UNLOCK_LATENCY_MUTEX(pw_audio_sink) g_mutex_unlock(&((pw_audio_sink)->latency_mutex))


struct _GstPwAudioSink
{
	GstBaseSink parent;

	/*< private >*/

	/** Object properties **/

	GstClockTimeDiff alignment_threshold;
	uint32_t target_object_id;
	GstStructure *stream_properties;
	int socket_fd;
	guint audio_buffer_queue_length;
	gchar *app_name;
	gchar *node_name;
	gchar *node_description;

	/** Playback format **/

	GstCaps *sink_caps;
	GstPwAudioFormat pw_audio_format;

	/** Playback states and queued data **/

	/* NOTE: Access to audio_buffer_queue requires its object lock to be taken
	 * if the pw_stream is connected. */
	GstPwAudioQueue *audio_buffer_queue;
	GCond audio_buffer_queue_cond;
	/* Calculated in start() out of the audio_buffer_queue_length property. */
	GstClockTime max_queue_fill_level;
	/* Set to 1 during the flush-start event, and back to 0 during the flush-stop one.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint flushing;
	/* Set to 1 during the PLAYING->PAUSED state change (not during READY->PAUSED!).
	 * Set back to 0 during PAUSED->PLAYING, and also during PAUSED->READY in case
	 * the pipeline is paused and gets shut down.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint paused;
	/* Set to 1 in on_process_stream() if the stream delay has changed.
	 * Read in render(). If it is set to 1, the code in render() will
	 * post a latency message to inform the pipeline about the stream
	 * delay (as latency). This is not done directly in on_process_stream()
	 * because that function must finish as quickly as possible, and
	 * posting a gstmessage could potentially block that function a bit
	 * too much in some cases.
	 * This is a gint, not a gboolean, since it is used by the GLib atomic functions. */
	gint notify_upstream_about_stream_delay;
	/* If this is FALSE, then on_process_stream() will get the current time of stream_clock
	 * and pass it to the gst_pw_audio_queue_retrieve_buffer() function to ensure that
	 * the output of the queue is in sync with the clock's current time. When the queue
	 * returns a buffer, this flag is set to TRUE. Followup gst_pw_audio_queue_retrieve_buffer()
	 * calls will then not try to synchronize the output, since it is assumed to already be
	 * in sync (since the audio stream is contiguous). This flag is set back to FALSE if some
	 * form of interruption in the stream happens.
	 * Access to this field requires the audio_buffer_queue object lock to be taken
	 * if the pw_stream is connected. */
	gboolean synced_playback_started;
	/* Used for checking the buffer PTS for discontinuities. */
	GstClockTime last_running_time_pts_end;

	/** Element clock **/

	/* Element clock based on the pw_stream. Always available, since it gets timestamps
	 * from the monotonic system clock and adjusts them according to the pw_stream
	 * feedback (see the io_changed() callback). Initially, the timestamps are
	 * unadjusted and equal those of the system clock. */
	GstPwStreamClock *stream_clock;

	/** PipeWire specifics **/

	GstPipewireCore *pipewire_core;
	struct pw_stream *stream;
	gboolean stream_is_connected;
	gboolean stream_is_active;
	struct spa_hook stream_listener;
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
	GstClockTime latency;
	guint64 quantum_size;
	/* The latency mutex synchronizes access to latency and stream_delay_in_ns. */
	GMutex latency_mutex;
};


struct _GstPwAudioSinkClass
{
	GstBaseSinkClass parent_class;
};


G_DEFINE_TYPE(GstPwAudioSink, gst_pw_audio_sink, GST_TYPE_BASE_SINK)


static void gst_pw_audio_sink_finalize(GObject *object);
static void gst_pw_audio_sink_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_pw_audio_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static GstStateChangeReturn gst_pw_audio_sink_change_state(GstElement *element, GstStateChange transition);
static GstClock* gst_pw_audio_sink_provide_clock(GstElement *element);
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

static GstFlowReturn gst_pw_audio_sink_render_contiguous(GstPwAudioSink *self, GstBuffer *original_incoming_buffer);
static GstFlowReturn gst_pw_audio_sink_render_packetized(GstPwAudioSink *self, GstBuffer *original_incoming_buffer);

static gboolean gst_pw_audio_sink_handle_convert_query(GstPwAudioSink *self, GstQuery *query);

static void gst_pw_audio_sink_set_provide_clock_flag(GstPwAudioSink *self, gboolean flag);
static gboolean gst_pw_audio_sink_get_provide_clock_flag(GstPwAudioSink *self);

static void gst_pw_audio_sink_activate_stream_unlocked(GstPwAudioSink *self, gboolean activate);
static void gst_pw_audio_sink_reset_audio_buffer_queue_unlocked(GstPwAudioSink *self);
static void gst_pw_audio_sink_drain(GstPwAudioSink *self);

static void gst_pw_audio_sink_pw_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error);
static void gst_pw_audio_sink_io_changed(void *data, uint32_t id, void *area, uint32_t size);
static void gst_pw_audio_sink_on_process_stream(void *data);
static void gst_pw_audio_sink_disconnect_stream(GstPwAudioSink *self);


static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = gst_pw_audio_sink_pw_state_changed,
	.io_changed = gst_pw_audio_sink_io_changed,
	.process = gst_pw_audio_sink_on_process_stream,
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

	object_class->finalize	   = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_finalize);
	object_class->set_property = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_set_property);
	object_class->get_property = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_get_property);

	element_class->change_state  = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_change_state);
	element_class->provide_clock = GST_DEBUG_FUNCPTR(gst_pw_audio_sink_provide_clock);
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
		PROP_AUDIO_BUFFER_QUEUE_LENGTH,
		g_param_spec_uint(
			"audio-buffer-queue-length",
			"Audio buffer queue length",
			"The length of the sink's audio buffer queue, in milliseconds (if filled to this capacity, sink will block until there's room in the queue)",
			1, G_MAXUINT,
			DEFAULT_AUDIO_BUFFER_QUEUE_LENGTH,
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
		PROP_NODE_NAME,
		g_param_spec_string(
			"node-description",
			"Node description",
			"One-line human readable description of this sink's client PipeWire node; example: \"Bluetooth headset\" (NULL = default)",
			DEFAULT_NODE_DESCRIPTION,
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
	self->target_object_id = DEFAULT_TARGET_OBJECT_ID;
	self->stream_properties = DEFAULT_STREAM_PROPERTIES;
	self->socket_fd = DEFAULT_SOCKET_FD;
	self->audio_buffer_queue_length = DEFAULT_AUDIO_BUFFER_QUEUE_LENGTH;
	self->app_name = g_strdup(DEFAULT_APP_NAME);
	self->node_name = g_strdup(DEFAULT_NODE_NAME);
	self->node_description = g_strdup(DEFAULT_NODE_DESCRIPTION);

	self->sink_caps = NULL;

	self->audio_buffer_queue = gst_pw_audio_queue_new();
	g_assert(self->audio_buffer_queue != NULL);
	g_cond_init(&(self->audio_buffer_queue_cond));
	self->max_queue_fill_level = 0;
	self->flushing = 0;
	self->paused = 0;
	self->notify_upstream_about_stream_delay = 0;
	self->synced_playback_started = FALSE;
	self->last_running_time_pts_end = GST_CLOCK_TIME_NONE;

	self->stream_clock = gst_pw_stream_clock_new();
	g_assert(self->stream_clock != NULL);

	self->pipewire_core = gst_pipewire_core_new();
	self->stream = NULL;
	self->stream_is_connected = FALSE;
	self->spa_position = NULL;
	self->spa_rate_match = NULL;
	self->stream_delay_in_ticks = 0;
	self->stream_delay_in_ns = 0;
	self->latency = 0;
	self->quantum_size = 0;
	g_mutex_init(&(self->latency_mutex));

	gst_pw_audio_sink_set_provide_clock_flag(self, DEFAULT_PROVIDE_CLOCK);
}


static void gst_pw_audio_sink_finalize(GObject *object)
{
	GstPwAudioSink *self = GST_PW_AUDIO_SINK(object);

	if (self->stream_clock != NULL)
		gst_object_unref(GST_OBJECT(self->stream_clock));

	if (self->pipewire_core != NULL)
		gst_object_unref(GST_OBJECT(self->pipewire_core));

	g_cond_clear(&(self->audio_buffer_queue_cond));

	if (self->audio_buffer_queue != NULL)
		gst_object_unref(self->audio_buffer_queue);

	g_free(self->app_name);
	g_free(self->node_name);
	g_free(self->node_description);

	g_mutex_clear(&(self->latency_mutex));

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

		case PROP_AUDIO_BUFFER_QUEUE_LENGTH:
			GST_OBJECT_LOCK(self);
			self->audio_buffer_queue_length = g_value_get_uint(value);
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

		case PROP_AUDIO_BUFFER_QUEUE_LENGTH:
			GST_OBJECT_LOCK(self);
			g_value_set_uint(value, self->audio_buffer_queue_length);
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
			gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			g_atomic_int_set(&(self->paused), 1);
			g_cond_signal(&(self->audio_buffer_queue_cond));

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
			break;

		default:
			break;
	}

	if ((result = GST_ELEMENT_CLASS(gst_pw_audio_sink_parent_class)->change_state(element, transition)) == GST_STATE_CHANGE_FAILURE)
		return result;

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
			GstClockTime stream_delay_in_ns, max_queue_fill_level;

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
				/* The extra latencies from this sink are the pw_stream delay
				 * and the maximum amount of data. The pw_stream delay is
				 * received in on_process_stream(), which will cause render()
				 * to post a LATENCY message, and that will re-query all elements
				 * (including this sink itself) for their current latency.
				 * The max_queue_fill_level defines how much time will pass
				 * from the moment data enters a full queue until it exits
				 * said queue. */

				/* max_queue_fill_level is set by the start() function, and it
				 * seems to be possible that a query is issued while start() runs,
				 * so synchronize access. */
				GST_OBJECT_LOCK(self);
				max_queue_fill_level = self->max_queue_fill_level;
				GST_OBJECT_UNLOCK(self);

				/* Synchronize access since the stream delay is set by
				 * the on_process_stream() function. */
				LOCK_LATENCY_MUTEX(self);
				stream_delay_in_ns = self->stream_delay_in_ns;
				UNLOCK_LATENCY_MUTEX(self);

				min_latency += stream_delay_in_ns + max_queue_fill_level;
				if (GST_CLOCK_TIME_IS_VALID(max_latency))
					max_latency += stream_delay_in_ns + max_queue_fill_level;

				GST_DEBUG_OBJECT(
					element,
					"PW stream delay: %" GST_TIME_FORMAT "  max queue fill level: %" GST_TIME_FORMAT
					"  => adjusted min/max latency: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT,
					GST_TIME_ARGS(stream_delay_in_ns),
					GST_TIME_ARGS(max_queue_fill_level),
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


static GstCaps* gst_pw_audio_sink_fixate(GstBaseSink *basesink, GstCaps *caps)
{
	caps = gst_pw_audio_format_fixate_caps(caps);
	return GST_BASE_SINK_CLASS(gst_pw_audio_sink_parent_class)->fixate(basesink, caps);
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
	gboolean must_reactivate_stream;
	guint8 builder_buffer[1024];

	GST_DEBUG_OBJECT(self, "got new sink caps %" GST_PTR_FORMAT, (gpointer)caps);

	/* set_caps() may be entered because playback just started and these are
	 * the caps of the initial data. Or, it may be entered because new data
	 * came in with different caps after previous data was played. In the
	 * former case, we don't activate right away, since that is taken care
	 * of in the PAUSED->PLAYING state change. In the latter case, we do,
	 * because this data with new caps comes in without any state change
	 * happening. We _have_ to first deactivate any ongoing active stream
	 * before we can set a new format, which is why this is relevant.
	 * Remember here whether or not the stream was already activated before
	 * so the code knows if reactivating is necessary. We take the PW thread
	 * loop lock since accessing self->stream_is_active requires this. */
	pw_thread_loop_lock(self->pipewire_core->loop);
	must_reactivate_stream = self->stream_is_active;
	pw_thread_loop_unlock(self->pipewire_core->loop);

	if (must_reactivate_stream)
		GST_DEBUG_OBJECT(self, "stream was already active; will immediately reactivate after stream was reconnected");
	else
		GST_DEBUG_OBJECT(self, "stream hasn't been activated already; will not activate right after stream was connected");

	/* Wait until any remaining audio data that uses the old caps is played.
	 * Then we can safely disconnect the stream and don't lose any audio data. */
	gst_pw_audio_sink_drain(self);
	gst_pw_audio_sink_disconnect_stream(self);

	/* Get rid of the old caps here. That way, should an error occur below,
	 * we won't be left with the old, obsolete caps. */
	gst_caps_replace(&(self->sink_caps), NULL);
	/* Queue should be clear by now, but to be sure, call this. */
	gst_pw_audio_queue_flush(self->audio_buffer_queue);

	/* Get a PW audio format out of the caps and initialize the POD
	 * that is then passed to pw_stream_connect() to specify the
	 * audio format params to the new PW stream. */
	if (!gst_pw_audio_format_from_caps(&(self->pw_audio_format), GST_ELEMENT_CAST(self), caps))
		goto error;
	if (!gst_pw_audio_format_to_spa_pod(&(self->pw_audio_format), GST_ELEMENT_CAST(self), builder_buffer, sizeof(builder_buffer), params))
		goto error;

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

	gst_pw_audio_queue_set_format(self->audio_buffer_queue, &(self->pw_audio_format));

	if (must_reactivate_stream)
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
	GstCaps *available_sinkcaps;

	GST_DEBUG_OBJECT(self, "new get-caps query");

	// TODO: Replace this placeholder template caps call with actual format enumeration.
	available_sinkcaps = gst_pw_audio_format_get_template_caps();

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
		GST_DEBUG_OBJECT(self, "  filtered available sink caps:   %" GST_PTR_FORMAT, (gpointer)available_sinkcaps);
		GST_DEBUG_OBJECT(self, "  filter:                         %" GST_PTR_FORMAT, (gpointer)filter);
		gst_caps_unref(unfiltered_available_sinkcaps);
	}
	else
		GST_DEBUG_OBJECT(self, "responding to query caps (query has no filter caps):  available sink caps: %" GST_PTR_FORMAT, (gpointer)available_sinkcaps);

	return available_sinkcaps;
}


static void gst_pw_audio_sink_get_times(G_GNUC_UNUSED GstBaseSink *basesink, G_GNUC_UNUSED GstBuffer *buffer, GstClockTime *start, GstClockTime *end)
{
	/* This sink handles the clock synchronization by itself. Setting
	 * *start and *end to GST_CLOCK_TIME_NONE informs the base class
	 * that it must not handle the synchronization on its own. */

	*start = GST_CLOCK_TIME_NONE;
	*end = GST_CLOCK_TIME_NONE;
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
	/* In here, the PipeWire core is started, and the stream is created.
	 * The latter is not yet connected and activated though, since for
	 * that, we need the sink caps. These are passed to a later set_caps()
	 * call, which is where the stream is connected and activated. */

	GstPwAudioSink *self = GST_PW_AUDIO_SINK(basesink);
	gboolean retval = TRUE;
	int socket_fd;
	struct pw_properties *pw_props;
	gchar *stream_media_name = NULL;

	/* Get GObject property values. */
	GST_OBJECT_LOCK(self);
	socket_fd = self->socket_fd;
	self->max_queue_fill_level = self->audio_buffer_queue_length * GST_MSECOND;
	GST_OBJECT_UNLOCK(self);

	GST_DEBUG_OBJECT(self, "starting PipeWire core");
	if (!gst_pipewire_core_start(self->pipewire_core, socket_fd))
	{
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE, ("Could not start PipeWire core"), (NULL));
		goto error;
	}

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
		GST_DEBUG_OBJECT(self, "extra propertie for the new PipeWire stream: %" GST_PTR_FORMAT, (gpointer)(self->stream_properties));
	}

	/* Reuse the node name as the stream name. We copy the string here
	 * to prevent potential race conditions if the user assigns a new
	 * name string to the node-name property while this code runs. */
	stream_media_name = g_strdup(self->node_name);

	GST_OBJECT_UNLOCK(self);

	self->stream = pw_stream_new(self->pipewire_core->core, stream_media_name, pw_props);
	if (G_UNLIKELY(self->stream == NULL))
	{
		GST_ERROR_OBJECT(self, "could not create PipeWire stream");
		goto error;
	}

	pw_stream_add_listener(self->stream, &(self->stream_listener), &stream_events, self);

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
		pw_stream_destroy(self->stream);
		self->stream = NULL;
	}

	self->spa_position = NULL;
	self->spa_rate_match = NULL;
	GST_DEBUG_OBJECT(self, "stopping PipeWire core");
	gst_pipewire_core_stop(self->pipewire_core);
	gst_pw_stream_clock_reset_states(self->stream_clock);

	gst_caps_replace(&(self->sink_caps), NULL);

	gst_pw_audio_sink_reset_audio_buffer_queue_unlocked(self);

	self->max_queue_fill_level = 0;
	self->flushing = 0;
	self->stream_delay_in_ticks = 0;
	self->stream_delay_in_ns = 0;
	self->latency = 0;
	self->notify_upstream_about_stream_delay = 0;
	self->quantum_size = 0;

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
			GST_DEBUG_OBJECT(self, "flushing started; setting flushing flag and resetting audio buffer queue");
			g_atomic_int_set(&(self->flushing), 1);
			g_cond_signal(&(self->audio_buffer_queue_cond));

			/* Deactivate the stream since we won't be producing data during flush. */
			pw_thread_loop_lock(self->pipewire_core->loop);
			gst_pw_audio_sink_activate_stream_unlocked(self, FALSE);
			pw_thread_loop_unlock(self->pipewire_core->loop);

			/* Get rid of all queued data during flush. */
			LOCK_AUDIO_BUFFER_QUEUE(self);
			gst_pw_audio_sink_reset_audio_buffer_queue_unlocked(self);
			UNLOCK_AUDIO_BUFFER_QUEUE(self);

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
			 * compensated for in render() by inserting nullsamples. We just
			 * log gaps here and do nothing further. */

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
			 * event is produced. Drain any queued data, then deactivate
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

			GST_DEBUG_OBJECT(self, "EOS received; draining queue and deactivating stream");

			gst_pw_audio_sink_drain(self);

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

	if (gst_pw_audio_format_data_is_contiguous(self->pw_audio_format.audio_type))
		return gst_pw_audio_sink_render_contiguous(self, incoming_buffer);
	else
		return gst_pw_audio_sink_render_packetized(self, incoming_buffer);
}


static GstFlowReturn gst_pw_audio_sink_render_contiguous(GstPwAudioSink *self, GstBuffer *original_incoming_buffer)
{
	GstFlowReturn flow_ret = GST_FLOW_OK;
	GstBaseSink *basesink = GST_BASE_SINK_CAST(self);
	GstBuffer *incoming_buffer_copy = NULL;
	gboolean sync_enabled;
	gboolean buffer_can_be_played_in_sync = FALSE;
	gboolean force_discontinuity_handling = FALSE;
	gsize num_silence_frames_to_insert = 0;
	GstClockTime buffer_duration;
	GstClockTime current_fill_level;
	gsize stride;

	stride = gst_pw_audio_format_get_stride(&(self->pw_audio_format));

	if (GST_BUFFER_DURATION_IS_VALID(original_incoming_buffer))
	{
		GST_LOG_OBJECT(self, "new incoming buffer: %" GST_PTR_FORMAT, (gpointer)original_incoming_buffer);
		buffer_duration = GST_BUFFER_DURATION(original_incoming_buffer);
	}
	else
	{
		buffer_duration = gst_pw_audio_format_calculate_duration_from_num_frames(
			&(self->pw_audio_format),
			gst_buffer_get_size(original_incoming_buffer) / stride
		);

		GST_LOG_OBJECT(
			self,
			"new incoming buffer: %" GST_PTR_FORMAT "; no valid duration set, estimated duration %" GST_TIME_FORMAT " based on its data",
			(gpointer)original_incoming_buffer,
			GST_TIME_ARGS(buffer_duration)
		);
	}

	if (G_UNLIKELY(GST_BUFFER_FLAG_IS_SET(original_incoming_buffer, GST_BUFFER_FLAG_DISCONT) || GST_BUFFER_FLAG_IS_SET(original_incoming_buffer, GST_BUFFER_FLAG_RESYNC)))
	{
		GST_DEBUG_OBJECT(self, "discont and/or resync flag set; forcing discontinuity handling");
		force_discontinuity_handling = TRUE;
	}

	sync_enabled = gst_base_sink_get_sync(basesink);

	/* If sync property is set to TRUE, and the incoming data is in a TIME
	 * segment & contains timestamped buffers, create a sub-buffer out of
	 * original_incoming_buffer and store that sub-buffer as the
	 * incoming_buffer_copy. This is done that way because sub-buffers
	 * can be set to cover only a portion of the original buffer's memory.
	 * That is how buffers are clipped (if they need to be clipped).
	 * The sub-buffer is given original_incoming_buffer's clipped timestamp
	 * (or the original timestamp if no clipping occurred), as well as its
	 * duration (also adjusted for clipping if necessary).
	 *
	 * Note that the incoming_buffer_copy's timestamp is translated to
	 * clock-time. This is done to allow the code in on_process_stream to
	 * immediately compare the buffer's timestamp against the stream_clock
	 * without first having to do the translation. That function must finish
	 * its execution ASAP, so offloading computation from it helps. */
	if (sync_enabled)
	{
		GstSegment *segment = &(basesink->segment);

		if ((segment->format == GST_FORMAT_TIME) && G_LIKELY(GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(original_incoming_buffer))))
		{
			GstClockTimeDiff ts_offset;
			GstClockTime render_delay;
			GstClockTime pts_begin, pts_end;
			GstClockTime clipped_pts_begin, clipped_pts_end;
			GstClockTimeDiff sync_offset;
			GstSegment pts_clipping_segment;

			pts_begin = GST_BUFFER_PTS(original_incoming_buffer);
			pts_end = GST_BUFFER_PTS(original_incoming_buffer) + GST_BUFFER_DURATION(original_incoming_buffer);

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
				GstClockTime running_time_pts_begin, running_time_pts_end;
				gsize clipped_begin_frames = 0, clipped_end_frames = 0;
				gsize original_num_frames;
				GstClockTime begin_clip_duration, end_clip_duration;
				GstClockTime pw_base_time;

				running_time_pts_begin = gst_segment_to_running_time(segment, GST_FORMAT_TIME, clipped_pts_begin);
				running_time_pts_end = gst_segment_to_running_time(segment, GST_FORMAT_TIME, clipped_pts_end);

				pw_base_time = GST_ELEMENT_CAST(self)->base_time;

				if (GST_CLOCK_TIME_IS_VALID(self->last_running_time_pts_end))
				{
					GstClockTimeDiff discontinuity = GST_CLOCK_DIFF(self->last_running_time_pts_end, running_time_pts_begin);

					// TODO: Accumulate discontinuity, and only perform compensation
					// if the accumulated discontinuity is nonzero after a while.
					// This filters out cases of alternating discontinuities, like
					// +1ms now -1ms next +1ms next -1ms next etc.

					if (G_UNLIKELY(ABS(discontinuity) > self->alignment_threshold) || ((discontinuity != 0) && force_discontinuity_handling))
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
							running_time_pts_begin += discontinuity;
							running_time_pts_end += discontinuity;
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
							 * the previous data and to account for the clipped amount. running_time_pts_end
							 * is not modified, however, since the overall duration of the data to play is
							 * reduced by (-discontinuity) nanoseconds by the clipping. */
							running_time_pts_begin += (-discontinuity);
							clipped_pts_begin += (-discontinuity);
							GST_DEBUG_OBJECT(
								self,
								"discontinuity detected (%" GST_TIME_FORMAT "); need to clip this (positive) amount of nanoseconds from the beginning of the gstbuffer",
								GST_TIME_ARGS(discontinuity)
							);
						}
					}
				}

				g_assert(GST_CLOCK_TIME_IS_VALID(running_time_pts_begin));
				g_assert(GST_CLOCK_TIME_IS_VALID(running_time_pts_end));

				begin_clip_duration = clipped_pts_begin - pts_begin;
				end_clip_duration = pts_end - clipped_pts_end;

				clipped_begin_frames = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), begin_clip_duration);
				clipped_end_frames = gst_pw_audio_format_calculate_num_frames_from_duration(&(self->pw_audio_format), end_clip_duration);

				original_num_frames = gst_buffer_get_size(original_incoming_buffer) / stride;

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
					clipped_begin_frames * stride,
					gst_buffer_get_size(original_incoming_buffer) - (clipped_begin_frames + clipped_end_frames) * stride
				);

				/* Set the incoming_buffer_copy's timestamp, translated to clock-time
				 * by adding pw_base_time to running_time_pts_begin. */
				GST_BUFFER_PTS(incoming_buffer_copy) = pw_base_time + running_time_pts_begin;
				GST_BUFFER_DURATION(incoming_buffer_copy) = clipped_pts_end - clipped_pts_begin;

				GST_LOG_OBJECT(
					self,
					"running time begin/end PTS: %" GST_TIME_FORMAT "/%" GST_TIME_FORMAT,
					GST_TIME_ARGS(running_time_pts_begin), GST_TIME_ARGS(running_time_pts_end)
				);

				GST_LOG_OBJECT(
					self,
					"base-time: %" GST_TIME_FORMAT "  clock-time clipped buffer PTS: %" GST_TIME_FORMAT "  clipped buffer duration: %" GST_TIME_FORMAT,
					GST_TIME_ARGS(pw_base_time),
					GST_TIME_ARGS(GST_BUFFER_PTS(incoming_buffer_copy)),
					GST_TIME_ARGS(GST_BUFFER_DURATION(incoming_buffer_copy))
				);

				buffer_can_be_played_in_sync = TRUE;
			}
			else
				GST_LOG_OBJECT(self, "clipped begin/end PTS invalid after clipping; not adjusting buffer timestamp and duration, not playing in sync");
		}
	}
	else
		GST_LOG_OBJECT(self, "synced playback disabled; not adjusting buffer timestamp and duration");

	if (!buffer_can_be_played_in_sync)
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
	}

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

		LOCK_AUDIO_BUFFER_QUEUE(self);

		current_fill_level = gst_pw_audio_queue_get_fill_level(self->audio_buffer_queue);

		if (current_fill_level >= self->max_queue_fill_level)
		{
			GST_LOG_OBJECT(
				self,
				"audio buffer queue is full (cur/max fill level: %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "); waiting until there is room for new data",
				current_fill_level, self->max_queue_fill_level
			);
			g_cond_wait(&(self->audio_buffer_queue_cond), GET_AUDIO_BUFFER_QUEUE_MUTEX(self));

			UNLOCK_AUDIO_BUFFER_QUEUE(self);
		}
		else
		{
			GST_LOG_OBJECT(
				self,
				"audio buffer queue has room for more data (cur/max fill level: %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "); can add incoming buffer",
				current_fill_level, self->max_queue_fill_level
			);

			if (G_UNLIKELY(num_silence_frames_to_insert > 0))
				gst_pw_audio_queue_push_silence_frames(self->audio_buffer_queue, num_silence_frames_to_insert);

			/* Pass the buffer to the queue, which takes ownership over the buffer. */
			gst_pw_audio_queue_push_buffer(self->audio_buffer_queue, incoming_buffer_copy);
			incoming_buffer_copy = NULL;

			UNLOCK_AUDIO_BUFFER_QUEUE(self);

			break;
		}
	}

finish:
	if (incoming_buffer_copy != NULL)
		gst_buffer_unref(incoming_buffer_copy);
	return flow_ret;
}


static GstFlowReturn gst_pw_audio_sink_render_packetized(GstPwAudioSink *self, GstBuffer *original_incoming_buffer)
{
	// TODO
	return GST_FLOW_OK;
}


static gboolean gst_pw_audio_sink_handle_convert_query(GstPwAudioSink *self, GstQuery *query)
{
	GstFormat source_gstformat, dest_gstformat;
	gint64 source_quantity, dest_quantity;

	gst_query_parse_convert(query, &source_gstformat, &source_quantity, &dest_gstformat, NULL);

	GST_DEBUG_OBJECT(
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

	GST_DEBUG_OBJECT(self, "conversion result: %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT, source_quantity, dest_quantity);

	gst_query_set_convert(query, source_gstformat, source_quantity, dest_gstformat, dest_quantity);

	return TRUE;

cannot_convert:
	return FALSE;
}


static void gst_pw_audio_sink_set_provide_clock_flag(GstPwAudioSink *self, gboolean flag)
{
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

	pw_stream_set_active(self->stream, activate);
	GST_DEBUG_OBJECT(self, "%s PipeWire stream", activate ? "activating" : "deactivating");

	self->stream_is_active = activate;
}


static void gst_pw_audio_sink_reset_audio_buffer_queue_unlocked(GstPwAudioSink *self)
{
	/* This must be called with the audio_buffer_queue object lock taken. */

	gst_pw_audio_queue_flush(self->audio_buffer_queue);

	/* Also reset these states, since a queue reset effectively ends
	 * any synchronized playback of the stream that was going on earlier,
	 * and there's no more old data to check for alignment with new data. */
	self->synced_playback_started = FALSE;
	self->last_running_time_pts_end = GST_CLOCK_TIME_NONE;
}


static void gst_pw_audio_sink_drain(GstPwAudioSink *self)
{
	LOCK_AUDIO_BUFFER_QUEUE(self);

	while (TRUE)
	{
		GstClockTime current_fill_level;

		if (g_atomic_int_get(&(self->flushing)))
		{
			GST_DEBUG_OBJECT(self, "aborting drain since we are flushing");
			return;
		}

		current_fill_level = gst_pw_audio_queue_get_fill_level(self->audio_buffer_queue);

		if (current_fill_level == 0)
		{
			GST_DEBUG_OBJECT(self, "queue is fully drained");
			break;
		}
		else
		{
			GST_DEBUG_OBJECT(self, "queue still contains data; current queue fill level: %" GST_TIME_FORMAT, GST_TIME_ARGS(current_fill_level));
			g_cond_wait(&(self->audio_buffer_queue_cond), GET_AUDIO_BUFFER_QUEUE_MUTEX(self));
		}
	}

	UNLOCK_AUDIO_BUFFER_QUEUE(self);
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
				GST_DEBUG_OBJECT(
					self,
					"got new SPA IO position:  offset: %" G_GINT64_FORMAT "  state: %s  num segments: %" G_GUINT32_FORMAT,
					(gint64)(self->spa_position->offset),
					spa_io_position_state_to_string((enum spa_io_position_state)(self->spa_position->state)),
					(guint32)(self->spa_position->n_segments)
				);
				GST_DEBUG_OBJECT(
					self,
					"SPA IO position  clock duration (= quantum size): %" G_GUINT64_FORMAT "  rate: %" G_GUINT32_FORMAT "/%" G_GUINT32_FORMAT,
					self->spa_position->clock.duration,
					self->spa_position->clock.rate.num, self->spa_position->clock.rate.denom
				);

				self->quantum_size = self->spa_position->clock.duration;
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
			 * is called, it does, so it is usable there. */

			self->spa_rate_match = (struct spa_io_rate_match *)area;

			break;
		}

		default:
			break;
	}
}


static void gst_pw_audio_sink_on_process_stream(void *data)
{
	/* In here, we pass data to the pw_stream and report the rate_diff
	 * to stream_clock. Not much else is done, since this function must
	 * finish ASAP; it is running in the thread that processes data in
	 * the PipeWire graph, and must not block said thread for long. */

	GstPwAudioSink *self = GST_PW_AUDIO_SINK_CAST(data);
	struct pw_time stream_time;
	struct pw_buffer *pw_buf;
	struct spa_data *inner_spa_data;
	GstClockTime latency;
	gint64 stream_delay_in_ns;
	gboolean produce_silence_quantum = TRUE;
	gsize stride;

	GST_LOG_OBJECT(self, COLOR_GREEN "new PipeWire graph tick" COLOR_DEFAULT);

	if ((self->spa_position != NULL) && (self->spa_position->clock.rate_diff > 0))
		gst_pw_stream_clock_set_rate_diff(self->stream_clock, self->spa_position->clock.rate_diff);

	pw_stream_get_time(self->stream, &stream_time);

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

	latency = self->latency;

	UNLOCK_LATENCY_MUTEX(self);

	/* Subtract the stream_delay_in_ns value from the latency value.
	 * The data we put into the SPA data chunks here now will be placed
	 * in stream_delay_in_ns nanoseconds, so it is already implicitly
	 * factored into our output. stream_delay_in_ns is added to the
	 * GStreamer base sink latency for GstBaseSink's own purposes and
	 * for correctly responding to latency queries. It is not meant
	 * for the gst_pw_audio_queue_retrieve_buffer() calls here. */
	if (G_LIKELY((GstClockTimeDiff)latency >= stream_delay_in_ns))
		latency -= stream_delay_in_ns;
	else
		latency = 0;

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

	/* We are about to retrieve data from the audio buffer queue and
	 * also are about to access synced_playback_started, so synchronize
	 * access by locking the audio buffer's gstobject mutex. It is unlocked
	 * immediately once it is no longer needed to minimize chances of
	 * thread starvation (in the render() function) and similar. */
	LOCK_AUDIO_BUFFER_QUEUE(self);

	stride = gst_pw_audio_format_get_stride(&(self->pw_audio_format));

	if (G_UNLIKELY(gst_pw_audio_queue_get_fill_level(self->audio_buffer_queue) == 0))
	{
		GST_DEBUG_OBJECT(self, "audio buffer queue empty/underrun; producing silence quantum");
		/* In case of an underrun we have to re-sync the output. */
		self->synced_playback_started = FALSE;
		UNLOCK_AUDIO_BUFFER_QUEUE(self);
	}
	else
	{
		switch (self->pw_audio_format.audio_type)
		{
			case GST_PIPEWIRE_AUDIO_TYPE_PCM:
			{
				GstMapInfo map_info;
				GstPwAudioQueueRetrievalDetails retrieval_details;
				GstPwAudioQueueRetrievalResult retrieval_result;
				GstClockTime num_frames_to_take;
				GstClockTime current_time = GST_CLOCK_TIME_NONE;
				gsize buffer_size;
				gsize spa_data_chunk_byte_offset = 0;

				num_frames_to_take = self->spa_rate_match->size;

				if (!self->synced_playback_started)
				{
					current_time = gst_clock_get_time(GST_ELEMENT_CLOCK(self));
					GST_LOG_OBJECT(
						self,
						"current time: %" GST_TIME_FORMAT "  "
						"latency: %" GST_TIME_FORMAT,
						GST_TIME_ARGS(current_time),
						GST_TIME_ARGS(latency)
					);
				}

				retrieval_result = gst_pw_audio_queue_retrieve_buffer(self->audio_buffer_queue, num_frames_to_take, num_frames_to_take, current_time, latency, &retrieval_details);
				if (G_UNLIKELY(retrieval_details.retrieved_buffer == NULL))
				{
					switch (retrieval_result)
					{
						case GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_QUEUE_IS_EMPTY:
						case GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_PAST:
							/* Either there is no data at all or all queued data was expired and had
							 * to be thrown away. In all of these cases we need to resynchronize. */
							self->synced_playback_started = FALSE;
							break;

						default:
							/* This point is reached if the lack of a buffer is OK, typically
							 * because all of the queued data lies in the future at the moment. */
							break;
					}

					UNLOCK_AUDIO_BUFFER_QUEUE(self);

					GST_DEBUG_OBJECT(self, "audio buffer queue could not return data; producing silence quantum");
					break;
				}

				self->synced_playback_started = TRUE;

				UNLOCK_AUDIO_BUFFER_QUEUE(self);

				/* If this point is reached the result must be one of these two possible ones.
				 * Otherwise this indicates that at least one result type is not handled properly. */
				g_assert((retrieval_result == GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_OK) || (retrieval_result == GST_PW_AUDIO_QUEUE_RETRIEVAL_RESULT_DATA_FULLY_IN_THE_FUTURE));

				if (retrieval_details.num_silence_frames_to_prepend > 0)
				{
					gst_pw_audio_format_write_silence_frames(&(self->pw_audio_format), inner_spa_data->data, retrieval_details.num_silence_frames_to_prepend);
					spa_data_chunk_byte_offset += retrieval_details.num_silence_frames_to_prepend * stride;
					GST_LOG_OBJECT(self, "will write PCM gstbuffer into SPA data chunk at byte offset %" G_GSIZE_FORMAT, spa_data_chunk_byte_offset);
				}

				GST_LOG_OBJECT(self, "got PCM gstbuffer from audio buffer queue (PTS shifted by element latency): %" GST_PTR_FORMAT, (gpointer)(retrieval_details.retrieved_buffer));

				gst_buffer_map(retrieval_details.retrieved_buffer, &map_info, GST_MAP_READ);

				buffer_size = map_info.size;
				g_assert((buffer_size + spa_data_chunk_byte_offset) <= (gsize)(inner_spa_data->maxsize));

				inner_spa_data->chunk->offset = 0;
				inner_spa_data->chunk->size = buffer_size + spa_data_chunk_byte_offset;
				inner_spa_data->chunk->stride = stride;

				memcpy((guint8 *)(inner_spa_data->data) + spa_data_chunk_byte_offset, map_info.data, buffer_size);

				gst_buffer_unmap(retrieval_details.retrieved_buffer, &map_info);

				gst_buffer_unref(retrieval_details.retrieved_buffer);

				produce_silence_quantum = FALSE;

				break;
			}

			default:
				break;
		}

		g_cond_signal(&(self->audio_buffer_queue_cond));
	}

	if (produce_silence_quantum)
	{
		guint64 num_silence_frames = self->spa_rate_match->size;
		guint64 num_silence_bytes = num_silence_frames * stride;

		g_assert(num_silence_frames <= (inner_spa_data->maxsize / stride));

		GST_LOG_OBJECT(self, "producing %" G_GUINT64_FORMAT " frame(s) of silence for silent quantum", num_silence_frames);

		inner_spa_data->chunk->offset = 0;
		inner_spa_data->chunk->size = num_silence_bytes;
		inner_spa_data->chunk->stride = stride;

		memset(inner_spa_data->data, 0, num_silence_bytes);

		g_cond_signal(&(self->audio_buffer_queue_cond));
	}

finish:
	pw_stream_queue_buffer(self->stream, pw_buf);
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

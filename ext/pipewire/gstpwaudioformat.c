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
 * SECTION:gstpwaudioformat
 * @title: GstPwAudioFormat
 * @short_description: Audio format details for GStreamer PipeWire audio elements.
 * @see_also: #GstAudioInfo, #GstAudioFormat
 *
 * Common code for use in GStreamer PipeWire audio elements. PipeWire supports
 * more audio types than just PCM, so #GstAudioInfo is insufficient, hence this
 * code. It introduces #GstPwAudioFormat and #GstPipewireAudioType. The "audio type"
 * introduces a high level distinction between major different types of audio data,
 * such as PCM, DSD, MPEG audio, AC-3 etc. For each type, format details can be
 * specified. In the PCM case, the #GstAudioInfo is used.
 *
 * In addition, several functions are provided that perform basic tasks such as
 * getting the stride of a format, converting #GstCaps to #GstPwAudioFormat,
 * setting up an SPA POD, etc. through a unified interface. Internally, audio
 * type specific code paths are used to perform these tasks. This helps declutter
 * the user's code by removing the need for switch-case blocks based on audio type.
 *
 * The notion of a "frame" also depends on the audio type. With PCM, one frame
 * is the collection of N samples that correspond to the same moment in time and
 * convey audio data to each channel, with N being the number of channels. For
 * example, for stereo sound, one frame consists of two samples, one for the
 * left channel, one for the right channel. Compressed formats use the term
 * "frame" differently, one "frame" describing one logical unit of compressed
 * information, typically containing roughly about 20-200 ms worth of audio data.
 *
 * Some audio types like PCM allow for arbitrary re-partitioning of audio  data.
 * For example, a PCM data buffer with 40ms of data and another buffer with 20ms
 * of data can be combined to one buffer with 60ms of data, or their contents can
 * be redistributed into two 30ms PCM data buffers etc. Data of such an audio type
 * is called "contiguous".
 * Other audio types aren't like that. For example, MPEG frames cannot be arbitarily
 * subdivided/re-partitioned. With such types, it is generally assumed 1 buffer is one
 * logical, indivisible packet. Data of such an audio type is called "packetized".
 */

#include "gstpwaudioformat.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/pod.h>

#pragma GCC diagnostic pop


GST_DEBUG_CATEGORY_EXTERN(pw_audio_format_debug);
#define GST_CAT_DEFAULT pw_audio_format_debug


typedef struct
{
	/* Caps in string form, intended to be used when adding
	 * GstPad templates to an element. Not directly used by callers;
	 * these use gst_pw_audio_format_get_template_caps() instead. */
	gchar const *template_caps_string;

	gboolean is_contiguous;
}
GstPipewireAudioTypeDetails;


// TODO: Currently, only PCM support is implemented. Fill in DSD
// support as well as compressed audio support (AC-3 etc).
// Note that DSD data can be subdivided, and contiguous buffers
// can be used with it, but otherwise, DSD cannot be (easily)
// processed, unlike PCM. The compressed formats cannot even
// be subdivided.


static GstPipewireAudioTypeDetails const audio_type_details[GST_NUM_PIPEWIRE_AUDIO_TYPES] = {
	/* PCM */
	{
		.template_caps_string = \
			GST_AUDIO_CAPS_MAKE(GST_AUDIO_FORMATS_ALL) \
			", layout = (string) { interleaved }",
		.is_contiguous = TRUE
	},

	/* DSD */
	{
		.template_caps_string = "audio/x-dsd", // TODO: more detailed caps
		.is_contiguous = TRUE
	},

	/* MPEG */
	{
		.template_caps_string = \
			"audio/mpeg, " \
			"parsed = (boolean) true, " \
			"mpegversion = (int) 1, " \
			"mpegaudioversion = (int) [ 1, 3 ]",
		.is_contiguous = FALSE
	},

	/* AC3 */
	{
		.template_caps_string = \
			"audio/x-ac3, " \
			"framed = (boolean) true, " \
			"alignment = (string) frame, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_contiguous = FALSE
	},

	/* EAC3 */
	{
		.template_caps_string = \
			"audio/x-eac3, " \
			"framed = (boolean) true, " \
			"alignment = (string) frame, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_contiguous = FALSE
	},

	/* TrueHD */
	{
		.template_caps_string = "audio/x-true-hd",
		.is_contiguous = FALSE
	},

	/* DTS */
	{
		.template_caps_string = "audio/x-dts",
		.is_contiguous = FALSE
	},

	/* DTS-HD */
	{
		.template_caps_string = "audio/x-dts",
		.is_contiguous = FALSE
	},

	/* MHAS */
	{
		.template_caps_string = "audio/x-mhas",
		.is_contiguous = FALSE
	}
};


/**
 * gst_pw_audio_format_get_template_caps:
 *
 * Returns #GstCaps suitable for pad templates. These caps contain all
 * formats and audio types supported by the GStreamer PipeWire audio plugin.
 *
 * Returns: (transfer full): the #GstCaps.
 */
GstCaps* gst_pw_audio_format_get_template_caps(void)
{
	GstCaps *aggregated_template_caps = NULL;
	gint i;

	for (i = 0; i < GST_NUM_PIPEWIRE_AUDIO_TYPES; ++i)
	{
		GstCaps *template_caps = gst_caps_from_string(audio_type_details[i].template_caps_string);

		if (i == 0)
			aggregated_template_caps = template_caps;
		else
			gst_caps_append(aggregated_template_caps, template_caps);
	}

	return aggregated_template_caps;
}


/**
 * gst_pw_audio_format_fixate_caps:
 * @caps: (transfer full): #GstCaps to fixate.
 *
 * This is a PipeWire audio specific variant of gst_caps_fixate().
 * The caps fixation is performed in an audio type specific way if appropriate.
 * For example, PCM caps are fixated to 44100 Hz stereo S16LE.
 * Otherwise, this behaves just like gst_caps_fixate(). And
 * like that function, this function does not accept ANY caps.
 *
 * Returns: (transfer full): the fixated #GstCaps.
 */
GstCaps* gst_pw_audio_format_fixate_caps(GstCaps *caps)
{
	GstStructure *s;

	g_assert(caps != NULL);
	g_assert(!gst_caps_is_any(caps));

	/* Passthrough empty caps, since we cannot do anything with them. */
	if (G_UNLIKELY(gst_caps_is_empty(caps)))
		return caps;

	/* The caps must be writable, otherwise its fields cannot be fixated. */
	caps = gst_caps_make_writable(caps);

	s = gst_caps_get_structure(caps, 0);
	g_assert(s != NULL);

	/* If this is PCM audio, we want to fixate un-fixated fields
	 * to 44100 Hz S16 stereo, native endianness. This is also
	 * known as CD audio, and is a very common PCM configuration. */
	if (g_strcmp0(gst_structure_get_name(s), "audio/x-raw") == 0)
	{
		gst_structure_fixate_field_string(s, "format", gst_audio_format_to_string(GST_AUDIO_FORMAT_S16));
		gst_structure_fixate_field_nearest_int(s, "channels", 2);
		gst_structure_fixate_field_nearest_int(s, "rate", 44100);

		if (gst_structure_has_field(s, "depth"))
		{
			gint width, depth;

			gst_structure_get_int(s, "width", &width);
			/* round width to nearest multiple of 8 for the depth */
			depth = GST_ROUND_UP_8(width);
			gst_structure_fixate_field_nearest_int(s, "depth", depth);
		}

		if (gst_structure_has_field(s, "signed"))
			gst_structure_fixate_field_boolean(s, "signed", TRUE);
		if (gst_structure_has_field(s, "endianness"))
			gst_structure_fixate_field_nearest_int(s, "endianness", G_BYTE_ORDER);
	}
	else
	{
		// TODO: Add code for non-PCM types here
	}

	/* For the remaining fields, use default fixation. */
	return gst_caps_fixate(caps);
}


/**
 * gst_pw_audio_format_data_is_contiguous:
 * @audio_type Audio type to check.
 *
 * Returns: TRUE if data of the given type is contiguous.
 */
gboolean gst_pw_audio_format_data_is_contiguous(GstPipewireAudioType audio_type)
{
	g_assert(((gint)audio_type) < GST_NUM_PIPEWIRE_AUDIO_TYPES);
	return audio_type_details[(gint)audio_type].is_contiguous;
}


/**
 * gst_pw_audio_format_from_caps:
 * @pw_audio_format: #GstPwAudioFormat to fill with format information.
 * @element: #GstElement to use for internal logging and error reporting.
 * @caps: #GstCaps to get format information from.
 *
 * This fills pw_audio_format with format information from the given caps.
 * The caps must be fixed. If the caps cannot be handled, an error is
 * reported via GST_ELEMENT_ERROR(). The specified #GstElement is passed
 * to that macro.
 *
 * Returns: TRUE if filling pw_audio_format with info from the caps was successful.
 */
gboolean gst_pw_audio_format_from_caps(GstPwAudioFormat *pw_audio_format, GstElement *element, GstCaps *caps)
{
	GstStructure *fmt_structure;
	gchar const *media_type;

	g_assert(pw_audio_format != NULL);
	g_assert(element != NULL);
	g_assert(caps != NULL);
	g_assert(gst_caps_is_fixed(caps));

	fmt_structure = gst_caps_get_structure(caps, 0);
	media_type = gst_structure_get_name(fmt_structure);

	if (g_strcmp0(media_type, "audio/x-raw") == 0)
	{
		if (!gst_audio_info_from_caps(&(pw_audio_format->pcm_audio_info), caps))
		{
			GST_ERROR_OBJECT(
				element,
				"could not convert caps \"%" GST_PTR_FORMAT "\" to a PCM audio info structure",
				(gpointer)caps
			);
			goto error;
		}

		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM;
	}
	// TODO: Add code for non-PCM types here
	else
	{
		GST_ELEMENT_ERROR(
			element,
			STREAM,
			FORMAT,
			("unsupported media type"),
			("format media type: \"%s\"", media_type)
		);
		goto error;
	}

	return TRUE;

error:
	return FALSE;
}


/**
 * gst_pw_audio_format_to_spa_pod:
 * @pw_audio_format: #GstPwAudioFormat to use for setting up an SPA POD.
 * @element: #GstElement to use for internal logging and error reporting.
 * @builder_buffer: Builder buffer to use for building the SPA POD.
 * @builder_buffer_size: Size of builder_buffer in bytes. Must be > 0.
 * @pod: Pointer to SPA POD pointer that shall be passed the new SPA POD.
 *
 * This builds an SPA POD out of the format information from pw_audio_format.
 * The resulting POD contains all parameters necessary for connecting
 * PipeWire streams and filters. If the SPA POD cannot be built, an error
 * is reported via GST_ELEMENT_ERROR(). The specified #GstElement is passed
 * to that macro.
 *
 * The SPA POD is built inside the builder_buffer, which must be of sufficient
 * size, specified by builder_buffer_size. Typical recommended size is 1024 bytes.
 * Note that the returned POD uses the builder_buffer as its memory, so that
 * buffer must remain valid for as long as the POD is in use.
 *
 * Returns: TRUE if the SPA POD could be built.
 */
gboolean gst_pw_audio_format_to_spa_pod(
	GstPwAudioFormat const *pw_audio_format,
	GstElement *element,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(builder_buffer, builder_buffer_size);
#pragma GCC diagnostic pop

	g_assert(pw_audio_format != NULL);
	g_assert(element != NULL);
	g_assert(pod != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			uint32_t spa_audio_format;
			GstAudioFormat gst_audio_format;
			gint sample_rate;
			gint num_channels;

			gst_audio_format = GST_AUDIO_INFO_FORMAT(&(pw_audio_format->pcm_audio_info));
			sample_rate = GST_AUDIO_INFO_RATE(&(pw_audio_format->pcm_audio_info));
			num_channels = GST_AUDIO_INFO_CHANNELS(&(pw_audio_format->pcm_audio_info));

			switch (gst_audio_format)
			{
				case GST_AUDIO_FORMAT_S16LE: spa_audio_format = SPA_AUDIO_FORMAT_S16_LE; break;
				case GST_AUDIO_FORMAT_S16BE: spa_audio_format = SPA_AUDIO_FORMAT_S16_BE; break;
				case GST_AUDIO_FORMAT_S18LE: spa_audio_format = SPA_AUDIO_FORMAT_S18_LE; break;
				case GST_AUDIO_FORMAT_S18BE: spa_audio_format = SPA_AUDIO_FORMAT_S18_BE; break;
				case GST_AUDIO_FORMAT_S20LE: spa_audio_format = SPA_AUDIO_FORMAT_S20_LE; break;
				case GST_AUDIO_FORMAT_S20BE: spa_audio_format = SPA_AUDIO_FORMAT_S20_BE; break;
				case GST_AUDIO_FORMAT_S24LE: spa_audio_format = SPA_AUDIO_FORMAT_S24_LE; break;
				case GST_AUDIO_FORMAT_S24BE: spa_audio_format = SPA_AUDIO_FORMAT_S24_BE; break;
				case GST_AUDIO_FORMAT_S24_32LE: spa_audio_format = SPA_AUDIO_FORMAT_S24_32_LE; break;
				case GST_AUDIO_FORMAT_S24_32BE: spa_audio_format = SPA_AUDIO_FORMAT_S24_32_BE; break;
				case GST_AUDIO_FORMAT_S32LE: spa_audio_format = SPA_AUDIO_FORMAT_S32_LE; break;
				case GST_AUDIO_FORMAT_S32BE: spa_audio_format = SPA_AUDIO_FORMAT_S32_BE; break;

				case GST_AUDIO_FORMAT_U16LE: spa_audio_format = SPA_AUDIO_FORMAT_U16_LE; break;
				case GST_AUDIO_FORMAT_U16BE: spa_audio_format = SPA_AUDIO_FORMAT_U16_BE; break;
				case GST_AUDIO_FORMAT_U18LE: spa_audio_format = SPA_AUDIO_FORMAT_U18_LE; break;
				case GST_AUDIO_FORMAT_U18BE: spa_audio_format = SPA_AUDIO_FORMAT_U18_BE; break;
				case GST_AUDIO_FORMAT_U20LE: spa_audio_format = SPA_AUDIO_FORMAT_U20_LE; break;
				case GST_AUDIO_FORMAT_U20BE: spa_audio_format = SPA_AUDIO_FORMAT_U20_BE; break;
				case GST_AUDIO_FORMAT_U24LE: spa_audio_format = SPA_AUDIO_FORMAT_U24_LE; break;
				case GST_AUDIO_FORMAT_U24BE: spa_audio_format = SPA_AUDIO_FORMAT_U24_BE; break;
				case GST_AUDIO_FORMAT_U24_32LE: spa_audio_format = SPA_AUDIO_FORMAT_U24_32_LE; break;
				case GST_AUDIO_FORMAT_U24_32BE: spa_audio_format = SPA_AUDIO_FORMAT_U24_32_BE; break;
				case GST_AUDIO_FORMAT_U32LE: spa_audio_format = SPA_AUDIO_FORMAT_U32_LE; break;
				case GST_AUDIO_FORMAT_U32BE: spa_audio_format = SPA_AUDIO_FORMAT_U32_BE; break;

				case GST_AUDIO_FORMAT_F32LE: spa_audio_format = SPA_AUDIO_FORMAT_F32_LE; break;
				case GST_AUDIO_FORMAT_F32BE: spa_audio_format = SPA_AUDIO_FORMAT_F32_BE; break;
				case GST_AUDIO_FORMAT_F64LE: spa_audio_format = SPA_AUDIO_FORMAT_F64_LE; break;
				case GST_AUDIO_FORMAT_F64BE: spa_audio_format = SPA_AUDIO_FORMAT_F64_BE; break;

				default:
					GST_ELEMENT_ERROR(
						element,
						STREAM,
						FORMAT,
						("unsupported audio format"),
						("audio format: \"%s\"", gst_audio_format_to_string(gst_audio_format))
					);
					goto error;
			}

			GST_DEBUG_OBJECT(
				element,
				"building SPA POD for PCM audio; params:  format: %s  sample rate: %d  num channels: %d",
				gst_audio_format_to_string(gst_audio_format),
				sample_rate,
				num_channels
			);

			*pod = spa_format_audio_raw_build(
				&builder, SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_RAW_INIT(
					.format = spa_audio_format,
					.channels = num_channels,
					.rate = sample_rate
				)
			);

			break;
		}

		// TODO: Add code for non-PCM types here

		default:
			goto error;
	}

	return TRUE;

error:
	return FALSE;
}


/**
 * gst_pw_audio_format_get_stride:
 * @pw_audio_format: #GstPwAudioFormat to get the stride from.
 * @see_also: #GST_AUDIO_INFO_BPF
 *
 * Returns the stride of a #GstPwAudioFormat. The "stride" is the
 * distance in bytes inside a block of memory between two logical
 * units of data. These units depend on the audio type. In PCM,
 * the stride defines the size of a PCM frame in bytes, and is
 * equivalent to the return value of GST_AUDIO_INFO_BPF().
 *
 * The stride is needed by PipeWire SPA data chunks and also
 * for converting between number of bytes and number of frames.
 * (num-bytes = num-frames * stride)
 *
 * Returns: The stride in bytes.
 */
gsize gst_pw_audio_format_get_stride(GstPwAudioFormat const *pw_audio_format)
{
	g_assert(pw_audio_format != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
			return GST_AUDIO_INFO_BPF(&(pw_audio_format->pcm_audio_info));

		// TODO: Add code for non-PCM types here

		default:
			/* Reaching this place is a hard error, since without a defined
			 * stride we cannot continue, so raise an assertion. */
			g_assert_not_reached();
			return 0;
	}
}


/**
 * gst_pw_audio_format_to_string:
 * @pw_audio_format: #GstPwAudioFormat to produce a string representation of.
 *
 * Produces a string representation of the given pw_audio_format. The
 * representation is in a human readable form and intended for use during
 * logging. It is not suitable for (de)serialization purposes.
 *
 * The string must be deallocated with g_free() after use.
 *
 * Returns: (transfer full): The allocated string.
 */
gchar* gst_pw_audio_format_to_string(GstPwAudioFormat const *pw_audio_format)
{
	g_assert(pw_audio_format != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			return gst_info_strdup_printf(
				"PCM: rate %d channels %d sample format %s bpf %d",
				GST_AUDIO_INFO_RATE(&(pw_audio_format->pcm_audio_info)),
				GST_AUDIO_INFO_CHANNELS(&(pw_audio_format->pcm_audio_info)),
				gst_audio_format_to_string(GST_AUDIO_INFO_FORMAT(&(pw_audio_format->pcm_audio_info))),
				GST_AUDIO_INFO_BPF(&(pw_audio_format->pcm_audio_info))
			);
		}

		// TODO: Add code for non-PCM types here

		default:
			return g_strdup("<unknown>");
	}
}


/**
 * gst_pw_audio_format_calculate_num_frames_from_duration:
 * @pw_audio_format: #GstPwAudioFormat to use for the conversion.
 * @duration: Duration to convert.
 *
 * Converts a given duration (in nanoseconds) to a number of frames.
 * The frame count that is equivalent to the duration depends on the
 * audio type. For PCM, duration = num-frames * GST_SECOND / sample-rate .
 *
 * duration must be a valid value.
 *
 * Returns: The number of frames corresponding to the duration.
 */
gsize gst_pw_audio_format_calculate_num_frames_from_duration(GstPwAudioFormat const *pw_audio_format, GstClockTime duration)
{
	g_assert(pw_audio_format != NULL);
	g_assert(GST_CLOCK_TIME_IS_VALID(duration));

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			GstAudioInfo const *info = &(pw_audio_format->pcm_audio_info);
			return gst_util_uint64_scale_int(duration, GST_AUDIO_INFO_RATE(info), GST_SECOND);
		}

		// TODO: Add code for non-PCM types here

		default:
			return 0;
	}
}


/**
 * gst_pw_audio_format_calculate_duration_from_num_frames:
 * @pw_audio_format: #GstPwAudioFormat to use for the conversion.
 * @num_frames: Frame count to convert.
 *
 * Converts a given frame count to a duration in nanoseconds.
 * The duration that is equivalent to the frame count depends on
 * the audio type. For PCM, num-frames = duration * sample-rate / GST_SECOND.
 *
 * Returns: The duration corresponding to the frame count.
 */
GstClockTime gst_pw_audio_format_calculate_duration_from_num_frames(GstPwAudioFormat const *pw_audio_format, gsize num_frames)
{
	g_assert(pw_audio_format != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			GstAudioInfo const *info = &(pw_audio_format->pcm_audio_info);
			return gst_util_uint64_scale_int(num_frames, GST_SECOND, GST_AUDIO_INFO_RATE(info));
		}

		// TODO: Add code for non-PCM types here

		default:
			return 0;
	}
}


/**
 * gst_pw_audio_format_write_silence_frames:
 * @pw_audio_format: #GstPwAudioFormat of the silence frames that shall be written.
 * @dest_frames: Destination memory region that shall be filled with silence.
 * @num_silence_frames_to_write: How many silence frames to write to dest_frames.
 *
 * Fills a memory region pointed at by dest_frames with num_silence_frames_to_write
 * frames. The definition of a "silence frame" depends on the audio type and the
 * format information. For example, for the PCM S16LE format, a silence frame is
 * simply a null 16-bit signed integer. For DSD, a 0x69 byte is a silence frame.
 *
 * This function does not do anything if pw_audio_format's audio type is a compressed
 * audio type like MPEG audio or AC-3, since there are no defined "silence frames"
 * for that kind of audio.
 */
void gst_pw_audio_format_write_silence_frames(GstPwAudioFormat const *pw_audio_format, gpointer dest_frames, gsize num_silence_frames_to_write)
{
	g_assert(pw_audio_format != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			gint num_silence_bytes = num_silence_frames_to_write * GST_AUDIO_INFO_BPF(&(pw_audio_format->pcm_audio_info));
#if GST_CHECK_VERSION(1, 20, 0)
			/* gst_audio_format_fill_silence() was deprecated in GStreamer 1.20
			 * in favor of gst_audio_format_fill_silence. See gst-plugins-base
			 * commit 3ec795f613c6201790a75189de28bd5493c37d3b for details. */
			gst_audio_format_info_fill_silence(pw_audio_format->pcm_audio_info.finfo, dest_frames, num_silence_bytes);
#else
			gst_audio_format_fill_silence(pw_audio_format->pcm_audio_info.finfo, dest_frames, num_silence_bytes);
#endif
			break;
		}

		// TODO: Add code for non-PCM types here

		default:
			break;
	}
}

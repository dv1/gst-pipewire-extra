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
 *
 * It is possible to probe a PipeWire graph for whether it can handle a certain
 * audio type. The #GstPwAudioFormatProbe takes care of this. It works by creating
 * dummy streams and checking if it successfully connected. If it did, then the
 * session manager was able to successfully link the stream, otherwise no such
 * link could be established in the graph. Note that a successful link does not
 * strictly mean that a particular sink node in the graph can directly handle this
 * audio type, just that the graph has a way to process this stream. This can
 * for example mean that the graph links the stream to some intermediate processing
 * nodes which can handle this audio type. (It is not possible nor practical to try
 * to directly query sinks from the pw_stream - that's up to the session manager.)
 */

#include <string.h>
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
	/* Human-readable name for this audio type. */
	gchar const *name;

	/* Caps in string form, intended to be used when adding
	 * GstPad templates to an element. Not directly used by callers;
	 * these use gst_pw_audio_format_get_template_caps() instead. */
	gchar const *template_caps_string;

	gboolean is_contiguous;
}
GstPipewireAudioTypeDetails;


// TODO: Currently, only PCM and DSD are supported.
// Fill in compressed audio support (AC-3 etc).


static GstPipewireAudioTypeDetails const audio_type_details[GST_NUM_PIPEWIRE_AUDIO_TYPES] = {
	/* PCM */
	{
		.name = "PCM",
		.template_caps_string = \
			GST_AUDIO_CAPS_MAKE(GST_AUDIO_FORMATS_ALL) \
			", layout = (string) { interleaved }",
		.is_contiguous = TRUE
	},

	/* DSD */
	{
		.name = "DSD",
		.template_caps_string = \
			"audio/x-dsd, " \
			"format = (string) { DSD_U8, DSD_U32BE, DSD_U16BE, DSD_U32LE, DSD_U16LE }, "
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		/* DSD data can be subdivided, and contiguous buffers can
		 * be used with such data, so mark this as true. However,
		 * DSD cannot be (easily) processed, unlike PCM. */
		.is_contiguous = TRUE
	},

	/* MPEG */
	{
		.name = "MPEG",
		.template_caps_string = \
			"audio/mpeg, " \
			"parsed = (boolean) true, " \
			"mpegversion = (int) 1, " \
			"mpegaudioversion = (int) [ 1, 3 ]",
		.is_contiguous = FALSE
	},

	/* AC3 */
	{
		.name = "AC3",
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
		.name = "EAC3",
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
		.name = "TrueHD",
		.template_caps_string = "audio/x-true-hd",
		.is_contiguous = FALSE
	},

	/* DTS */
	{
		.name = "DTS",
		.template_caps_string = "audio/x-dts",
		.is_contiguous = FALSE
	},

	/* DTS-HD */
	{
		.name = "DTS-HD",
		.template_caps_string = "audio/x-dts",
		.is_contiguous = FALSE
	},

	/* MHAS */
	{
		.name = "MPEG-H audio stream",
		.template_caps_string = "audio/x-mhas",
		.is_contiguous = FALSE
	}
};


static void spa_to_gst_channel_positions(uint32_t const *spa_channel_positions, GstAudioChannelPosition *gst_channel_positions, gint num_channels)
{
	gint channel_nr;

	for (channel_nr = 0; channel_nr < num_channels; ++channel_nr)
	{
		switch (spa_channel_positions[channel_nr])
		{
			case SPA_AUDIO_CHANNEL_MONO: gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_MONO; break;
			case SPA_AUDIO_CHANNEL_NA:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_INVALID; break;
			case SPA_AUDIO_CHANNEL_FL:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT; break;
			case SPA_AUDIO_CHANNEL_FR:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT; break;
			case SPA_AUDIO_CHANNEL_FC:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER; break;
			case SPA_AUDIO_CHANNEL_LFE:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_LFE1; break;
			case SPA_AUDIO_CHANNEL_RL:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT; break;
			case SPA_AUDIO_CHANNEL_RR:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT; break;
			case SPA_AUDIO_CHANNEL_FLC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER; break;
			case SPA_AUDIO_CHANNEL_FRC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER; break;
			case SPA_AUDIO_CHANNEL_RC:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER; break;
			case SPA_AUDIO_CHANNEL_LFE2: gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_LFE2; break;
			case SPA_AUDIO_CHANNEL_SL:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT; break;
			case SPA_AUDIO_CHANNEL_SR:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT; break;
			case SPA_AUDIO_CHANNEL_TFL:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT; break;
			case SPA_AUDIO_CHANNEL_TFR:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT; break;
			case SPA_AUDIO_CHANNEL_TFC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER; break;
			case SPA_AUDIO_CHANNEL_TC:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_CENTER; break;
			case SPA_AUDIO_CHANNEL_TRL:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT; break;
			case SPA_AUDIO_CHANNEL_TRR:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT; break;
			case SPA_AUDIO_CHANNEL_TSL:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT; break;
			case SPA_AUDIO_CHANNEL_TSR:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT; break;
			case SPA_AUDIO_CHANNEL_TRC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER; break;
			case SPA_AUDIO_CHANNEL_BC:   gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER; break;
			case SPA_AUDIO_CHANNEL_BLC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT; break;
			case SPA_AUDIO_CHANNEL_BRC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT; break;
			case SPA_AUDIO_CHANNEL_FLW:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT; break;
			case SPA_AUDIO_CHANNEL_FRW:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT; break;
			case SPA_AUDIO_CHANNEL_RLC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT; break;
			case SPA_AUDIO_CHANNEL_RRC:  gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT; break;

			default:
				gst_channel_positions[channel_nr] = GST_AUDIO_CHANNEL_POSITION_INVALID;
		}
	}
}


static void gst_to_spa_channel_positions(GstAudioChannelPosition const *gst_channel_positions, uint32_t *spa_channel_positions, gint num_channels)
{
	gint channel_nr;

	for (channel_nr = 0; channel_nr < num_channels; ++channel_nr)
	{
		switch (gst_channel_positions[channel_nr])
		{
			case GST_AUDIO_CHANNEL_POSITION_MONO:                  spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_MONO; break;
			case GST_AUDIO_CHANNEL_POSITION_INVALID:               spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_NA; break;
			case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:            spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FL; break;
			case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:           spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FR; break;
			case GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER:          spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FC; break;
			case GST_AUDIO_CHANNEL_POSITION_LFE1:                  spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_LFE; break;
			case GST_AUDIO_CHANNEL_POSITION_REAR_LEFT:             spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_RL; break;
			case GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT:            spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_RR; break;
			case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:  spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FLC; break;
			case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER: spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FRC; break;
			case GST_AUDIO_CHANNEL_POSITION_REAR_CENTER:           spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_RC; break;
			case GST_AUDIO_CHANNEL_POSITION_LFE2:                  spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_LFE2; break;
			case GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT:             spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_SL; break;
			case GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT:            spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_SR; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT:        spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TFL; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT:       spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TFR; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER:      spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TFC; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_CENTER:            spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TC; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT:         spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TRL; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT:        spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TRR; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT:         spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TSL; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT:        spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TSR; break;
			case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER:       spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_TRC; break;
			case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER:   spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_BC; break;
			case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT:     spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_BLC; break;
			case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT:    spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_BRC; break;
			case GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT:             spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FLW; break;
			case GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT:            spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_FRW; break;
			case GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT:         spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_RLC; break;
			case GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT:        spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_RRC; break;

			default:
				spa_channel_positions[channel_nr] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
	}
}


GstPipewireDsdFormat gst_pipewire_dsd_format_from_string(gchar const *str)
{
	if (g_strcmp0(str, "DSD_U8") == 0) return GST_PIPEWIRE_DSD_FORMAT_DSD_U8;
	else if (g_strcmp0(str, "DSD_U16LE") == 0) return GST_PIPEWIRE_DSD_FORMAT_DSD_U16LE;
	else if (g_strcmp0(str, "DSD_U16BE") == 0) return GST_PIPEWIRE_DSD_FORMAT_DSD_U16BE;
	else if (g_strcmp0(str, "DSD_U32LE") == 0) return GST_PIPEWIRE_DSD_FORMAT_DSD_U32LE;
	else if (g_strcmp0(str, "DSD_U32BE") == 0) return GST_PIPEWIRE_DSD_FORMAT_DSD_U32BE;
	else return GST_PIPEWIRE_DSD_FORMAT_DSD_UNKNOWN;
}


gchar const * gst_pipewire_dsd_format_to_string(GstPipewireDsdFormat format)
{
	switch (format)
	{
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U8: return "DSD_U8";
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U16LE: return "DSD_U16LE";
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U16BE: return "DSD_U16BE";
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U32LE: return "DSD_U32LE";
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U32BE: return "DSD_U32BE";
		default: return NULL;
	}
}


guint gst_pipewire_dsd_format_get_width(GstPipewireDsdFormat format)
{
	switch (format)
	{
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U8: return 1;
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U16LE: return 2;
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U16BE: return 2;
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U32LE: return 4;
		case GST_PIPEWIRE_DSD_FORMAT_DSD_U32BE: return 4;
		default: return 0;
	}
}


/**
 * gst_pw_audio_format_get_audio_type_name:
 * @audio_type Audio type to get a name for.
 *
 * This returns a human-readable name for this audio type. The name is useful
 * for logging and for UIs. Do not use this as an string ID for the audio type.
 *
 * Returns: The name for this audio type.
 */
gchar const *gst_pw_audio_format_get_audio_type_name(GstPipewireAudioType audio_type)
{
	g_assert((audio_type >= 0) && (audio_type < GST_NUM_PIPEWIRE_AUDIO_TYPES));
	return audio_type_details[audio_type].name;
}


/**
 * gst_pw_audio_format_get_template_caps:
 *
 * Returns #GstCaps suitable for pad templates. The return value is equivalent
 * to calling gst_pw_audio_format_get_template_caps_for_type() for all audio
 * types and concatenating the results of all those calls.
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
 * @gst_pw_audio_format_get_template_caps_for_type:
 * @audio_type Audio type to get template caps for.
 *
 * Returns #GstCaps suitable for pad templates. These caps specify
 * the capabilities supporter for the given audio_type.
 *
 * Returns: (transfer full): the #GstCaps.
 */
GstCaps* gst_pw_audio_format_get_template_caps_for_type(GstPipewireAudioType audio_type)
{
	g_assert((audio_type >= 0) && (audio_type < GST_NUM_PIPEWIRE_AUDIO_TYPES));
	return gst_caps_from_string(audio_type_details[audio_type].template_caps_string);
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
	else if (g_strcmp0(gst_structure_get_name(s), "audio/x-dsd") == 0)
	{
		gst_structure_fixate_field_string(s, "format", "DSD_U8");
		gst_structure_fixate_field_nearest_int(s, "channels", 2);
		gst_structure_fixate_field_nearest_int(s, "rate", 64 * 44100); /* 64 * 44100 = DSD64 */
	}
	else
	{
		// TODO: Add code for more non-PCM types here
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
gboolean gst_pw_audio_format_from_caps(GstPwAudioFormat *pw_audio_format, GstObject *parent, GstCaps *caps)
{
	GstStructure *fmt_structure;
	gchar const *media_type;

	g_assert(pw_audio_format != NULL);
	g_assert(parent != NULL);
	g_assert(caps != NULL);
	g_assert(gst_caps_is_fixed(caps));

	fmt_structure = gst_caps_get_structure(caps, 0);
	media_type = gst_structure_get_name(fmt_structure);

	if (g_strcmp0(media_type, "audio/x-raw") == 0)
	{
		if (!gst_audio_info_from_caps(&(pw_audio_format->info.pcm_audio_info), caps))
		{
			GST_ERROR_OBJECT(
				parent,
				"could not convert caps \"%" GST_PTR_FORMAT "\" to a PCM audio info structure",
				(gpointer)caps
			);
			goto error;
		}

		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM;
	}
	else if (g_strcmp0(media_type, "audio/x-dsd") == 0)
	{
		GstPipewireDsdInfo *dsd_audio_info = &(pw_audio_format->info.dsd_audio_info);
		gchar const *format_str = NULL;
		guint64 channel_mask = 0;

		format_str = gst_structure_get_string(fmt_structure, "format");
		if (format_str == NULL)
		{
			GST_ERROR_OBJECT(parent, "caps have no format field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		dsd_audio_info->format = gst_pipewire_dsd_format_from_string(format_str);
		if (dsd_audio_info->format == GST_PIPEWIRE_DSD_FORMAT_DSD_UNKNOWN)
		{
			GST_ERROR_OBJECT(parent, "caps have unsupported/invalid format field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		if (!gst_structure_get_int(fmt_structure, "rate", &(dsd_audio_info->rate)))
		{
			GST_ERROR_OBJECT(parent, "caps have no rate field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		if (dsd_audio_info->rate < 1)
		{
			GST_ERROR_OBJECT(parent, "caps have invalid rate field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		if (!gst_structure_get_int(fmt_structure, "channels", &(dsd_audio_info->channels)))
		{
			GST_ERROR_OBJECT(parent, "caps have no channels field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		if (dsd_audio_info->channels < 1)
		{
			GST_ERROR_OBJECT(parent, "caps have invalid channels field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
			goto error;
		}

		if (!gst_structure_get(fmt_structure, "channel-mask", GST_TYPE_BITMASK, &channel_mask, NULL) ||
			((channel_mask == 0) && (dsd_audio_info->channels == 1))
		)
		{
			switch (dsd_audio_info->channels)
			{
				case 1:
					dsd_audio_info->positions[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
					break;

				case 2:
					dsd_audio_info->positions[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
					dsd_audio_info->positions[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
					break;

				default:
					GST_ERROR_OBJECT(parent, "caps indicate raw multichannel data but have no channel-mask field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
					goto error;
			}
		}
		else if (channel_mask == 0)
		{
			gint i;
			for (i = 0; i < MIN(64, dsd_audio_info->channels); i++)
				dsd_audio_info->positions[i] = GST_AUDIO_CHANNEL_POSITION_NONE;
		}
		else
		{
			if (!gst_audio_channel_positions_from_mask(dsd_audio_info->channels, channel_mask, dsd_audio_info->positions))
			{
				GST_ERROR_OBJECT(
					parent,
					"invalid channel mask 0x%016" G_GINT64_MODIFIER "x for %d channels",
					channel_mask,
					dsd_audio_info->channels
				);
				goto error;
			}
		}

		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_DSD;
	}
	// TODO: Add code for more non-PCM types here
	else
	{
		GST_ERROR_OBJECT(parent, "unsupported media type \"%s\"", media_type);
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
	GstObject *parent,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(builder_buffer, builder_buffer_size);
#pragma GCC diagnostic pop

	g_assert(pw_audio_format != NULL);
	g_assert(parent != NULL);
	g_assert(pod != NULL);

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
		{
			uint32_t spa_audio_format;
			GstAudioFormat gst_audio_format;
			gint sample_rate;
			gint num_channels;
			uint32_t spa_channel_positions[SPA_AUDIO_MAX_CHANNELS];

			gst_audio_format = GST_AUDIO_INFO_FORMAT(&(pw_audio_format->info.pcm_audio_info));
			sample_rate = GST_AUDIO_INFO_RATE(&(pw_audio_format->info.pcm_audio_info));
			num_channels = GST_AUDIO_INFO_CHANNELS(&(pw_audio_format->info.pcm_audio_info));

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
					GST_ERROR_OBJECT(parent, "unsupported PCM format \"%s\"", gst_audio_format_to_string(gst_audio_format));
					goto error;
			}

			GST_DEBUG_OBJECT(
				parent,
				"building SPA POD for PCM audio; params:  format: %s  sample rate: %d  num channels: %d",
				gst_audio_format_to_string(gst_audio_format),
				sample_rate,
				num_channels
			);

			gst_to_spa_channel_positions(pw_audio_format->info.pcm_audio_info.position, spa_channel_positions, num_channels);

			{
				struct spa_audio_info_raw pcm_info = {
					.format = spa_audio_format,
					.flags = 0,
					.rate = sample_rate,
					.channels = num_channels
				};

				memcpy(pcm_info.position, spa_channel_positions, sizeof(uint32_t) * num_channels);

				*pod = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &pcm_info);
			}

			break;
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
		{
			uint32_t spa_channel_positions[SPA_AUDIO_MAX_CHANNELS];
			gint interleave;
			gint num_channels;

			num_channels = pw_audio_format->info.dsd_audio_info.channels;

			switch (pw_audio_format->info.dsd_audio_info.format)
			{
				case GST_PIPEWIRE_DSD_FORMAT_DSD_U8: interleave = 1; break;
				case GST_PIPEWIRE_DSD_FORMAT_DSD_U16LE: interleave = -2; break;
				case GST_PIPEWIRE_DSD_FORMAT_DSD_U16BE: interleave = +2; break;
				case GST_PIPEWIRE_DSD_FORMAT_DSD_U32LE: interleave = -4; break;
				case GST_PIPEWIRE_DSD_FORMAT_DSD_U32BE: interleave = +4; break;
				default:
					GST_ERROR_OBJECT(parent, "unsupported DSD format \"%s\"", gst_pipewire_dsd_format_to_string(pw_audio_format->info.dsd_audio_info.format));
					goto error;
			}

			gst_to_spa_channel_positions(pw_audio_format->info.dsd_audio_info.positions, spa_channel_positions, num_channels);

			{
				struct spa_audio_info_dsd dsd_info = {
					.bitorder = SPA_PARAM_BITORDER_msb,
					.flags = 0,
					.interleave = interleave,
					.rate = pw_audio_format->info.dsd_audio_info.rate,
					.channels = num_channels
				};

				memcpy(dsd_info.position, spa_channel_positions, sizeof(uint32_t) * num_channels);

				*pod = spa_format_audio_dsd_build(&builder, SPA_PARAM_EnumFormat, &dsd_info);
			}

			break;
		}

		// TODO: Add code for more non-PCM types here

		default:
			goto error;
	}

	return TRUE;

error:
	return FALSE;
}


/**
 * gst_pw_audio_format_build_spa_pod_for_probing:
 * @audio_type Audio type to get an SPA POD for.
 * @builder_buffer: Builder buffer to use for building the SPA POD.
 * @builder_buffer_size: Size of builder_buffer in bytes. Must be > 0.
 * @pod: Pointer to SPA POD pointer that shall be passed the new SPA POD.
 *
 * This builds a minimal SPA POD for the given audio type. That POD is suitable
 * for probing the PipeWire graph whether it can handle such an audio type or not.
 * Do not use this function for constructing PODs for actual playback - only use
 * this for probing. For actual playback, use gst_pw_audio_format_to_spa_pod().
 *
 * The SPA POD is built inside the builder_buffer, which must be of sufficient
 * size, specified by builder_buffer_size. Typical recommended size is 1024 bytes.
 * Note that the returned POD uses the builder_buffer as its memory, so that
 * buffer must remain valid for as long as the POD is in use.
 *
 * Returns: TRUE if the SPA POD could be built.
 */
gboolean gst_pw_audio_format_build_spa_pod_for_probing(
	GstPipewireAudioType audio_type,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(builder_buffer, builder_buffer_size);
#pragma GCC diagnostic pop

	g_assert(pod != NULL);

	switch (audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
			*pod = spa_format_audio_raw_build(
				&builder, SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_RAW_INIT(
					/* TODO: Initially, format SPA_AUDIO_FORMAT_UNKNOWN channels 0 rate 0
					 * was used here, but this causes the probing stream to not reach any
					 * state beyond paused, not even the error state, so we currently
					 * have to use a valid probing state. We pick S16LE 48 kHz mono,
					 * since this is widely supported. But it would be better to not
					 * have to rely on that. */
					.format = SPA_AUDIO_FORMAT_S16_LE,
					.channels = 1,
					.rate = 48000
				)
			);
			break;

#if PW_CHECK_VERSION(0, 3, 37)
		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
			*pod = spa_format_audio_dsd_build(
				&builder, SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_DSD_INIT(
					/* TODO: On DSD capable DACs, see if we can use these unknown/zero values
					 * for probing, or if this causes the stream state change to hang like in
					 * the PCM case (= we had to pick S16LE 48 kHz mono instead of "unknown"
					 * for PCM probing, otherwise the stream state would get stuck at paused,
					 * and never progress to streaming or error). */
					.bitorder = SPA_PARAM_BITORDER_unknown,
					.interleave = 0,
					.channels = 0,
					.rate = 0
				)
			);
			break;
#endif

		default:
			return FALSE;
	}

	return TRUE;
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
			return GST_AUDIO_INFO_BPF(&(pw_audio_format->info.pcm_audio_info));

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
			return pw_audio_format->info.dsd_audio_info.channels * gst_pipewire_dsd_format_get_width(pw_audio_format->info.dsd_audio_info.format);

		// TODO: Add code for more non-PCM types here

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
				GST_AUDIO_INFO_RATE(&(pw_audio_format->info.pcm_audio_info)),
				GST_AUDIO_INFO_CHANNELS(&(pw_audio_format->info.pcm_audio_info)),
				gst_audio_format_to_string(GST_AUDIO_INFO_FORMAT(&(pw_audio_format->info.pcm_audio_info))),
				GST_AUDIO_INFO_BPF(&(pw_audio_format->info.pcm_audio_info))
			);
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
		{
			return gst_info_strdup_printf(
				"DSD: rate %d channels %d format %s width %d",
				pw_audio_format->info.dsd_audio_info.rate,
				pw_audio_format->info.dsd_audio_info.channels,
				gst_pipewire_dsd_format_to_string(pw_audio_format->info.dsd_audio_info.format),
				gst_pipewire_dsd_format_get_width(pw_audio_format->info.dsd_audio_info.format)
			);
		}

		// TODO: Add code for more non-PCM types here

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
			GstAudioInfo const *info = &(pw_audio_format->info.pcm_audio_info);
			return gst_util_uint64_scale_int(duration, GST_AUDIO_INFO_RATE(info), GST_SECOND);
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
		{
			GstPipewireDsdInfo const *info = &(pw_audio_format->info.dsd_audio_info);
			return gst_util_uint64_scale_int(duration, info->rate, GST_SECOND);
		}

		// TODO: Add code for more non-PCM types here

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
			GstAudioInfo const *info = &(pw_audio_format->info.pcm_audio_info);
			return gst_util_uint64_scale_int(num_frames, GST_SECOND, GST_AUDIO_INFO_RATE(info));
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
		{
			GstPipewireDsdInfo const *info = &(pw_audio_format->info.dsd_audio_info);
			return gst_util_uint64_scale_int(num_frames, GST_SECOND, info->rate);
		}

		// TODO: Add code for more non-PCM types here

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
			gint num_silence_bytes = num_silence_frames_to_write * gst_pw_audio_format_get_stride(pw_audio_format);
#if GST_CHECK_VERSION(1, 20, 0)
			/* gst_audio_format_fill_silence() was deprecated in GStreamer 1.20
			 * in favor of gst_audio_format_fill_silence. See gst-plugins-base
			 * commit 3ec795f613c6201790a75189de28bd5493c37d3b for details. */
			gst_audio_format_info_fill_silence(pw_audio_format->info.pcm_audio_info.finfo, dest_frames, num_silence_bytes);
#else
			gst_audio_format_fill_silence(pw_audio_format->info.pcm_audio_info.finfo, dest_frames, num_silence_bytes);
#endif
			break;
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
		{
			gint num_silence_bytes = num_silence_frames_to_write * gst_pw_audio_format_get_stride(pw_audio_format);
			/* In DSD, silence requires the bit pattern 0x69. 0x00 does not produce silence. */
			memset(dest_frames, 0x69, num_silence_bytes);
			break;
		}

		// TODO: Add code for more non-PCM types here

		default:
			break;
	}
}


#define AUDIO_FORMAT_PROBE_BUILDER_BUFFER_SIZE 1024


struct _GstPwAudioFormatProbe
{
	GstObject parent;

	GMutex mutex;
	GCond cond;

	GstPipewireCore *core;

	struct pw_stream *probing_stream;
	struct spa_hook probing_stream_listener;

	struct spa_pod const *format_params[1];
	guint8 builder_buffer[AUDIO_FORMAT_PROBE_BUILDER_BUFFER_SIZE];

	enum pw_stream_state last_state;

	gboolean cancelled;
};


struct _GstPwAudioFormatProbeClass
{
	GstObjectClass parent_class;
};


G_DEFINE_TYPE(GstPwAudioFormatProbe, gst_pw_audio_format_probe, GST_TYPE_OBJECT)


static void gst_pw_audio_format_probe_dispose(GObject *object);
static void gst_pw_audio_format_probe_finalize(GObject *object);

static void pw_on_probing_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error);


static const struct pw_stream_events probing_stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pw_on_probing_state_changed
};


static void gst_pw_audio_format_probe_class_init(GstPwAudioFormatProbeClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = GST_DEBUG_FUNCPTR(gst_pw_audio_format_probe_dispose);
	object_class->finalize = GST_DEBUG_FUNCPTR(gst_pw_audio_format_probe_finalize);
}


static void gst_pw_audio_format_probe_init(GstPwAudioFormatProbe *self)
{
	g_mutex_init(&(self->mutex));
	g_cond_init(&(self->cond));

	self->core = NULL;
	self->probing_stream = NULL;
	self->cancelled = FALSE;
}


static void gst_pw_audio_format_probe_dispose(GObject *object)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE(object);

	if (self->core != NULL)
	{
		gst_object_unref(GST_OBJECT(self->core));
		self->core = NULL;
	}

	G_OBJECT_CLASS(gst_pw_audio_format_probe_parent_class)->dispose(object);
}


static void gst_pw_audio_format_probe_finalize(GObject *object)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE(object);

	g_mutex_clear(&(self->mutex));
	g_cond_clear(&(self->cond));

	G_OBJECT_CLASS(gst_pw_audio_format_probe_parent_class)->finalize(object);
}


static void pw_on_probing_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE_CAST(data);

	GST_TRACE_OBJECT(
		self,
		"PipeWire probing stream state changed:  old: %s  new: %s  error: \"%s\"",
		pw_stream_state_as_string(old_state),
		pw_stream_state_as_string(new_state),
		(error == NULL) ? "<none>" : error
	);

	switch (new_state)
	{
		case PW_STREAM_STATE_STREAMING:
		case PW_STREAM_STATE_ERROR:
		{
			g_mutex_lock(&(self->mutex));
			self->last_state = new_state;
			g_cond_signal(&(self->cond));
			g_mutex_unlock(&(self->mutex));
			break;
		}
		default:
			break;
	}
}


/**
 * gst_pw_audio_format_probe_new:
 * @core: (transfer none): a #GstPipewireCore
 *
 * Create a new #GstPwAudioFormatProbe.
 *
 * This refs the core. The core is unref'd when this probe is disposed of.
 *
 * Before actually using the probe, call gst_pw_audio_format_probe_setup().
 *
 * Returns: (transfer full): new #GstPwAudioFormatProbe instance.
 */
GstPwAudioFormatProbe* gst_pw_audio_format_probe_new(GstPipewireCore *core)
{
	GstPwAudioFormatProbe *pw_audio_format_probe;

	g_assert(core != NULL);
	g_assert(GST_IS_PIPEWIRE_CORE(core));

	pw_audio_format_probe = GST_PW_AUDIO_FORMAT_PROBE_CAST(g_object_new(GST_TYPE_PW_AUDIO_FORMAT_PROBE, NULL));
	pw_audio_format_probe->core = gst_object_ref(core);

	/* Clear the floating flag. */
	gst_object_ref_sink(GST_OBJECT(pw_audio_format_probe));

	return pw_audio_format_probe;
}


/**
 * gst_pw_audio_format_probe_setup:
 * @pw_audio_format_probe: a #GstPwAudioFormatProbe
 *
 * Sets up a #GstPwAudioFormatProbe for probing the PipeWire graph.
 *
 * This internally creates a pw_stream that is later used by
 * gst_pw_audio_format_probe_probe_audio_type() to see if a particular
 * audio type is supported. This setup function only needs to be called
 * once before gst_pw_audio_format_probe_probe_audio_type() calls, but
 * must be called before that function can be used.
 *
 * Once probing is done, gst_pw_audio_format_probe_teardown() must be called.
 *
 * MT safe (it takes the object lock while running).
 */
void gst_pw_audio_format_probe_setup(GstPwAudioFormatProbe *pw_audio_format_probe)
{
	gchar *stream_name;
	struct pw_properties *probing_stream_props;

	g_assert(GST_IS_PW_AUDIO_FORMAT_PROBE(pw_audio_format_probe));

	GST_OBJECT_LOCK(pw_audio_format_probe);

	pw_audio_format_probe->cancelled = FALSE;

	stream_name = g_strdup_printf("probing_stream_%s", GST_OBJECT_NAME(pw_audio_format_probe));
	GST_DEBUG_OBJECT(pw_audio_format_probe, "creating new probing stream with name \"%s\"", stream_name);

	probing_stream_props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_APP_NAME, "pwaudiosink",
		PW_KEY_NODE_NAME, stream_name,
		PW_KEY_NODE_DESCRIPTION, "probing stream",
		NULL
	);

	pw_audio_format_probe->probing_stream = pw_stream_new(pw_audio_format_probe->core->core, stream_name, probing_stream_props);
	g_assert(pw_audio_format_probe->probing_stream != NULL);
	pw_stream_add_listener(pw_audio_format_probe->probing_stream, &(pw_audio_format_probe->probing_stream_listener), &probing_stream_events, pw_audio_format_probe);

	g_free(stream_name);

	GST_OBJECT_UNLOCK(pw_audio_format_probe);
}


/**
 * gst_pw_audio_format_probe_teardown:
 * @pw_audio_format_probe: a #GstPwAudioFormatProbe
 *
 * Tears down resources inside #GstPwAudioFormatProbe for probing the PipeWire graph.
 *
 * This is the counterpart to gst_pw_audio_format_probe_setup(). It is called once
 * all gst_pw_audio_format_probe_probe_audio_type() are done. After this call,
 * probing can only be done if gst_pw_audio_format_probe_setup() is called again.
 *
 * This also calls gst_pw_audio_format_probe_cancel() in case probing is ongoing.
 *
 * MT safe (it takes the object lock while running).
 */
void gst_pw_audio_format_probe_teardown(GstPwAudioFormatProbe *pw_audio_format_probe)
{
	g_assert(GST_IS_PW_AUDIO_FORMAT_PROBE(pw_audio_format_probe));

	/* If a gst_pw_audio_format_probe_probe_audio_type() call is ingoing, we can't
	 * proceed without running into problems. Cancel that call. Do this _before_
	 * taking the object lock below, otherwise this would produce a deadlock
	 * (since gst_pw_audio_format_probe_probe_audio_type() takes that lock as well). */
	gst_pw_audio_format_probe_cancel(pw_audio_format_probe);

	GST_OBJECT_LOCK(pw_audio_format_probe);

	if (pw_audio_format_probe->probing_stream != NULL)
	{
		pw_stream_destroy(pw_audio_format_probe->probing_stream);
		pw_audio_format_probe->probing_stream = NULL;
	}

	GST_OBJECT_UNLOCK(pw_audio_format_probe);
}


/**
 * gst_pw_audio_format_probe_probe_audio_type:
 * @pw_audio_format_probe: a #GstPwAudioFormatProbe
 * @audio_type: the #GstPipewireAudioType to probe
 * @target_object_id: Target object ID to connect to while probing
 *
 * Probes if the PipeWire graph can handle the given audio type.
 *
 * If the probing stream shall not connect to any particular target
 * object, set target_object_id to PW_ID_ANY.
 *
 * MT safe (it takes the object lock while running).
 *
 * Returns: The result of the probing.
 */
GstPwAudioFormatProbeResult gst_pw_audio_format_probe_probe_audio_type(GstPwAudioFormatProbe *pw_audio_format_probe, GstPipewireAudioType audio_type, guint32 target_object_id)
{
	GstPwAudioFormatProbeResult probe_result;
	int connect_ret;
	gboolean cancelled;
	gboolean can_handle_audio_type;

	g_assert(pw_audio_format_probe != NULL);

	GST_TRACE_OBJECT(pw_audio_format_probe, "about to probe PipeWire graph for \"%s\" audio type support", gst_pw_audio_format_get_audio_type_name(audio_type));

	GST_OBJECT_LOCK(pw_audio_format_probe);

	g_mutex_lock(&(pw_audio_format_probe->mutex));
	cancelled = pw_audio_format_probe->cancelled;
	g_mutex_unlock(&(pw_audio_format_probe->mutex));

	if (G_UNLIKELY(cancelled))
	{
		probe_result = GST_PW_AUDIO_FORMAT_PROBE_RESULT_CANCELLED;
		goto finish;
	}

	pw_audio_format_probe->last_state = PW_STREAM_STATE_UNCONNECTED;

	if (!gst_pw_audio_format_build_spa_pod_for_probing(
		audio_type,
		pw_audio_format_probe->builder_buffer,
		AUDIO_FORMAT_PROBE_BUILDER_BUFFER_SIZE,
		pw_audio_format_probe->format_params
	))
	{
		GST_FIXME_OBJECT(pw_audio_format_probe, "audio type \"%s\" is currently not supported", gst_pw_audio_format_get_audio_type_name(audio_type));
		probe_result = GST_PW_AUDIO_FORMAT_PROBE_RESULT_NOT_SUPPORTED;
		goto finish;
	}

	pw_thread_loop_lock(pw_audio_format_probe->core->loop);
	connect_ret = pw_stream_connect(
		pw_audio_format_probe->probing_stream,
		PW_DIRECTION_OUTPUT,
		target_object_id,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
		pw_audio_format_probe->format_params, 1
	);
	pw_thread_loop_unlock(pw_audio_format_probe->core->loop);

	if (connect_ret == 0)
	{
		g_mutex_lock(&(pw_audio_format_probe->mutex));

		while (pw_audio_format_probe->last_state == PW_STREAM_STATE_UNCONNECTED)
		{
			if (pw_audio_format_probe->cancelled)
			{
				g_mutex_unlock(&(pw_audio_format_probe->mutex));
				GST_DEBUG_OBJECT(pw_audio_format_probe, "probing cancelled");
				probe_result = GST_PW_AUDIO_FORMAT_PROBE_RESULT_CANCELLED;
				goto finish;
			}
			g_cond_wait(&(pw_audio_format_probe->cond), &(pw_audio_format_probe->mutex));
		}

		g_mutex_unlock(&(pw_audio_format_probe->mutex));

		pw_thread_loop_lock(pw_audio_format_probe->core->loop);
		pw_stream_disconnect(pw_audio_format_probe->probing_stream);
		pw_thread_loop_unlock(pw_audio_format_probe->core->loop);
	}
	else
		GST_WARNING_OBJECT(pw_audio_format_probe, "error while trying to connect probing stream: %s (%d)", strerror(-connect_ret), -connect_ret);

	can_handle_audio_type = (connect_ret == 0) && (pw_audio_format_probe->last_state != PW_STREAM_STATE_ERROR);
	probe_result = can_handle_audio_type ? GST_PW_AUDIO_FORMAT_PROBE_RESULT_SUPPORTED : GST_PW_AUDIO_FORMAT_PROBE_RESULT_NOT_SUPPORTED;

	GST_DEBUG_OBJECT(pw_audio_format_probe, "audio type \"%s\" can be handled by the PipeWire graph: %s", gst_pw_audio_format_get_audio_type_name(audio_type), can_handle_audio_type ? "yes" : "no");

finish:
	GST_OBJECT_UNLOCK(pw_audio_format_probe);
	return probe_result;
}


/**
 * gst_pw_audio_format_probe_cancel:
 * @pw_audio_format_probe: a #GstPwAudioFormatProbe
 *
 * Cancels an ongoing gst_pw_audio_format_probe_probe_audio_type() call. Such a call
 * blocks. If something is wrong in the session manager, that call can block indefinitely,
 * so provide an exit strategy by making that function cancellable. This should be
 * cancelled when an element's state switches from PAUSED to READY for example.
 *
 * If no gst_pw_audio_format_probe_probe_audio_type() call is currently ongoing,
 * this does nothing.
 *
 * MT safe. Does NOT take the object lock.
 */
void gst_pw_audio_format_probe_cancel(GstPwAudioFormatProbe *pw_audio_format_probe)
{
	g_assert(pw_audio_format_probe != NULL);

	/* Because gst_pw_audio_format_probe_probe_audio_type() takes the object lock
	 * we have to avoid doing the same here, otherwise we'd run into a deadlock
	 * and could never cancel that function. */

	g_mutex_lock(&(pw_audio_format_probe->mutex));
	pw_audio_format_probe->cancelled = TRUE;
	g_cond_signal(&(pw_audio_format_probe->cond));
	g_mutex_unlock(&(pw_audio_format_probe->mutex));
}

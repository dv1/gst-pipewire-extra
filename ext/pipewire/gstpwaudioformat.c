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
 * is called "raw".
 * Other audio types aren't like that. For example, MPEG frames cannot be arbitarily
 * subdivided/re-partitioned. With such types, it is generally assumed 1 buffer is one
 * logical, indivisible frame that contains audio data in some representation that
 * cannot be considered "raw". Data of such an audio type is called "encoded".
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

/* Turn off -pedantic to mask the "ISO C forbids braced-groups within expressions"
 * warnings that occur because PipeWire uses such braced-groups extensively. */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/aac.h>
#include <spa/pod/pod.h>
#include <spa/utils/result.h>

/* Wrapper around spa_strerror() to avoid "ISO C forbids braced-groups within expressions"
 * warnings in the code below */
static inline char* wrapped_spa_strerror(int err)
{
	return spa_strerror(err);
}

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

	gboolean is_raw;
}
GstPipewireAudioTypeDetails;


// TODO: Currently, only PCM and DSD are supported.
// Fill in compressed audio support (AC-3 etc).


/* Order the PCM formats by quality and performance. 32-bit integer samples
 * are the first choice - they have plenty of dynamic range and are processed
 * efficiently. Next come 32- and 64-bit floating point formats, which are
 * overkill for PCM for 99% of all cases. Next come 24-bit formats (the ones
 * with 8 extra padding bits are preferred). Then come 16-bit formats, which
 * have lower dynamic range, but it still suffices in >95% of all cases, and
 * these samples can be processed very efficiently. After that come formats
 * that are used very rarely. */

#if G_BYTE_ORDER == G_BIG_ENDIAN  /* architecture is big endian based; prefer big endian formats */

#define GST_PW_PCM_FORMATS "{ " \
	"S32BE, S32LE, U32BE, U32LE, " \
	"F32BE, F32LE, F64BE, F64LE, " \
	"S24_32BE, S24_32LE, U24_32BE, U24_32LE, " \
	"S24BE, S24LE, U24BE, U24LE, " \
	"S16BE, S16LE, U16BE, U16LE, " \
	"S20BE, S20LE, U20BE, U20LE, " \
	"S18BE, S18LE, U18BE, U18LE, " \
	"S8, U8 }"

#else /* architecture is little endian based; prefer little endian formats */

#define GST_PW_PCM_FORMATS "{ " \
	"S32LE, S32BE, U32LE, U32BE, " \
	"F32LE, F32BE, F64LE, F64BE, " \
	"S24_32LE, S24_32BE, U24_32LE, U24_32BE, " \
	"S24LE, S24BE, U24LE, U24BE, " \
	"S16LE, S16BE, U16LE, U16BE, " \
	"S20LE, S20BE, U20LE, U20BE, " \
	"S18LE, S18BE, U18LE, U18BE, " \
	"S8, U8 }"

#endif


static GstPipewireAudioTypeDetails const audio_type_details[GST_NUM_PIPEWIRE_AUDIO_TYPES] = {
	/* PCM */
	{
		.name = "PCM",
		.template_caps_string = \
			GST_AUDIO_CAPS_MAKE(GST_PW_PCM_FORMATS) \
			", layout = (string) { interleaved }",
		.is_raw = TRUE
	},

	/* DSD */
	{
		.name = "DSD",
		.template_caps_string = \
			"audio/x-dsd, " \
			"format = (string) { DSD_U8, DSD_U32BE, DSD_U16BE, DSD_U32LE, DSD_U16LE }, "
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		/* DSD data can be subdivided, and the ring buffer can
		 * be used with such data, so mark this as true. However,
		 * DSD cannot be (easily) processed, unlike PCM. */
		.is_raw = TRUE
	},

	/* MP3 */
	{
		.name = "MP3",
		.template_caps_string = \
			"audio/mpeg, " \
			"parsed = (boolean) true, " \
			"mpegversion = (int) 1, " \
			"layer = (int) 3, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* AAC */
	{
		.name = "AAC",
		.template_caps_string = \
			"audio/mpeg, " \
			"framed = (boolean) true, " \
			"mpegversion = (int) { 2, 4 }, " \
			"stream-format = (string) { raw, adts, adif, loas }, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* Vorbis */
	{
		.name = "Vorbis",
		.template_caps_string = \
			"audio/x-vorbis, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* FLAC */
	{
		.name = "FLAC",
		.template_caps_string = \
			"audio/x-flac, " \
			"framed = (boolean) true, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* WMA */
	{
		.name = "WMA",
		.template_caps_string = \
			"audio/x-wma, " \
			"wmaversion = (int) { 1, 2, 3, 4 }, " \
			"block_align = (int) [ 0, MAX ], " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* ALAC */
	{
		.name = "ALAC",
		.template_caps_string = \
			"audio/x-alac, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},

	/* Real Audio */
	{
		.name = "Real Audio",
		.template_caps_string = \
			"audio/x-pn-realaudio, " \
			"rate = (int) [ 1, MAX ], " \
			"channels = (int) [ 1, MAX ]",
		.is_raw = FALSE
	},
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


static gchar const * spa_aac_stream_format_to_string(enum spa_audio_aac_stream_format stream_format)
{
	switch (stream_format)
	{
		case SPA_AUDIO_AAC_STREAM_FORMAT_RAW: return "raw AAC frames";
		case SPA_AUDIO_AAC_STREAM_FORMAT_MP2ADTS: return "ISO/IEC 13818-7 MPEG-2 Audio Data Transport Stream (ADTS)";
		case SPA_AUDIO_AAC_STREAM_FORMAT_MP4ADTS: return "ISO/IEC 14496-3 MPEG-4 Audio Data Transport Stream (ADTS)";
		case SPA_AUDIO_AAC_STREAM_FORMAT_MP4LOAS: return "ISO/IEC 14496-3 Low Overhead Audio Stream (LOAS)";
		case SPA_AUDIO_AAC_STREAM_FORMAT_MP4LATM: return "ISO/IEC 14496-3 Low Overhead Audio Transport Multiplex (LATM)";
		case SPA_AUDIO_AAC_STREAM_FORMAT_ADIF: return "ISO/IEC 14496-3 Audio Data Interchange Format (ADIF)";
		case SPA_AUDIO_AAC_STREAM_FORMAT_MP4FF: return "ISO/IEC 14496-12 MPEG-4 file format";
		default: return "<unknown>";
	}
}


static gchar const * spa_wma_profile_to_string(enum spa_audio_wma_profile profile)
{
	switch (profile)
	{
		case SPA_AUDIO_WMA_PROFILE_WMA7: return "WMA 7";
		case SPA_AUDIO_WMA_PROFILE_WMA8: return "WMA 8";
		case SPA_AUDIO_WMA_PROFILE_WMA9: return "WMA 9";
		case SPA_AUDIO_WMA_PROFILE_WMA10: return "WMA 10";
		case SPA_AUDIO_WMA_PROFILE_WMA9_PRO: return "WMA 9 Pro";
		case SPA_AUDIO_WMA_PROFILE_WMA9_LOSSLESS: return "WMA 9 Lossless";
		case SPA_AUDIO_WMA_PROFILE_WMA10_LOSSLESS: return "WMA 10 Lossless";
		default: return "<unknown>";
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
 * @gst_pipewire_dsd_convert:
 * @input_data Pointer to memory block to read input DSD data from for conversion.
 * @output_data Pointer to memory block where the converted DSD data shall be written to.
 * @input_format DSD format of the input data.
 * @output_format DSD format of the output data.
 * @num_output_bytes Number of output bytes.
 * @num_channels Number of DSD input data channels. Must be >0.
 *
 * Converts the input DSD data from input_data, converting from the input_format to the
 * output_format, and writes the converted result to output_data. If both formats are
 * equal, this just memcpy()'s the first num_output_bytes of input_data to output_data.
 *
 * num_output_bytes must not be greater than the size of the memory block that input_data
 * points to, but must be at least as large as the stride of the output data (the stride
 * being gst_pipewire_dsd_format_get_width(output_format) * num_channels), and needs to be
 * an integer multiple of that stride.
 */
void gst_pipewire_dsd_convert(guint8 const *input_data, guint8 *output_data, GstPipewireDsdFormat input_format, GstPipewireDsdFormat output_format, gsize num_output_bytes, gint num_channels)
{
	guint out_index;
	guint in_word_width, out_word_width;
	guint in_stride, out_stride;
	gboolean input_is_le = gst_pipewire_dsd_format_is_le(input_format);
	gboolean output_is_le = gst_pipewire_dsd_format_is_le(output_format);

	if (input_format == output_format)
	{
		memcpy(output_data, input_data, num_output_bytes);
		return;
	}

	in_word_width = gst_pipewire_dsd_format_get_width(input_format);
	out_word_width = gst_pipewire_dsd_format_get_width(output_format);
	in_stride = in_word_width * num_channels;
	out_stride = out_word_width * num_channels;

	for (out_index = 0; out_index < num_output_bytes; ++out_index)
	{
		guint in_word_index, in_word_offset;
		guint out_word_index, out_word_offset;
		guint in_index;
		guint channel_nr;
		guint position;

		out_word_index = out_index / out_word_width;
		out_word_offset = out_index - out_word_index * out_word_width;
		if (output_is_le)
			out_word_offset = out_word_width - 1 - out_word_offset;

		channel_nr = out_word_index % num_channels;
		position = (out_index / out_stride) * out_word_width + out_word_offset;

		in_word_index = (position / in_word_width) * in_stride + channel_nr * in_word_width;
		in_word_offset = position % in_word_width;
		if (input_is_le)
			in_word_offset = in_word_width - 1 - in_word_offset;

		in_index = in_word_index + in_word_offset;

		output_data[out_index] = input_data[in_index];
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
		gst_structure_fixate_field_nearest_int(s, "rate", GST_PIPEWIRE_DSD_DSD64_BYTE_RATE);
	}
	else
	{
		// TODO: Add code for more non-PCM types here
	}

	/* For the remaining fields, use default fixation. */
	return gst_caps_fixate(caps);
}


/**
 * gst_pw_audio_format_data_is_raw:
 * @audio_type Audio type to check.
 *
 * Returns: TRUE if data of the given type is raw.
 */
gboolean gst_pw_audio_format_data_is_raw(GstPipewireAudioType audio_type)
{
	g_assert(((gint)audio_type) < GST_NUM_PIPEWIRE_AUDIO_TYPES);
	return audio_type_details[(gint)audio_type].is_raw;
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
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM;
	else if (g_strcmp0(media_type, "audio/x-dsd") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_DSD;
	else if (g_strcmp0(media_type, "audio/mpeg") == 0)
		/* This also includes AAC. MP3 and AAC are distinguished
		 * by the mpegversion caps field further below. */
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_MP3;
	else if (g_strcmp0(media_type, "audio/x-vorbis") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_VORBIS;
	else if (g_strcmp0(media_type, "audio/x-flac") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_FLAC;
	else if (g_strcmp0(media_type, "audio/x-wma") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_WMA;
	else if (g_strcmp0(media_type, "audio/x-alac") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_ALAC;
	else if (g_strcmp0(media_type, "audio/x-pn-realaudio") == 0)
		pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO;
	else
	{
		GST_ERROR_OBJECT(parent, "unsupported media type \"%s\"", media_type);
		goto error;
	}

	switch (pw_audio_format->audio_type)
	{
		case GST_PIPEWIRE_AUDIO_TYPE_PCM:
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

			break;
		}

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
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

			break;
		}

		case GST_PIPEWIRE_AUDIO_TYPE_MP3:
		case GST_PIPEWIRE_AUDIO_TYPE_VORBIS:
		case GST_PIPEWIRE_AUDIO_TYPE_FLAC:
		case GST_PIPEWIRE_AUDIO_TYPE_WMA:
		case GST_PIPEWIRE_AUDIO_TYPE_ALAC:
		case GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO:
		{
			/* All encoded formats have a rate and channels field in their caps.
			 * Some have additional information, such as the profile in WMA. */

			GstPipewireEncodedAudioInfo *encoded_audio_info = &(pw_audio_format->info.encoded_audio_info);

			if (!gst_structure_get_int(fmt_structure, "rate", &(encoded_audio_info->rate)))
			{
				GST_ERROR_OBJECT(parent, "caps have no rate field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
				goto error;
			}

			if (encoded_audio_info->rate < 1)
			{
				GST_ERROR_OBJECT(parent, "caps have invalid rate field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
				goto error;
			}

			if (!gst_structure_get_int(fmt_structure, "channels", &(encoded_audio_info->channels)))
			{
				GST_ERROR_OBJECT(parent, "caps have no channels field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
				goto error;
			}

			if (encoded_audio_info->channels < 1)
			{
				GST_ERROR_OBJECT(parent, "caps have invalid channels field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
				goto error;
			}

			/* Handle additional, format specific caps. */
			switch (pw_audio_format->audio_type)
			{
				case GST_PIPEWIRE_AUDIO_TYPE_MP3:
				{
					gint mpegversion;

					if (!gst_structure_get_int(fmt_structure, "mpegversion", &mpegversion))
					{
						GST_ERROR_OBJECT(parent, "caps have no mpegversion field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
						goto error;
					}

					switch (mpegversion)
					{
						case 1:
							pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_MP3;
							break;

						case 2:
						case 4:
						{
							gchar const *stream_format_str = gst_structure_get_string(fmt_structure, "stream-format");
							if (stream_format_str == NULL)
							{
								GST_ERROR_OBJECT(parent, "caps describe AAC content, but stream-format field is missing; caps: %" GST_PTR_FORMAT, (gpointer)caps);
								goto error;
							}

							if (g_strcmp0(stream_format_str, "raw") == 0)
								encoded_audio_info->details.aac.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_RAW;
							else if (g_strcmp0(stream_format_str, "adts") == 0)
								encoded_audio_info->details.aac.stream_format = (mpegversion == 2) ? SPA_AUDIO_AAC_STREAM_FORMAT_MP2ADTS : SPA_AUDIO_AAC_STREAM_FORMAT_MP4ADTS;
							else if (g_strcmp0(stream_format_str, "adif") == 0)
								encoded_audio_info->details.aac.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_ADIF;
							else if (g_strcmp0(stream_format_str, "loas") == 0)
								encoded_audio_info->details.aac.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_MP4LOAS;
							else
							{
								GST_ERROR_OBJECT(parent, "caps describe AAC content, but its stream-format is unsupported; caps: %" GST_PTR_FORMAT, (gpointer)caps);
								goto error;
							}

							pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_AAC;

							break;
						}

						default:
							GST_ERROR_OBJECT(parent, "caps contain unsupported MPEG version; caps: %" GST_PTR_FORMAT, (gpointer)caps);
							goto error;
					}

					break;
				}

				case GST_PIPEWIRE_AUDIO_TYPE_WMA:
				{
					gint block_align;
					gint wmaversion;

					if (!gst_structure_get_int(fmt_structure, "block_align", &block_align))
					{
						GST_ERROR_OBJECT(parent, "caps have no block_align field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
						goto error;
					}

					if (!gst_structure_get_int(fmt_structure, "wmaversion", &wmaversion))
					{
						GST_ERROR_OBJECT(parent, "caps have no wmaversion field; caps: %" GST_PTR_FORMAT, (gpointer)caps);
						goto error;
					}

					encoded_audio_info->details.wma.block_align = block_align;

					switch (wmaversion)
					{
						case 1: encoded_audio_info->details.wma.profile = SPA_AUDIO_WMA_PROFILE_WMA7; break;
						case 2: encoded_audio_info->details.wma.profile = SPA_AUDIO_WMA_PROFILE_WMA8; break;
						case 3: encoded_audio_info->details.wma.profile = SPA_AUDIO_WMA_PROFILE_WMA9; break;
						case 4: encoded_audio_info->details.wma.profile = SPA_AUDIO_WMA_PROFILE_WMA9_LOSSLESS; break;
						default:
							GST_ERROR_OBJECT(parent, "caps contain unsupported WMA version; caps: %" GST_PTR_FORMAT, (gpointer)caps);
							goto error;
					}

					break;
				}

				default:
					break;
			}

			break;
		}

		default:
			g_assert_not_reached();
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
				case GST_AUDIO_FORMAT_S8: spa_audio_format = SPA_AUDIO_FORMAT_S8; break;
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

				case GST_AUDIO_FORMAT_U8: spa_audio_format = SPA_AUDIO_FORMAT_U8; break;
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
				case GST_PIPEWIRE_DSD_FORMAT_DSD_UNKNOWN: interleave = 0; break;
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

#define ENCODED_FORMAT_CASE(TYPE_NAME, name, ...) \
		case GST_PIPEWIRE_AUDIO_TYPE_##TYPE_NAME : \
		{ \
			struct spa_audio_info_##name name ##_info = { \
				.rate = pw_audio_format->info.encoded_audio_info.rate, \
				.channels = pw_audio_format->info.encoded_audio_info.channels, \
				__VA_ARGS__ \
			}; \
\
			*pod = spa_format_audio_##name##_build(&builder, SPA_PARAM_EnumFormat, & name##_info); \
\
			break; \
		}

		ENCODED_FORMAT_CASE(MP3, mp3)
		ENCODED_FORMAT_CASE(AAC, aac,
			.stream_format = (enum spa_audio_aac_stream_format)(pw_audio_format->info.encoded_audio_info.details.aac.stream_format)
			/* bitrate field is not needed for decoding */
		)
		ENCODED_FORMAT_CASE(VORBIS, vorbis)
		ENCODED_FORMAT_CASE(FLAC, flac)
		ENCODED_FORMAT_CASE(WMA, wma,
			.block_align = pw_audio_format->info.encoded_audio_info.details.wma.block_align,
			.profile = (enum spa_audio_wma_profile)(pw_audio_format->info.encoded_audio_info.details.wma.profile),
		)
		ENCODED_FORMAT_CASE(ALAC, alac)
		ENCODED_FORMAT_CASE(REAL_AUDIO, ra)

		default:
			goto error;
	}

	return TRUE;

#undef ENCODED_FORMAT_CASE

error:
	return FALSE;
}


gboolean gst_pw_audio_format_from_spa_pod_with_format_param(
	GstPwAudioFormat *pw_audio_format,
	GstObject *parent,
	struct spa_pod const *format_param_pod
)
{
	struct spa_audio_info info = { 0 };
	int err;

	if ((err = spa_format_parse(format_param_pod, &info.media_type, &info.media_subtype)) < 0)
	{
		GST_ERROR_OBJECT(parent, "could not parse format: %s (%d)", wrapped_spa_strerror(err), -err);
		goto error;
	}

	if (info.media_type != SPA_MEDIA_TYPE_audio)
	{
		GST_DEBUG_OBJECT(parent, "this isn't an audio format - ignoring");
		goto error;
	}

	switch (info.media_subtype)
	{
		case SPA_MEDIA_SUBTYPE_raw:
		{
			GstAudioFormat gst_audio_format;
			GstAudioChannelPosition gst_channel_positions[SPA_AUDIO_MAX_CHANNELS];

			if (spa_format_audio_raw_parse(format_param_pod, &(info.info.raw)) < 0)
			{
				GST_ERROR_OBJECT(parent, "could not parse PCM format: %s (%d)", wrapped_spa_strerror(err), -err);
				goto error;
			}

			switch (info.info.raw.format)
			{
				case SPA_AUDIO_FORMAT_S16_LE: gst_audio_format = GST_AUDIO_FORMAT_S16LE; break;
				case SPA_AUDIO_FORMAT_S16_BE: gst_audio_format = GST_AUDIO_FORMAT_S16BE; break;
				case SPA_AUDIO_FORMAT_S18_LE: gst_audio_format = GST_AUDIO_FORMAT_S18LE; break;
				case SPA_AUDIO_FORMAT_S18_BE: gst_audio_format = GST_AUDIO_FORMAT_S18BE; break;
				case SPA_AUDIO_FORMAT_S20_LE: gst_audio_format = GST_AUDIO_FORMAT_S20LE; break;
				case SPA_AUDIO_FORMAT_S20_BE: gst_audio_format = GST_AUDIO_FORMAT_S20BE; break;
				case SPA_AUDIO_FORMAT_S24_LE: gst_audio_format = GST_AUDIO_FORMAT_S24LE; break;
				case SPA_AUDIO_FORMAT_S24_BE: gst_audio_format = GST_AUDIO_FORMAT_S24BE; break;
				case SPA_AUDIO_FORMAT_S24_32_LE: gst_audio_format = GST_AUDIO_FORMAT_S24_32LE; break;
				case SPA_AUDIO_FORMAT_S24_32_BE: gst_audio_format = GST_AUDIO_FORMAT_S24_32BE; break;
				case SPA_AUDIO_FORMAT_S32_LE: gst_audio_format = GST_AUDIO_FORMAT_S32LE; break;
				case SPA_AUDIO_FORMAT_S32_BE: gst_audio_format = GST_AUDIO_FORMAT_S32BE; break;

				case SPA_AUDIO_FORMAT_U16_LE: gst_audio_format = GST_AUDIO_FORMAT_U16LE; break;
				case SPA_AUDIO_FORMAT_U16_BE: gst_audio_format = GST_AUDIO_FORMAT_U16BE; break;
				case SPA_AUDIO_FORMAT_U18_LE: gst_audio_format = GST_AUDIO_FORMAT_U18LE; break;
				case SPA_AUDIO_FORMAT_U18_BE: gst_audio_format = GST_AUDIO_FORMAT_U18BE; break;
				case SPA_AUDIO_FORMAT_U20_LE: gst_audio_format = GST_AUDIO_FORMAT_U20LE; break;
				case SPA_AUDIO_FORMAT_U20_BE: gst_audio_format = GST_AUDIO_FORMAT_U20BE; break;
				case SPA_AUDIO_FORMAT_U24_LE: gst_audio_format = GST_AUDIO_FORMAT_U24LE; break;
				case SPA_AUDIO_FORMAT_U24_BE: gst_audio_format = GST_AUDIO_FORMAT_U24BE; break;
				case SPA_AUDIO_FORMAT_U24_32_LE: gst_audio_format = GST_AUDIO_FORMAT_U24_32LE; break;
				case SPA_AUDIO_FORMAT_U24_32_BE: gst_audio_format = GST_AUDIO_FORMAT_U24_32BE; break;
				case SPA_AUDIO_FORMAT_U32_LE: gst_audio_format = GST_AUDIO_FORMAT_U32LE; break;
				case SPA_AUDIO_FORMAT_U32_BE: gst_audio_format = GST_AUDIO_FORMAT_U32BE; break;

				case SPA_AUDIO_FORMAT_F32_LE: gst_audio_format = GST_AUDIO_FORMAT_F32LE; break;
				case SPA_AUDIO_FORMAT_F32_BE: gst_audio_format = GST_AUDIO_FORMAT_F32BE; break;
				case SPA_AUDIO_FORMAT_F64_LE: gst_audio_format = GST_AUDIO_FORMAT_F64LE; break;
				case SPA_AUDIO_FORMAT_F64_BE: gst_audio_format = GST_AUDIO_FORMAT_F64BE; break;

				default:
					GST_ERROR_OBJECT(parent, "unsupported SPA audio format %d", (gint)(info.info.raw.format));
					goto error;
			}

			spa_to_gst_channel_positions(info.info.raw.position, gst_channel_positions, info.info.raw.channels);

			pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_PCM;

			gst_audio_info_set_format(
				&(pw_audio_format->info.pcm_audio_info),
				gst_audio_format,
				info.info.raw.rate,
				info.info.raw.channels,
				gst_channel_positions
			);

			break;
		}

		case SPA_MEDIA_SUBTYPE_dsd:
		{
			if (spa_format_audio_dsd_parse(format_param_pod, &(info.info.dsd)) < 0)
			{
				GST_ERROR_OBJECT(parent, "could not parse DSD format: %s (%d)", wrapped_spa_strerror(err), -err);
				goto error;
			}

			switch (info.info.dsd.interleave)
			{
				case 1: pw_audio_format->info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_U8; break;
				case -2: pw_audio_format->info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_U16LE; break;
				case +2: pw_audio_format->info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_U16BE; break;
				case -4: pw_audio_format->info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_U32LE; break;
				case +4: pw_audio_format->info.dsd_audio_info.format = GST_PIPEWIRE_DSD_FORMAT_DSD_U32BE; break;
				default:
					GST_ERROR_OBJECT(parent, "unsupported SPA DSD interleave quantity %" G_GINT32_FORMAT, (gint32)(info.info.dsd.interleave));
					goto error;
			}

			pw_audio_format->info.dsd_audio_info.rate = info.info.dsd.rate;
			pw_audio_format->info.dsd_audio_info.channels = info.info.dsd.channels;
			spa_to_gst_channel_positions(info.info.dsd.position, pw_audio_format->info.dsd_audio_info.positions, info.info.dsd.channels);

			pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_DSD;

			break;
		}

#define COMMON_ENCODED_FORMAT_PROCESSING(LOWER_NAME, UPPER_NAME) \
		G_STMT_START { \
			if (spa_format_audio_##LOWER_NAME##_parse(format_param_pod, &(info.info.LOWER_NAME)) < 0) \
			{ \
				GST_ERROR_OBJECT(parent, "could not parse " #UPPER_NAME " format: %s (%d)", wrapped_spa_strerror(err), -err); \
				goto error; \
			} \
\
			pw_audio_format->info.encoded_audio_info.rate = info.info.LOWER_NAME.rate; \
			pw_audio_format->info.encoded_audio_info.channels = info.info.LOWER_NAME.channels; \
 \
			pw_audio_format->audio_type = GST_PIPEWIRE_AUDIO_TYPE_ ##UPPER_NAME; \
		} G_STMT_END

		case SPA_MEDIA_SUBTYPE_mp3:
			COMMON_ENCODED_FORMAT_PROCESSING(mp3, MP3);
			break;

		case SPA_MEDIA_SUBTYPE_aac:
		{
			COMMON_ENCODED_FORMAT_PROCESSING(aac, AAC);

			switch (info.info.aac.stream_format)
			{
				case SPA_AUDIO_AAC_STREAM_FORMAT_RAW:
				case SPA_AUDIO_AAC_STREAM_FORMAT_MP2ADTS:
				case SPA_AUDIO_AAC_STREAM_FORMAT_MP4ADTS:
				case SPA_AUDIO_AAC_STREAM_FORMAT_ADIF:
				case SPA_AUDIO_AAC_STREAM_FORMAT_MP4LOAS:
					pw_audio_format->info.encoded_audio_info.details.aac.stream_format = info.info.aac.stream_format;
					break;

				default:
					GST_ERROR_OBJECT(
						parent,
						"could not parse AAC format: unsupported stream format %" G_GUINT32_FORMAT,
						(guint32)(info.info.aac.stream_format)
					);
					goto error;
			}

			break;
		}

		case SPA_MEDIA_SUBTYPE_vorbis:
			COMMON_ENCODED_FORMAT_PROCESSING(vorbis, VORBIS);
			break;

		case SPA_MEDIA_SUBTYPE_flac:
			COMMON_ENCODED_FORMAT_PROCESSING(flac, FLAC);
			break;

		case SPA_MEDIA_SUBTYPE_wma:
		{
			COMMON_ENCODED_FORMAT_PROCESSING(wma, WMA);

			pw_audio_format->info.encoded_audio_info.details.wma.block_align = info.info.wma.block_align;

			switch (info.info.wma.profile)
			{
				case SPA_AUDIO_WMA_PROFILE_WMA7:
				case SPA_AUDIO_WMA_PROFILE_WMA8:
				case SPA_AUDIO_WMA_PROFILE_WMA9:
				case SPA_AUDIO_WMA_PROFILE_WMA9_LOSSLESS:
					pw_audio_format->info.encoded_audio_info.details.wma.profile = info.info.wma.profile;
					break;

				default:
					GST_ERROR_OBJECT(
						parent,
						"could not parse WMA format: unsupported profile %" G_GUINT32_FORMAT,
						(guint32)(info.info.wma.profile)
					);
					goto error;
			}

			break;
		}

		case SPA_MEDIA_SUBTYPE_alac:
			COMMON_ENCODED_FORMAT_PROCESSING(alac, ALAC);
			break;

		case SPA_MEDIA_SUBTYPE_ra:
			COMMON_ENCODED_FORMAT_PROCESSING(ra, REAL_AUDIO);
			break;

		default:
			GST_ERROR_OBJECT(parent, "unsupported SPA media subtype %#010" G_GINT32_MODIFIER "x", (guint32)(info.media_subtype));
			goto error;
	}

	return TRUE;

#undef COMMON_ENCODED_FORMAT_PROCESSING

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
					/* Fixate the sample format, but leave the rest unfixated.
					 * This is sufficient for probing. */
					.format = SPA_AUDIO_FORMAT_S16,
					.channels = 0,
					.rate = 0
				)
			);
			break;

		case GST_PIPEWIRE_AUDIO_TYPE_DSD:
			*pod = spa_format_audio_dsd_build(
				&builder, SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_DSD_INIT(
					.bitorder = SPA_PARAM_BITORDER_unknown,
					.interleave = 0,
					.channels = 0,
					.rate = 0
				)
			);
			break;

			/* Use 44.1 kHz stereo as formats to probe for. We really
			 * just want to know if the format is supported at all, so
			 * these are picked as safe defaults - any device capable
			 * of playing that format must be able to handle this. */
#define ENCODED_FORMAT_CASE(TYPE_NAME, SPA_TYPE_NAME, name, ...) \
		case GST_PIPEWIRE_AUDIO_TYPE_##TYPE_NAME : \
			*pod = spa_format_audio_##name##_build( \
				&builder, SPA_PARAM_EnumFormat, \
				&SPA_AUDIO_INFO_##SPA_TYPE_NAME##_INIT( \
					.channels = 2, \
					.rate = 44100, \
					__VA_ARGS__ \
				) \
			); \
			break;

		ENCODED_FORMAT_CASE(MP3, MP3, mp3)
		ENCODED_FORMAT_CASE(AAC, AAC, aac,
			.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_RAW
		)
		ENCODED_FORMAT_CASE(VORBIS, VORBIS, vorbis)
		ENCODED_FORMAT_CASE(FLAC, FLAC, flac)
		ENCODED_FORMAT_CASE(WMA, WMA, wma,
			.profile = SPA_AUDIO_WMA_PROFILE_WMA8,
			.block_align = 16384,
		)
		ENCODED_FORMAT_CASE(ALAC, ALAC, alac)
		ENCODED_FORMAT_CASE(REAL_AUDIO, RA, ra)

		default:
			return FALSE;
	}

#undef ENCODED_FORMAT_CASE

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
 * NOTE: In DSD, the "stride" equals the DSD format width multiplied
 * by the number of channels. For example, DSD_U32BE has 4 bytes,
 * and with 2 channels, this means the stride equals 8 bytes.
 * However, in DSD, the format specifies the *grouping* of DSD bits.
 * There is no real "sample format" in DSD. DSD_U32BE contains 32
 * DSD bits, while DSD_U8 contains 8 DSD bits. This means that unlike
 * with PCM, different DSD formats imply different playtimes. For
 * example, DSD_U32BE covers 4 times as much playtime as DSD_U8. This
 * is important to keep in mind when converting between DSD formats.
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

		/* Stride has no real meaning in encoded audio. Just use 1 byte as stride. */
		case GST_PIPEWIRE_AUDIO_TYPE_MP3:
		case GST_PIPEWIRE_AUDIO_TYPE_AAC:
		case GST_PIPEWIRE_AUDIO_TYPE_VORBIS:
		case GST_PIPEWIRE_AUDIO_TYPE_FLAC:
		case GST_PIPEWIRE_AUDIO_TYPE_WMA:
		case GST_PIPEWIRE_AUDIO_TYPE_ALAC:
		case GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO:
			return 1;

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

#define DEFAULT_ENCODED_FORMAT_CASE(TYPE_NAME, STR) \
		case GST_PIPEWIRE_AUDIO_TYPE_##TYPE_NAME: \
		{ \
			return gst_info_strdup_printf( \
				STR ": rate %d channels %d", \
				pw_audio_format->info.encoded_audio_info.rate, \
				pw_audio_format->info.encoded_audio_info.channels \
			); \
		}

		DEFAULT_ENCODED_FORMAT_CASE(MP3, "MP3")
		DEFAULT_ENCODED_FORMAT_CASE(VORBIS, "Vorbis")
		DEFAULT_ENCODED_FORMAT_CASE(FLAC, "FLAC")
		DEFAULT_ENCODED_FORMAT_CASE(ALAC, "ALAC")
		DEFAULT_ENCODED_FORMAT_CASE(REAL_AUDIO, "Real Audio")

		case GST_PIPEWIRE_AUDIO_TYPE_AAC:
		{
			return gst_info_strdup_printf(
				"AAC: rate %d channels %d stream format \"%s\"",
				pw_audio_format->info.encoded_audio_info.rate,
				pw_audio_format->info.encoded_audio_info.channels,
				spa_aac_stream_format_to_string(pw_audio_format->info.encoded_audio_info.details.aac.stream_format)
			);
		}

		case GST_PIPEWIRE_AUDIO_TYPE_WMA:
		{
			return gst_info_strdup_printf(
				"WMA: rate %d channels %d block-align %" G_GUINT32_FORMAT " profile \"%s\"",
				pw_audio_format->info.encoded_audio_info.rate,
				pw_audio_format->info.encoded_audio_info.channels,
				pw_audio_format->info.encoded_audio_info.details.wma.block_align,
				spa_wma_profile_to_string(pw_audio_format->info.encoded_audio_info.details.wma.profile)
			);
		}

#undef DEFAULT_ENCODED_FORMAT_CASE

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

		case GST_PIPEWIRE_AUDIO_TYPE_MP3:
		case GST_PIPEWIRE_AUDIO_TYPE_AAC:
		case GST_PIPEWIRE_AUDIO_TYPE_VORBIS:
		case GST_PIPEWIRE_AUDIO_TYPE_FLAC:
		case GST_PIPEWIRE_AUDIO_TYPE_WMA:
		case GST_PIPEWIRE_AUDIO_TYPE_ALAC:
		case GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO:
		{
			GstPipewireEncodedAudioInfo const *info = &(pw_audio_format->info.encoded_audio_info);
			return gst_util_uint64_scale_int(duration, info->rate, GST_SECOND);
		}

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

		case GST_PIPEWIRE_AUDIO_TYPE_MP3:
		case GST_PIPEWIRE_AUDIO_TYPE_AAC:
		case GST_PIPEWIRE_AUDIO_TYPE_VORBIS:
		case GST_PIPEWIRE_AUDIO_TYPE_FLAC:
		case GST_PIPEWIRE_AUDIO_TYPE_WMA:
		case GST_PIPEWIRE_AUDIO_TYPE_ALAC:
		case GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO:
		{
			GstPipewireEncodedAudioInfo const *info = &(pw_audio_format->info.encoded_audio_info);
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

	GstPwAudioFormat pw_audio_format;

	enum pw_stream_state last_state;

	gboolean cancelled;

	guint64 quantum_size;
	gint32 stride;
};


struct _GstPwAudioFormatProbeClass
{
	GstObjectClass parent_class;
};


G_DEFINE_TYPE(GstPwAudioFormatProbe, gst_pw_audio_format_probe, GST_TYPE_OBJECT)


static void gst_pw_audio_format_probe_dispose(GObject *object);
static void gst_pw_audio_format_probe_finalize(GObject *object);

static void gst_pw_probing_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error);
static void gst_pw_probing_param_changed(void *data, uint32_t id, const struct spa_pod *param);
static void gst_pw_probing_io_changed(void *data, uint32_t id, void *area, uint32_t size);
static void gst_pw_probing_process_stream(void *data);


static const struct pw_stream_events probing_stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = gst_pw_probing_state_changed,
	.param_changed = gst_pw_probing_param_changed,
	.io_changed = gst_pw_probing_io_changed,
	.process = gst_pw_probing_process_stream
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


static void gst_pw_probing_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state new_state, const char *error)
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


static void gst_pw_probing_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE_CAST(data);

	if ((id != SPA_PARAM_Format) || (param == NULL))
		return;

	gst_pw_audio_format_from_spa_pod_with_format_param(
		&(self->pw_audio_format),
		GST_OBJECT_CAST(self),
		param
	);

	self->stride = gst_pw_audio_format_get_stride(&(self->pw_audio_format));

	{
		gchar *format_str = gst_pw_audio_format_to_string(&(self->pw_audio_format));
		GST_DEBUG_OBJECT(self, "format param changed; parsing and analyzing;  stride: %" G_GINT32_FORMAT "  audio format details: %s", self->stride, format_str);
		g_free(format_str);
	}
}


static void gst_pw_probing_io_changed(void *data, uint32_t id, void *area, G_GNUC_UNUSED uint32_t size)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE_CAST(data);

	switch (id)
	{
		case SPA_IO_Position:
		{
			struct spa_io_position *position = (struct spa_io_position *)area;
			if (position != NULL)
			{
				self->quantum_size = position->clock.duration;
				GST_DEBUG_OBJECT(
					self,
					"got new quantum size %" G_GUINT64_FORMAT " from clock duration in new SPA IO position",
					self->quantum_size
				);
			}

			break;
		}

		default:
			break;
	}
}


static void gst_pw_probing_process_stream(void *data)
{
	GstPwAudioFormatProbe *self = GST_PW_AUDIO_FORMAT_PROBE_CAST(data);
	struct pw_buffer *pw_buf;
	struct spa_data *inner_spa_data;

	pw_buf = pw_stream_dequeue_buffer(self->probing_stream);
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

	inner_spa_data->chunk->offset = 0;
	inner_spa_data->chunk->size = self->quantum_size * self->stride;
	inner_spa_data->chunk->stride = self->stride;

	GST_TRACE_OBJECT(self, "producing %" G_GSIZE_FORMAT " byte(s) of silence", (gsize)(inner_spa_data->chunk->size));

	gst_pw_audio_format_write_silence_frames(&(self->pw_audio_format), inner_spa_data->data, self->quantum_size);

finish:
	pw_stream_queue_buffer(self->probing_stream, pw_buf);
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
 * @probed_details: Optional output value for storing probed details in
 *
 * Probes if the PipeWire graph can handle the given audio type.
 *
 * If the probing stream shall not connect to any particular target
 * object, set target_object_id to PW_ID_ANY.
 *
 * If probed_details is non-NULL, then format details about the
 * probed type are filled in. These details are useful for having
 * preferred format details for the given type.
 *
 * MT safe (it takes the object lock while running).
 *
 * Returns: The result of the probing.
 */
GstPwAudioFormatProbeResult gst_pw_audio_format_probe_probe_audio_type(GstPwAudioFormatProbe *pw_audio_format_probe, GstPipewireAudioType audio_type, guint32 target_object_id, GstPwAudioFormat **probed_details)
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
		gboolean cancelled = FALSE;

		g_mutex_lock(&(pw_audio_format_probe->mutex));

		while (pw_audio_format_probe->last_state == PW_STREAM_STATE_UNCONNECTED)
		{
			if (pw_audio_format_probe->cancelled)
			{
				GST_DEBUG_OBJECT(pw_audio_format_probe, "probing cancelled");
				probe_result = GST_PW_AUDIO_FORMAT_PROBE_RESULT_CANCELLED;
				cancelled = TRUE;
				break;
			}
			g_cond_wait(&(pw_audio_format_probe->cond), &(pw_audio_format_probe->mutex));
		}

		g_mutex_unlock(&(pw_audio_format_probe->mutex));

		pw_thread_loop_lock(pw_audio_format_probe->core->loop);
		pw_stream_disconnect(pw_audio_format_probe->probing_stream);
		pw_thread_loop_unlock(pw_audio_format_probe->core->loop);

		if (cancelled)
			goto finish;
	}
	else
		GST_WARNING_OBJECT(pw_audio_format_probe, "error while trying to connect probing stream: %s (%d)", strerror(-connect_ret), -connect_ret);

	can_handle_audio_type = (connect_ret == 0) && (pw_audio_format_probe->last_state != PW_STREAM_STATE_ERROR);
	probe_result = can_handle_audio_type ? GST_PW_AUDIO_FORMAT_PROBE_RESULT_SUPPORTED : GST_PW_AUDIO_FORMAT_PROBE_RESULT_NOT_SUPPORTED;

	if (probed_details != NULL)
		*probed_details = &(pw_audio_format_probe->pw_audio_format);

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

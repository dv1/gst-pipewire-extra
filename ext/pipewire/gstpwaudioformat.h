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

#ifndef __GST_PW_AUDIO_FORMAT_H__
#define __GST_PW_AUDIO_FORMAT_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstdsd.h>
#include "gstpipewirecore.h"


G_BEGIN_DECLS


struct spa_pod;


typedef enum
{
	GST_PIPEWIRE_AUDIO_TYPE_PCM = 0,
	GST_PIPEWIRE_AUDIO_TYPE_DSD,
	GST_PIPEWIRE_AUDIO_TYPE_MP3,
	GST_PIPEWIRE_AUDIO_TYPE_AAC,
	GST_PIPEWIRE_AUDIO_TYPE_VORBIS,
	GST_PIPEWIRE_AUDIO_TYPE_FLAC,
	GST_PIPEWIRE_AUDIO_TYPE_WMA,
	GST_PIPEWIRE_AUDIO_TYPE_ALAC,
	GST_PIPEWIRE_AUDIO_TYPE_REAL_AUDIO,

	GST_NUM_PIPEWIRE_AUDIO_TYPES
}
GstPipewireAudioType;


typedef struct
{
	gint rate;
	gint channels;

	union
	{
		struct
		{
			guint32 block_align;
			guint32 profile;
		} wma;

		struct
		{
			guint32 stream_format;
		} aac;
	} details;
}
GstPipewireEncodedAudioInfo;

typedef struct
{
	GstPipewireAudioType audio_type;

	union
	{
		GstAudioInfo pcm_audio_info;
		GstDsdInfo dsd_audio_info;
		GstPipewireEncodedAudioInfo encoded_audio_info;
	} info;
}
GstPwAudioFormat;


gchar const *gst_pw_audio_format_get_audio_type_name(GstPipewireAudioType audio_type);

GstCaps* gst_pw_audio_format_get_template_caps(void);
GstCaps* gst_pw_audio_format_get_template_caps_for_type(GstPipewireAudioType audio_type);

GstCaps* gst_pw_audio_format_fixate_caps(GstCaps *caps);

gboolean gst_pw_audio_format_data_is_raw(GstPipewireAudioType audio_type);
gboolean gst_pw_audio_format_from_caps(GstPwAudioFormat *pw_audio_format, GstObject *parent, GstCaps *caps);
gboolean gst_pw_audio_format_to_spa_pod(
	GstPwAudioFormat const *pw_audio_format,
	GstObject *parent,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
);
gboolean gst_pw_audio_format_from_spa_pod_with_format_param(
	GstPwAudioFormat *pw_audio_format,
	GstObject *parent,
	struct spa_pod const *format_param_pod
);
gboolean gst_pw_audio_format_build_spa_pod_for_probing(
	GstPipewireAudioType audio_type,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
);
gsize gst_pw_audio_format_get_stride(GstPwAudioFormat const *pw_audio_format);
gchar* gst_pw_audio_format_to_string(GstPwAudioFormat const *pw_audio_format);
gsize gst_pw_audio_format_calculate_num_frames_from_duration(GstPwAudioFormat const *pw_audio_format, GstClockTime duration);
GstClockTime gst_pw_audio_format_calculate_duration_from_num_frames(GstPwAudioFormat const *pw_audio_format, gsize num_frames);
void gst_pw_audio_format_write_silence_frames(GstPwAudioFormat const *pw_audio_format, gpointer dest_frames, gsize num_silence_frames_to_write);


/**
 * GstPwAudioFormatProbe:
 *
 * Opaque #GstPwAudioFormatProbe structure.
 */
typedef struct _GstPwAudioFormatProbe GstPwAudioFormatProbe;
typedef struct _GstPwAudioFormatProbeClass GstPwAudioFormatProbeClass;

typedef enum
{
	GST_PW_AUDIO_FORMAT_PROBE_RESULT_SUPPORTED,
	GST_PW_AUDIO_FORMAT_PROBE_RESULT_NOT_SUPPORTED,
	GST_PW_AUDIO_FORMAT_PROBE_RESULT_CANCELLED
}
GstPwAudioFormatProbeResult;

#define GST_TYPE_PW_AUDIO_FORMAT_PROBE             (gst_pw_audio_format_probe_get_type())
#define GST_PW_AUDIO_FORMAT_PROBE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PW_AUDIO_FORMAT_PROBE, GstPwAudioFormatProbe))
#define GST_PW_AUDIO_FORMAT_PROBE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PW_AUDIO_FORMAT_PROBE, GstPwAudioFormatProbeClass))
#define GST_PW_AUDIO_FORMAT_PROBE_CAST(obj)        ((GstPwAudioFormatProbe *)(obj))
#define GST_IS_PW_AUDIO_FORMAT_PROBE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PW_AUDIO_FORMAT_PROBE))
#define GST_IS_PW_AUDIO_FORMAT_PROBE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PW_AUDIO_FORMAT_PROBE))

GType gst_pw_audio_format_probe_get_type(void);

GstPwAudioFormatProbe* gst_pw_audio_format_probe_new(GstPipewireCore *core);
void gst_pw_audio_format_probe_setup(GstPwAudioFormatProbe *pw_audio_format_probe);
void gst_pw_audio_format_probe_teardown(GstPwAudioFormatProbe *pw_audio_format_probe);
GstPwAudioFormatProbeResult gst_pw_audio_format_probe_probe_audio_type(GstPwAudioFormatProbe *pw_audio_format_probe, GstPipewireAudioType audio_type, guint32 target_object_id, GstPwAudioFormat **probed_details);
void gst_pw_audio_format_probe_cancel(GstPwAudioFormatProbe *pw_audio_format_probe);


G_END_DECLS


#endif /* __GST_PW_AUDIO_FORMAT_H__ */

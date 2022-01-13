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

#ifndef __GST_PW_AUDIO_FORMAT_H__
#define __GST_PW_AUDIO_FORMAT_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>


G_BEGIN_DECLS


struct spa_pod;


typedef enum
{
	GST_PIPEWIRE_AUDIO_TYPE_PCM = 0,
	GST_PIPEWIRE_AUDIO_TYPE_DSD,
	GST_PIPEWIRE_AUDIO_TYPE_MPEG,
	GST_PIPEWIRE_AUDIO_TYPE_AC3,
	GST_PIPEWIRE_AUDIO_TYPE_EAC3,
	GST_PIPEWIRE_AUDIO_TYPE_TRUEHD,
	GST_PIPEWIRE_AUDIO_TYPE_DTS,
	GST_PIPEWIRE_AUDIO_TYPE_DTS_HD,
	GST_PIPEWIRE_AUDIO_TYPE_MHAS,
	/* TODO: More types */

	GST_NUM_PIPEWIRE_AUDIO_TYPES
}
GstPipewireAudioType;


typedef struct
{
	GstPipewireAudioType audio_type;

	/* PCM specifics */
	GstAudioInfo pcm_audio_info;
}
GstPwAudioFormat;


GstCaps* gst_pw_audio_format_get_template_caps(void);

GstCaps* gst_pw_audio_format_fixate_caps(GstCaps *caps);

gboolean gst_pw_audio_format_data_is_contiguous(GstPipewireAudioType audio_type);
gboolean gst_pw_audio_format_from_caps(GstPwAudioFormat *pw_audio_format, GstElement *element, GstCaps *caps);
gboolean gst_pw_audio_format_to_spa_pod(
	GstPwAudioFormat const *pw_audio_format,
	GstElement *element,
	guint8 *builder_buffer, gsize builder_buffer_size,
	struct spa_pod const **pod
);
gsize gst_pw_audio_format_get_stride(GstPwAudioFormat const *pw_audio_format);
gchar* gst_pw_audio_format_to_string(GstPwAudioFormat const *pw_audio_format);
gsize gst_pw_audio_format_calculate_num_frames_from_duration(GstPwAudioFormat const *pw_audio_format, GstClockTime duration);
GstClockTime gst_pw_audio_format_calculate_duration_from_num_frames(GstPwAudioFormat const *pw_audio_format, gsize num_frames);
void gst_pw_audio_format_write_silence_frames(GstPwAudioFormat const *pw_audio_format, gpointer dest_frames, gsize num_silence_frames_to_write);


G_END_DECLS


#endif // __GST_PW_AUDIO_FORMAT_H__

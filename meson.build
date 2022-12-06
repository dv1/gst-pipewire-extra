project('gst-pipewire-extra', 'c', default_options : ['c_std=gnu99'], version : '1.0.0')

gstreamer_dep       = dependency('gstreamer-1.0',       required : true)
gstreamer_base_dep  = dependency('gstreamer-base-1.0',  required : true)
gstreamer_check_dep = dependency('gstreamer-check-1.0', required : true)
gstreamer_audio_dep = dependency('gstreamer-audio-1.0', required : false)

libpipewire_dep = dependency('libpipewire-0.3', required : true)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')


configinc = include_directories('.')


conf_data = configuration_data()
conf_data.set_quoted('GST_PACKAGE_NAME', get_option('package-name'))
conf_data.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))
conf_data.set_quoted('PACKAGE', 'gst-pipewire-extra')
conf_data.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/dv1/gst-pipewire-extra')
conf_data.set_quoted('VERSION', meson.project_version())


gstpipewireextra_plugin = library(
	'gstpipewireextra',
	[
		'ext/pipewire/gstpwaudioformat.c',
		'ext/pipewire/gstpwaudioringbuffer.c',
		'ext/pipewire/gstpwaudiosink.c',
		'ext/pipewire/gstpwstreamclock.c',
		'ext/pipewire/gstpipewirecore.c',
		'ext/pipewire/plugin.c'
	],
	install : true,
	install_dir: plugins_install_dir,
	include_directories: [configinc],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, libpipewire_dep]
)


test_check_stream_clock = executable(
	'check_stream_clock',
	['test/check_stream_clock.c'],
	link_with: [gstpipewireextra_plugin],
	include_directories: [configinc, 'ext/pipewire'],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, gstreamer_check_dep, libpipewire_dep]
)
test('check_stream_clock', test_check_stream_clock)

test_check_dsd_convert = executable(
	'check_dsd_convert',
	['test/check_dsd_convert.c'],
	link_with: [gstpipewireextra_plugin],
	include_directories: [configinc, 'ext/pipewire'],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, gstreamer_check_dep, libpipewire_dep]
)
test('check_dsd_convert', test_check_dsd_convert)

test_check_dsd_convert = executable(
	'check_utils',
	['test/check_utils.c'],
	link_with: [gstpipewireextra_plugin],
	include_directories: [configinc, 'ext/pipewire'],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, gstreamer_check_dep, libpipewire_dep]
)
test('check_utils', test_check_dsd_convert)

test_check_dsd_convert = executable(
	'check_pwaudioringbuffer',
	['test/check_pwaudioringbuffer.c'],
	link_with: [gstpipewireextra_plugin],
	include_directories: [configinc, 'ext/pipewire'],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, gstreamer_check_dep, libpipewire_dep]
)
test('check_pwaudioringbuffer', test_check_dsd_convert)


configure_file(output : 'config.h', configuration : conf_data)

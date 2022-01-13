project('gstpipewire', 'c', default_options : ['c_std=gnu99'], version : '1.0.0')

gstreamer_dep       = dependency('gstreamer-1.0',       required : true)
gstreamer_base_dep  = dependency('gstreamer-base-1.0',  required : true)
gstreamer_audio_dep = dependency('gstreamer-audio-1.0', required : false)

libpipewire_dep = dependency('libpipewire-0.3', required : true)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')


configinc = include_directories('.')


conf_data = configuration_data()
conf_data.set_quoted('GST_PACKAGE_NAME', get_option('package-name'))
conf_data.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))
conf_data.set_quoted('PACKAGE', 'gstpipewire')
conf_data.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/dv1/gstpipewire')
conf_data.set_quoted('VERSION', meson.project_version())


library(
	'gstpipewire',
	[
		'ext/pipewire/gstpwaudiosink.c',
		'ext/pipewire/gstpwaudioformat.c',
		'ext/pipewire/gstpwaudioqueue.c',
		'ext/pipewire/gstpwstreamclock.c',
		'ext/pipewire/gstpipewirecore.c',
		'ext/pipewire/plugin.c'
	],
	install : true,
	install_dir: plugins_install_dir,
	include_directories: [configinc],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_audio_dep, libpipewire_dep]
)


configure_file(output : 'config.h', configuration : conf_data)

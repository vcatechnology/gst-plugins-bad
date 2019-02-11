autocon_sources = [
  'gstautoconvert2.c',
  'gstautovideoconvert2.c',
  'plugin.c',
]

gstautoconvert2 = library('gstautoconvert2',
  autocon_sources,
  c_args : gst_plugins_bad_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep],
  install : true,
  install_dir : plugins_install_dir,
)

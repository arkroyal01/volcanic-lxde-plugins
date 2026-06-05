# lxpanel widgets for the Volcanic/LXDE session. GTK version is inherited from
# the installed lxpanel via pkg-config (GTK3 here); built into lxpanel's pluginsdir.
pkg_check_modules(LXPANEL REQUIRED IMPORTED_TARGET lxpanel)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_get_variable(LXPANEL_PLUGINSDIR lxpanel pluginsdir)

function(volcanic_lxpanel_plugin name)
  add_library(${name} MODULE ${name}.c)
  target_link_libraries(${name} PkgConfig::LXPANEL PkgConfig::GIO)
  set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
  install(TARGETS ${name} DESTINATION ${LXPANEL_PLUGINSDIR})
endfunction()

volcanic_lxpanel_plugin(volcanic-overview)
# volcanic_lxpanel_plugin(volcanic-power)    # later
# volcanic_lxpanel_plugin(volcanic-volume)   # later

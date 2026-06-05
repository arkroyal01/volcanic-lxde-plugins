# lxpanel widgets for the Volcanic/LXDE session. GTK version is inherited from
# the installed lxpanel via pkg-config (GTK2 here); built into lxpanel's pluginsdir.
pkg_check_modules(LXPANEL REQUIRED IMPORTED_TARGET lxpanel)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(LIBNM IMPORTED_TARGET libnm)        # for volcanic-network (optional)
pkg_get_variable(LXPANEL_PLUGINSDIR lxpanel pluginsdir)

# volcanic_lxpanel_plugin(<name> [EXTRA <pkgconfig-imported-targets...>])
function(volcanic_lxpanel_plugin name)
  cmake_parse_arguments(P "" "" "EXTRA" ${ARGN})
  add_library(${name} MODULE ${name}.c)
  target_link_libraries(${name} PkgConfig::LXPANEL PkgConfig::GIO ${P_EXTRA})
  set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
  install(TARGETS ${name} DESTINATION ${LXPANEL_PLUGINSDIR})
endfunction()

volcanic_lxpanel_plugin(volcanic-overview)

if(LIBNM_FOUND)
  volcanic_lxpanel_plugin(volcanic-network EXTRA PkgConfig::LIBNM)
else()
  message(WARNING "libnm not found -- skipping volcanic-network (NetworkManager applet)")
endif()
# volcanic_lxpanel_plugin(volcanic-power)    # later
# volcanic_lxpanel_plugin(volcanic-volume)   # later

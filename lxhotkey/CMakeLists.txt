# lxhotkey WM backend for KWin. Builds kwin.so into $libdir/lxhotkey/ where
# lxhotkey (PACKAGE_PLUGINS_DIR) scans for per-WM backends.
pkg_check_modules(LXHOTKEY REQUIRED IMPORTED_TARGET lxhotkey)

add_library(kwin MODULE kwin.c)
target_link_libraries(kwin PkgConfig::LXHOTKEY)

# lxhotkey loads plugins by the FM_DEFINE_MODULE name via dlsym, so the file
# just needs to be a plain "kwin.so" (no lib prefix, no version) in the dir.
set_target_properties(kwin PROPERTIES PREFIX "" OUTPUT_NAME "kwin")

install(TARGETS kwin DESTINATION ${CMAKE_INSTALL_LIBDIR}/lxhotkey)

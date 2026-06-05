# volcanic-control: centralized GTK2 settings hub for the Volcanic/LXDE session.
# GTK version follows the rest of the stack (GTK2); GDesktopAppInfo comes from
# gio-unix-2.0.
pkg_check_modules(GTK2 REQUIRED IMPORTED_TARGET gtk+-2.0)
pkg_check_modules(GIOUNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)

add_executable(volcanic-control volcanic-control.c)
target_link_libraries(volcanic-control PkgConfig::GTK2 PkgConfig::GIOUNIX)

install(TARGETS volcanic-control RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES volcanic-control.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

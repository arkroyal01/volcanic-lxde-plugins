cmake_minimum_required(VERSION 3.16)
project(sonic-lxde-plugins C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)

add_subdirectory(lxhotkey)
# add_subdirectory(lxpanel)   # sonic-power / sonic-volume / sonic-overview (later)

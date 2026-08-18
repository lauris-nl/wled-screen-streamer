cmake_minimum_required(VERSION 3.20)
project(wled-screen-streamer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(X11 REQUIRED COMPONENTS Xext Xrandr)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

add_executable(wled-screen-streamer src/main.cpp)
target_link_libraries(wled-screen-streamer PRIVATE X11::X11 X11::Xext X11::Xrandr PkgConfig::FFMPEG Threads::Threads)
target_compile_options(wled-screen-streamer PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion
  $<$<CONFIG:Release>:-O3;-march=native;-mtune=native;-flto>)
target_link_options(wled-screen-streamer PRIVATE $<$<CONFIG:Release>:-flto>)

include(GNUInstallDirs)
install(TARGETS wled-screen-streamer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

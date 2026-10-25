# CMake build. Linux (X11 and Wayland, GCC 11+ or Clang): the tested Linux path, see README.
# Windows: build.ps1 / build.ninja is the tested path; the Windows half of this file was not tested
# (no CMake on the machine it was written on).
cmake_minimum_required(VERSION 3.16)
project(dotd_desktop C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ─── GLFW 3.4 ────────────────────────────────────────────────────────────────
set(GLFW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/src)
set(GLFW_SOURCES context.c init.c input.c monitor.c platform.c vulkan.c window.c egl_context.c osmesa_context.c
  null_init.c null_monitor.c null_window.c null_joystick.c)

if(WIN32)
  list(APPEND GLFW_SOURCES win32_init.c win32_joystick.c win32_module.c win32_monitor.c win32_thread.c win32_time.c
    win32_window.c wgl_context.c)
  list(TRANSFORM GLFW_SOURCES PREPEND ${GLFW_SRC}/)
  add_library(glfw STATIC ${GLFW_SOURCES})
  target_compile_definitions(glfw PRIVATE _GLFW_WIN32 UNICODE _UNICODE)
else()
  # Both backends go into one binary; main.cpp picks X11 when there is an X server (XWayland included)
  option(DOTD_X11 "GLFW X11 backend" ON)
  option(DOTD_WAYLAND "GLFW Wayland backend" ON)
  if(NOT DOTD_X11 AND NOT DOTD_WAYLAND)
    message(FATAL_ERROR "Turn on DOTD_X11, DOTD_WAYLAND or both")
  endif()
  list(APPEND GLFW_SOURCES posix_module.c posix_poll.c posix_thread.c posix_time.c xkb_unicode.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND GLFW_SOURCES linux_joystick.c)
  endif()
  if(DOTD_X11)
    list(APPEND GLFW_SOURCES x11_init.c x11_monitor.c x11_window.c glx_context.c)
  endif()
  if(DOTD_WAYLAND)
    list(APPEND GLFW_SOURCES wl_init.c wl_monitor.c wl_window.c)
  endif()
  list(TRANSFORM GLFW_SOURCES PREPEND ${GLFW_SRC}/)
  add_library(glfw STATIC ${GLFW_SOURCES})
  target_compile_definitions(glfw PRIVATE _DEFAULT_SOURCE)
  # GLFW opens libX11 / libwayland-client / libGL / libEGL at run time: headers only here
  if(DOTD_X11)
    find_package(X11 REQUIRED)
    foreach(part Xrandr Xinerama Xkb Xcursor Xi Xshape)
      if(NOT X11_${part}_INCLUDE_PATH)
        message(FATAL_ERROR "${part} headers not found (Debian/Ubuntu: libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev)")
      endif()
    endforeach()
    target_compile_definitions(glfw PRIVATE _GLFW_X11)
    target_include_directories(glfw PRIVATE ${X11_X11_INCLUDE_PATH})
  endif()
  if(DOTD_WAYLAND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND wayland-client>=0.2.7 wayland-cursor>=0.2.7 wayland-egl>=0.2.7 xkbcommon>=0.5.0)
    if(NOT WAYLAND_FOUND)
      message(FATAL_ERROR "Wayland headers not found (Debian/Ubuntu: libwayland-dev libxkbcommon-dev), or configure with -DDOTD_WAYLAND=OFF")
    endif()
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(NOT WAYLAND_SCANNER)
      message(FATAL_ERROR "wayland-scanner not found (Debian/Ubuntu: libwayland-bin), or configure with -DDOTD_WAYLAND=OFF")
    endif()
    # the protocol headers, generated from GLFW 3.4's own copies of the XML (third_party/glfw/deps/wayland)
    set(WL_XML ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/deps/wayland)
    set(WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wayland)
    file(MAKE_DIRECTORY ${WL_GEN})
    foreach(p wayland viewporter xdg-shell idle-inhibit-unstable-v1 pointer-constraints-unstable-v1
              relative-pointer-unstable-v1 fractional-scale-v1 xdg-activation-v1 xdg-decoration-unstable-v1)
      add_custom_command(OUTPUT ${WL_GEN}/${p}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${WL_XML}/${p}.xml ${WL_GEN}/${p}-client-protocol.h
        DEPENDS ${WL_XML}/${p}.xml VERBATIM)
      add_custom_command(OUTPUT ${WL_GEN}/${p}-client-protocol-code.h
        COMMAND ${WAYLAND_SCANNER} private-code ${WL_XML}/${p}.xml ${WL_GEN}/${p}-client-protocol-code.h
        DEPENDS ${WL_XML}/${p}.xml VERBATIM)
      target_sources(glfw PRIVATE ${WL_GEN}/${p}-client-protocol.h ${WL_GEN}/${p}-client-protocol-code.h)
    endforeach()
    include(CheckFunctionExists)   # as GLFW's own CMake does (wl_window.c defines _GNU_SOURCE for the declaration)
    check_function_exists(memfd_create HAVE_MEMFD_CREATE)
    if(HAVE_MEMFD_CREATE)
      target_compile_definitions(glfw PRIVATE HAVE_MEMFD_CREATE)
    endif()
    target_compile_definitions(glfw PRIVATE _GLFW_WAYLAND)
    target_include_directories(glfw PRIVATE ${WL_GEN} ${WAYLAND_INCLUDE_DIRS})
  endif()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(glfw PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
target_include_directories(glfw PUBLIC third_party/glfw/include third_party/glfw/deps)

# ─── The game ────────────────────────────────────────────────────────────────
# the achievement rules' unit test: cmake --build build/linux --target ach_test && build/linux/ach_test
add_executable(ach_test EXCLUDE_FROM_ALL tests/ach_test.cpp src/achievements.cpp src/storage.cpp src/log.cpp)
target_include_directories(ach_test PRIVATE src)

add_executable(dotd
  src/main.cpp src/game.cpp src/game_ui.cpp src/game_save.cpp src/render.cpp src/assets.cpp
  src/audio.cpp src/storage.cpp src/achievements.cpp src/ui.cpp src/log.cpp src/jobs.cpp src/display.cpp src/gpu_particles.cpp src/post.cpp src/nuke_fx.cpp src/weather.cpp src/third_party_impl.c
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_glfw.cpp
  third_party/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(dotd PRIVATE src third_party/imgui third_party/imgui/backends third_party/glm
  third_party/cgltf third_party/stb third_party/miniaudio)
target_compile_definitions(dotd PRIVATE GLFW_INCLUDE_NONE GLM_ENABLE_EXPERIMENTAL)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dotd PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-function>)   # as build.ninja
endif()
target_link_libraries(dotd PRIVATE glfw)

if(WIN32)
  set_target_properties(dotd PROPERTIES WIN32_EXECUTABLE ON)
  target_compile_definitions(dotd PRIVATE NOMINMAX)
  target_link_libraries(dotd PRIVATE dxgi opengl32 gdi32 user32 shell32 ole32 uuid mfplat mfreadwrite mfuuid winmm)
  if(MINGW)
    target_link_options(dotd PRIVATE -static)
  endif()
else()
  # The music is AAC (ADTS), as in the web version; FFmpeg decodes it (Windows: Media Foundation)
  option(DOTD_MUSIC "Decode the AAC music with FFmpeg (libavformat, libavcodec, libavutil)" ON)
  if(DOTD_MUSIC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
    if(NOT FFMPEG_FOUND)
      message(FATAL_ERROR "The music needs FFmpeg's libavformat, libavcodec and libavutil "
        "(Debian/Ubuntu: libavformat-dev libavcodec-dev libavutil-dev; Fedora: ffmpeg-free-devel or ffmpeg-devel; "
        "Arch: ffmpeg). Or configure with -DDOTD_MUSIC=OFF for a build without music.")
    endif()
    target_compile_definitions(dotd PRIVATE DOTD_FFMPEG)
    target_link_libraries(dotd PRIVATE PkgConfig::FFMPEG)
  else()
    message(WARNING "DOTD_MUSIC=OFF: this build plays the sound effects but no music")
  endif()
endif()

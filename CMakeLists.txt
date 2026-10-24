# CMake build (alternative to build.ninja / build.ps1). Windows only: the music
# decoder uses Media Foundation. Not tested on the machine this was written on
# (no CMake there); build.ps1 is the tested path.
cmake_minimum_required(VERSION 3.16)
project(dotd_desktop C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(glfw_win32 STATIC
  third_party/glfw/src/context.c third_party/glfw/src/init.c third_party/glfw/src/input.c
  third_party/glfw/src/monitor.c third_party/glfw/src/platform.c third_party/glfw/src/vulkan.c
  third_party/glfw/src/window.c third_party/glfw/src/egl_context.c third_party/glfw/src/osmesa_context.c
  third_party/glfw/src/null_init.c third_party/glfw/src/null_monitor.c third_party/glfw/src/null_window.c
  third_party/glfw/src/null_joystick.c third_party/glfw/src/win32_init.c third_party/glfw/src/win32_joystick.c
  third_party/glfw/src/win32_module.c third_party/glfw/src/win32_monitor.c third_party/glfw/src/win32_thread.c
  third_party/glfw/src/win32_time.c third_party/glfw/src/win32_window.c third_party/glfw/src/wgl_context.c)
target_compile_definitions(glfw_win32 PRIVATE _GLFW_WIN32 UNICODE _UNICODE)
target_include_directories(glfw_win32 PUBLIC third_party/glfw/include third_party/glfw/deps)

add_executable(dotd WIN32
  src/main.cpp src/game.cpp src/game_ui.cpp src/game_save.cpp src/render.cpp src/assets.cpp
  src/audio.cpp src/storage.cpp src/ui.cpp src/log.cpp src/jobs.cpp src/display.cpp src/gpu_particles.cpp src/post.cpp src/third_party_impl.c
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_glfw.cpp
  third_party/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(dotd PRIVATE src third_party/imgui third_party/imgui/backends third_party/glm
  third_party/cgltf third_party/stb third_party/miniaudio)
target_compile_definitions(dotd PRIVATE GLFW_INCLUDE_NONE GLM_ENABLE_EXPERIMENTAL NOMINMAX)
target_link_libraries(dotd PRIVATE glfw_win32 dxgi opengl32 gdi32 user32 shell32 ole32 uuid mfplat mfreadwrite mfuuid winmm)
if(MINGW)
  target_link_options(dotd PRIVATE -static)
endif()

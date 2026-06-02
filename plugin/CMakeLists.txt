cmake_minimum_required(VERSION 3.16)

project(mumble_voice_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build a shared library. Mumble loads this file as a general-purpose plugin:
#   Windows -> mumble_voice_overlay.dll
#   Linux   -> libmumble_voice_overlay.so   (handy for local compile checks)
#   macOS   -> libmumble_voice_overlay.dylib
add_library(mumble_voice_overlay SHARED src/voice_overlay_plugin.cpp)

target_include_directories(mumble_voice_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Hide everything except the MUMBLE_PLUGIN_EXPORT-marked entry points so the
# DLL exposes exactly the plugin ABI Mumble expects.
set_target_properties(mumble_voice_overlay PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "")

if(WIN32)
    target_link_libraries(mumble_voice_overlay PRIVATE Ws2_32)
endif()

if(MSVC)
    target_compile_options(mumble_voice_overlay PRIVATE /W3)
else()
    target_compile_options(mumble_voice_overlay PRIVATE -Wall -Wextra)
endif()

cmake_minimum_required(VERSION 3.16)
project(mumble_voice_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mumble_voice_overlay SHARED src/voice_overlay_plugin.cpp)

target_include_directories(mumble_voice_overlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

set_target_properties(mumble_voice_overlay PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "")

if(WIN32)
    # GDI+, shell (SHGetFolderPath), and WinSock are all needed on Windows.
    # The #pragma comment(lib,...) lines in the source cover MSVC builds;
    # these target_link_libraries cover MinGW and CMake generator-agnostic builds.
    target_link_libraries(mumble_voice_overlay PRIVATE
        gdiplus gdi32 user32 shell32 ole32)
endif()

if(MSVC)
    target_compile_options(mumble_voice_overlay PRIVATE /W3 /utf-8)
else()
    target_compile_options(mumble_voice_overlay PRIVATE -Wall -Wextra)
    if(NOT WIN32)
        # UDP socket support on Linux/macOS
        target_link_libraries(mumble_voice_overlay PRIVATE)
    endif()
endif()

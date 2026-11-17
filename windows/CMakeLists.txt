# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
# khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
# mh4u-win: the Windows shell of the native port (phase 3 of the project roadmap).
# Like app_native/ for Android, it builds the runtime (mh4n/native, with its app modules) and adds its own
# executable on top. Nothing under mh4n/ or android/ is edited.
#
#   cmake -S windows -B build -G Ninja -DCMAKE_PREFIX_PATH=<SDL3 for MinGW> -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake_minimum_required(VERSION 3.25)
project(mh4u_windows C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MH4N_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mh4n" ABSOLUTE)
find_package(SDL3 3.4.18 CONFIG REQUIRED)
set(MH4N_TESTS OFF CACHE BOOL "" FORCE)
set(MH4N_APP ON CACHE BOOL "" FORCE)
add_subdirectory(${MH4N_DIR}/native mh4n)

# a windows-subsystem program: no console window when started by a double click (win_main.cpp sends the log to a file)
# The menu's look (menu_gui.cpp) is Dear ImGui over Vulkan with the Cinzel font (SIL OFL), which is embedded in the program:
# CMake turns the font file into a source file (an array of bytes) when it configures.
set(CINZEL_TTF "${CMAKE_CURRENT_SOURCE_DIR}/assets/Cinzel.ttf")
set(CINZEL_CPP "${CMAKE_CURRENT_BINARY_DIR}/cinzel_font.cpp")
file(READ "${CINZEL_TTF}" CINZEL_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CINZEL_ARRAY "${CINZEL_HEX}")
file(SIZE "${CINZEL_TTF}" CINZEL_SIZE)
file(WRITE "${CINZEL_CPP}" "extern const unsigned char kCinzelTtf[] = {${CINZEL_ARRAY}};
extern const unsigned int kCinzelTtfSize = ${CINZEL_SIZE};
")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CINZEL_TTF}")

add_executable(mh4u-win WIN32 win_main.cpp menu_gui.cpp imgui_vulkan_backend.cpp "${CINZEL_CPP}")
set_target_properties(mh4u-win PROPERTIES OUTPUT_NAME Caravan4URecomp)  # Caravan4URecomp.exe
target_link_libraries(mh4u-win PRIVATE mh4n::app mh4n_warnings SDL3::SDL3 mh4n_imgui)
# the swapchain, present and screen_layout headers are included as "swapchain/..."; screen_layout.h includes the port's platform.h
target_include_directories(mh4u-win PRIVATE ${MH4N_DIR}/native/src ${MH4N_DIR}/native/app ${MH4N_DIR}/port/src)

# Dear ImGui's Vulkan backend, built against the loader's functions (it is given them at run time, as the shared app does)
set(MH4N_IMGUI_VULKAN_DIR "${MH4N_DIR}/port/app/third_party/imgui")
target_include_directories(mh4u-win PRIVATE "${MH4N_IMGUI_VULKAN_DIR}" "${MH4N_IMGUI_VULKAN_DIR}/backends")
set_source_files_properties(imgui_vulkan_backend.cpp PROPERTIES COMPILE_OPTIONS "-w")

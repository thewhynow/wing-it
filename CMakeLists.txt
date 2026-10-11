# TO BUILD
# cmake --build build -j
# ./build/wing-it


cmake_minimum_required(VERSION 3.20)
project(wing_it LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)  # clangd uses compile_flags.txt instead
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Some deps declare old cmake_minimum_required versions that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
  GIT_SHALLOW    TRUE
  GIT_PROGRESS    TRUE)
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_SHALLOW    TRUE
  GIT_PROGRESS    TRUE)
# No usable CMake projects in these - fetch sources only.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.9
  GIT_SHALLOW    TRUE
  GIT_PROGRESS    TRUE)
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG        851cdbdd60d1dfb64c23e987969694de0f547a6b
  GIT_PROGRESS    TRUE)
FetchContent_Declare(rlimgui   # raylib <-> ImGui backend
  GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
  GIT_TAG        1550009359ad975927f7f0e4a3f47e3f27123ea9
  GIT_PROGRESS    TRUE)
FetchContent_Declare(tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG        v2.0.0rc13
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _none
  GIT_PROGRESS    TRUE)
FetchContent_Declare(qrcodegen 
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git 
    GIT_TAG v1.8.0 
    GIT_SHALLOW TRUE 
    GIT_PROGRESS TRUE
)
FetchContent_Declare(cpp-httplib 
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git 
    GIT_TAG v0.48.0 
    GIT_SHALLOW TRUE 
    GIT_PROGRESS TRUE
)

# JSON for the Hack Club AI requests (header-only, fetch sources only)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _none
  GIT_PROGRESS    TRUE)

# Native "open file" dialog (header-only; osascript on macOS, zenity/kdialog on Linux, Win32 on Windows)
FetchContent_Declare(pfd
  GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
  GIT_TAG        c12ea8c9a727f5320a2b4570aee863bbede2a204
  SOURCE_SUBDIR  _none
  GIT_PROGRESS    TRUE)

FetchContent_MakeAvailable(raylib glm imgui implot rlimgui tinyobjloader qrcodegen cpp-httplib pfd json)

# libcurl does the HTTPS calls to ai.hackclub.com (ships with macOS; Linux: libcurl4-openssl-dev)
find_package(CURL REQUIRED)

# Dear ImGui + ImPlot + rlImGui as one static lib
add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
  ${implot_SOURCE_DIR}/implot_demo.cpp
  ${rlimgui_SOURCE_DIR}/rlImGui.cpp)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR} ${implot_SOURCE_DIR} ${rlimgui_SOURCE_DIR})
target_compile_definitions(imgui_lib PUBLIC NO_FONT_AWESOME)
target_link_libraries(imgui_lib PUBLIC raylib)

find_package(Threads REQUIRED)

# OpenMP: Apple clang lacks it; use Homebrew libomp (brew install libomp).
if(APPLE AND NOT OpenMP_ROOT)
  foreach(prefix /opt/homebrew/opt/libomp /usr/local/opt/libomp)
    if(EXISTS "${prefix}/lib/libomp.dylib")
      set(OpenMP_ROOT ${prefix})
      break()
    endif()
  endforeach()
endif()
find_package(OpenMP COMPONENTS CXX)
if(NOT OpenMP_CXX_FOUND)
  message(WARNING "OpenMP not found - solver will be single-threaded (macOS: brew install libomp)")
endif()

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/*.cpp)
if(NOT APP_SOURCES)
  message(WARNING "No sources in src/ yet - add src/main.cpp to build the app.")
  return()
endif()

add_executable(wing_it ${APP_SOURCES}
  ${tinyobjloader_SOURCE_DIR}/tiny_obj_loader.cc
${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp
        src/common.cpp)  # defines TINYOBJLOADER_IMPLEMENTATION - do not define it in src/
target_include_directories(wing_it PRIVATE inc ${tinyobjloader_SOURCE_DIR} ${qrcodegen_SOURCE_DIR}/cpp ${pfd_SOURCE_DIR} ${json_SOURCE_DIR}/include)
target_link_libraries(wing_it PRIVATE raylib imgui_lib glm::glm Threads::Threads httplib::httplib CURL::libcurl)
if(OpenMP_CXX_FOUND)
  target_link_libraries(wing_it PRIVATE OpenMP::OpenMP_CXX)
endif()
if(WIN32)
  target_compile_definitions(wing_it PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(wing_it PRIVATE ws2_32 iphlpapi)
endif()
if(APPLE)
  target_link_libraries(wing_it PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

add_custom_command(TARGET wing_it POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:wing_it>/assets"
        COMMENT "Copying assets directory to build folder..."
)

include(packaging/packaging.cmake)

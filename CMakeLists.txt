cmake_minimum_required(VERSION 3.20)
project(wing_it LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
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
  GIT_TAG        v1.91.9
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

FetchContent_MakeAvailable(raylib glm imgui implot rlimgui tinyobjloader)

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
find_package(OpenMP COMPONENTS CXX)
if(NOT OpenMP_CXX_FOUND AND APPLE)
  foreach(prefix /opt/homebrew/opt/libomp /usr/local/opt/libomp)
    if(EXISTS "${prefix}/lib/libomp.dylib")
      set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${prefix}/include")
      set(OpenMP_CXX_LIB_NAMES omp)
      set(OpenMP_omp_LIBRARY "${prefix}/lib/libomp.dylib")
      find_package(OpenMP COMPONENTS CXX)
      break()
    endif()
  endforeach()
endif()
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

add_executable(wing_it ${APP_SOURCES})
target_include_directories(wing_it PRIVATE inc ${tinyobjloader_SOURCE_DIR})
target_link_libraries(wing_it PRIVATE raylib imgui_lib glm::glm Threads::Threads)
if(OpenMP_CXX_FOUND)
  target_link_libraries(wing_it PRIVATE OpenMP::OpenMP_CXX)
endif()
if(APPLE)
  target_link_libraries(wing_it PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

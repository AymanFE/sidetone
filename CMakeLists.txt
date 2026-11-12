cmake_minimum_required(VERSION 3.16)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(Sidetone VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Static C runtime on Windows so the exe runs on any PC without redistributables
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 build also runs under XWayland

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.8 GIT_SHALLOW TRUE
                     SOURCE_SUBDIR _none_)
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.21 GIT_SHALLOW TRUE
                     SOURCE_SUBDIR _none_)
FetchContent_MakeAvailable(glfw imgui miniaudio)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

set(SOURCES src/main.cpp src/engine.cpp src/settings.cpp src/wasapi_direct.cpp src/miniaudio_impl.c)
if(WIN32)
  list(APPEND SOURCES assets/sidetone.rc)
endif()
add_executable(Sidetone WIN32 MACOSX_BUNDLE ${SOURCES})
target_include_directories(Sidetone PRIVATE ${miniaudio_SOURCE_DIR})
target_compile_definitions(Sidetone PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(Sidetone PRIVATE imgui)

if(WIN32)
  target_link_libraries(Sidetone PRIVATE ole32 avrt)
endif()
if(MSVC)
  target_compile_options(Sidetone PRIVATE /O2 /utf-8)
  target_link_options(Sidetone PRIVATE /ENTRY:mainCRTStartup)
elseif(MINGW)
  target_link_options(Sidetone PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(APPLE)
  target_link_libraries(Sidetone PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  set_target_properties(Sidetone PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/assets/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME Sidetone
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.aymanfe.sidetone
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(Sidetone PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

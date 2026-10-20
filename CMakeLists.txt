cmake_minimum_required(VERSION 3.24)
project(GMod_ESP_Chams_x64 LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project builds a Windows x64 DLL only; configure with -A x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui minhook)
add_library(gmod_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp)
target_include_directories(gmod_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(gmod_imgui PUBLIC d3d9 user32 gdi32 imm32)
add_library(GModESP_Chams_x64 SHARED GModESP.cpp Config.hpp)
target_compile_definitions(GModESP_Chams_x64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(GModESP_Chams_x64 PRIVATE gmod_imgui minhook d3d9 user32 gdi32)
if(MSVC)
    target_compile_options(GModESP_Chams_x64 PRIVATE /W3 /EHsc /MP)
    target_link_options(GModESP_Chams_x64 PRIVATE /INCREMENTAL:NO)
endif()
set_target_properties(GModESP_Chams_x64 PROPERTIES OUTPUT_NAME "GModESP_Chams_x64")

cmake_minimum_required(VERSION 3.20)
project(imgui VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
#  Options
# ============================================================================
option(IMGUI_VR_VULKAN   "Build Vulkan render path"  ON)
option(IMGUI_VR_OPENGL   "Build OpenGL render path"  OFF)
option(IMGUI_VR_MINHOOK  "Use MinHook (Windows)"     OFF)
option(IMGUI_VR_DOBBY    "Use Dobby (Android/Linux)"  ON)

# ============================================================================
#  Paths
#  IMGUI_DIR   = root of the imgui fork (one level up from imgui-vr/)
#  OPENXR_DIR  = OpenXR-SDK source root  (headers live in include/)
#  OPENXR_BUILD_DIR = where the OpenXR SDK was built (loader .a lives here)
#  DOBBY_DIR   = Dobby source root       (headers in include/, lib in build/)
#  DOBBY_BUILD_DIR  = where Dobby was built (libdobby.a lives here)
# ============================================================================
set(IMGUI_DIR        "${CMAKE_SOURCE_DIR}/.."  CACHE PATH "Path to imgui root")
set(OPENXR_DIR       ""                        CACHE PATH "Path to OpenXR-SDK source")
set(OPENXR_BUILD_DIR ""                        CACHE PATH "Path to OpenXR-SDK build output")
set(DOBBY_DIR        ""                        CACHE PATH "Path to Dobby source")
set(DOBBY_BUILD_DIR  ""                        CACHE PATH "Path to Dobby build output")
set(VULKAN_DIR       "$ENV{VULKAN_SDK}"        CACHE PATH "Path to Vulkan SDK (desktop only)")
set(MINHOOK_DIR      ""                        CACHE PATH "Path to MinHook root (Windows only)")

# ============================================================================
#  ImGui core sources
# ============================================================================
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp       # remove in production
)

# ============================================================================
#  Our backend
# ============================================================================
set(BACKEND_SOURCES
    ${CMAKE_SOURCE_DIR}/backends/imgui_impl_openxr.cpp
)

# ============================================================================
#  Renderer backend
# ============================================================================
if(IMGUI_VR_VULKAN)
    list(APPEND BACKEND_SOURCES ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
    add_compile_definitions(IMGUI_IMPL_OPENXR_VULKAN)
endif()
if(IMGUI_VR_OPENGL)
    list(APPEND BACKEND_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    add_compile_definitions(IMGUI_IMPL_OPENXR_OPENGL)
endif()

# ============================================================================
#  Hook framework
# ============================================================================
if(IMGUI_VR_MINHOOK)
    add_compile_definitions(IMGUI_IMPL_OPENXR_USE_MINHOOK)
endif()
if(IMGUI_VR_DOBBY)
    add_compile_definitions(IMGUI_IMPL_OPENXR_USE_DOBBY)
endif()

# ============================================================================
#  Target — output is libimgui.so
# ============================================================================
add_library(imgui SHARED
    ${IMGUI_SOURCES}
    ${BACKEND_SOURCES}
    ${CMAKE_SOURCE_DIR}/examples/imgui_vr_example.cpp
)

target_include_directories(imgui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/backends
    ${OPENXR_DIR}/include
)

# Vulkan — on Android the driver is loaded at runtime, no link lib needed
if(IMGUI_VR_VULKAN)
    if(ANDROID)
        # Headers only; Vulkan is always available on Quest via the loader
        find_path(VULKAN_INCLUDE vulkan/vulkan.h
            PATHS ${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include)
        target_include_directories(imgui PRIVATE ${VULKAN_INCLUDE})
    else()
        target_include_directories(imgui PRIVATE ${VULKAN_DIR}/Include)
        target_link_libraries(imgui PRIVATE ${VULKAN_DIR}/Lib/vulkan-1.lib)
    endif()
endif()

# OpenXR loader
if(ANDROID)
    # Link against the static loader we built from source
    target_link_libraries(imgui PRIVATE
        ${OPENXR_BUILD_DIR}/src/loader/libopenxr_loader.a
        android log)
else()
    target_link_libraries(imgui PRIVATE
        ${OPENXR_DIR}/x64/lib/openxr_loader.lib)
endif()

# Dobby (Android only)
if(IMGUI_VR_DOBBY)
    target_include_directories(imgui PRIVATE ${DOBBY_DIR}/include)
    target_link_libraries(imgui PRIVATE
        ${DOBBY_BUILD_DIR}/libdobby.a
        dl)
endif()

# MinHook (Windows only)
if(IMGUI_VR_MINHOOK AND WIN32)
    target_include_directories(imgui PRIVATE ${MINHOOK_DIR}/include)
    target_link_libraries(imgui PRIVATE ${MINHOOK_DIR}/lib/libMinHook.lib)
    target_compile_definitions(imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR)
    target_link_libraries(imgui PRIVATE d3d11)
endif()

# Strip in release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(imgui PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

message(STATUS "libimgui.so config:")
message(STATUS "  Renderer : Vulkan=${IMGUI_VR_VULKAN}  OpenGL=${IMGUI_VR_OPENGL}")
message(STATUS "  Hooks    : MinHook=${IMGUI_VR_MINHOOK}  Dobby=${IMGUI_VR_DOBBY}")
message(STATUS "  IMGUI_DIR      = ${IMGUI_DIR}")
message(STATUS "  OPENXR_DIR     = ${OPENXR_DIR}")
message(STATUS "  OPENXR_BUILD   = ${OPENXR_BUILD_DIR}")
message(STATUS "  DOBBY_DIR      = ${DOBBY_DIR}")
message(STATUS "  DOBBY_BUILD    = ${DOBBY_BUILD_DIR}")

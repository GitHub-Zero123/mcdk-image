cmake_minimum_required(VERSION 3.15)

project(MCDK-IMAGE LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# By Zero123
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(MSVC)
        add_compile_options(/utf-8 /EHsc)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# 内部依赖库
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann
)

find_package(Threads REQUIRED)

option(MCDK_IMAGE_REQUIRE_VULKAN "Require Vulkan SDK/glslc for the local stable-diffusion.cpp backend" ON)

add_subdirectory(libs/cpp-mcp)
target_link_libraries(mcp PUBLIC nlohmann_json::nlohmann_json)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/stable-diffusion/CMakeLists.txt" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/ggml/CMakeLists.txt")
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "sd: build examples" FORCE)
    set(SD_WEBP OFF CACHE BOOL "sd: enable WebP image I/O support" FORCE)
    set(SD_WEBM OFF CACHE BOOL "sd: enable WebM video output support" FORCE)
    find_package(Vulkan COMPONENTS glslc QUIET)
    if(Vulkan_FOUND AND Vulkan_glslc_FOUND)
        set(SD_VULKAN ON CACHE BOOL "sd: vulkan backend" FORCE)
        set(GGML_VULKAN ON CACHE BOOL "ggml: enable Vulkan backend" FORCE)
    elseif(MCDK_IMAGE_REQUIRE_VULKAN)
        message(FATAL_ERROR "Vulkan SDK/glslc was not found, but MCDK_IMAGE_REQUIRE_VULKAN=ON. Install LunarG Vulkan SDK and reconfigure, or set -DMCDK_IMAGE_REQUIRE_VULKAN=OFF for CPU-only development builds.")
    else()
        message(WARNING "Vulkan SDK/glslc was not found; building stable-diffusion.cpp with CPU backend only. Install Vulkan SDK to enable Vulkan acceleration.")
        set(SD_VULKAN OFF CACHE BOOL "sd: vulkan backend" FORCE)
        set(GGML_VULKAN OFF CACHE BOOL "ggml: enable Vulkan backend" FORCE)
    endif()
    set(SD_CUDA OFF CACHE BOOL "sd: cuda backend" FORCE)
    set(SD_USE_SYSTEM_GGML ON CACHE BOOL "sd: use root-provided GGML library" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "ggml: enable CUDA backend" FORCE)
    add_subdirectory(libs/ggml)
    add_subdirectory(libs/stable-diffusion)
    target_compile_definitions(stable-diffusion PRIVATE GGML_MAX_NAME=128)
else()
    message(WARNING "stable-diffusion.cpp backend is not enabled because libs/stable-diffusion or root libs/ggml is missing. Initialize root submodules to build the local sdcpp provider.")
endif()

add_subdirectory(apps/mcp-image-server)

cmake_minimum_required(VERSION 3.21)

project(lod-check VERSION 0.1.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
    # std::format's floating-point output needs libc++'s to_chars, which macOS ships from 13.3.
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

add_executable(lod-check
    src/lod_check.hh
    src/main.cc
    src/analysis.cc
    src/platform.cc
)
target_compile_definitions(lod-check PRIVATE LOD_CHECK_VERSION="${PROJECT_VERSION}")

# The HLSL and MSL are compiled at run time, so they are embedded as strings.
file(READ shaders/probe.hlsl LOD_CHECK_PROBE_HLSL)
file(READ shaders/probe.metal LOD_CHECK_PROBE_METAL)
configure_file(src/shader_sources.hh.in ${CMAKE_CURRENT_BINARY_DIR}/generated/shader_sources.hh @ONLY)
target_include_directories(lod-check PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS shaders/probe.hlsl shaders/probe.metal)

# Vulkan needs only the headers: the loader is opened at run time.
# A Vulkan SDK's headers are used when one is installed; otherwise Khronos' header repository is fetched.
function(lod_check_use_vulkan_headers)
    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Headers)
        target_link_libraries(lod-check PRIVATE Vulkan::Headers)
    else()
        include(FetchContent)
        FetchContent_Declare(vulkan_headers
            URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.296.tar.gz
        )
        FetchContent_MakeAvailable(vulkan_headers)
        target_link_libraries(lod-check PRIVATE Vulkan::Headers)
    endif()
endfunction()

if(WIN32)
    target_sources(lod-check PRIVATE src/d3d12.cc src/vulkan.cc)
    target_compile_definitions(lod-check PRIVATE LOD_CHECK_D3D12=1 LOD_CHECK_VULKAN=1)
    target_link_libraries(lod-check PRIVATE d3d12 dxgi d3dcompiler)
    lod_check_use_vulkan_headers()
    if(MSVC)
        # A single self-contained exe: no Visual C++ redistributable needed.
        set_property(TARGET lod-check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
elseif(APPLE)
    target_sources(lod-check PRIVATE src/metal.mm)
    target_compile_definitions(lod-check PRIVATE LOD_CHECK_METAL=1)
    target_compile_options(lod-check PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(lod-check PRIVATE "-framework Foundation" "-framework Metal")
else()
    target_sources(lod-check PRIVATE src/vulkan.cc)
    target_compile_definitions(lod-check PRIVATE LOD_CHECK_VULKAN=1)
    target_link_libraries(lod-check PRIVATE ${CMAKE_DL_LIBS})
    lod_check_use_vulkan_headers()
    # Runs on distributions older than the build machine's libstdc++.
    target_link_options(lod-check PRIVATE -static-libstdc++ -static-libgcc)
endif()

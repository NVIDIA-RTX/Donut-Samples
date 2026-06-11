#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.


cmake_minimum_required(VERSION 3.18)

project(donut_examples)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")

add_subdirectory(donut)

if (NVRHI_WITH_DX12)
    set(DONUT_D3D_AGILITY_SDK_URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.717.1-preview")
    set(DONUT_D3D_AGILITY_SDK_FETCH_DIR "" CACHE STRING "" FORCE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/donut/cmake/FetchAgilitySDK.cmake")

    file(COPY
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll"
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12SDKLayers.dll"
        DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/")
endif()

add_subdirectory(feature_demo)
add_subdirectory(examples/basic_triangle)
add_subdirectory(examples/vertex_buffer)
add_subdirectory(examples/deferred_shading)
add_subdirectory(examples/headless)

if (NVRHI_WITH_VULKAN OR NVRHI_WITH_DX12)
    add_subdirectory(examples/bindless_rendering)
    add_subdirectory(examples/variable_shading)
    add_subdirectory(examples/rt_triangle)
    add_subdirectory(examples/rt_shadows)
    add_subdirectory(examples/rt_bindless)
    add_subdirectory(examples/rt_particles)
    add_subdirectory(examples/meshlets)
    add_subdirectory(examples/threaded_rendering)
    add_subdirectory(examples/async_compute)
endif()

if (NVRHI_WITH_DX12)
    # Work Graphs use a DX12-specific API
    add_subdirectory(examples/work_graphs)

    # The RT Reflections example uses local bindings (local root signatures) which are unsupported on Vulkan
    add_subdirectory(examples/rt_reflections)
endif()

if (NVRHI_WITH_VULKAN)
    # Shader specializations are a unique Vulkan feature
    add_subdirectory(examples/shader_specializations)
endif()

if (DONUT_WITH_AFTERMATH AND NVRHI_WITH_AFTERMATH)
    add_subdirectory(examples/aftermath)
endif()

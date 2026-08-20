cmake_minimum_required(VERSION 3.10)

project(RaptorEngine2 VERSION 0.1.0)

# Set common properties inherited to all subdirectories

# Set runtime naming
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${cfg}" cfg_u)
  set("CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_u}" "${CMAKE_SOURCE_DIR}/bin")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(RaptorOptions)
include(RaptorChapters)

find_package(Vulkan REQUIRED)

if (UNIX)
find_package(SDL2 REQUIRED)
endif()

# RaptorFoundation #######################################################
add_library(RaptorFoundation STATIC
    source/raptor/foundation/array.hpp
    source/raptor/foundation/assert.cpp
    source/raptor/foundation/assert.hpp
    source/raptor/foundation/bit.cpp
    source/raptor/foundation/bit.hpp
    source/raptor/foundation/blob_serialization.cpp
    source/raptor/foundation/blob_serialization.hpp
    source/raptor/foundation/blob.hpp
    source/raptor/foundation/camera.cpp
    source/raptor/foundation/camera.hpp
    source/raptor/foundation/color.cpp
    source/raptor/foundation/color.hpp
    source/raptor/foundation/data_structures.cpp
    source/raptor/foundation/data_structures.hpp
    source/raptor/foundation/file.cpp
    source/raptor/foundation/file.hpp
    source/raptor/foundation/hash_map.hpp
    source/raptor/foundation/log.cpp
    source/raptor/foundation/log.hpp
    source/raptor/foundation/memory_utils.hpp
    source/raptor/foundation/memory.cpp
    source/raptor/foundation/memory.hpp
    source/raptor/foundation/numerics.cpp
    source/raptor/foundation/numerics.hpp
    source/raptor/foundation/platform.hpp
    source/raptor/foundation/process.cpp
    source/raptor/foundation/process.hpp
    source/raptor/foundation/relative_data_structures.hpp
    source/raptor/foundation/resource_manager.cpp
    source/raptor/foundation/resource_manager.hpp
    source/raptor/foundation/serialization.cpp
    source/raptor/foundation/serialization.hpp
    source/raptor/foundation/service_manager.cpp
    source/raptor/foundation/service_manager.hpp
    source/raptor/foundation/service.cpp
    source/raptor/foundation/service.hpp
    source/raptor/foundation/span.hpp
    source/raptor/foundation/static_array.hpp
    source/raptor/foundation/static_string.hpp
    source/raptor/foundation/static_string.cpp
    source/raptor/foundation/string.cpp
    source/raptor/foundation/string.hpp
    source/raptor/foundation/string_view.hpp
    source/raptor/foundation/time.cpp
    source/raptor/foundation/time.hpp

    raptor.natvis
)

set_property(TARGET RaptorFoundation PROPERTY CXX_STANDARD 20)

set(RAPTOR_DEFINES
    _CRT_SECURE_NO_WARNINGS

    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_SYSTEM_TRACING

    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

target_compile_definitions(RaptorFoundation PRIVATE
    ${RAPTOR_DEFINES}
)

if (WIN32)
    target_compile_options(RaptorFoundation PRIVATE
        /MP
    )
endif()

target_include_directories(RaptorFoundation PUBLIC
    source
    source/raptor
)

# RaptorApp ##############################################################
add_library(RaptorApp STATIC
    source/raptor/application/application.cpp
    source/raptor/application/application.hpp
    source/raptor/application/game_camera.cpp
    source/raptor/application/game_camera.hpp
    source/raptor/application/game_application.cpp
    source/raptor/application/game_application.hpp
    source/raptor/application/input.cpp
    source/raptor/application/input.hpp
    source/raptor/application/keys.cpp
    source/raptor/application/keys.hpp
    source/raptor/application/window.cpp
    source/raptor/application/window.hpp
)

set_property(TARGET RaptorApp PROPERTY CXX_STANDARD 20)

target_compile_definitions(RaptorApp PRIVATE
    ${RAPTOR_DEFINES}
)

target_include_directories(RaptorApp PUBLIC
    source
    source/raptor
    )

if (WIN32)
    target_compile_options(RaptorApp PRIVATE
        /MP
    )

    target_include_directories(RaptorApp PRIVATE
        binaries/SDL3-3.2.16/include)

    target_compile_definitions(RaptorApp PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        VK_USE_PLATFORM_WIN32_KHR
    )
else()
    target_include_directories(RaptorApp PRIVATE
        ${SDL2_INCLUDE_DIRS})
endif()

# RaptorExternal #########################################################
add_library(RaptorExternal STATIC
    source/external/tlsf.c

    source/external/volk.h
    source/external/volk.c

    source/external/json.hpp

    source/external/imgui/imconfig.h
    source/external/imgui/imgui_draw.cpp
    source/external/imgui/imgui_impl_sdl3.cpp
    source/external/imgui/imgui_impl_sdl3.h
    source/external/imgui/imgui_internal.h
    source/external/imgui/imgui_memory_editor.h
    source/external/imgui/imgui_tables.cpp
    source/external/imgui/imgui_widgets.cpp
    source/external/imgui/imgui.cpp
    source/external/imgui/imgui.h
    source/external/imgui/imstb_rectpack.h
    source/external/imgui/imstb_textedit.h
    source/external/imgui/imstb_truetype.h
    source/external/imgui/stb_image.h
    source/external/imgui/TextEditor.cpp
    source/external/imgui/TextEditor.h

    source/external/tracy/tracy/Tracy.hpp
    source/external/tracy/tracy/TracyVulkan.hpp
    source/external/tracy/TracyClient.cpp

    source/external/enkiTS/LockLessMultiReadPipe.h
    source/external/enkiTS/TaskScheduler.cpp
    source/external/enkiTS/TaskScheduler.h

    source/external/meshoptimizer/allocator.cpp
    source/external/meshoptimizer/clusterizer.cpp
    source/external/meshoptimizer/indexanalyzer.cpp
    source/external/meshoptimizer/indexcodec.cpp
    source/external/meshoptimizer/indexgenerator.cpp
    source/external/meshoptimizer/meshoptimizer.h
    source/external/meshoptimizer/overdrawoptimizer.cpp
    source/external/meshoptimizer/partition.cpp
    source/external/meshoptimizer/quantization.cpp
    source/external/meshoptimizer/rasterizer.cpp
    source/external/meshoptimizer/simplifier.cpp
    source/external/meshoptimizer/spatialorder.cpp
    source/external/meshoptimizer/stripifier.cpp
    source/external/meshoptimizer/vcacheoptimizer.cpp
    source/external/meshoptimizer/vertexcodec.cpp
    source/external/meshoptimizer/vertexfilter.cpp
    source/external/meshoptimizer/vfetchoptimizer.cpp

    source/external/SPIRV-Reflect/spirv_reflect.c

    source/external/ufbx/ufbx.c

    source/external/glm/glm.natvis
)

if (WIN32)
    target_sources(RaptorExternal PRIVATE
        source/external/StackWalker.cpp)
endif()

target_compile_definitions(RaptorExternal PRIVATE
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_SYSTEM_TRACING

    UFBX_REAL_IS_FLOAT
)

if (WIN32)
    target_compile_options(RaptorExternal PRIVATE
        /MP
    )

    target_include_directories(RaptorExternal PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        binaries/SDL3-3.2.16/include
        source/raptor
    )

    target_compile_definitions(RaptorExternal PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        VK_USE_PLATFORM_WIN32_KHR
    )
else()
    target_link_libraries(RaptorExternal PRIVATE
        dl
    )
    target_include_directories(RaptorExternal PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS})
endif()

target_compile_definitions(RaptorExternal PRIVATE VK_NO_PROTOTYPES)

set_property(TARGET RaptorExternal PROPERTY CXX_STANDARD 20)

# RaptorGraphics #########################################################

add_library(RaptorGraphics STATIC
    source/raptor/graphics/asynchronous_loader.cpp
    source/raptor/graphics/asynchronous_loader.hpp
    source/raptor/graphics/command_buffer.cpp
    source/raptor/graphics/command_buffer.hpp
    source/raptor/graphics/fbx_model.cpp
    source/raptor/graphics/fbx_model.hpp
    source/raptor/graphics/frame_graph.cpp
    source/raptor/graphics/frame_graph.hpp
    source/raptor/graphics/frame_renderer.hpp
    source/raptor/graphics/frame_renderer.cpp
    source/raptor/graphics/gltf_model.cpp
    source/raptor/graphics/gltf_model.hpp
    source/raptor/graphics/gpu_device.cpp
    source/raptor/graphics/gpu_device.hpp
    source/raptor/graphics/gpu_enum.hpp
    source/raptor/graphics/gpu_profiler.cpp
    source/raptor/graphics/gpu_profiler.hpp
    source/raptor/graphics/gpu_resources.cpp
    source/raptor/graphics/gpu_resources.hpp
    source/raptor/graphics/mesh.hpp
    source/raptor/graphics/obj_model.cpp
    source/raptor/graphics/obj_model.hpp
    source/raptor/graphics/render_blackboard.hpp
    source/raptor/graphics/render_blackboard.cpp
    source/raptor/graphics/render_scene.cpp
    source/raptor/graphics/render_scene.hpp
    source/raptor/graphics/renderer.cpp
    source/raptor/graphics/renderer.hpp
    source/raptor/graphics/scene_graph.cpp
    source/raptor/graphics/scene_graph.hpp
    source/raptor/graphics/shader_compiler.cpp
    source/raptor/graphics/shader_compiler.hpp
    source/raptor/graphics/shader_reflection.hpp
    source/raptor/graphics/spirv_parser.cpp
    source/raptor/graphics/spirv_parser.hpp
    source/raptor/graphics/raptor_imgui.cpp
    source/raptor/graphics/raptor_imgui.hpp
)

set_property(TARGET RaptorGraphics PROPERTY CXX_STANDARD 20)

target_compile_definitions(RaptorGraphics PRIVATE
    ${RAPTOR_DEFINES}
)

target_compile_definitions(RaptorGraphics PRIVATE
    RAPTOR_WORKING_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}"
    RAPTOR_SHADER_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/"
    RAPTOR_DATA_FOLDER="${CMAKE_SOURCE_DIR}/binaries/data"

    UFBX_REAL_IS_FLOAT
)

target_include_directories(RaptorGraphics PRIVATE
    .
    ..
    raptor
    binaries/assimp/include
    ${Vulkan_INCLUDE_DIRS}
)


target_compile_definitions(RaptorGraphics PRIVATE VK_NO_PROTOTYPES)


if (WIN32)
    target_link_directories(RaptorGraphics PRIVATE
        binaries/assimp/windows/bin
        binaries/assimp/windows/lib
        binaries/SDL3-3.2.16/lib/x64
    )

    target_include_directories(RaptorGraphics PRIVATE
        binaries/SDL3-3.2.16/include)
else()
    target_link_directories(RaptorGraphics PRIVATE
        binaries/assimp/linux/lib)

    target_include_directories(RaptorGraphics PRIVATE
        ${SDL2_INCLUDE_DIRS})
endif()

if (WIN32)
    target_link_libraries(RaptorGraphics PRIVATE

        debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
        debug $ENV{VULKAN_SDK}/Lib/spirv-cross-reflectd.lib optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-reflect.lib
        debug $ENV{VULKAN_SDK}/Lib/slangd.lib optimized $ENV{VULKAN_SDK}/Lib/slang.lib
        debug $ENV{VULKAN_SDK}/Lib/slang-rtd.lib optimized $ENV{VULKAN_SDK}/Lib/slang-rt.lib

        assimp-vc143-mt
        SDL3)
else()
    target_link_libraries(RaptorGraphics PRIVATE
        dl
        pthread
        assimp
        SDL2::SDL2)
endif()

target_link_libraries(RaptorGraphics PRIVATE
    RaptorFoundation
    RaptorExternal
    RaptorApp
)


# Add common options specified in cmake/RaptorOptions.cmake file
target_link_libraries(RaptorFoundation PUBLIC RaptorBuildOptions)
target_link_libraries(RaptorApp        PUBLIC RaptorBuildOptions)
target_link_libraries(RaptorExternal   PUBLIC RaptorBuildOptions)
target_link_libraries(RaptorGraphics   PUBLIC RaptorBuildOptions)

add_subdirectory(source/chapter01)
add_subdirectory(source/chapter02)
add_subdirectory(source/chapter03)
add_subdirectory(source/chapter04)
add_subdirectory(source/chapter05)
add_subdirectory(source/chapter06)
add_subdirectory(source/chapter11)
add_subdirectory(source/chapter12)
add_subdirectory(source/chapter14)
#add_subdirectory(source/chapter15)

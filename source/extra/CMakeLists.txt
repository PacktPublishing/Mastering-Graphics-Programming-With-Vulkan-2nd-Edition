project(RaptorExtra VERSION 0.1.0)

# Configuration based setup
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin)
# Set configuration dependant names
foreach( OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES} )
    # Output folder
    string( TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG )
    set( CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_HOME_DIRECTORY}/bin )
endforeach( OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES )

add_executable(${PROJECT_NAME}
    ../raptor/graphics/render_passes/culling_pass.hpp
    ../raptor/graphics/render_passes/culling_pass.cpp
    ../raptor/graphics/render_passes/ddgi_pass.hpp
    ../raptor/graphics/render_passes/ddgi_pass.cpp
    ../raptor/graphics/render_passes/debug_pass.hpp
    ../raptor/graphics/render_passes/debug_pass.cpp
    ../raptor/graphics/render_passes/depth_of_field_pass.hpp
    ../raptor/graphics/render_passes/depth_of_field_pass.cpp
    ../raptor/graphics/render_passes/depth_pre_pass.hpp
    ../raptor/graphics/render_passes/depth_pre_pass.cpp
    ../raptor/graphics/render_passes/depth_pyramid_pass.hpp
    ../raptor/graphics/render_passes/depth_pyramid_pass.cpp
    ../raptor/graphics/render_passes/gbuffer_pass.hpp
    ../raptor/graphics/render_passes/gbuffer_pass.cpp
    ../raptor/graphics/render_passes/lighting_pass.hpp
    ../raptor/graphics/render_passes/lighting_pass.cpp
    ../raptor/graphics/render_passes/motion_vector_pass.hpp
    ../raptor/graphics/render_passes/motion_vector_pass.cpp
    ../raptor/graphics/render_passes/pointlight_shadow_pass.hpp
    ../raptor/graphics/render_passes/pointlight_shadow_pass.cpp
    ../raptor/graphics/render_passes/raytraced_reflections_pass.hpp
    ../raptor/graphics/render_passes/raytraced_reflections_pass.cpp
    ../raptor/graphics/render_passes/raytracing_test_pass.hpp
    ../raptor/graphics/render_passes/raytracing_test_pass.cpp
    ../raptor/graphics/render_passes/shadow_visibility_pass.hpp
    ../raptor/graphics/render_passes/shadow_visibility_pass.cpp
    ../raptor/graphics/render_passes/svgf_pass.hpp
    ../raptor/graphics/render_passes/svgf_pass.cpp
    ../raptor/graphics/render_passes/temporal_anti_aliasing_pass.hpp
    ../raptor/graphics/render_passes/temporal_anti_aliasing_pass.cpp
    ../raptor/graphics/render_passes/transparent_pass.hpp
    ../raptor/graphics/render_passes/transparent_pass.cpp
    ../raptor/graphics/render_passes/volumetric_fog_pass.hpp
    ../raptor/graphics/render_passes/volumetric_fog_pass.cpp
    main.cpp
)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)

foreach( OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES} )
    # Output executable name based on configuration.
    # OUTPUTCONFIG used for RUNTIME_OUTPUT_NAME_ must be uppercase.
    # OUTPUT_NAME for the executable is lowercase.
    string( TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG )
    string( TOLOWER ${OUTPUTCONFIG} OUTPUT_NAME )
    set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_NAME_${OUTPUTCONFIG} "${PROJECT_NAME}_${OUTPUT_NAME}")
endforeach( OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES )

if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        VK_USE_PLATFORM_WIN32_KHR
        NOMINMAX)

    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP
    )
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    RAPTOR_WORKING_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}/../raptor"
    RAPTOR_SHADER_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}/../raptor/shaders/"
    RAPTOR_DATA_FOLDER="${CMAKE_SOURCE_DIR}/binaries/data"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_SYSTEM_TRACING
)

target_include_directories(${PROJECT_NAME} PRIVATE
    .
    ..
    ../raptor
    ../../binaries/assimp/include
    ${Vulkan_INCLUDE_DIRS}
)

if (WIN32)
    target_link_directories(${PROJECT_NAME} PRIVATE
        ../../binaries/assimp/windows/bin
        ../../binaries/assimp/windows/lib
        ../../binaries/SDL3-3.2.16/lib/x64
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        ../../binaries/SDL3-3.2.16/include)
else()
    target_link_directories(${PROJECT_NAME} PRIVATE
        ../../binaries/assimp/linux/lib)

    target_include_directories(${PROJECT_NAME} PRIVATE
        ${SDL2_INCLUDE_DIRS})
endif()

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE

        debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
        debug $ENV{VULKAN_SDK}/Lib/spirv-cross-reflectd.lib optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-reflect.lib
        debug $ENV{VULKAN_SDK}/Lib/slangd.lib optimized $ENV{VULKAN_SDK}/Lib/slang.lib
        debug $ENV{VULKAN_SDK}/Lib/slang-rtd.lib optimized $ENV{VULKAN_SDK}/Lib/slang-rt.lib

        assimp-vc143-mt
        SDL3)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dl
        pthread
        assimp
        SDL2::SDL2)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    RaptorFoundation
    RaptorExternal
    RaptorApp
    RaptorGraphics
)

if (WIN32)
    set(DLLS_TO_COPY
        ${CMAKE_CURRENT_SOURCE_DIR}/../../binaries/SDL3-3.2.16/lib/x64/SDL3.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/../../binaries/assimp/windows/bin/assimp-vc143-mt.dll
    )

    foreach(DLL ${DLLS_TO_COPY})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${DLL} "$(OutDir)"
            VERBATIM
        )
    endforeach()
endif()

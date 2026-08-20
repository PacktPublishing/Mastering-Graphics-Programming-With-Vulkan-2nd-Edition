function(raptor_add_chapter target_name)
  add_executable(${target_name} ${ARGN})

  # Name exe with config suffix (bin/Chapter2_Debug ecc.)
  set_target_properties(${target_name} PROPERTIES
    RUNTIME_OUTPUT_NAME "${target_name}_$<CONFIG>"
  )

  target_link_libraries(${target_name} PRIVATE
    RaptorBuildOptions
    RaptorFoundation
    RaptorExternal
    RaptorApp
    RaptorGraphics
  )

  target_compile_definitions(${target_name} PRIVATE
    RAPTOR_WORKING_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}/../raptor"
    RAPTOR_SHADER_FOLDER="${CMAKE_CURRENT_SOURCE_DIR}/../shaders/"
    RAPTOR_DATA_FOLDER="${CMAKE_SOURCE_DIR}/binaries/data"
  )
endfunction()

function(raptor_link_chapter_deps target_name)

  target_include_directories(${target_name} PRIVATE ${Vulkan_INCLUDE_DIRS})

  # Assimp include is common
  target_include_directories(${target_name} PRIVATE
    ${CMAKE_SOURCE_DIR}/binaries/assimp/include
  )

  set(RAPTOR_DEFINES
    _CRT_SECURE_NO_WARNINGS

    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_SYSTEM_TRACING

    GLM_FORCE_DEPTH_ZERO_TO_ONE
  )

  target_compile_definitions(${target_name} PRIVATE
      ${RAPTOR_DEFINES}
  )

  if (WIN32)
    target_include_directories(${target_name} PRIVATE
      ${CMAKE_SOURCE_DIR}/binaries/SDL3-3.2.16/include
    )

    target_link_directories(${target_name} PRIVATE
      ${CMAKE_SOURCE_DIR}/binaries/assimp/windows/bin
      ${CMAKE_SOURCE_DIR}/binaries/assimp/windows/lib
      ${CMAKE_SOURCE_DIR}/binaries/SDL3-3.2.16/lib/x64
    )

    target_link_libraries(${target_name} PRIVATE
      assimp-vc143-mt
      SDL3
    )

    target_link_libraries(${target_name} PRIVATE
      debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib    optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
      debug $ENV{VULKAN_SDK}/Lib/spirv-cross-reflectd.lib optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-reflect.lib
      debug $ENV{VULKAN_SDK}/Lib/slangd.lib               optimized $ENV{VULKAN_SDK}/Lib/slang.lib
      debug $ENV{VULKAN_SDK}/Lib/slang-rtd.lib            optimized $ENV{VULKAN_SDK}/Lib/slang-rt.lib
    )

    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/binaries/SDL3-3.2.16/lib/x64/SDL3.dll"
        "$<TARGET_FILE_DIR:${target_name}>"
      VERBATIM
    )

    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/binaries/assimp/windows/bin/assimp-vc143-mt.dll"
        "$<TARGET_FILE_DIR:${target_name}>"
      VERBATIM
    )

  else()
    target_include_directories(${target_name} PRIVATE
      ${SDL2_INCLUDE_DIRS}
    )

    target_link_directories(${target_name} PRIVATE
      ${CMAKE_SOURCE_DIR}/binaries/assimp/linux/lib
    )

    target_link_libraries(${target_name} PRIVATE
      dl
      pthread
      assimp
      SDL2::SDL2
    )
  endif()
endfunction()

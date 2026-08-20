add_library(RaptorBuildOptions INTERFACE)

target_compile_features(RaptorBuildOptions INTERFACE cxx_std_20)

target_compile_definitions(RaptorBuildOptions INTERFACE
  TRACY_ENABLE
  TRACY_ON_DEMAND
  TRACY_NO_SYSTEM_TRACING
)

if (WIN32)
  target_compile_definitions(RaptorBuildOptions INTERFACE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    VK_USE_PLATFORM_WIN32_KHR
    VK_NO_PROTOTYPES
  )
  target_compile_options(RaptorBuildOptions INTERFACE /MP)
endif()

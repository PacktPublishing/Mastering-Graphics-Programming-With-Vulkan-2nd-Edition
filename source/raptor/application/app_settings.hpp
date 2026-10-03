#pragma once

#include "foundation/platform.hpp"

#include "graphics/gpu_enum.hpp"

#include "external/glm/vec3.hpp"

namespace raptor {

struct AppSettings {

    void            load( cstring data_folder, cstring chapter );

    // window
    u32             window_width            = 1280;
    u32             window_height           = 800;

    // camera
    f32             camera_near             = 0.1f;
    f32             camera_far              = 100.f;
    f32             camera_fov_y            = 60.f;
    f32             camera_rotation_speed   = 20.f;
    f32             camera_movement_speed   = 6.f;
    f32             camera_movement_delta   = 0.1f;

    glm::vec3       camera_position         = { -3.f, 1.f, 0.f };
    glm::vec3       camera_direction        = { 1.f, 0.f, 0.f };

    // gpu
    bool            gpu_validation          = false;
    u32             gpu_buffer_pool         = 1024;
    bool            gpu_timestamps          = true;

    VulkanDebugMode::Enum gpu_debug = VulkanDebugMode::Default;   // replaces gpu_validation
    PresentMode::Enum gpu_present_mode = PresentMode::VSync;

    bool            upscale_enabled = false;
    f32             upscale_render_scale = 0.75f;

    // memory
    u32             memory_max_dynamic_mb   = 2048;

}; // struct AppSettings

} // namespace raptor

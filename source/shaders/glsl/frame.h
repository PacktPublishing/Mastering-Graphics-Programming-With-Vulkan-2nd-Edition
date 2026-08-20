
#ifndef RAPTOR_GLSL_SCENE_H
#define RAPTOR_GLSL_SCENE_H

#include "../shared_structs.h"

// Scene common code /////////////////////////////////////////////////////
layout ( std140, set = MATERIAL_SET, binding = 0 ) uniform FrameConstants {
    GpuFrameConstants frame;
};

bool enable_volumetric_fog_opacity_anti_aliasing() {
    return (frame.volumetric_fog_application_options & 1) == 1;
}

bool enable_volumetric_fog_opacity_tricubic_filtering() {
    return (frame.volumetric_fog_application_options & 2) == 2;
}

// Options ///////////////////////////////////////////////////////////////
bool disable_frustum_cull_meshes() {
    return (frame.culling_options & 1) != 1;
}

bool disable_frustum_cull_meshlets() {
    return (frame.culling_options & 2) != 2;
}

bool disable_occlusion_cull_meshes() {
    return (frame.culling_options & 4) != 4;
}

bool disable_occlusion_cull_meshlets() {
    return (frame.culling_options & 8) != 8;
}

bool freeze_occlusion_camera() {
    return (frame.culling_options & 16) == 16;
}

bool disable_shadow_meshlets_cone_cull() {
    return ( frame.culling_options & 32 ) != 32;
}

bool disable_shadow_meshlets_sphere_cull() {
    return ( frame.culling_options & 64 ) != 64;
}

bool disable_shadow_meshlets_cubemap_face_cull() {
    return ( frame.culling_options & 128 ) != 128;
}

bool disable_shadow_meshes_sphere_cull() {
    return ( frame.culling_options & 256 ) != 256;
}

// Utility methods ///////////////////////////////////////////////////////
float dither(vec2 screen_pixel_position, float value)
{
    float dither_value = texelFetch(global_textures[nonuniformEXT(frame.dither_texture_index)], ivec2(int(screen_pixel_position.x) % 4, int(screen_pixel_position.y) % 4), 0).r;
    return value - dither_value;
}

// Convert raw_depth (0..1) to linear depth (near...far)
float linearize_raw_depth(float raw_depth) {
    // NOTE(marco): Vulkan depth is [0, 1]
    return frame.z_near * frame.z_far / (frame.z_far + raw_depth * (frame.z_near - frame.z_far));
}

#endif // RAPTOR_GLSL_SCENE_H

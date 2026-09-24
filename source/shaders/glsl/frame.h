
#ifndef RAPTOR_GLSL_FRAME_H
#define RAPTOR_GLSL_FRAME_H

#include "../shared_structs.h"

// Scene common code /////////////////////////////////////////////////////
layout ( std140, set = MATERIAL_SET, binding = 0 ) uniform FrameConstants {
    GpuFrameConstants frame;
};

bool enable_volumetric_fog_opacity_anti_aliasing() {
    return (frame.volumetric_fog_application_options & k_vfog_opacity_anti_aliasing) != 0u;
}

bool enable_volumetric_fog_opacity_tricubic_filtering() {
    return (frame.volumetric_fog_application_options & k_vfog_tricubic_filtering) != 0u;
}

// Options ///////////////////////////////////////////////////////////////
bool disable_frustum_cull_meshes() {
    return ( frame.culling_options & k_culling_frustum_meshes ) == 0u;
}

bool disable_frustum_cull_meshlets() {
    return ( frame.culling_options & k_culling_frustum_meshlets ) == 0u;
}

bool disable_occlusion_cull_meshes() {
    return ( frame.culling_options & k_culling_occlusion_meshes ) == 0u;
}

bool disable_occlusion_cull_meshlets() {
    return ( frame.culling_options & k_culling_occlusion_meshlets ) == 0u;
}

bool freeze_occlusion_camera() {
    return ( frame.culling_options & k_culling_freeze_occlusion_camera ) != 0u;
}

bool disable_shadow_meshlets_cone_cull() {
    return ( frame.culling_options & k_culling_shadow_meshlets_cone ) == 0u;
}

bool disable_shadow_meshlets_sphere_cull() {
    return ( frame.culling_options & k_culling_shadow_meshlets_sphere ) == 0u;
}

bool disable_shadow_meshlets_cubemap_face_cull() {
    return ( frame.culling_options & k_culling_shadow_meshlets_cubemap_face ) == 0u;
}

bool disable_shadow_meshes_sphere_cull() {
    return ( frame.culling_options & k_culling_shadow_meshes_sphere ) == 0u;
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

// Utilities for camera freeze
mat4 culling_world_to_camera() {
    return freeze_occlusion_camera() ? frame.world_to_camera_debug : frame.world_to_camera;
}

vec3 culling_camera_position() {
    return freeze_occlusion_camera() ? frame.camera_position_debug.xyz : frame.camera_position.xyz;
}

mat4 get_culling_view_projection( bool late ) {
    if ( freeze_occlusion_camera() ) {
        return frame.view_projection_debug;
    }
    return late ? frame.view_projection : frame.previous_view_projection;
}

#endif // RAPTOR_GLSL_FRAME_H

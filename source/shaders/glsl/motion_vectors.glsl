#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#include "frame.h"


#if defined(COMPUTE_COMPOSITE_CAMERA_MOTION)

layout( push_constant ) uniform MotionVectorPushConstants {
    uint motion_vectors_index;
    uint visibility_motion_vectors_index;
    uint normals_index;
    uint pad000_mvpc;
} pc;

layout( rg16f, set = GLOBAL_SET, binding = BINDLESS_IMAGES ) uniform image2D global_images_2d_rg16f[];

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    if ( pos.x >= frame.resolution.x || pos.y >= frame.resolution.y ) {
        return;
    }

    const float raw_depth = texelFetch(global_textures[nonuniformEXT(frame.depth_texture_index)], pos.xy, 0).r;
    const vec2 screen_uv = uv_nearest(pos.xy, frame.resolution);
    const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

    vec4 current_position_ndc = vec4( ndc_from_uv_raw_depth( screen_uv, raw_depth ), 1.0f );
    vec4 previous_position_ndc = frame.previous_view_projection * vec4(pixel_world_position, 1.0f);
    previous_position_ndc.xyz /= previous_position_ndc.w;

    vec2 jitter_difference = (frame.jitter_xy - frame.previous_jitter_xy);
    vec2 velocity = current_position_ndc.xy - previous_position_ndc.xy;
    velocity -= jitter_difference;

    imageStore( global_images_2d[ pc.motion_vectors_index ], pos.xy, vec4(velocity, 0, 0) );

    // NOTE(marco): compute values for shadow visibility buffer
    vec2 encoded_normal = texelFetch( global_textures[nonuniformEXT(pc.normals_index)], pos.xy, 0 ).rg;
    vec3 normal = octahedral_decode( encoded_normal );
    vec4 view_normal = frame.world_to_camera * vec4( normal, 0.0 );

    float c1 = 0.003;
    float c2 = 0.017;
    float depth_diff = abs( 1.0 - ( previous_position_ndc.z / current_position_ndc.z ) );
    float eps = c1 + c2 * abs( view_normal.z );

    vec2 visibility_velocity = current_position_ndc.xy - previous_position_ndc.xy;
    visibility_velocity = velocity;

    vec2 visibility_motion = depth_diff < eps ? visibility_velocity : vec2( -1, -1 );
    //visibility_motion = velocity;

    // TODO(marco): the article wants to store the previous depth value, but it doesn't look like it's needed?!
    imageStore( global_images_2d[ pc.visibility_motion_vectors_index  ], pos.xy, vec4(visibility_motion, 0, 0) );
}

#endif // COMPUTE_COMPOSITE_CAMERA_MOTION

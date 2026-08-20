#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : enable

#include "platform.glslh"

struct ray_payload {
    int instance_id;
    int geometry_id;
    int primitive_id;
    vec2 barycentric_weights;
    float t;
};

#ifdef RAYGEN

#include "frame.h"
#include "mesh.h"

layout( location = 0 ) rayPayloadEXT ray_payload payload;

layout( binding = 1, set = MATERIAL_SET ) uniform accelerationStructureEXT as;
layout( binding = 3, set = MATERIAL_SET ) uniform rayParams
{
    uint sbt_offset; // shader binding table offset
    uint sbt_stride; // shader binding table stride
    uint miss_index;
    uint out_image_index;
};

vec3 compute_ray_dir( uvec3 launch_id, uvec3 launch_size ) {

    const float ndc_x = 2.0 * ( float( launch_id.x ) + 0.5 ) / float( launch_size.x ) - 1.0;
    const float ndc_y = 1.0 - 2.0 * ( float( launch_id.y ) + 0.5 ) / float( launch_size.y );

    vec4 world_far = frame.inverse_view_projection * vec4( ndc_x, ndc_y, 1.0, 1.0 );

    world_far /= world_far.w;

    return normalize( world_far.xyz - frame.camera_position.xyz );
}

float cross2( vec2 a, vec2 b ) {
    return a.x * b.y - a.y * b.x;
}

float compute_projected_triangle_lod( vec4 p0_world, vec4 p1_world, vec4 p2_world,
                                      vec2 uv0, vec2 uv1, vec2 uv2,
                                      ivec2 texture_size, vec2 screen_size ) {
    vec4 p0_clip = frame.view_projection * p0_world;
    vec4 p1_clip = frame.view_projection * p1_world;
    vec4 p2_clip = frame.view_projection * p2_world;

    vec2 p0_ndc = p0_clip.xy / p0_clip.w;
    vec2 p1_ndc = p1_clip.xy / p1_clip.w;
    vec2 p2_ndc = p2_clip.xy / p2_clip.w;

    vec2 p0_px = ( p0_ndc * 0.5 + 0.5 ) * screen_size;
    vec2 p1_px = ( p1_ndc * 0.5 + 0.5 ) * screen_size;
    vec2 p2_px = ( p2_ndc * 0.5 + 0.5 ) * screen_size;

    // Both are parallelogram areas. The 0.5 triangle factor cancels out.
    float uv_area = abs( cross2( uv1 - uv0, uv2 - uv0 ) );
    float screen_area = abs( cross2( p1_px - p0_px, p2_px - p0_px ) );

    float texture_texel_count = float( texture_size.x * texture_size.y );
    float texel_area = texture_texel_count * uv_area;

    float texels_per_pixel = texel_area / max( screen_area, 1.0 );

    float lod = 0.5 * log2( max( texels_per_pixel, 1e-8 ) );
    return max( lod, 0.0 );
}

void main()
{
    traceRayEXT( as, // topLevel
                 gl_RayFlagsOpaqueEXT, // rayFlags
                 0xff, // cullMask
                 sbt_offset, // sbtRecordOffset
                 sbt_stride, // sbtRecordStride
                 miss_index, // missIndex
                 frame.camera_position.xyz, // origin
                 0.0, // Tmin
                 compute_ray_dir( gl_LaunchIDEXT, gl_LaunchSizeEXT ), // direction
                 100.0, // Tmax
                 0 // payload index
                );

    if ( payload.instance_id != -1 ) {
        uint mesh_instance_index = payload.instance_id + payload.geometry_id;
        MeshInstanceDraw instance = mesh_instance_draws[ mesh_instance_index ];
        uint mesh_index = instance.mesh_draw_index;
        MeshDraw mesh = mesh_draws[ mesh_index ];

        vec2 screen_size = vec2(frame.resolution.x, frame.resolution.y);
        ivec2 texture_size = textureSize( global_textures[ nonuniformEXT( mesh.textures.x ) ], 0 );

        int_array_type index_buffer = int_array_type( mesh.index_buffer );
        int i0 = index_buffer[ payload.primitive_id * 3 ].v;
        int i1 = index_buffer[ payload.primitive_id * 3 + 1 ].v;
        int i2 = index_buffer[ payload.primitive_id * 3 + 2 ].v;

        float_array_type vertex_buffer = float_array_type( mesh.position_buffer );
        vec4 p0 = vec4(
            vertex_buffer[ i0 * 3 + 0 ].v,
            vertex_buffer[ i0 * 3 + 1 ].v,
            vertex_buffer[ i0 * 3 + 2 ].v,
            1.0 );
        vec4 p1 = vec4(
            vertex_buffer[ i1 * 3 + 0 ].v,
            vertex_buffer[ i1 * 3 + 1 ].v,
            vertex_buffer[ i1 * 3 + 2 ].v,
            1.0 );
        vec4 p2 = vec4(
            vertex_buffer[ i2 * 3 + 0 ].v,
            vertex_buffer[ i2 * 3 + 1 ].v,
            vertex_buffer[ i2 * 3 + 2 ].v,
            1.0 );


        vec2_array_type uv_buffer = vec2_array_type( mesh.uv_buffer );
        vec2 uv0 = uv_buffer[ i0 ].v;
        vec2 uv1 = uv_buffer[ i1 ].v;
        vec2 uv2 = uv_buffer[ i2 ].v;

        vec4 p0_world = vec4( instance.model * p0 );
        vec4 p1_world = vec4( instance.model * p1 );
        vec4 p2_world = vec4( instance.model * p2 );

        // TODO(marco): use ray differentials or ray cones.
        float lod = compute_projected_triangle_lod( p0_world, p1_world, p2_world,
                                                    uv0, uv1, uv2, texture_size, screen_size );

        float b = payload.barycentric_weights.x;
        float c = payload.barycentric_weights.y;
        float a = 1 - b - c;

        vec2 uv = ( a * uv0 + b * uv1 + c * uv2 );

        vec3 diffuse = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod ).rgb;

        imageStore( global_images_2d[ out_image_index ], ivec2( gl_LaunchIDEXT.xy ), vec4( diffuse, 1.0 ) );
    } else {
        vec4 color = vec4( 0.529, 0.807, 0.921, 1 );
        imageStore( global_images_2d[ out_image_index ], ivec2( gl_LaunchIDEXT.xy ), color );
    }
}

#endif

#ifdef CLOSEST_HIT

layout( location = 0 ) rayPayloadInEXT ray_payload payload;
hitAttributeEXT vec2 barycentric_weights;

void main() {
    payload.instance_id = gl_InstanceCustomIndexEXT;
    payload.geometry_id = gl_GeometryIndexEXT;
    payload.primitive_id = gl_PrimitiveID;
    payload.barycentric_weights = barycentric_weights;
    payload.t = gl_HitTEXT;
}

#endif

#ifdef MISS

layout( location = 0 ) rayPayloadInEXT ray_payload payload;

void main() {
    payload.instance_id = -1;
    payload.geometry_id = -1;
    payload.primitive_id = -1;
    payload.barycentric_weights = vec2( 0 );
}

#endif

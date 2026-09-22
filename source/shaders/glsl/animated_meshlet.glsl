#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "meshlet.h"

// Common bindings ///////////////////////////////////////////////////////

layout(set = MATERIAL_SET, binding = 1) buffer Meshlets
{
    Meshlet meshlets[];
};

layout(set = MATERIAL_SET, binding = 3) readonly buffer MeshletData
{
    uint meshletData[];
};

layout(set = MATERIAL_SET, binding = 5) buffer VertexData
{
    VertexExtraData vertex_data[];
};

layout(set = MATERIAL_SET, binding = 9) buffer OutMeshletVertexBuffer
{
    VertexPosition meshlet_vertex_buffer[];
};

layout(set = MATERIAL_SET, binding = 11) readonly buffer InMeshletDataBuffer
{
    VertexExtraData meshlet_vertex_data[];
};

layout( push_constant ) uniform PushConstants
{
    uint mesh_index;
};

layout(set = MATERIAL_SET, binding = 15) buffer VisibleMeshCount
{
    uint opaque_mesh_visible_count;
    uint pad002_vmc;
    uint transparent_mesh_visible_count;
    uint transparent_mesh_culled_count;

    uint mesh_instances_count;
    uint depth_pyramid_texture_index;
    uint pad003_vmc;
    uint meshlet_index_count;

    uint dispatch_task_x;
    uint dispatch_task_y;
    uint dispatch_task_z;
    uint pad001_vmc;
};

#if defined(COMPUTE_ANIMATED_MESHLET_VERTEX_UPDATE)

layout(std430, set = MATERIAL_SET, binding = 6) buffer VertexBuffer
{
    float mesh_vertex_buffer[];
};

layout(set = MATERIAL_SET, binding = 7) buffer JointIndexBuffer
{
    u16vec4 joint_index_buffer[];
};

layout(set = MATERIAL_SET, binding = 8) buffer JointWeightBuffer
{
    vec4 joint_weight_buffer[];
};

layout(std430, set = MATERIAL_SET, binding = 10) readonly buffer JointMatrices {
	mat4 joint_matrices[];
};


layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
void main() {

    // Each thread processes a vertex
    uint vertex_index = gl_GlobalInvocationID.x;
    MeshDraw mesh_draw = mesh_draws[ mesh_index ];

    if ( vertex_index >= mesh_draw.vertex_count ) {
        return;
    }

    uint global_vertex_index = ( mesh_draw.position_buffer_offset / 12 ) + vertex_index;

    vec3 vertex_position = vec3( mesh_vertex_buffer[ global_vertex_index * 3 + 0 ],
                                 mesh_vertex_buffer[ global_vertex_index * 3 + 1 ],
                                 mesh_vertex_buffer[ global_vertex_index * 3 + 2 ] );
    u16vec4 joint_indices = joint_index_buffer[ global_vertex_index ];
    vec4 joint_weights = joint_weight_buffer[ global_vertex_index ];

    mat4 skinning_transform =
        joint_weights.x * joint_matrices[(joint_indices.x)] +
        joint_weights.y * joint_matrices[(joint_indices.y)] +
        joint_weights.z * joint_matrices[(joint_indices.z)] +
        joint_weights.w * joint_matrices[(joint_indices.w)];

    vec4 animated_position = skinning_transform * vec4( vertex_position, 1.0 );

    float i8_inverse = 1.0 / 127.0;
    vec3 normal_os = vec3( int( meshlet_vertex_data[ global_vertex_index ].nx),
                           int( meshlet_vertex_data[ global_vertex_index ].ny),
                           int( meshlet_vertex_data[ global_vertex_index ].nz) ) * i8_inverse - 1.0;

    vec4 animated_normal = skinning_transform * vec4( normal_os, 0.0 );
    animated_normal.xyz = normalize( animated_normal.xyz );

    meshlet_vertex_buffer[ global_vertex_index ].v = animated_position.xyz;

    vertex_data[ global_vertex_index ].nx = uint8_t( ( animated_normal.x + 1.0 ) * 127.0 );
    vertex_data[ global_vertex_index ].ny = uint8_t( ( animated_normal.y + 1.0 ) * 127.0 );
    vertex_data[ global_vertex_index ].nz = uint8_t( ( animated_normal.z + 1.0 ) * 127.0 );
}

#endif // COMPUTE_ANIMATED_MESHLET_VERTEX_UPDATE

#if defined(COMPUTE_ANIMATED_MESHLET_UPDATE)

int8_t to_int8( float value ) {
    float round = (value >= 0 ? 0.5f : -0.5f);

    value = (value >= -1) ? value : -1;
    value = (value <= +1) ? value : +1;

    return int8_t( value * 127.0f + round );
}

layout (local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
void main() {
    // Each thread group processes a meshlet
    uint meshlet_index = gl_WorkGroupID.x;
    MeshDraw mesh_draw = mesh_draws[ mesh_index ];

    if ( meshlet_index >= mesh_draw.meshlet_count ) {
        return;
    }

    uint global_meshlet_index = mesh_draw.meshlet_offset + meshlet_index;
    Meshlet meshlet = meshlets[ global_meshlet_index ];

    uint vertex_count = uint(meshlet.vertex_count);
    uint triangle_count = uint(meshlet.triangle_count);
    uint start_vertex_index = gl_LocalInvocationID.x;
    uint connectivity_data_offset = meshlets[global_meshlet_index].connectivity_data_offset;
    uint vertex_offset = connectivity_data_offset;

    float i8_inverse = 1.0 / 127.0;

    vec3 pos_min = vec3( 1e10 );
    vec3 pos_max = vec3( -1e10 );
    vec3 cone_min = vec3( 1e10 );
    vec3 cone_max = vec3( -1e10 );

    vec3 cached_normals[3];
    uint cache_count = 0;
    for (uint i = start_vertex_index; i < vertex_count; i += 32)
    {
        uint vi = meshletData[ vertex_offset + i ];

        vec3 position = vec3(meshlet_vertex_buffer[vi].v.x,
                             meshlet_vertex_buffer[vi].v.y,
                             meshlet_vertex_buffer[vi].v.z);

        pos_min = min( pos_min, position );
        pos_max = max( pos_max, position );

        vec3 normal_os = vec3( int(vertex_data[vi].nx),
                               int(vertex_data[vi].ny),
                               int(vertex_data[vi].nz) ) * i8_inverse - 1.0;
        normal_os = normalize( normal_os );

        cone_min = min( cone_min, normal_os );
        cone_max = max( cone_max, normal_os );

        // Cache normals in shared memory for second pass
        cached_normals[cache_count] = normal_os;
        cache_count++;
    }

    pos_min = subgroupMin( pos_min );
    pos_max = subgroupMax( pos_max );
    cone_min = subgroupMin( cone_min );
    cone_max = subgroupMax( cone_max );

    vec3 cone_avg = subgroupBroadcastFirst( ( cone_min + cone_max ) * 0.5 );
    float cone_len2 = dot( cone_avg, cone_avg );

    vec3 cone = vec3(0.0);
    float cutoff = 1.0;

    if (cone_len2 > 1e-6) {
        cone = cone_avg * inversesqrt( cone_len2 );

        for (uint i = 0; i < cache_count; i++)
        {
            cutoff = min( cutoff, dot( cached_normals[ i ], cone ) );
        }
    }
    cutoff = subgroupMin( cutoff );

    if ( gl_LocalInvocationID.x == 0 )
    {
        meshlets[ global_meshlet_index ].center = pos_min + 0.5 * ( pos_max - pos_min );
        meshlets[ global_meshlet_index ].radius = length( pos_max - pos_min ) * 0.5;

        if (cutoff <= 0.1) {
            meshlets[ global_meshlet_index ].cone_axis[0] = int8_t(0);
            meshlets[ global_meshlet_index ].cone_axis[1] = int8_t(0);
            meshlets[ global_meshlet_index ].cone_axis[2] = int8_t(0);
            meshlets[ global_meshlet_index ].cone_cutoff = int8_t(127);
        } else {
            // Same code as meshoptimizer
            int8_t cone_axis_x = to_int8( cone.x );
            int8_t cone_axis_y = to_int8( cone.y );
            int8_t cone_axis_z = to_int8( cone.z );

            float cone_axis_s8_e0 = abs( cone_axis_x / 127.f - cone.x );
            float cone_axis_s8_e1 = abs( cone_axis_y / 127.f - cone.y );
            float cone_axis_s8_e2 = abs( cone_axis_z / 127.f - cone.z );

            meshlets[ global_meshlet_index ].cone_axis[0] = to_int8( cone.x );
            meshlets[ global_meshlet_index ].cone_axis[1] = to_int8( cone.y );
            meshlets[ global_meshlet_index ].cone_axis[2] = to_int8( cone.z );

            float cone_cutoff = sqrt( 1.0 - cutoff * cutoff );
            int cone_cutoff_s8 = int(127 * (cone_cutoff + cone_axis_s8_e0 + cone_axis_s8_e1 + cone_axis_s8_e2) + 1);
            meshlets[ global_meshlet_index ].cone_cutoff = cone_cutoff_s8 > 127 ? int8_t(127) : int8_t(cone_cutoff_s8);
        }
    }
}

#endif // COMPUTE_ANIMATED_MESHLET_UPDATE

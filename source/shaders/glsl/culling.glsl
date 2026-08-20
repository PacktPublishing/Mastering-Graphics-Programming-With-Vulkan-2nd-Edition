#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "debug_rendering.h"
#include "culling.h"

//
// Function that given a sphere and a transform matrix, performs a frustum and occlusion culling check.
bool is_sphere_visible( mat4 model, vec4 bounding_sphere, mat4 culling_view_projection, uint depth_pyramid_texture_index ) {
    // Transform bounding sphere to view space.
    vec4 world_bounding_center = model * vec4(bounding_sphere.xyz, 1);
    vec4 view_bounding_center = freeze_occlusion_camera() ? frame.world_to_camera * world_bounding_center : frame.world_to_camera_debug * world_bounding_center;

    float scale = length( model[0] );
    float radius = bounding_sphere.w * scale * 1.1; // Artificially inflate bounding sphere.

    bool frustum_visible = true;
    for ( uint i = 0; i < 6; ++i ) {
        frustum_visible = frustum_visible && (dot( frame.frustum_planes[i], view_bounding_center) > -radius);
    }

    frustum_visible = frustum_visible || disable_frustum_cull_meshes();

    bool occlusion_visible = true;
    if ( frustum_visible ) {

        vec3 camera_world_position = freeze_occlusion_camera() ? frame.camera_position.xyz : frame.camera_position_debug.xyz;

        occlusion_visible = occlusion_cull( view_bounding_center.xyz, radius, frame.z_near, frame.projection_00, frame.projection_11,
                                            depth_pyramid_texture_index, world_bounding_center.xyz, camera_world_position,
                                            culling_view_projection );
    }

    occlusion_visible = occlusion_visible || disable_occlusion_cull_meshes();

    return frustum_visible && occlusion_visible;
}

// Common bindings ///////////////////////////////////////////////////////
layout(set = MATERIAL_SET, binding = 1) buffer OutputCommands
{
    MeshDrawCommand draw_commands[];
};

layout(set = MATERIAL_SET, binding = 3) buffer CulledMeshInstances
{
    uint culled_mesh_instance_ids[];
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

layout(set = MATERIAL_SET, binding = 16) buffer CullingHandoff
{
    // candidate_count for late
    uint early_opaque_culled_count;
    uint pad0;
    uint pad1;
    uint pad2;
};

//
void write_draw_command( uint out_index, uint mesh_instance_index, MeshDraw mesh_draw ) {

    draw_commands[out_index].mesh_instance_index = mesh_instance_index;
    draw_commands[out_index].taskOffset = mesh_draw.meshlet_offset;

    draw_commands[out_index].indexCount = mesh_draw.meshlet_index_count;
    draw_commands[out_index].instanceCount = 1;
    draw_commands[out_index].firstIndex = 0;
    draw_commands[out_index].vertexOffset = mesh_draw.vertexOffset;
    draw_commands[out_index].firstInstance = 0;

    draw_commands[out_index].groupCountX = (mesh_draw.meshlet_count + 31) / 32;
    draw_commands[out_index].groupCountY = 1;
    draw_commands[out_index].groupCountZ = 1;
}


#if defined(COMPUTE_MESH_CULLING_EARLY)

layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
void main() {

    // Early culling: 0..mesh instances count
    uint mesh_instance_index = gl_GlobalInvocationID.x;

    if ( mesh_instance_index >= mesh_instances_count ) {
        return;
    }


    uint mesh_draw_index = mesh_instance_draws[mesh_instance_index].mesh_draw_index;
    mat4 model = mesh_instance_draws[mesh_instance_index].model;

    MeshDraw mesh_draw = mesh_draws[mesh_draw_index];

    mat4 culling_view_projection = frame.previous_view_projection;

    vec4 bounding_sphere = mesh_bounds[mesh_draw_index];
    bool mesh_instance_visible = is_sphere_visible( model, bounding_sphere,
                                                    culling_view_projection,
                                                    depth_pyramid_texture_index );

    uint flags = mesh_draw.flags;
    if ( mesh_instance_visible ) {
        // Add opaque draws
        if ( ((flags & (DrawFlags_AlphaMask | DrawFlags_Transparent)) == 0 ) ){
            uint out_index = atomicAdd( opaque_mesh_visible_count, 1 );
            write_draw_command( out_index, mesh_instance_index, mesh_draw );

            // TODO: add optional flags for dispatch of task shaders emulation
            //atomicAdd( dispatch_task_x, task_count );
        }
        else {
            // Transparent draws are written after mesh_instances_count commands in the same buffer.
            uint out_index = atomicAdd( transparent_mesh_visible_count, 1 ) + mesh_instances_count;
            write_draw_command( out_index, mesh_instance_index, mesh_draw );
        }
    } else {
        // Add culled object for re-test
        if ( (flags & (DrawFlags_AlphaMask | DrawFlags_Transparent)) == 0 ) {
            uint out_index = atomicAdd( early_opaque_culled_count, 1 );

            culled_mesh_instance_ids[out_index] = mesh_instance_index;
        }
    }
}

#endif // COMPUTE_MESH_CULLING_EARLY

#if defined(COMPUTE_MESH_CULLING_LATE)

layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
void main() {

    if ( gl_GlobalInvocationID.x >= early_opaque_culled_count ) {
        return;
    }

    // Re-test culled mesh instances
    uint mesh_instance_index = culled_mesh_instance_ids[gl_GlobalInvocationID.x];
    uint mesh_draw_index = mesh_instance_draws[mesh_instance_index].mesh_draw_index;
    mat4 model = mesh_instance_draws[mesh_instance_index].model;

    MeshDraw mesh_draw = mesh_draws[mesh_draw_index];
    vec4 bounding_sphere = mesh_bounds[mesh_draw_index];

    mat4 culling_view_projection = frame.view_projection;
    bool mesh_instance_visible = is_sphere_visible( model, bounding_sphere,
                                                culling_view_projection,
                                                depth_pyramid_texture_index );

    uint flags = mesh_draw.flags;
    if ( mesh_instance_visible ) {
        // Add opaque draws
        if ( ((flags & (DrawFlags_AlphaMask | DrawFlags_Transparent)) == 0 ) ){
            uint out_index = atomicAdd( opaque_mesh_visible_count, 1 );
            write_draw_command( out_index, mesh_instance_index, mesh_draw );

            // TODO: add optional flags for dispatch of task shaders emulation
            //atomicAdd( dispatch_task_x, task_count );
        }
        else {
            // Transparent draws are written after mesh_instances_count commands in the same buffer.
            uint out_index = atomicAdd( transparent_mesh_visible_count, 1 ) + mesh_instances_count;
            write_draw_command( out_index, mesh_instance_index, mesh_draw );
        }
    }
}

#endif // COMPUTE_MESH_CULLING_LATE

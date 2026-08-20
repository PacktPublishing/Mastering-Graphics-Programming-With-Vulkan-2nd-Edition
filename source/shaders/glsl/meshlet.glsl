
#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "meshlet.h"
#include "debug_rendering.h"

#if defined (TASK_DEPTH_PRE) || defined(TASK_GBUFFER_CULLING) || defined(TASK_TRANSPARENT_NO_CULL)

#include "culling.h"

#define CULL 1

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(set = MATERIAL_SET, binding = 1) readonly buffer Meshlets
{
    Meshlet meshlets[];
};

layout(set = MATERIAL_SET, binding = 6) buffer VisibleMeshInstances
{
    MeshDrawCommand draw_commands[];
};

layout(set = MATERIAL_SET, binding = 7) readonly buffer VisibleMeshCount
{
    uint opaque_mesh_visible_count;
    uint opaque_mesh_culled_count;
    uint transparent_mesh_visible_count;
    uint transparent_mesh_culled_count;

    uint total_count;
    uint depth_pyramid_texture_index;
    uint late_flag;
    uint meshlet_index_count;

    uint dispatch_task_x;
    uint dispatch_task_y;
    uint dispatch_task_z;
    uint meshlet_instances_count;
};

struct Payload
{
    uint meshlet_indices[32];
    uint draw_index;
};

taskPayloadSharedEXT Payload payload;

void main()
{
    uint task_index = gl_LocalInvocationID.x;
    uint draw_index = gl_DrawIDARB;

#if defined(TASK_TRANSPARENT_NO_CULL)
    draw_index += total_count;
    uint mesh_instance_index = draw_commands[draw_index].mesh_instance_index;
    uint task_offset = draw_commands[draw_index].taskOffset;
#else
    uint mesh_instance_index = draw_commands[draw_index].mesh_instance_index;
    uint task_offset = draw_commands[draw_index].taskOffset;
#endif // TASK_TRANSPARENT_NO_CULL

    uint meshlet_group_index = gl_WorkGroupID.x;
    uint global_meshlet_index = meshlet_group_index * 32 + task_index + task_offset;

    mat4 model = mesh_instance_draws[mesh_instance_index].model;


#if CULL
    vec4 world_center = model * vec4(meshlets[global_meshlet_index].center, 1);
    float scale = length( model[0] );
    float radius = meshlets[global_meshlet_index].radius * scale * 1.1;   // Artificially inflate bounding sphere.
    vec3 cone_axis = mat3( model ) * vec3(int(meshlets[global_meshlet_index].cone_axis[0]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[1]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[2]) / 127.0);
    float cone_cutoff = int(meshlets[global_meshlet_index].cone_cutoff) / 127.0;

    bool accept = false;

    vec4 view_center = vec4(0);
    // Backface culling and move meshlet in camera space
    if ( freeze_occlusion_camera() ) {
        accept = !coneCull(world_center.xyz, radius, cone_axis, cone_cutoff, frame.camera_position.xyz);
        view_center = frame.world_to_camera * world_center;
    } else {
        accept = !coneCull(world_center.xyz, radius, cone_axis, cone_cutoff, frame.camera_position_debug.xyz);
        view_center = frame.world_to_camera_debug * world_center;
    }

    bool frustum_visible = true;
    for ( uint i = 0; i < 6; ++i ) {
        frustum_visible = frustum_visible && (dot( frame.frustum_planes[i], view_center) > -radius);
    }

    frustum_visible = frustum_visible || disable_frustum_cull_meshlets();

    bool occlusion_visible = true;
    if ( frustum_visible ) {

        vec3 camera_world_position = freeze_occlusion_camera() ? frame.camera_position.xyz : frame.camera_position_debug.xyz;
        mat4 culling_view_projection = late_flag == 0 ? frame.previous_view_projection : frame.view_projection;

        occlusion_visible = occlusion_cull( view_center.xyz, radius, frame.z_near, frame.projection_00, frame.projection_11,
                                            depth_pyramid_texture_index, world_center.xyz, camera_world_position,
                                            culling_view_projection );
    }

    occlusion_visible = occlusion_visible || disable_occlusion_cull_meshlets();

    accept = accept && frustum_visible && occlusion_visible;

    uvec4 ballot = subgroupBallot(accept);

    uint index = subgroupBallotExclusiveBitCount(ballot);

    if (accept)
        payload.meshlet_indices[index] = global_meshlet_index;

    if (task_index == 0)
        payload.draw_index = draw_index;

    uint count = subgroupBallotBitCount(ballot);

    if (subgroupElect())
        EmitMeshTasksEXT( count, 1, 1 );
#else
    payload.meshlet_indices[task_index] = global_meshlet_index;

    if (subgroupElect())
        EmitMeshTasksEXT( 32, 1, 1 );
#endif // CULL

}

#endif // TASK


#if defined(MESH_GBUFFER_CULLING) || defined(MESH_MESH) || defined(MESH_TRANSPARENT_NO_CULL)


#include "debug_rendering.h"

#define CULL 1

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

layout(set = MATERIAL_SET, binding = 1) readonly buffer Meshlets
{
    Meshlet meshlets[];
};

layout(set = MATERIAL_SET, binding = 3) readonly buffer MeshletData
{
    uint meshletData[];
};

layout(set = MATERIAL_SET, binding = 4) readonly buffer VertexPositions
{
    VertexPosition vertex_positions[];
};

layout(set = MATERIAL_SET, binding = 5) readonly buffer VertexData
{
    VertexExtraData vertex_data[];
};

layout(set = MATERIAL_SET, binding = 6) readonly buffer VisibleMeshInstances
{
    MeshDrawCommand draw_commands[];
};

layout(set = MATERIAL_SET, binding = 7) readonly buffer VisibleMeshCount
{
    uint opaque_mesh_visible_count;
    uint opaque_mesh_culled_count;
    uint transparent_mesh_visible_count;
    uint transparent_mesh_culled_count;

    uint total_count;
    uint depth_pyramid_texture_index;
    uint late_flag;
    uint meshlet_index_count;

    uint dispatch_task_x;
    uint dispatch_task_y;
    uint dispatch_task_z;
    uint meshlet_instances_count;
};

struct Payload
{
    uint meshlet_indices[32];
    uint draw_index;
};

taskPayloadSharedEXT Payload payload;

layout (location = 0) out vec3 vTexcoord0_W[];
layout (location = 1) out vec4 vNormal_BiTanX[];
layout (location = 2) out vec4 vTangent_BiTanY[];
layout (location = 3) out vec4 vPosition_BiTanZ[];
layout (location = 4) out flat uint mesh_draw_index[];

#if DEBUG
layout (location = 5) out vec4 vColour[];
layout (location = 6) out flat uint triangle_index[];
#endif // DEBUG

#if CULL
shared vec3 vertex_clip[64];
#endif // CULL

uint hash(uint a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}

vec3 hash_color(uint a)
{
    uint mhash = hash(a);

    vec3 result = vec3(float(mhash & 255), float((mhash >> 8) & 255), float((mhash >> 16) & 255)) / 255.0;

    return result;
}


uint read_meshlet_index_u8( uint base_word_offset, uint byte_offset ) {
    uint word_index = base_word_offset + (byte_offset >> 2u);
    uint shift      = (byte_offset & 3u) * 8u;
    return (meshletData[word_index] >> shift) & 0xffu;
}

void main() {

    uint task_index = gl_LocalInvocationID.x;
    uint global_meshlet_index = payload.meshlet_indices[gl_WorkGroupID.x];
    uint draw_index = payload.draw_index;

    MeshDraw mesh_draw = mesh_draws[ meshlets[global_meshlet_index].mesh_index ];

    uint vertex_count = uint(meshlets[global_meshlet_index].vertex_count);
    uint triangle_count = uint(meshlets[global_meshlet_index].triangle_count);

    uint connectivity_data_offset = meshlets[global_meshlet_index].connectivity_data_offset;
    uint vertex_offset = connectivity_data_offset;
    uint index_offset = connectivity_data_offset + vertex_count;

    bool has_normals = (mesh_draw.flags & DrawFlags_HasNormals) != 0;
    bool has_tangents = (mesh_draw.flags & DrawFlags_HasTangents) != 0;

    float i8_inverse = 1.0 / 127.0;

    uint mesh_instance_index = draw_commands[draw_index].mesh_instance_index;

#if DEBUG
    vec3 mcolor = hash_color(global_meshlet_index);
#endif


    mat4 model = mesh_instance_draws[mesh_instance_index].model;
    // TODO: restore
    mat4 model_inverse = model;//mesh_instance_draws[mesh_instance_index].model_inverse;

    SetMeshOutputsEXT( vertex_count, triangle_count );

    // TODO: if we have meshlets with 62 or 63 vertices then we pay a small penalty for branch divergence here - we can instead redundantly xform the last vertex
    for (uint i = task_index; i < vertex_count; i += 32)
    {
        uint vi = meshletData[vertex_offset + i];

        vec3 position = vec3(vertex_positions[vi].v.x, vertex_positions[vi].v.y, vertex_positions[vi].v.z);

        if ( has_normals ) {
            // Object space normal
            vec3 normal_os = vec3(int(vertex_data[vi].nx),
                                  int(vertex_data[vi].ny),
                                  int(vertex_data[vi].nz)) * i8_inverse - 1.0;

            vec3 normal_ws = normalize( mat3(model_inverse) * normal_os );
            vNormal_BiTanX[ i ].xyz = normal_ws;

            if ( has_tangents ) {
                vec3 tangent_os = vec3(int(vertex_data[vi].tx),
                                       int(vertex_data[vi].ty),
                                       int(vertex_data[vi].tz)) * i8_inverse - 1.0;

                vec3 tangent_ws = normalize( mat3(model) * tangent_os );
                vTangent_BiTanY[i].xyz = tangent_ws;

                float handedness = (int(vertex_data[vi].tw) * i8_inverse - 1.0);
                vec3 bitangent_ws = cross( normal_ws, tangent_ws ) * handedness;

                vNormal_BiTanX[i].w   = bitangent_ws.x;
                vTangent_BiTanY[i].w  = bitangent_ws.y;
                vPosition_BiTanZ[i].w = bitangent_ws.z;
            }
        }

        vec4 position_clip = frame.view_projection * (model * vec4(position, 1));
        gl_MeshVerticesEXT[ i ].gl_Position = position_clip;

        vec4 worldPosition = model * vec4(position, 1.0);
        vPosition_BiTanZ[ i ].xyz = worldPosition.xyz / worldPosition.w;

        vTexcoord0_W[i] = vec3( vertex_data[vi].tu, vertex_data[vi].tv, worldPosition );

        mesh_draw_index[ i ] = meshlets[global_meshlet_index].mesh_index;

#if CULL
        vertex_clip[i] = vec3((position_clip.xy / position_clip.w * 0.5 + vec2(0.5)) * frame.resolution, position_clip.w);
#endif // CULL


#if DEBUG
        vColour[i] = vec4(mcolor, 1.0);
        triangle_index[ i ] = vi;
#endif // DEBUG
    }

#if CULL
    barrier();
#endif // CULL

    for (uint i = task_index; i < triangle_count; i += 32) {

        // Each triangle has 3 consecutive 8-bit indices
        uint byte_base = i * 3u;

        uint a = read_meshlet_index_u8(index_offset, byte_base + 0u);
        uint b = read_meshlet_index_u8(index_offset, byte_base + 1u);
        uint c = read_meshlet_index_u8(index_offset, byte_base + 2u);

        gl_PrimitiveTriangleIndicesEXT[i] = uvec3(a, b, c);

    #if CULL
        bool culled = false;

        vec2 pa = vertex_clip[a].xy, pb = vertex_clip[b].xy, pc = vertex_clip[c].xy;

        // backface culling + zero-area culling
        vec2 eb = pb - pa;
        vec2 ec = pc - pa;

        culled = culled || (eb.x * ec.y <= eb.y * ec.x);

        // small primitive culling
        vec2 bmin = min(pa, min(pb, pc));
        vec2 bmax = max(pa, max(pb, pc));
        float sbprec = 1.0 / 256.0; // note: this can be set to 1/2^subpixelPrecisionBits

        // note: this is slightly imprecise (doesn't fully match hw behavior and is both too loose and too strict)
        culled = culled || (round(bmin.x - sbprec) == round(bmax.x) || round(bmin.y) == round(bmax.y + sbprec));

        // the computations above are only valid if all vertices are in front of perspective plane
        culled = culled && (vertex_clip[a].z > 0 && vertex_clip[b].z > 0 && vertex_clip[c].z > 0);
        gl_MeshPrimitivesEXT[i].gl_CullPrimitiveEXT = culled;
    #endif // CULL
    }

    // uvec3 index_entry_offset[4] =
    // {
    //     uvec3( 0, 0, 0 ),
    //     uvec3( 0, 1, 1 ),
    //     uvec3( 1, 1, 2 ),
    //     uvec3( 2, 2 ,2 ),
    // };

    // uvec3 index_entry_shift[4] =
    // {
    //     uvec3(  0,  8, 16 ),
    //     uvec3( 24,  0,  8 ),
    //     uvec3( 16, 24,  0 ),
    //     uvec3(  8, 16, 24 ),
    // };

    // for (uint i = task_index; i < triangle_count; i += 32)
    // {
    //     uint index_entry = i % 4;
    //     uint new_index = index_offset + ( i / 4 ) * 3;
    //     gl_PrimitiveTriangleIndicesEXT[ i ] = uvec3(
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].x ] >> index_entry_shift[ index_entry ].x ) & 0xff,
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].y ] >> index_entry_shift[ index_entry ].y ) & 0xff,
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].z ] >> index_entry_shift[ index_entry ].z ) & 0xff
    //     );
    // }
}

#endif // MESH


#if defined(MESH_DEPTH_PRE)

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

layout(set = MATERIAL_SET, binding = 1) readonly buffer Meshlets
{
    Meshlet meshlets[];
};

layout(set = MATERIAL_SET, binding = 3) readonly buffer MeshletData
{
    uint meshletData[];
};

layout(set = MATERIAL_SET, binding = 4) readonly buffer VertexPositions
{
    VertexPosition vertex_positions[];
};

layout(set = MATERIAL_SET, binding = 5) readonly buffer VertexData
{
    VertexExtraData vertex_data[];
};

layout(set = MATERIAL_SET, binding = 6) readonly buffer VisibleMeshInstances
{
    MeshDrawCommand draw_commands[];
};

struct Payload
{
    uint meshlet_indices[32];
    uint draw_index;
};

taskPayloadSharedEXT Payload payload;

void main()
{
    uint task_index = gl_LocalInvocationID.x;
    uint global_meshlet_index = payload.meshlet_indices[gl_WorkGroupID.x];
    uint draw_index = payload.draw_index;

    MeshDraw mesh_draw = mesh_draws[ meshlets[global_meshlet_index].mesh_index ];

    uint vertex_count = uint(meshlets[global_meshlet_index].vertex_count);
    uint triangle_count = uint(meshlets[global_meshlet_index].triangle_count);

    uint connectivity_data_offset = meshlets[global_meshlet_index].connectivity_data_offset;
    uint vertex_offset = connectivity_data_offset;
    uint index_offset = connectivity_data_offset + vertex_count;

    uint mesh_instance_index = draw_commands[draw_index].mesh_instance_index;
    mat4 model = mesh_instance_draws[mesh_instance_index].model;

    SetMeshOutputsEXT( vertex_count, triangle_count );

    // TODO: if we have meshlets with 62 or 63 vertices then we pay a small penalty for branch divergence here - we can instead redundantly xform the last vertex
    for (uint i = task_index; i < vertex_count; i += 32)
    {
        uint vi = meshletData[vertex_offset + i];

        vec3 position = vec3(vertex_positions[vi].v.x, vertex_positions[vi].v.y, vertex_positions[vi].v.z);

        gl_MeshVerticesEXT[ i ].gl_Position = frame.view_projection * (model * vec4(position, 1));
    }

    for (uint i = task_index; i < triangle_count; i += 32) {

        // Each triangle has 3 consecutive 8-bit indices
        uint byte_base = i * 3u;

        uint a = read_meshlet_index_u8(index_offset, byte_base + 0u);
        uint b = read_meshlet_index_u8(index_offset, byte_base + 1u);
        uint c = read_meshlet_index_u8(index_offset, byte_base + 2u);

        gl_PrimitiveTriangleIndicesEXT[i] = uvec3(a, b, c);
    }

    // uvec3 index_entry_offset[4] =
    // {
    //     uvec3( 0, 0, 0 ),
    //     uvec3( 0, 1, 1 ),
    //     uvec3( 1, 1, 2 ),
    //     uvec3( 2, 2 ,2 ),
    // };

    // uvec3 index_entry_shift[4] =
    // {
    //     uvec3(  0,  8, 16 ),
    //     uvec3( 24,  0,  8 ),
    //     uvec3( 16, 24,  0 ),
    //     uvec3(  8, 16, 24 ),
    // };

    // for (uint i = task_index; i < triangle_count; i += 32)
    // {
    //     uint index_entry = i % 4;
    //     uint new_index = index_offset + ( i / 4 ) * 3;
    //     gl_PrimitiveTriangleIndicesEXT[ i ] = uvec3(
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].x ] >> index_entry_shift[ index_entry ].x ) & 0xff,
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].y ] >> index_entry_shift[ index_entry ].y ) & 0xff,
    //         ( meshletData[ new_index + index_entry_offset[ index_entry ].z ] >> index_entry_shift[ index_entry ].z ) & 0xff
    //     );
    // }
}

#endif // MESH

#if defined(FRAGMENT_GBUFFER_CULLING) || defined(FRAGMENT_MESH) || defined(FRAGMENT_EMULATION_GBUFFER_CULLING) || defined(FRAGMENT_GBUFFER_SKINNING)

layout (location = 0) in vec3 vTexcoord0_W;
layout (location = 1) in vec4 vNormal_BiTanX;
layout (location = 2) in vec4 vTangent_BiTanY;
layout (location = 3) in vec4 vPosition_BiTanZ;
layout (location = 4) in flat uint mesh_draw_index;

#if DEBUG
layout (location = 5) in vec4 vColour;
layout (location = 6) in flat uint triangle_index;
#endif

layout (location = 0) out vec4 color_out;
layout (location = 1) out vec2 normal_out;
layout (location = 2) out vec4 occlusion_roughness_metalness_out;
layout (location = 3) out vec4 emissive_out;
layout (location = 4) out uint mesh_id;
layout (location = 5) out vec2 depth_normal_fwidth;
layout (location = 6) out vec2 linear_z_dd;
#if DEBUG
layout (location = 7) out vec4 debug_color_out;
#endif

uint hash(uint a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}

vec3 hash_color(uint a)
{
    uint mhash = hash(a);

    vec3 result = vec3(float(mhash & 255), float((mhash >> 8) & 255), float((mhash >> 16) & 255)) / 255.0;

    return result;
}

void main() {
    MeshDraw mesh_draw = mesh_draws[mesh_draw_index];

    // Diffuse color
    vec4 base_colour = compute_diffuse_color( mesh_draw.base_color_factor, mesh_draw.textures.x, vTexcoord0_W.xy );

    const uint flags = mesh_draw.flags;

    apply_alpha_discards( flags, base_colour.a, mesh_draw.alpha_cutoff, vTexcoord0_W.xy, mesh_draw.alpha_texture_index );

#if DEBUG
    debug_color_out = vColour;
#endif
    color_out = base_colour;

    // Geometric Normals
    vec3 world_position = vPosition_BiTanZ.xyz;

    vec3 normal = normalize(vNormal_BiTanX.xyz);
    vec3 tangent = normalize(vTangent_BiTanY.xyz);
    vec3 bitangent = normalize(vec3(vNormal_BiTanX.w, vTangent_BiTanY.w, vPosition_BiTanZ.w));

    calculate_geometric_TBN( normal, tangent, bitangent, vTexcoord0_W.xy, world_position, flags );

    normal = apply_pixel_normal( mesh_draw.textures.z, vTexcoord0_W.xy, normal, tangent, bitangent );

    bool double_sided = ( mesh_draw.flags & DrawFlags_DoubleSided ) != 0;

    if ( !gl_FrontFacing && double_sided ) {
        normal *= -1;
    }

    normal_out.rg = octahedral_encode(normal);

    // PBR Parameters
    occlusion_roughness_metalness_out.rgb = calculate_pbr_parameters( mesh_draw.metallic_roughness_occlusion_factor.x, mesh_draw.metallic_roughness_occlusion_factor.y,
                                                                      mesh_draw.textures.y, mesh_draw.metallic_roughness_occlusion_factor.z, mesh_draw.textures.w, vTexcoord0_W.xy );

    emissive_out = vec4( calculate_emissive(mesh_draw.emissive.rgb, uint(mesh_draw.emissive.w), vTexcoord0_W.xy ), 1.0 );

    mesh_id = mesh_draw_index;

    depth_normal_fwidth = vec2( length( fwidth( world_position ) ), length( fwidth( normal ) ) );

    // TODO(marco): this gives us the wrong z? Save this in the history?
    float linear_z = gl_FragCoord.z / gl_FragCoord.w;
    linear_z_dd = vec2( linear_z, max( abs( dFdx( linear_z ) ), abs( dFdy( linear_z ) ) ) );
}

#endif // FRAGMENT


#if defined(FRAGMENT_TRANSPARENT_NO_CULL)

#include "lighting.h"

layout (location = 0) in vec3 vTexcoord0_W;
layout (location = 1) in vec4 vNormal_BiTanX;
layout (location = 2) in vec4 vTangent_BiTanY;
layout (location = 3) in vec4 vPosition_BiTanZ;
layout (location = 4) in flat uint mesh_draw_index;

#if DEBUG
layout (location = 5) in vec4 vColour;
layout (location = 6) in flat uint triangle_index;
#endif

layout (location = 0) out vec4 color_out;

void main() {
    MeshDraw mesh_draw = mesh_draws[mesh_draw_index];
    uint flags = mesh_draw.flags;

    // Diffuse color
    vec4 base_colour = compute_diffuse_color_alpha( mesh_draw.base_color_factor, mesh_draw.textures.x, vTexcoord0_W.xy );

    apply_alpha_discards( flags, base_colour.a, mesh_draw.alpha_cutoff, vTexcoord0_W.xy, mesh_draw.alpha_texture_index );

    vec3 world_position = vPosition_BiTanZ.xyz;
    vec3 normal = normalize(vNormal_BiTanX.xyz);
    vec3 tangent = normalize(vTangent_BiTanY.xyz);
    vec3 bitangent = normalize(vec3(vNormal_BiTanX.w, vTangent_BiTanY.w, vPosition_BiTanZ.w));

    calculate_geometric_TBN( normal, tangent, bitangent, vTexcoord0_W.xy, world_position, flags );
    // Pixel normals
    normal = apply_pixel_normal( mesh_draw.textures.z, vTexcoord0_W.xy, normal, tangent, bitangent );

    vec3 orm = calculate_pbr_parameters( mesh_draw.metallic_roughness_occlusion_factor.x, mesh_draw.metallic_roughness_occlusion_factor.y,
                                                                      mesh_draw.textures.y, mesh_draw.metallic_roughness_occlusion_factor.z, mesh_draw.textures.w, vTexcoord0_W.xy );

    vec3 emissive_colour = calculate_emissive(mesh_draw.emissive.rgb, uint(mesh_draw.emissive.w), vTexcoord0_W.xy );

#if DEBUG
    color_out = vColour;
#else
    // NOTE(marco): integer fragment position and top-left origin
    // TODO(marco): refactor into function
    uvec2 position = uvec2(gl_FragCoord.x - 0.5, gl_FragCoord.y - 0.5);
    position.y = uint( frame.resolution.y ) - position.y;

    const vec2 screen_uv = uv_from_pixels(ivec2( gl_FragCoord.xy ), uint(frame.resolution.x), uint(frame.resolution.y));
    vec4 final_color = calculate_lighting( base_colour, orm, normal, emissive_colour.rgb, world_position, position, screen_uv, true );

    final_color.rgb = apply_volumetric_fog( screen_uv, gl_FragCoord.z, final_color.rgb );
    final_color.rgb = encode_srgb( final_color.rgb );

    color_out = final_color;
#endif

}

#endif // FRAGMENT_TRANSPARENT_NO_CULL


#define AS_GROUP_SIZE 32

struct TaskPayload {
    uint meshlet_indices[AS_GROUP_SIZE];
    uint draw_index[AS_GROUP_SIZE];
};

#include "platform.slangh"
#include "mesh.slangh"
#include "meshlet.slangh"

struct VertexOutput {

    float4 position             : SV_Position;
    float3 vTexcoord0_W         : NORMAL0;
    float4 vNormal_BiTanX       : NORMAL1;
    float4 vTangent_BiTanY      : NORMAL2;
    float4 vPosition_BiTanZ     : POSITION0;
    nointerpolation uint mesh_draw_index : COLOR0;
#if DEBUG
    nointerpolation float4 vColour : COLOR1;
    nointerpolation uint triangle_index : COLOR2;
#endif
};

StructuredBuffer<Meshlet> meshlets              : register(t1, space1);
StructuredBuffer<uint> meshletData              : register(t3, space1);
StructuredBuffer<VertexPosition> vertex_positions : register(t4, space1);
StructuredBuffer<VertexExtraData> vertex_data   : register(t5, space1);
StructuredBuffer<MeshDrawCommand> draw_commands : register(t6, space1);

uint hash(uint a)
{
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

float3 hash_color(uint a)
{
    uint mhash = hash(a);
    float3 result = float3(float(mhash & 255), float((mhash >> 8) & 255), float((mhash >> 16) & 255)) / 255.0;

    return result;
}

#if defined (TASK_DEPTH_PRE_SLANG) || defined(TASK_GBUFFER_CULLING_SLANG) || defined(TASK_TRANSPARENT_NO_CULL_SLANG) || defined(TASK_DEPTH_CUBEMAP_SLANG) || defined(TASK_DEPTH_TETRAHEDRON_SLANG)

groupshared TaskPayload task_payload;

[shader("amplification")]
[numthreads(AS_GROUP_SIZE, 1, 1)]
void main( uint draw_id : SV_DrawIndex, uint3 local_invocation : SV_GroupThreadID,
           uint3 work_group_id : SV_GroupID ) {

    uint task_index = local_invocation.x;//gl_LocalInvocationID.x;
    uint draw_index = draw_id;//gl_DrawIDARB;

#if defined (TASK_DEPTH_CUBEMAP) || defined(TASK_DEPTH_TETRAHEDRON)
    uint meshlet_group_index = gl_WorkGroupID.x;

    // TODO: meshlet_draw_commands does not have notion of offset!
    // gl_drawIDARB will be 0...5 for two subsequent calls, thus we need a mechanism
    // to differentiate between calls, or we call everything in one batch.
    uint packed_light_index_face_index = meshlet_draw_commands[command_read_offset + draw_index].w;
    const uint meshlet_index = meshlet_group_index * AS_GROUP_SIZE + task_index;
    const uint light_index = packed_light_index_face_index >> 16;
    const uint meshlet_index_read_offset = light_index * 45000;
    uint mesh_instance_index = meshlet_instances[meshlet_index_read_offset + meshlet_index].x;
    uint global_meshlet_index = meshlet_instances[meshlet_index_read_offset + meshlet_index].y;

    const uint face_index = (packed_light_index_face_index & 0xf);
#else
#if defined(TASK_TRANSPARENT_NO_CULL)
    draw_index += total_count;
    uint mesh_instance_index = draw_commands[draw_index].drawId;
    uint task_offset = draw_commands[draw_index].taskOffset;
#else
    uint mesh_instance_index = draw_commands[draw_index].drawId;
    uint task_offset = draw_commands[draw_index].taskOffset;
#endif // TASK_TRANSPARENT_NO_CULL

    uint meshlet_group_index = work_group_id.x; // gl_WorkGroupID.x;
    uint global_meshlet_index = meshlet_group_index * AS_GROUP_SIZE + task_index + task_offset;

#endif // TASK_DEPTH_CUBEMAP

    float4x4 model = mesh_instance_draws[mesh_instance_index].model;

    task_payload.draw_index[task_index] = draw_index;
    task_payload.meshlet_indices[task_index] = global_meshlet_index;

    DispatchMesh(AS_GROUP_SIZE, 1, 1, task_payload);
}

#endif


#if defined(MESH_GBUFFER_CULLING_SLANG) || defined(MESH_MESH_SLANG) || defined(MESH_TRANSPARENT_NO_CULL_SLANG)

#include "frame.slangh"

[shader("mesh")]
[numthreads(AS_GROUP_SIZE, 1, 1)]
[outputtopology("triangle")]
void main( in payload TaskPayload task_payload,
           uint3 work_group_id : SV_GroupID,
           uint3 local_invocation : SV_GroupThreadID,
           out vertices VertexOutput verts[64],
           out indices uint3 prims[124] ) {

    uint task_index = local_invocation.x;//gl_LocalInvocationID.x;
    const uint global_meshlet_index = task_payload.meshlet_indices[work_group_id.x];
    uint draw_index = task_payload.draw_index[work_group_id.x];

    Meshlet meshlet = meshlets[global_meshlet_index];
    MeshDraw mesh_draw = mesh_draws[ meshlet.mesh_index ];

    uint2 meshlet_counts = unpackCounts(meshlet.packed_counts);
    uint vertex_count = meshlet_counts.x;
    uint triangle_count = meshlet_counts.y;

    uint indexCount = triangle_count * 3;

    uint data_offset = meshlet.data_offset;
    uint vertexOffset = data_offset;
    uint indexOffset = data_offset + vertex_count;

    bool has_normals = (mesh_draw.flags & DrawFlags_HasNormals) != 0;
    bool has_tangents = (mesh_draw.flags & DrawFlags_HasTangents) != 0;

    float i8_inverse = 1.0 / 127.0;

    uint mesh_instance_index = draw_commands[draw_index].drawId;

#if DEBUG
    float3 mcolor = hash_color(draw_index);
#endif

    float4x4 model = mesh_instance_draws[mesh_instance_index].model;
    float4x4 model_inverse = mesh_instance_draws[mesh_instance_index].model_inverse;

    // Emit vertices
    SetMeshOutputCounts(vertex_count, triangle_count);

    // TODO: if we have meshlets with 62 or 63 vertices then we pay a small penalty for branch divergence here - we can instead redundantly xform the last vertex
    for (uint i = task_index; i < vertex_count; i += AS_GROUP_SIZE)
    {
        uint vi = meshletData[vertexOffset + i];// + mesh_draw.vertexOffset;

        float3 position = float3(vertex_positions[vi].v.x, vertex_positions[vi].v.y, vertex_positions[vi].v.z);

        //gl_MeshVerticesEXT[ i ].gl_Position = view_projection * (model * vec4(position, 1));
        float4 world_position = mul(model, float4(position, 1));
        verts[i].position = mul(frame.view_projection, world_position);
        // verts[i].vNormal_BiTanX.xyz = float3(0,0,0);
        // verts[i].vTangent_BiTanY = float4(0,0,0,0);

        if ( has_normals ) {
            float3 normal_os = unpack_normal( vertex_data[vi].packed_normal );

            float3 normal_ws = normalize( mul(float3x3(model_inverse), normal_os ));
            verts[i].vNormal_BiTanX.xyz = normal_ws;

            if ( has_tangents ) {
                float3 tangent_os = unpack_tangent( vertex_data[vi].packed_tangent );

                float3 tangent_ws = normalize( mul(float3x3(model), tangent_os ));
                verts[i].vTangent_BiTanY.xyz = tangent_ws;

                int handedness = int(((vertex_data[vi].packed_tangent) >> 24 ) & 0xff ); // int(vertex_data[vi].tw)
                float3 bitangent = cross( normal_ws, tangent_ws ) * ( handedness * ( 2.0 / 255.0 ) - 1.0 );
                verts[i].vNormal_BiTanX.w = bitangent.x;
                verts[i].vTangent_BiTanY.w = bitangent.y;
                verts[i].vPosition_BiTanZ.w = bitangent.z;
            }
        }

        verts[i].vPosition_BiTanZ.xyz = world_position.xyz / world_position.w;

        float2 uv = unpack_uv( vertex_data[vi].packed_uvs );
        verts[i].vTexcoord0_W = float3( uv.x, uv.y, world_position.w );

        verts[i].mesh_draw_index = meshlets[global_meshlet_index].mesh_index;

#if DEBUG
        verts[i].triangle_index = vi;
        verts[i].vColour = float4(mcolor, 1.0);
#endif
    }

    // for (uint i = vertex_count; i < 64; i += AS_GROUP_SIZE) {
    //     verts[i].position = float4(0,0,0,-1);
    // }

    uint3 index_entry_offset[4] =
    {
        uint3( 0, 0, 0 ),
        uint3( 0, 1, 1 ),
        uint3( 1, 1, 2 ),
        uint3( 2, 2 ,2 ),
    };

    uint3 index_entry_shift[4] =
    {
        uint3(  0,  8, 16 ),
        uint3( 24,  0,  8 ),
        uint3( 16, 24,  0 ),
        uint3(  8, 16, 24 ),
    };

    for (uint i = task_index; i < triangle_count; i += 32)
    {
        uint index_entry = i % 4;
        uint new_index = indexOffset + ( i / 4 ) * 3;

        uint3 offset = index_entry_offset[index_entry];
        uint3 shift  = index_entry_shift[index_entry];

        prims[ i ] = uint3(
            ( meshletData[ new_index + offset.x ] >> shift.x ) & 0xff,
            ( meshletData[ new_index + offset.y ] >> shift.y ) & 0xff,
            ( meshletData[ new_index + offset.z ] >> shift.z ) & 0xff
        );

        //prims[ i ] = uint3( i * 3, (i * 3) + 1, (i * 3) + 2 );
    }
}

#endif


#if defined(MESH_DEPTH_PRE_SLANG) || defined(MESH_DEPTH_CUBEMAP_SLANG) || defined(MESH_DEPTH_TETRAHEDRON_SLANG)

#include "frame.slangh"

struct MeshVertex {
    float3 position;
};

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main( in payload TaskPayload payload,
           uint3 work_group_id : SV_GroupID,
           uint3 local_invocation : SV_GroupThreadID,
           out vertices MeshVertex verts[64],
           out indices uint3 prims[124] ) {

    uint task_index = local_invocation.x;//gl_LocalInvocationID.x;
    uint global_meshlet_index = payload.meshlet_indices[work_group_id.x];
    uint draw_index = payload.draw_index[work_group_id.x];

    MeshDraw mesh_draw = mesh_draws[ meshlets[global_meshlet_index].mesh_index ];

    uint2 meshlet_counts = unpackCounts(meshlets[global_meshlet_index].packed_counts);
    //uint vertex_count = uint(meshlets[global_meshlet_index].vertex_count);
    //uint triangle_count = uint(meshlets[global_meshlet_index].triangle_count);
    uint vertex_count = meshlet_counts.x;
    uint triangle_count = meshlet_counts.y;

    uint indexCount = triangle_count * 3;

    uint data_offset = meshlets[global_meshlet_index].data_offset;
    uint vertexOffset = data_offset;
    uint indexOffset = data_offset + vertex_count;

    uint mesh_instance_index = draw_commands[draw_index].drawId;
    float4x4 model = mesh_instance_draws[mesh_instance_index].model;

    // Emit vertices
    SetMeshOutputCounts(vertex_count, triangle_count);

    // TODO: if we have meshlets with 62 or 63 vertices then we pay a small penalty for branch divergence here - we can instead redundantly xform the last vertex
    for (uint i = task_index; i < vertex_count; i += 32)
    {
        uint vi = meshletData[vertexOffset + i];// + mesh_draw.vertexOffset;

        float3 position = float3(vertex_positions[vi].v.x, vertex_positions[vi].v.y, vertex_positions[vi].v.z);
#if defined(MESH_DEPTH_CUBEMAP)
        gl_MeshVerticesEXT[ i ].gl_Position = view_projections[layer_index] * (model * vec4(position, 1));
#elif defined(MESH_DEPTH_TETRAHEDRON)
        gl_MeshVerticesEXT[ i ].gl_Position = view_projections[layer_index] * (model * vec4(position, 1));

        float clip_distances[3];
        const uint faceIndex = 0;
        uint inside = 0;
        const float3 light_position = camera_spheres[light_index].xyz;
        for(uint sideIndex=0; sideIndex<3; sideIndex++)
        {
            const uint planeIndex = (faceIndex*3)+sideIndex;
            const uint bit = 1 << sideIndex;
            clip_distances[sideIndex] = get_clip_distance(light_position, gl_MeshVerticesEXT[ i ].gl_Position.xyz, planeIndex);
            inside |= (clip_distances[sideIndex] > 0.001) ? bit : 0;
        }

        gl_MeshVerticesEXT[ i ].gl_ClipDistance[0] = clip_distances[0];
        gl_MeshVerticesEXT[ i ].gl_ClipDistance[1] = clip_distances[1];
        gl_MeshVerticesEXT[ i ].gl_ClipDistance[2] = clip_distances[2];

#else
        //gl_MeshVerticesEXT[ i ].gl_Position = view_projection * (model * vec4(position, 1));
        float4 world_position = mul(model, float4(position, 1));
        verts[i].position = mul(frame.view_projection, world_position).xyz;
#endif // MESH_DEPTH_CUBEMAP
    }

    // Write vertex positions (object space)
    //verts[0].position = float3(0, .5, 0);
    //SetVertex(0, float3( 0.0,  0.5, 0.0));
    //SetVertex(1, float3(-0.5, -0.5, 0.0));
    //SetVertex(2, float3( 0.5, -0.5, 0.0));

    // Emit a triangle
    //prims[0] = uint3(0, 1, 2);
    //SetPrimitiveTriangleIndices(0, uint3(0, 1, 2));

    uint3 index_entry_offset[4] =
    {
        uint3( 0, 0, 0 ),
        uint3( 0, 1, 1 ),
        uint3( 1, 1, 2 ),
        uint3( 2, 2 ,2 ),
    };

    uint3 index_entry_shift[4] =
    {
        uint3(  0,  8, 16 ),
        uint3( 24,  0,  8 ),
        uint3( 16, 24,  0 ),
        uint3(  8, 16, 24 ),
    };

    for (uint i = task_index; i < triangle_count; i += 32)
    {
        uint index_entry = i % 4;
        uint new_index = indexOffset + ( i / 4 ) * 3;
        prims[ i ] = uint3(
            ( meshletData[ new_index + index_entry_offset[ index_entry ].x ] >> index_entry_shift[ index_entry ].x ) & 0xff,
            ( meshletData[ new_index + index_entry_offset[ index_entry ].y ] >> index_entry_shift[ index_entry ].y ) & 0xff,
            ( meshletData[ new_index + index_entry_offset[ index_entry ].z ] >> index_entry_shift[ index_entry ].z ) & 0xff
        );
    }
}

#endif

#if defined(FRAGMENT_GBUFFER_CULLING_SLANG) || defined(FRAGMENT_MESH_SLANG) || defined(FRAGMENT_EMULATION_GBUFFER_CULLING_SLANG)

#include "platform.slangh"
#include "frame.slangh"
#include "material_utils.slangh"

struct FragmentOutput
{
    float4 color_out                          : SV_Target0;
    float2 normal_out                         : SV_Target1;
    float4 occlusion_roughness_metalness_out  : SV_Target2;
    float4 emissive_out                       : SV_Target3;
    uint   mesh_id                            : SV_Target4;
    float2 depth_normal_fwidth                : SV_Target5;
    float2 linear_z_dd                        : SV_Target6;
};


[shader("fragment")]
FragmentOutput main(VertexOutput input, bool is_front_face : SV_IsFrontFace) {

    FragmentOutput output;

    MeshDraw mesh_draw = mesh_draws[input.mesh_draw_index];

    output.color_out = float4(input.vTexcoord0_W.xy,0,1);
    output.normal_out = float2(0,0);
    output.occlusion_roughness_metalness_out = float4(0,0,0,0);
    output.emissive_out = float4(0,0,0,0);
    output.depth_normal_fwidth = float2(0,0);
    output.linear_z_dd = float2(0,0);

    // Diffuse color
    float4 base_color = compute_diffuse_color( mesh_draw.base_color_factor, mesh_draw.textures.x, input.vTexcoord0_W.xy );

    const uint flags = mesh_draw.flags;

    apply_alpha_discards( flags, base_color.a, mesh_draw.alpha_cutoff, input.vTexcoord0_W.xy, mesh_draw.alpha_texture_index );

#if DEBUG
    output.color_out = input.vColour;
#else
    output.color_out = base_color;
#endif

    // Geometric Normals
    float3 world_position = input.vPosition_BiTanZ.xyz;

    float3 normal = normalize(input.vNormal_BiTanX.xyz);
    float3 tangent = normalize(input.vTangent_BiTanY.xyz);
    float3 bitangent = normalize(float3(input.vNormal_BiTanX.w, input.vTangent_BiTanY.w, input.vPosition_BiTanZ.w));

    calculate_geometric_TBN( normal, tangent, bitangent, input.vTexcoord0_W.xy, world_position, flags, is_front_face );

    normal = apply_pixel_normal( mesh_draw.textures.z, input.vTexcoord0_W.xy, normal, tangent, bitangent );

    bool double_sided = ( mesh_draw.flags & DrawFlags_DoubleSided ) != 0;

    if ( !is_front_face && double_sided ) {
        normal *= -1;
    }

    output.normal_out = octahedral_encode(normal);

    // PBR Parameters
    output.occlusion_roughness_metalness_out.rgb = calculate_pbr_parameters( mesh_draw.metallic_roughness_occlusion_factor.x, mesh_draw.metallic_roughness_occlusion_factor.y,
                                                                      mesh_draw.textures.y, mesh_draw.metallic_roughness_occlusion_factor.z, mesh_draw.textures.w, input.vTexcoord0_W.xy );

    output.emissive_out = float4( calculate_emissive(mesh_draw.emissive.rgb, uint(mesh_draw.emissive.w), input.vTexcoord0_W.xy ), 1.0 );

//     mesh_id = mesh_draw_index;
    output.mesh_id = input.mesh_draw_index;

    output.depth_normal_fwidth = float2( length( fwidth( world_position ) ), length( fwidth( normal ) ) );

    // TODO(marco): this gives us the wrong z? Save this in the history?
    float linear_z = world_position.z * input.vTexcoord0_W.z;
    output.linear_z_dd = float2( linear_z, max( abs( ddx( linear_z ) ), abs( ddy( linear_z ) ) ) );

    return output;
}

#endif // FRAGMENT

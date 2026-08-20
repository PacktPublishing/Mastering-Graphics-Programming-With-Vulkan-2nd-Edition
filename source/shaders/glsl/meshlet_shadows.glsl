
#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : require

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "meshlet.h"
#include "debug_rendering.h"


// NEW
struct ShadowDrawKey {
    uint    light_index;
    uint    face_index;
    uint    mip_level;
    uint    shadow_slot;
};

layout(set = MATERIAL_SET, binding = 30) buffer ShadowDrawKeyBuffer {
    ShadowDrawKey keys[];
} shadow_draw_key_sb;


struct ShadowDrawMeta {
    uint light_index;
    uint face_index;
    uint mip_level;
    uint shadow_slot;

    uint meshlet_base;          // base index into meshlet_instances_shadow[]
    uint meshlet_count;         // number of meshlets for this draw

    uint draw_command_index;    // index into indirect command buffer
    uint meshlet_allocated;     // for debug (usually == meshlet_count)
};

layout(std430, set = MATERIAL_SET, binding = 31) buffer ShadowDrawMetaBuffer {
    ShadowDrawMeta metas[];
} shadow_draw_meta_sb;


struct ShadowMeshletInstance {
    uint mesh_instance_index;   // Index into the mesh instance buffer.
    uint global_meshlet_index;  // Index into the global meshlet buffer.
};

const uint k_shadow_meshlet_index_bits = 26u;
const uint k_shadow_meshlet_index_mask = (1u << k_shadow_meshlet_index_bits) - 1u;
const uint k_shadow_face_mask = 0x3fu;

uint pack_shadow_meshlet(uint global_meshlet_index, uint face_mask) {
    return (global_meshlet_index & k_shadow_meshlet_index_mask) | ((face_mask & k_shadow_face_mask) << k_shadow_meshlet_index_bits);
}

uint unpack_shadow_meshlet_index(uint packed_value) {
    return packed_value & k_shadow_meshlet_index_mask;
}

uint unpack_shadow_face_mask(uint packed_value) {
    return packed_value >> k_shadow_meshlet_index_bits;
}


layout(std430, set = MATERIAL_SET, binding = 32) buffer ShadowMeshletInstanceBuffer {
    ShadowMeshletInstance instances[];
} shadow_meshlet_instances_sb;


struct DrawMeshTasksIndirectCommand {
    uint group_count_x;
    uint group_count_y;
    uint group_count_z;
};

layout(std430, set = MATERIAL_SET, binding = 33) buffer ShadowIndirectCommandBuffer {
    DrawMeshTasksIndirectCommand commands[];
} shadow_indirect_cmds_sb;

// Counter
layout(std430, set = MATERIAL_SET, binding = 34) buffer ShadowIndirectCountBuffer {
    uint draw_count;
} shadow_indirect_count_sb;


// Allocation offset
layout(std430, set = MATERIAL_SET, binding = 35) buffer ShadowMeshletAllocator {
    uint meshlet_instance_cursor;
} shadow_meshlet_alloc_sb;

// Debug buffers
struct ShadowDrawDebug {
    uint allocated_meshlets;
    uint emitted_meshlets;
    uint culled_meshlets;

    uint face_used;
    uint mip_used;
    uint padding0;
};

layout(std430, set = MATERIAL_SET, binding = 36) buffer ShadowDrawDebugBuffer {
    ShadowDrawDebug draw_debug[];
} shadow_draw_debug_sb;


layout(std430, set = MATERIAL_SET, binding = 37) buffer ShadowGlobalDebugBuffer {
    uint total_draws_built;
    uint total_meshlets_allocated;
    uint total_meshlets_emitted;
    uint padding1;
} shadow_global_debug_sb;


layout(std430, set = MATERIAL_SET, binding = 38) readonly buffer ShadowViews {
    mat4 view_projections[];
} shadow_views_sb;

layout (std430, set = MATERIAL_SET, binding = 39) readonly buffer ShadowCameraSpheres {
    vec4 camera_spheres[];
} shadow_camera_spheres_sb;

#if defined(COMPUTE_BUILD_LISTS) || defined(TASK_MESHLET_DEPTH)

struct FaceBasis {
    vec3 forward;
    vec3 right;
    vec3 up;
};

// Cubemap face order: +X, -X, +Y, -Y, +Z, -Z.
FaceBasis make_face_basis(uint face) {
    const vec3 forwards[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
    const vec3 rights[6] = vec3[](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(-1, 0, 0));
    const vec3 ups[6] = vec3[](vec3(0, -1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, -1, 0), vec3(0, -1, 0));

    FaceBasis basis;
    basis.forward = forwards[face];
    basis.right = rights[face];
    basis.up = ups[face];
    return basis;
}

// Returns true when a world-space AABB intersects one 90-degree cubemap face frustum.
bool aabb_intersects_cubemap_face(vec3 light_position, vec3 face_forward, vec3 face_right, vec3 face_up, vec3 aabb_min, vec3 aabb_max, float near_z, float far_z) {
    vec3 center = (aabb_min + aabb_max) * 0.5;
    vec3 extents = (aabb_max - aabb_min) * 0.5;
    vec3 relative_center = center - light_position;

    float z = dot(relative_center, face_forward);
    float x = dot(relative_center, face_right);
    float y = dot(relative_center, face_up);

    float ez = dot(abs(face_forward), extents);
    float ex = dot(abs(face_right), extents);
    float ey = dot(abs(face_up), extents);

    if (z + ez < near_z || z - ez > far_z) return false;
    if (abs(x) > z + ez + ex) return false;
    if (abs(y) > z + ez + ey) return false;
    return true;
}

#endif // COMPUTE_BUILD_LISTS || TASK_MESHLET_DEPTH

// COMPUTE_CLEAR_COUNTERS ////////////////////////////////////////////////
#if defined (COMPUTE_CLEAR_COUNTERS)

// layout(push_constant, scalar) uniform Push {
//     uint clear_debug; // 0/1
// } pc;

void main() {
    if (gl_GlobalInvocationID.x == 0) {
        shadow_indirect_count_sb.draw_count = 0;
        shadow_meshlet_alloc_sb.meshlet_instance_cursor = 0;

        //if (pc.clear_debug != 0) 
        {
            shadow_global_debug_sb.total_draws_built         = 0;
            shadow_global_debug_sb.total_meshlets_allocated  = 0;
            shadow_global_debug_sb.total_meshlets_emitted    = 0;
        }
    }
}
#endif // COMPUTE_CLEAR_COUNTERS

// COMPUTE_BUILD_LISTS ///////////////////////////////////////////////////
#if defined(COMPUTE_BUILD_LISTS)

struct Light {
    vec3 world_position;
    float radius;

    vec3 color;
    float intensity;

    float shadow_map_resolution;
    float rcp_n_minus_f;
    float padding_l_001;
    float padding_l_002;
};

layout(set = MATERIAL_SET, binding = 21) readonly buffer Lights {
    Light lights[];
};

layout(push_constant) uniform Push {
    uint key_count;
    uint mesh_instance_count;
    uint tiles_per_key;
    uint pad001;
} pc;

// One count and one coarse face mask per mesh instance in the tile.
shared uint s_meshlet_count[256];
shared uint s_face_mask[256];

// Six independent prefix sums, one for each cubemap face.
shared uint s_face_prefix[1536];

// Final meshlet count and global allocation base for every face.
shared uint s_face_total[6];
shared uint s_face_base[6];

// Total number of meshlet entries allocated by this workgroup.
shared uint s_total_meshlet_count;

// First index of the six draw metadata entries.
shared uint s_first_draw_index;

bool aabb_intersects_sphere(vec3 aabb_min, vec3 aabb_max, vec3 sphere_center, float sphere_radius) {
    vec3 closest = clamp(sphere_center, aabb_min, aabb_max);
    vec3 delta = sphere_center - closest;
    return dot(delta, delta) <= sphere_radius * sphere_radius;
}

void aabb_transform(mat4 world, vec3 local_min, vec3 local_max, out vec3 world_min, out vec3 world_max) {
    vec3 local_center = (local_min + local_max) * 0.5;
    vec3 local_extents = (local_max - local_min) * 0.5;

    vec3 world_center = (world * vec4(local_center, 1.0)).xyz;

    mat3 linear = mat3(world);
    mat3 absolute_linear = mat3(abs(linear[0]), abs(linear[1]), abs(linear[2]));
    vec3 world_extents = absolute_linear * local_extents;

    world_min = world_center - world_extents;
    world_max = world_center + world_extents;
}

bool sphere_intersects_cubemap_face(vec3 light_position, vec3 face_forward, vec3 face_right, vec3 face_up, vec3 sphere_center, float sphere_radius, float near_z, float far_z) {
    vec3 relative_center = sphere_center - light_position;

    float z = dot(relative_center, face_forward);
    float x = dot(relative_center, face_right);
    float y = dot(relative_center, face_up);

    if (z + sphere_radius < near_z) return false;
    if (z - sphere_radius > far_z) return false;
    if (abs(x) > z + sphere_radius) return false;
    if (abs(y) > z + sphere_radius) return false;

    return true;
}

uint calculate_face_mask(vec3 light_position, float light_radius, vec3 sphere_center, float sphere_radius) {
    uint face_mask = 0u;

    for (uint face = 0u; face < 6u; ++face) {
        FaceBasis basis = make_face_basis(face);

        if (sphere_intersects_cubemap_face(light_position, basis.forward, basis.right, basis.up, sphere_center, sphere_radius, 0.01, light_radius)) {
            face_mask |= 1u << face;
        }
    }

    // A mesh intersecting the light sphere must affect at least one cubemap face.
    // Fall back to all faces if bounds or numerical errors produce an empty mask.
    return face_mask != 0u ? face_mask : k_shadow_face_mask;
}

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    const uint tile_index = gl_WorkGroupID.x;
    const uint key_index = gl_WorkGroupID.y;
    const uint lane = gl_LocalInvocationID.x;

    if (key_index >= pc.key_count || tile_index >= pc.tiles_per_key) {
        return;
    }

    const ShadowDrawKey key = shadow_draw_key_sb.keys[key_index];
    const Light light = lights[key.light_index];
    const uint mesh_instance_index = tile_index * 256u + lane;

    uint meshlet_count = 0u;
    uint face_mask = 0u;

    if (mesh_instance_index < pc.mesh_instance_count) {
        const MeshInstanceDraw mesh_instance = mesh_instance_draws[mesh_instance_index];
        const uint mesh_draw_index = mesh_instance.mesh_draw_index;
        const MeshDraw mesh_draw = mesh_draws[mesh_draw_index];

        if ((mesh_draw.flags & (DrawFlags_AlphaMask | DrawFlags_Transparent)) == 0u) {
            const vec3 local_aabb_min = mesh_aabb_sb.aabbs[mesh_draw_index * 2u].xyz;
            const vec3 local_aabb_max = mesh_aabb_sb.aabbs[mesh_draw_index * 2u + 1u].xyz;

            vec3 world_aabb_min;
            vec3 world_aabb_max;
            aabb_transform(mesh_instance.model, local_aabb_min, local_aabb_max, world_aabb_min, world_aabb_max);

            if (aabb_intersects_sphere(world_aabb_min, world_aabb_max, light.world_position, light.radius)) {
                const vec4 local_bounds = mesh_bounds[mesh_draw_index];
                const vec3 world_center = (mesh_instance.model * vec4(local_bounds.xyz, 1.0)).xyz;

                const float scale_x = length(mesh_instance.model[0].xyz);
                const float scale_y = length(mesh_instance.model[1].xyz);
                const float scale_z = length(mesh_instance.model[2].xyz);
                const float maximum_scale = max(scale_x, max(scale_y, scale_z));
                const float world_radius = local_bounds.w * maximum_scale * 1.1;

                meshlet_count = mesh_draw.meshlet_count;
                face_mask = calculate_face_mask(light.world_position, light.radius, world_center, world_radius);
            }
        }
    }

    s_meshlet_count[lane] = meshlet_count;
    s_face_mask[lane] = face_mask;

    barrier();

    if (lane == 0u) {
        uint total_meshlet_count = 0u;

        // Build six independent prefix sums.
        for (uint face = 0u; face < 6u; ++face) {
            const uint face_bit = 1u << face;
            const uint prefix_base = face * 256u;

            uint running = 0u;

            for (uint instance_lane = 0u; instance_lane < 256u; ++instance_lane) {
                s_face_prefix[prefix_base + instance_lane] = running;

                if ((s_face_mask[instance_lane] & face_bit) != 0u) {
                    running += s_meshlet_count[instance_lane];
                }
            }

            s_face_total[face] = running;
            s_face_base[face] = total_meshlet_count;
            total_meshlet_count += running;
        }

        s_total_meshlet_count = total_meshlet_count;

        if (total_meshlet_count > 0u) {
            const uint allocation_base = atomicAdd(shadow_meshlet_alloc_sb.meshlet_instance_cursor, total_meshlet_count);

            for (uint face = 0u; face < 6u; ++face) {
                s_face_base[face] += allocation_base;
            }

            s_first_draw_index = atomicAdd(shadow_indirect_count_sb.draw_count, 6u);

#if defined(SHADOW_LIST_DEBUG)
            atomicAdd(shadow_global_debug_sb.total_draws_built, 6u);
            atomicAdd(shadow_global_debug_sb.total_meshlets_allocated, total_meshlet_count);
#endif
        }
    }

    barrier();

    // This value is workgroup-uniform, so every invocation exits together.
    if (s_total_meshlet_count == 0u) {
        return;
    }

    const uint local_meshlet_count = s_meshlet_count[lane];
    const uint local_face_mask = s_face_mask[lane];

    if (local_meshlet_count > 0u) {
        const MeshInstanceDraw mesh_instance = mesh_instance_draws[mesh_instance_index];
        const MeshDraw mesh_draw = mesh_draws[mesh_instance.mesh_draw_index];
        const uint meshlet_offset = mesh_draw.meshlet_offset;

        // Duplicate the meshlets only into the cubemap faces touched by this mesh instance.
        for (uint face = 0u; face < 6u; ++face) {
            const uint face_bit = 1u << face;

            if ((local_face_mask & face_bit) == 0u) {
                continue;
            }

            const uint prefix_index = face * 256u + lane;
            const uint destination_base = s_face_base[face] + s_face_prefix[prefix_index];

            for (uint meshlet_index = 0u; meshlet_index < local_meshlet_count; ++meshlet_index) {
                ShadowMeshletInstance candidate;
                candidate.mesh_instance_index = mesh_instance_index;
                candidate.global_meshlet_index = pack_shadow_meshlet(meshlet_offset + meshlet_index, face_bit);

                shadow_meshlet_instances_sb.instances[destination_base + meshlet_index] = candidate;
            }
        }
    }

    barrier();

    // Write one independent draw for every cubemap face.
    if (lane < 6u) {
        const uint face = lane;
        const uint draw_index = s_first_draw_index + face;

        ShadowDrawMeta meta;
        meta.light_index = key.light_index;
        meta.face_index = face;
        meta.mip_level = key.mip_level;
        meta.shadow_slot = key.shadow_slot;
        meta.meshlet_base = s_face_base[face];
        meta.meshlet_count = s_face_total[face];
        meta.draw_command_index = draw_index;
        meta.meshlet_allocated = s_face_total[face];

        shadow_draw_meta_sb.metas[draw_index] = meta;
    }
}

#endif // COMPUTE_BUILD_LISTS

// COMPUTE_BUILD_INDIRECT_CMDS ///////////////////////////////////////////
#if defined (COMPUTE_BUILD_INDIRECT_CMDS)

layout(push_constant, scalar) uniform Push {
    uint max_draws;
    uint meshlets_per_task_wg; // typically 32
    uint pad000;
    uint pad001;
} pc;

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint dc = shadow_indirect_count_sb.draw_count;

    if (i >= dc || i >= pc.max_draws) {
        return;
    }

    uint meshlet_count = shadow_draw_meta_sb.metas[i].meshlet_count;
    uint wg = (meshlet_count + (pc.meshlets_per_task_wg - 1)) / pc.meshlets_per_task_wg;

    shadow_indirect_cmds_sb.commands[i].group_count_x = wg;
    shadow_indirect_cmds_sb.commands[i].group_count_y = 1;
    shadow_indirect_cmds_sb.commands[i].group_count_z = 1;
}


#endif // COMPUTE_BUILD_INDIRECT_CMDS

// Common TASK/MESH shaders code /////////////////////////////////////////

#if defined(TASK_MESHLET_DEPTH) || defined(MESH_MESHLET_DEPTH)

struct PayloadEntry {
    uint mesh_instance_index;
    uint global_meshlet_index;
};

struct Payload {
    PayloadEntry entries[32];
    uint draw_index;
    uint light_index;
    uint face_index;
    uint mip_level;
    uint shadow_slot;
};

taskPayloadSharedEXT Payload payload;

#endif // TASK_MESHLET_DEPTH || MESH_MESHLET_DEPTH

#if defined(TASK_MESHLET_DEPTH)

#include "culling.h"

layout(set = MATERIAL_SET, binding = 40) readonly buffer Meshlets {
    Meshlet meshlets[];
};

bool sphere_intersects_cubemap_face(vec3 light_position, vec3 face_forward, vec3 face_right, vec3 face_up, vec3 sphere_center, float sphere_radius, float near_z, float far_z) {
    vec3 relative_center = sphere_center - light_position;
    float z = dot(relative_center, face_forward);
    float x = dot(relative_center, face_right);
    float y = dot(relative_center, face_up);

    if (z + sphere_radius < near_z || z - sphere_radius > far_z) {
        return false;
    }

    if (abs(x) > z + sphere_radius || abs(y) > z + sphere_radius) {
        return false;
    }
    return true;
}

bool spheres_intersect(vec3 center_a, float radius_a, vec3 center_b, float radius_b) {
    vec3 delta = center_a - center_b;
    float combined_radius = radius_a + radius_b;
    return dot(delta, delta) <= combined_radius * combined_radius;
}

bool accept_meshlet(uint mesh_instance_index, uint global_meshlet_index, ShadowDrawMeta meta) {
    const vec4 light_sphere = shadow_camera_spheres_sb.camera_spheres[meta.light_index];
    const mat4 model = mesh_instance_draws[mesh_instance_index].model;
    const vec3 world_center = (model * vec4(meshlets[global_meshlet_index].center, 1.0)).xyz;
    const float scale = length(model[0]);
    const float radius = meshlets[global_meshlet_index].radius * scale;
    const FaceBasis basis = make_face_basis(meta.face_index);

    if (!sphere_intersects_cubemap_face(light_sphere.xyz, basis.forward, basis.right, basis.up, world_center, radius, 0.01, light_sphere.w) && !disable_shadow_meshlets_cubemap_face_cull()) return false;

    const vec3 cone_axis = mat3(model) * vec3(int(meshlets[global_meshlet_index].cone_axis[0]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[1]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[2]) / 127.0);
    const float cone_cutoff = int(meshlets[global_meshlet_index].cone_cutoff) / 127.0;

    if (coneCull(world_center, radius, cone_axis, cone_cutoff, light_sphere.xyz) && !disable_shadow_meshlets_cone_cull()) {
        return false;
    }

    return true;
}


bool accept_meshlet2(uint mesh_instance_index, uint global_meshlet_index, ShadowDrawMeta meta) {

    const vec4 light_sphere = shadow_camera_spheres_sb.camera_spheres[meta.light_index];

    vec3 local_center = meshlets[global_meshlet_index].center;
    float local_radius = meshlets[global_meshlet_index].radius * 1.0;

    FaceBasis b = make_face_basis(meta.face_index);

    // Face cull
    mat4 model = mesh_instance_draws[mesh_instance_index].model;
    vec3 world_center = (model * vec4(local_center, 1)).xyz;

    float scale = length(model[0]);
    float radius = local_radius * scale;

    if (!spheres_intersect(world_center, radius, light_sphere.xyz, light_sphere.w) && !disable_shadow_meshlets_sphere_cull()) {
        return false;
    }

    bool face_visible = sphere_intersects_cubemap_face( 
                            light_sphere.xyz,
                            b.forward, b.right, b.up,
                            world_center.xyz, radius,
                            0.01, light_sphere.w );

    if (!face_visible && !disable_shadow_meshlets_cubemap_face_cull()) {
        return false;
    }

    //debug_draw_sphere_3circles( world_center, radius, 8, vec4(0,1,0,1));

    // Broken
    // if (!sphere_intersect(world_center, radius, light_sphere.xyz, light_sphere.w) &&
    //     !disable_shadow_meshlets_sphere_cull()) {
    //     return false;
    // }
    
    vec3 cone_axis = mat3( model ) * vec3(int(meshlets[global_meshlet_index].cone_axis[0]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[1]) / 127.0, int(meshlets[global_meshlet_index].cone_axis[2]) / 127.0);
    float cone_cutoff = int(meshlets[global_meshlet_index].cone_cutoff) / 127.0;

    if (coneCull(world_center, radius, cone_axis, cone_cutoff, light_sphere.xyz) &&
        !disable_shadow_meshlets_cone_cull()) {
        return false; 
    }       

    return true;
}

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

void main() {
    const uint lane = gl_LocalInvocationID.x;
    const uint draw_index = gl_DrawIDARB;
    const ShadowDrawMeta meta = shadow_draw_meta_sb.metas[draw_index];
    const uint local_meshlet_index = gl_WorkGroupID.x * 32u + lane;

    bool accept = false;
    PayloadEntry entry;

    if (local_meshlet_index < meta.meshlet_count) {
        const uint candidate_index = meta.meshlet_base + local_meshlet_index;
        const ShadowMeshletInstance candidate = shadow_meshlet_instances_sb.instances[candidate_index];

        const uint global_meshlet_index = unpack_shadow_meshlet_index(candidate.global_meshlet_index);
        const uint face_mask = unpack_shadow_face_mask(candidate.global_meshlet_index);
        const uint face_bit = 1u << meta.face_index;

        if ((face_mask & face_bit) != 0u || disable_shadow_meshlets_cubemap_face_cull()) {
            entry.mesh_instance_index = candidate.mesh_instance_index;
            entry.global_meshlet_index = global_meshlet_index;
            accept = accept_meshlet2(entry.mesh_instance_index, entry.global_meshlet_index, meta);
        }
    }

    const uvec4 ballot = subgroupBallot(accept);
    const uint compact_index = subgroupBallotExclusiveBitCount(ballot);

    if (accept) payload.entries[compact_index] = entry;

    if (lane == 0u) {
        payload.draw_index = draw_index;
        payload.light_index = meta.light_index;
        payload.face_index = meta.face_index;
        payload.mip_level = meta.mip_level;
        payload.shadow_slot = meta.shadow_slot;
    }

    const uint visible_count = subgroupBallotBitCount(ballot);
    if (subgroupElect() && visible_count > 0u) {
        EmitMeshTasksEXT(visible_count, 1, 1);
    }
}

#endif // TASK_MESHLET_DEPTH

#if defined (MESH_MESHLET_DEPTH)

#include "meshlet.h"


layout(set = MATERIAL_SET, binding = 40) readonly buffer Meshlets
{
    Meshlet meshlets[];
};

layout(set = MATERIAL_SET, binding = 41) readonly buffer MeshletData
{
    uint meshletData[];
};

layout(set = MATERIAL_SET, binding = 42) readonly buffer VertexPositions
{
    VertexPosition vertex_positions[];
};

layout(set = MATERIAL_SET, binding = 43) readonly buffer MeshletPositionOnlyData
{
    uint meshletPositionOnlyData[];
};


uint read_meshlet_index_u8( uint base_word_offset, uint byte_offset ) {
    uint word_index = base_word_offset + (byte_offset >> 2u);
    uint shift      = (byte_offset & 3u) * 8u;
    return (meshletData[word_index] >> shift) & 0xffu;
}

uvec3 read_meshlet_triangle(uint base_word_offset, uint triangle_index) {
    uint byte_offset = triangle_index * 3u;
    uint word_index = base_word_offset + (byte_offset >> 2u);
    uint byte_in_word = byte_offset & 3u;
    uint shift = byte_in_word * 8u;

    uint word0 = meshletData[word_index];
    uint packed_triangle = word0 >> shift;

    if (shift > 8u) {
        uint word1 = meshletData[word_index + 1u];
        packed_triangle |= word1 << (32u - shift);
    }

    return uvec3(
        packed_triangle & 0xffu,
        (packed_triangle >> 8u) & 0xffu,
        (packed_triangle >> 16u) & 0xffu
    );
}

vec3 unpack_mesh_position_101010(uint packed_position, vec3 aabb_min, vec3 aabb_max) {
    const uvec3 quantized = uvec3(
        packed_position & 1023u,
        (packed_position >> 10u) & 1023u,
        (packed_position >> 20u) & 1023u
    );

    const vec3 normalized = vec3(quantized) * (1.0 / 1023.0);
    return mix(aabb_min, aabb_max, normalized);
}

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

void main() {
    PayloadEntry e = payload.entries[gl_WorkGroupID.x];

    uint mesh_instance_index = e.mesh_instance_index;
    uint global_meshlet_index = e.global_meshlet_index;
    int layer_index = int(payload.light_index * 6 + payload.face_index);
    uint task_index = gl_LocalInvocationID.x;
    uint draw_index = payload.draw_index;

    //MeshDraw mesh_draw = mesh_draws[ meshlets[global_meshlet_index].mesh_index ];
    
    uint vertex_count = uint(meshlets[global_meshlet_index].vertex_count);
    uint triangle_count = uint(meshlets[global_meshlet_index].triangle_count);

    if (vertex_count > 64 || triangle_count > 124) {
        SetMeshOutputsEXT(0,0);
        return;
    }


    const uint mesh_index = meshlets[global_meshlet_index].mesh_index;
    const vec3 mesh_aabb_min = mesh_aabb_sb.aabbs[mesh_index * 2u].xyz;
    const vec3 mesh_aabb_max = mesh_aabb_sb.aabbs[mesh_index * 2u + 1u].xyz;

    uint connectivity_data_offset = meshlets[global_meshlet_index].connectivity_data_offset;
    uint vertex_offset = connectivity_data_offset;
    uint index_offset = connectivity_data_offset + vertex_count;

    const mat4 model = mesh_instance_draws[mesh_instance_index].model;
    const mat4 view_projection = shadow_views_sb.view_projections[layer_index];
    mat4 mvp = view_projection * model;

    // TODO: if we have meshlets with 62 or 63 vertices then we pay a small penalty for branch divergence here - we can instead redundantly xform the last vertex
    for (uint i = task_index; i < vertex_count; i += 32)
    {
        uint vi = meshletData[vertex_offset + i];

        //vec3 position = vec3(vertex_positions[vi].v.x, vertex_positions[vi].v.y, vertex_positions[vi].v.z);

        const uint packed_position = meshletPositionOnlyData[connectivity_data_offset + i];
        vec3 position = unpack_mesh_position_101010(packed_position, mesh_aabb_min, mesh_aabb_max);

        gl_MeshVerticesEXT[ i ].gl_Position = (mvp * vec4(position, 1));
    }

    for (uint i = task_index; i < triangle_count; i += 32) {

        // Each triangle has 3 consecutive 8-bit indices
        // uint byte_base = i * 3u;

        // uint a = read_meshlet_index_u8(index_offset, byte_base + 0u);
        // uint b = read_meshlet_index_u8(index_offset, byte_base + 1u);
        // uint c = read_meshlet_index_u8(index_offset, byte_base + 2u);

        // gl_PrimitiveTriangleIndicesEXT[i] = uvec3(a, b, c);
        gl_PrimitiveTriangleIndicesEXT[i] = read_meshlet_triangle(index_offset, i);
        gl_MeshPrimitivesEXT[i].gl_Layer = layer_index;
        gl_MeshPrimitivesEXT[i].gl_CullPrimitiveEXT = false;
    }

    SetMeshOutputsEXT( vertex_count, triangle_count );

    // Debug triangle
    // if (task_index < 3) {
    //   vec2 p[3] = vec2[3]( vec2(-0.8,-0.8), vec2(0.8,-0.8), vec2(0.0,0.8) );
    //   gl_MeshVerticesEXT[task_index].gl_Position = vec4(p[task_index], 0.5, 1.0);
    // }
    // SetMeshOutputsEXT(3, 1);
    // gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0,1,2);
    // gl_MeshPrimitivesEXT[0].gl_Layer = 0;
}

#endif // MESH_MESHLET_DEPTH

#if defined (COMPUTE_POINTSHADOWS_RESOLUTION_CALCULATION)

struct Light {
    vec3            world_position;
    float           radius;

    vec3            color;
    float           intensity;

    float           shadow_map_resolution;
    float           lpad00;
    float           lpad01;
    float           lpad02;
};

layout(set = MATERIAL_SET, binding = 35) readonly buffer LightsAABBArray {
    vec4            light_aabbs[];
};

layout(set = MATERIAL_SET, binding = 36) buffer ShadowResolutions {
    uint            shadow_resolutions[];
};

layout(set = MATERIAL_SET, binding = 21) readonly buffer Lights {
    Light           lights[];
};


layout( push_constant ) uniform PushConstants {
    uint            depth_pyramid_texture_index;
};

vec3 line_intersection_to_z_plane( vec3 a, vec3 b, float z_plane ) {
    // Plane: z = z_plane in view space (RH: in front is negative Z)
    vec3 ab = b - a;

    // Avoid divide-by-zero if ray is parallel to plane.
    float denom = ab.z;
    if ( abs( denom ) < 1.0e-8 ) {
        return vec3( a.xy, z_plane );
    }

    float t = ( z_plane - a.z ) / denom;
    return a + ab * t;
}

vec3 screen_to_view(vec2 screen_pos, mat4 inverse_projection, float clip_z) {
    // screen_pos: pixels, origin top-left
    vec2 uv = uv_from_pixels( ivec2(screen_pos), uint(frame.resolution.x), uint(frame.resolution.y) );

    // NDC: x in [-1,1], y in [-1,1] with top-left screen => flip y here.
    vec4 clip = vec4( uv.x * 2.0 - 1.0,
                      (1.0 - uv.y) * 2.0 - 1.0,
                      clip_z,
                      1.0 );

    vec4 view4 = inverse_projection * clip;

    // Perspective divide is REQUIRED.
    view4.xyz /= max(view4.w, 1.0e-8);

    return view4.xyz;
}

bool sphere_intersects_aabb( vec3 c, float r, vec3 bmin, vec3 bmax ) {
    vec3 q = clamp( c, bmin, bmax );
    vec3 d = c - q;
    return dot(d,d) <= r * r;
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    const uint tile_x_count = (uint(frame.resolution.x) + 63u) / 64u;
    const uint tile_y_count = (uint(frame.resolution.y) + 63u) / 64u;

    if (uint(pos.x) >= tile_x_count || uint(pos.y) >= tile_y_count) {
        return;
    }

    const float tile_size   = 64.0;
    const float tile_pixels = tile_size * tile_size;

    vec2 min_screen = vec2( float(pos.x) * tile_size, float(pos.y) * tile_size );
    vec2 max_screen = vec2( float(pos.x + 1) * tile_size, float(pos.y + 1) * tile_size );

    vec2 tile_center = (min_screen + max_screen) * 0.5;

    const uint  z_count      = 32u;
    const float z_ratio      = frame.z_far / frame.z_near;
    const float z_bin_range  = 1.0 / float(z_count);

    const float tile_radius_sq = pow(tile_size * 0.5, 2.0) * 2.0;

    // Build rays through tile corners (use far plane for stable direction).
    // ZO => far clip_z = 1.
    vec3 max_point_view = screen_to_view( max_screen, frame.inverse_projection, 1.0 );
    vec3 min_point_view = screen_to_view( min_screen, frame.inverse_projection, 1.0 );

    // Depth pyramid: mip choice is your old heuristic; keep for now.
    const float raw_depth = texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], pos.xy, 7).r;

    const vec2 screen_uv = uv_from_pixels(pos.xy, uint(frame.resolution.x), uint(frame.resolution.y));
    const vec3 pixel_view_position = view_position_from_depth(screen_uv, raw_depth, frame.inverse_projection);

    // RH: in front of camera => pixel_view_position.z is negative.
    // Convert to positive distance.
    float center_d = -pixel_view_position.z;

    // Clamp to [near, far] to avoid out-of-range bins.
    center_d = clamp(center_d, frame.z_near, frame.z_far);

    float linear_d = (center_d - frame.z_near) / (frame.z_far - frame.z_near);
    int bin_index  = int( clamp(linear_d / BIN_WIDTH, 0.0, float(z_count - 1u)) );

    // Logarithmic slice distances (positive).
    float tile_near_d = frame.z_near * pow( z_ratio, float(bin_index)     * z_bin_range );
    float tile_far_d  = frame.z_near * pow( z_ratio, float(bin_index + 1) * z_bin_range );

    // RH view-space planes are NEGATIVE z in front.
    float z_plane_near = -tile_near_d;
    float z_plane_far  = -tile_far_d;

    // Eye/camera position for this math must be in VIEW SPACE.
    // If camera_position is in world-space, this is WRONG.
    // Best: use vec3(0,0,0) because we're in view space after unprojection.
    vec3 eye_vs = vec3(0.0);

    vec3 min_point_near = line_intersection_to_z_plane( eye_vs, min_point_view, z_plane_near );
    vec3 min_point_far  = line_intersection_to_z_plane( eye_vs, min_point_view, z_plane_far );
    vec3 max_point_near = line_intersection_to_z_plane( eye_vs, max_point_view, z_plane_near );
    vec3 max_point_far  = line_intersection_to_z_plane( eye_vs, max_point_view, z_plane_far );

    vec3 min_point_aabb_view = min( min( min_point_near, min_point_far ), min( max_point_near, max_point_far ) );
    vec3 max_point_aabb_view = max( max( min_point_near, min_point_far ), max( max_point_near, max_point_far ) );

    // Optional debug world AABB
    vec4 min_point_aabb_world = frame.inverse_view * vec4(min_point_aabb_view, 1.0);
    vec4 max_point_aabb_world = frame.inverse_view * vec4(max_point_aabb_view, 1.0);

    for ( uint l = 0u; l < active_lights; ++l ) {

        // Prefer light center + radius, but if you only have AABBs, you can keep both paths.
        // If lights[] contains radius, use sphere-vs-AABB to test cluster overlap more robustly.
        vec3 light_world_position = lights[ l ].world_position;
        float light_radius = lights[ l ].radius;

        vec3 light_view_pos = (frame.view * vec4(light_world_position, 1.0)).xyz;

        // Quick reject: sphere vs cluster AABB in VIEW space.
        if ( !sphere_intersects_aabb( light_view_pos, light_radius, min_point_aabb_view, max_point_aabb_view ) ) {
            continue;
        }

        // Project to NDC.
        vec4 sphere_clip = frame.view_projection * vec4( light_world_position, 1.0 );
        if ( sphere_clip.w == 0.0 ) {
            continue;
        }

        vec2 ndc = sphere_clip.xy / sphere_clip.w;

        // Convert to screen pixels with TOP-LEFT origin (flip Y).
        vec2 sphere_screen = vec2(
            (ndc.x * 0.5 + 0.5) * frame.resolution.x,
            (1.0 - (ndc.y * 0.5 + 0.5)) * frame.resolution.y
        );

        float d = length( sphere_screen - tile_center );

        // If d < tile_radius, solid angle saturates.
        float diff = max( d * d - tile_radius_sq, 0.0 );

        float solid_angle;
        if ( d > 1.0e-6 ) {
            solid_angle = ( 2.0 * PI ) * ( 1.0 - ( sqrt( diff ) / d ) );
        } else {
            solid_angle = ( 2.0 * PI );
        }

        float res_f = sqrt( ( 4.0 * PI * tile_pixels ) / ( 6.0 * max(solid_angle, 1.0e-8) ) );

        atomicMax( shadow_resolutions[l], uint(res_f) );
    }
}

// OLD CODE
#if 0

vec3 line_intersection_to_z_plane( vec3 a, vec3 b, float z ) {
    // All clusters planes are aligned in the same z direction
    vec3 normal = vec3( 0.0, 0.0, 1.0 );

    // Getting the line from the eye to the tile
    vec3 ab = b - a;

    // Computing the intersection length for the line and the plane
    float t = ( z - dot( normal, a ) ) / dot( normal, ab );

    // Computing the actual xyz position of the point along the line
    vec3 result = a + (ab * t);

    return result;
}

vec3 screen_to_view(vec2 screen_pos, mat4 inverse_projection, float depth) {

    const vec2 uv = uv_from_pixels(ivec2(screen_pos.xy), uint(resolution.x), uint(resolution.y));

    vec4 H = vec4( uv.x * 2 - 1, (1 - uv.y) * 2 - 1, depth, 1.0 );
    vec4 D = inverse_projection * H;

    return D.xyz;
}


layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    const uint tile_x_count = (uint(resolution.x) + 63) / 64;
    const uint tile_y_count = (uint(resolution.y) + 63) / 64;

    if (pos.x >= tile_x_count || pos.y >= tile_y_count) {
        return;
    }

    const float tile_size = 64.0f;
    const float tile_pixels = tile_size * tile_size;

    vec4 max_point_screen = vec4((pos.x + 1) * tile_size, (pos.y + 1) * tile_size, 0, 1);
    vec4 min_point_screen = vec4(pos.x * tile_size, pos.y * tile_size, 0, 1);

    vec4 tile_center_screen = (min_point_screen + max_point_screen) * 0.5f;
    vec2 tile_center = tile_center_screen.xy;

    const uint z_count = 32;
    const float z_ratio = frame.z_far / z_near;
    const float z_bin_range = 1.0f / float(z_count);

    const float tile_radius_sq = ( ( tile_size * 0.5f ) * ( tile_size * 0.5f ) ) * 2;

    // Pass min and max to view space
    vec3 max_point_view = screen_to_view( max_point_screen.xy, inverse_projection, 0.0f );
    vec3 min_point_view = screen_to_view( min_point_screen.xy, inverse_projection, 0.0f );

    // With a tile size of 64, read the 7th mipmap of the depth pyramid.
    const float raw_depth = texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], pos.xy, 7).r;
    const vec2 screen_uv = uv_from_pixels(pos.xy, uint(resolution.x), uint(resolution.y));
    const vec3 pixel_view_position = view_position_from_depth(screen_uv, raw_depth, inverse_projection);

    // Get the frustum for this z bin
    // TODO: use linear tiles for now.
    float linear_d = ( pixel_view_position.z - z_near ) / ( frame.z_far - z_near );
    int bin_index = int( linear_d / BIN_WIDTH );

    //float tile_near = z_near + (bin_index * z_bin_range) * (z_far - z_near);
    //float tile_far = tile_near + ((z_far - z_near) * z_bin_range);

    // TODO: use this when everything is working.
    // Near and far values of the cluster in view space
    // We use equation (2) directly to obtain the tile values
    float tile_near  = z_near * pow( z_ratio, float( bin_index ) * z_bin_range );
    float tile_far   = z_near * pow( z_ratio, float( bin_index + 1 ) * z_bin_range );

    //Finding the 4 intersection points made from each point to the cluster near/far plane
    vec3 min_point_near = line_intersection_to_z_plane( camera_position.xyz, min_point_view, tile_near );
    vec3 min_point_far  = line_intersection_to_z_plane( camera_position.xyz, min_point_view, tile_far );
    vec3 max_point_near = line_intersection_to_z_plane( camera_position.xyz, max_point_view, tile_near );
    vec3 max_point_far  = line_intersection_to_z_plane( camera_position.xyz, max_point_view, tile_far );

    vec3 min_point_aabb_view = min( min( min_point_near, min_point_far ), min( max_point_near, max_point_far ) );
    vec3 max_point_aabb_view = max( max( min_point_near, min_point_far ), max( max_point_near, max_point_far ) );

    vec4 min_point_aabb_world = vec4( min_point_aabb_view.xyz, 1.0f );
    vec4 max_point_aabb_world = vec4( max_point_aabb_view.xyz, 1.0f );

    min_point_aabb_world = inverse_view * min_point_aabb_world;
    max_point_aabb_world = inverse_view * max_point_aabb_world;

    for ( uint l = 0; l < active_lights; ++l ) {
        const vec3 light_aabb_min = light_aabbs[ l * 2 ].xyz;
        const vec3 light_aabb_max = light_aabbs[ l * 2 + 1 ].xyz;

        float minx = min( min( light_aabb_min.x, light_aabb_max.x ), min( min_point_aabb_view.x, max_point_aabb_view.x ) );
        float miny = min( min( light_aabb_min.y, light_aabb_max.y ), min( min_point_aabb_view.y, max_point_aabb_view.y ) );
        float minz = min( min( light_aabb_min.z, light_aabb_max.z ), min( min_point_aabb_view.z, max_point_aabb_view.z ) );

        float maxx = max( max( light_aabb_min.x, light_aabb_max.x ), max( min_point_aabb_view.x, max_point_aabb_view.x ) );
        float maxy = max( max( light_aabb_min.y, light_aabb_max.y ), max( min_point_aabb_view.y, max_point_aabb_view.y ) );
        float maxz = max( max( light_aabb_min.z, light_aabb_max.z ), max( min_point_aabb_view.z, max_point_aabb_view.z ) );

        float dx = abs( maxx - minx );
        float dy = abs( maxy - miny );
        float dz = abs( maxz - minz );

        float allx = abs( light_aabb_max.x - light_aabb_min.x ) + abs( max_point_aabb_view.x - min_point_aabb_view.x );
        float ally = abs( light_aabb_max.y - light_aabb_min.y ) + abs( max_point_aabb_view.y - min_point_aabb_view.y );
        float allz = abs( light_aabb_max.z - light_aabb_min.z ) + abs( max_point_aabb_view.z - min_point_aabb_view.z );

        bool intersects = ( dx <= allx ) && ( dy < ally ) && ( dz <= allz );

        if ( intersects ) {
            vec3 light_world_position = lights[ l ].world_position;
            vec4 sphere_world = vec4( light_world_position.xyz, 1.0f );
            vec4 sphere_ndc = view_projection * sphere_world;

            sphere_ndc.x /= sphere_ndc.w;
            sphere_ndc.y /= sphere_ndc.w;

            vec2 sphere_screen = vec2( ( ( sphere_ndc.x + 1.0f ) * 0.5f ) * resolution.x, ( ( sphere_ndc.y + 1.0f ) * 0.5f ) * resolution.y );

            float d = length( sphere_screen - tile_center );

            float diff = d * d - tile_radius_sq;

            if ( diff < 1.0e-4 ) {
                continue;
            }

            // NOTE(marco): as defined in https://math.stackexchange.com/questions/73238/calculating-solid-angle-for-a-sphere-in-space
            float solid_angle = ( 2.0f * PI ) * ( 1.0f - ( sqrt( diff ) / d ) );

            // NOTE(marco): following https://efficientshading.com/wp-content/uploads/s2015_shadows.pdf
            float resolution = sqrt( ( 4.0f * PI * tile_pixels ) / ( 6 * solid_angle ) );

            atomicMax(shadow_resolutions[l], uint(resolution));
        }
    }
}
#endif // 0 old code

#endif // COMPUTE_POINTSHADOWS_RESOLUTION_CALCULATION

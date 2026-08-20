
#ifndef RAPTOR_GLSL_CULLING_H
#define RAPTOR_GLSL_CULLING_H

// NOTE(marco): as described in meshoptimizer.h
bool coneCull(vec3 center, float radius, vec3 cone_axis, float cone_cutoff, vec3 camera_position)
{
    return dot(center - camera_position, cone_axis) >= cone_cutoff * length(center - camera_position) + radius;
}

bool sphere_intersect( vec3 center_a, float radius_a, vec3 center_b, float radius_b ) {
    const vec3 v = center_b - center_a;
    const float total_radius = radius_a + radius_b;

    return dot(v, v) < total_radius;
}

// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
// Returns an AABB in UV space.
bool project_sphere(vec3 C, float r, float znear, float P00, float P11, out vec4 aabb) {
    // RH, forward -Z -> positive depth
    float z = -C.z;
    if (z - r < znear)
        return false;

    vec2 cx = vec2(C.x, z);
    float dx2 = dot(cx, cx) - r * r;
    vec2 vx = vec2(sqrt(max(dx2, 0.0)), r);
    vec2 minx = mat2(vx.x,  vx.y,
                    -vx.y,  vx.x) * cx;
    vec2 maxx = mat2(vx.x, -vx.y,
                     vx.y,  vx.x) * cx;

    vec2 cy = vec2(-C.y, z);
    float dy2 = dot(cy, cy) - r * r;
    vec2 vy = vec2(sqrt(max(dy2, 0.0)), r);
    vec2 miny = mat2(vy.x,  vy.y,
                    -vy.y,  vy.x) * cy;
    vec2 maxy = mat2(vy.x, -vy.y,
                     vy.y,  vy.x) * cy;

    vec4 ndc = vec4(
        (minx.x / minx.y) * P00,
        (miny.x / miny.y) * P11,
        (maxx.x / maxx.y) * P00,
        (maxy.x / maxy.y) * P11
    );

    // NDC -> UV
    aabb = ndc.xwzy * vec4(0.5, 0.5, 0.5, 0.5) + vec4(0.5);
    return true;
}

// aabb_uv: (u_min, v_min, u_max, v_max) in [0..1]
// depth_sphere_01: sphere "near" depth in 0..1
// level_f: chosen mip level (float ok)
//
// Function that adaptively samples up to 9 texels.
// It starts with the center, then the mid-edges, and finally the corners.
// Mostly should be done with the tap-center.
bool hiz_test_sphere_aabb_adaptive( vec4 aabb_uv, float depth_sphere_01, float level_f,
                                    uint depth_pyramid_texture_index ) {

    // Clamp uv and early out on degenerate aabb.
    vec2 uv_min = clamp(aabb_uv.xy, vec2(0.0), vec2(1.0));
    vec2 uv_max = clamp(aabb_uv.zw, vec2(0.0), vec2(1.0));

    // if (uv_max.x <= uv_min.x || uv_max.y <= uv_min.y) {
    //     return true;
    // }

    int mip_count = textureQueryLevels(global_textures[nonuniformEXT(depth_pyramid_texture_index)]);
    int level = clamp(int(level_f), 0, mip_count - 1);

    ivec2 size_l = textureSize(global_textures[nonuniformEXT(depth_pyramid_texture_index)], level);

    // Convert UV AABB to a texel-aligned rect at mip level
    // Inclusive min, inclusive max.
    ivec2 t_min = ivec2(floor(uv_min * vec2(size_l)));
    ivec2 t_max = ivec2(ceil (uv_max * vec2(size_l))) - ivec2(1);

    t_min = clamp(t_min, ivec2(0), size_l - 1);
    t_max = clamp(t_max, ivec2(0), size_l - 1);

    // Enforce minimum 2x2 footprint (or 1x1 if you prefer)
    //t_max = max(t_max, t_min + ivec2(1));
    //t_max = min(t_max, size_l - 1);

    // Expand by 1 texel for safety (optional but helps popping a lot).
    t_min = max(t_min - ivec2(1), ivec2(0));
    t_max = min(t_max + ivec2(1), size_l - 1);

    ivec2 t_mid = (t_min + t_max) / 2;

    // mip texel covers 2^level base texels
    float texel_world = float(1 << level);
    // Epsilon: grows with mip level (bigger texels -> more conservative).
    // Tune this to influence culling aggressiveness
    float eps = 2.0e-4 * texel_world;

    // First: 1 tap (center)
    float depth = texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], t_mid, level).r;

    // Assumes standard z (less or equal, NOT reversed Z).
    // If visible stop.
    if (depth_sphere_01 <= depth - eps) {
        return true;
    }

    // Pass 2: +4 mid-edges (cross)
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_min.x, t_mid.y), level).r);
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_max.x, t_mid.y), level).r);
    
    if (depth_sphere_01 <= depth - eps) {
        return true;
    }

    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_mid.x, t_min.y), level).r);
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_mid.x, t_max.y), level).r);

    if (depth_sphere_01 <= depth - eps) {
        return true;
    }

    // Pass 3: +4 corners (only if still borderline)
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_min.x, t_min.y), level).r);
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_max.x, t_min.y), level).r);
    
    if (depth_sphere_01 <= depth - eps) {
        return true;
    }

    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_min.x, t_max.y), level).r);
    depth = max(depth, texelFetch(global_textures[nonuniformEXT(depth_pyramid_texture_index)], ivec2(t_max.x, t_max.y), level).r);

    // final decision, no eps needed here (already conservative)
    return (depth_sphere_01 <= depth);
}



bool occlusion_cull( vec3 view_bounding_center, float radius, float z_near, float projection_00, float projection_11,
                     uint depth_pyramid_texture_index, vec3 world_bounding_center, vec3 camera_world_position,
                     mat4 culling_view_projection ) {
    // aabb is in UV space
    vec4 aabb;
    bool occlusion_visible = true;
    if ( project_sphere(view_bounding_center, radius, z_near, projection_00, projection_11, aabb ) ) {

        // Calculate sphere depth
        vec3 dir = normalize(camera_world_position - world_bounding_center.xyz);
        vec4 sceen_space_center_last = culling_view_projection * vec4(world_bounding_center + dir * radius, 1.0);

        float depth_sphere = sceen_space_center_last.z / sceen_space_center_last.w;

        vec3 view_near_point = view_bounding_center.xyz + vec3(0.0, 0.0, radius);
        vec4 clip = inverse(frame.inverse_projection) * vec4(view_near_point, 1.0);
        depth_sphere = clip.z / clip.w;

        // Clamp aabb for numerical safety
        aabb.xy = clamp(aabb.xy, vec2(0.0), vec2(1.0));
        aabb.zw = clamp(aabb.zw, vec2(0.0), vec2(1.0));

        // Invalid aabbs are considered visible
        if (aabb.z <= aabb.x || aabb.w <= aabb.y) {
            return true;
        }

        // Calculate mip level based on AABB size.
        ivec2 depth_pyramid_size = textureSize(global_textures[nonuniformEXT(depth_pyramid_texture_index)], 0);
        float width = (aabb.z - aabb.x) * depth_pyramid_size.x;
        float height = (aabb.w - aabb.y) * depth_pyramid_size.y;

        float level = ceil(log2(max(width, height)));
        level = max( 0.f, level );

        occlusion_visible = hiz_test_sphere_aabb_adaptive( aabb, depth_sphere, level, depth_pyramid_texture_index );

        // Debug: write world space calculated bounds.
        // vec2 uv_center = (aabb.xy + aabb.zw) * 0.5;
        // vec4 sspos = vec4(uv_center * 2 - 1, depth, 1);
        // vec4 aabb_world = inverse_view_projection * sspos;
        // aabb_world.xyz /= aabb_world.w;

        //debug_draw_box( world_center.xyz - vec3(radius), world_center.xyz + vec3(radius), vec4(1,1,1,0.5));
        //debug_draw_box( aabb_world.xyz - vec3(radius * 1.1), aabb_world.xyz + vec3(radius * 1.1), vec4(0,depth_sphere - depth,1,0.5));

        //debug_draw_2d_box(aabb.xy * 2.0 - 1, aabb.zw * 2 - 1, occlusion_visible ? vec4(0,1,0,1) : vec4(1,0,0,1));
    }

    return occlusion_visible;
}

uint get_cube_face_mask( vec3 cube_map_pos, vec3 aabb_min, vec3 aabb_max ) {

    vec3 plane_normals[] = { vec3(-1, 1, 0), vec3(1, 1, 0), vec3(1, 0, 1), vec3(1, 0, -1), vec3(0, 1, 1), vec3(0, -1, 1) };
    vec3 abs_plane_normals[] = { vec3(1, 1, 0), vec3(1, 1, 0), vec3(1, 0, 1), vec3(1, 0, 1), vec3(0, 1, 1), vec3(0, 1, 1) };

    vec3 aabb_center = (aabb_min + aabb_max) * 0.5f;

    vec3 center = aabb_center - cube_map_pos;
    vec3 extents = (aabb_max - aabb_min) * 0.5f;

    bool rp[ 6 ];
    bool rn[ 6 ];

    for ( uint  i = 0; i < 6; ++i ) {
        float dist = dot( center, plane_normals[ i ] );
        float radius = dot( extents, abs_plane_normals[ i ] );

        rp[ i ] = dist > -radius;
        rn[ i ] = dist < radius;
    }

    uint fpx = (rn[ 0 ] && rp[ 1 ] && rp[ 2 ] && rp[ 3 ] && aabb_max.x > cube_map_pos.x) ? 1 : 0;
    uint fnx = (rp[ 0 ] && rn[ 1 ] && rn[ 2 ] && rn[ 3 ] && aabb_min.x < cube_map_pos.x) ? 1 : 0;
    uint fpy = (rp[ 0 ] && rp[ 1 ] && rp[ 4 ] && rn[ 5 ] && aabb_max.y > cube_map_pos.y) ? 1 : 0;
    uint fny = (rn[ 0 ] && rn[ 1 ] && rn[ 4 ] && rp[ 5 ] && aabb_min.y < cube_map_pos.y) ? 1 : 0;
    uint fpz = (rp[ 2 ] && rn[ 3 ] && rp[ 4 ] && rp[ 5 ] && aabb_max.z > cube_map_pos.z) ? 1 : 0;
    uint fnz = (rn[ 2 ] && rp[ 3 ] && rn[ 4 ] && rn[ 5 ] && aabb_min.z < cube_map_pos.z) ? 1 : 0;

    return fpx | ( fnx << 1 ) | ( fpy << 2 ) | ( fny << 3 ) | ( fpz << 4 ) | ( fnz << 5 );
}

#endif // RAPTOR_GLSL_CULLING_H
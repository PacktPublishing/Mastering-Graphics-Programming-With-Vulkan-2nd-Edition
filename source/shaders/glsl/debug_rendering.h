#ifndef RAPTOR_GLSL_DEBUG_RENDERING_H
#define RAPTOR_GLSL_DEBUG_RENDERING_H

// Config ////////////////////////////////////////////////////////////////
#define DEBUG_FRAMES_IN_FLIGHT 2u

// Per-frame budgets (linee, non vertici)
const uint k_max_lines_3d_per_frame    = 90000u;
const uint k_max_lines_2d_px_per_frame = 10000u;

// Derivati
const uint k_max_vertices_3d_per_frame    = k_max_lines_3d_per_frame * 2u;
const uint k_max_vertices_2d_px_per_frame = k_max_lines_2d_px_per_frame * 2u;

const uint k_frame_stride_vertices =
    k_max_vertices_3d_per_frame + k_max_vertices_2d_px_per_frame;

const uint k_total_vertices =
    k_frame_stride_vertices * DEBUG_FRAMES_IN_FLIGHT;

const float k_line_width_px = 1.5f;

// Data //////////////////////////////////////////////////////////////////
struct DebugLineVertex {
    vec3 position;
    uint color;
};

layout(set = MATERIAL_SET, binding = 50) buffer DebugLinesBuffer {
    DebugLineVertex vertices[];
} debug_lines_sb;


struct DebugLineCounts {
    uint vertex_count_3d;       // Counts of vertices written in the 3D slice of that frame
    uint vertex_count_2d_px;    // Counts of vertices written in the 2Dpx slice of that frame

    uint pad000;
    uint pad001;
};

layout(set = MATERIAL_SET, binding = 51) buffer DebugLinesCount {

    DebugLineCounts counts[ DEBUG_FRAMES_IN_FLIGHT ];

    uint frame_slot;            // 0..max frames - 1
    uint pad0;
} debug_line_counts_sb;

struct DrawCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

layout(set = MATERIAL_SET, binding = 52) buffer DebugLinesCommands {
    DrawCommand draw_3d;
    DrawCommand draw_2d_px;
} debug_lines_commands_sb;

// Internal helpers //////////////////////////////////////////////////////
uint debug_frame_slot() {
    return debug_line_counts_sb.frame_slot % DEBUG_FRAMES_IN_FLIGHT;
}

uint debug_base_vertex_3d(uint frame_slot) {
    return frame_slot * k_frame_stride_vertices;
}

uint debug_base_vertex_2d_px(uint frame_slot) {
    return frame_slot * k_frame_stride_vertices + k_max_vertices_3d_per_frame;
}

// Reserve vertices in the current frame slice.
// Returns ~0u on overflow.
uint debug_reserve_vertices_3d(uint vertex_count) {
    const uint slot = debug_frame_slot();
    const uint old  = atomicAdd(debug_line_counts_sb.counts[ slot ].vertex_count_3d, vertex_count);

    if (old + vertex_count > k_max_vertices_3d_per_frame) {
        // Rollback is not safe with atomics; just signal overflow.
        return ~0u;
    }

    return debug_base_vertex_3d(slot) + old;
}

uint debug_reserve_vertices_2d_px(uint vertex_count) {
    const uint slot = debug_frame_slot();
    const uint old  = atomicAdd(debug_line_counts_sb.counts[ slot ].vertex_count_2d_px, vertex_count);

    if (old + vertex_count > k_max_vertices_2d_px_per_frame) {
        return ~0u;
    }

    return debug_base_vertex_2d_px(slot) + old;
}

void debug_write_line_vertices(uint base_vertex,
                               vec3 start, vec3 end,
                               uint start_color, uint end_color) {
    debug_lines_sb.vertices[base_vertex + 0u].position = start;
    debug_lines_sb.vertices[base_vertex + 0u].color    = start_color;

    debug_lines_sb.vertices[base_vertex + 1u].position = end;
    debug_lines_sb.vertices[base_vertex + 1u].color    = end_color;
}

// 3D API (wireframe) ////////////////////////////////////////////////////
void debug_draw_line_coloru(vec3 start, vec3 end, uint start_color, uint end_color) {
    const uint v = debug_reserve_vertices_3d(2u);
    if (v == ~0u) {
        return; 
    }
    debug_write_line_vertices(v, start, end, start_color, end_color);
}

void debug_draw_line(vec3 start, vec3 end, vec4 start_color, vec4 end_color) {
    debug_draw_line_coloru(start, end, vec4_to_rgba(start_color), vec4_to_rgba(end_color));
}

// Batch: AABB = 12 lines = 24 vertices, one atomicAdd
void debug_draw_aabb(vec3 min_v, vec3 max_v, vec4 color) {
    const uint v = debug_reserve_vertices_3d(24u);
    if (v == ~0u) {
        return;
    }

    const float x0 = min_v.x, y0 = min_v.y, z0 = min_v.z;
    const float x1 = max_v.x, y1 = max_v.y, z1 = max_v.z;

    const uint c = vec4_to_rgba(color);

    // 12 edges -> 12 * 2 vertices
    uint o = 0u;

    debug_write_line_vertices(v + o, vec3(x0,y0,z0), vec3(x0,y1,z0), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x0,y1,z0), vec3(x1,y1,z0), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y1,z0), vec3(x1,y0,z0), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y0,z0), vec3(x0,y0,z0), c, c); o += 2u;

    debug_write_line_vertices(v + o, vec3(x0,y0,z0), vec3(x0,y0,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x0,y1,z0), vec3(x0,y1,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y1,z0), vec3(x1,y1,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y0,z0), vec3(x1,y0,z1), c, c); o += 2u;

    debug_write_line_vertices(v + o, vec3(x0,y0,z1), vec3(x0,y1,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x0,y1,z1), vec3(x1,y1,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y1,z1), vec3(x1,y0,z1), c, c); o += 2u;
    debug_write_line_vertices(v + o, vec3(x1,y0,z1), vec3(x0,y0,z1), c, c);
}

// Wire sphere: 3 great circles (cheap)
void debug_draw_sphere_3circles(vec3 center, float radius, uint segments, vec4 color) {
    
    segments = max(segments, 6u);
    
    const uint line_count = segments * 3u;
    const uint v = debug_reserve_vertices_3d(line_count * 2u);
    if (v == ~0u) {
        return;
    }

    const uint c = vec4_to_rgba(color);

    float step = 6.28318530718f / float(segments);
    uint o = 0u;

    for (uint i = 0u; i < segments; ++i) {
        float a0 = step * float(i);
        float a1 = step * float(i + 1u);

        // XY circle
        vec3 p0 = center + radius * vec3(cos(a0), sin(a0), 0.0);
        vec3 p1 = center + radius * vec3(cos(a1), sin(a1), 0.0);
        debug_write_line_vertices(v + o, p0, p1, c, c); o += 2u;

        // XZ circle
        p0 = center + radius * vec3(cos(a0), 0.0, sin(a0));
        p1 = center + radius * vec3(cos(a1), 0.0, sin(a1));
        debug_write_line_vertices(v + o, p0, p1, c, c); o += 2u;

        // YZ circle
        p0 = center + radius * vec3(0.0, cos(a0), sin(a0));
        p1 = center + radius * vec3(0.0, cos(a1), sin(a1));
        debug_write_line_vertices(v + o, p0, p1, c, c); o += 2u;
    }
}

// Wire cone: base circle + spokes to apex
// axis must be normalized. angle is half-angle in radians.
void debug_draw_cone(vec3 apex, vec3 axis, float height, float angle, uint segments, vec4 color) {
    segments = max(segments, 6u);

    // Build an orthonormal basis (u,v,axis)
    vec3 up = (abs(axis.y) < 0.999f) ? vec3(0,1,0) : vec3(1,0,0);
    vec3 u  = normalize(cross(up, axis));
    vec3 v  = cross(axis, u);

    float radius = tan(angle) * height;
    vec3  base_center = apex + axis * height;

    const uint line_count = segments /*circle*/ + segments /*spokes*/;
    const uint vb = debug_reserve_vertices_3d(line_count * 2u);
    if (vb == ~0u) { return; }

    const uint c = vec4_to_rgba(color);

    float step = 6.28318530718f / float(segments);
    uint o = 0u;

    vec3 first_p = base_center + radius * (u * cos(0.0) + v * sin(0.0));
    vec3 prev_p  = first_p;

    for (uint i = 1u; i <= segments; ++i) {
        float a = step * float(i);
        vec3 p = base_center + radius * (u * cos(a) + v * sin(a));

        // base circle edge
        debug_write_line_vertices(vb + o, prev_p, p, c, c); o += 2u;

        // spoke to apex (use p of previous segment so it's stable)
        debug_write_line_vertices(vb + o, apex, prev_p, c, c); o += 2u;

        prev_p = p;
    }

    // close spoke for the last point (optional): already covered by loop using prev_p
}

// Frustum from 8 corners
void debug_draw_frustum_corners(vec3 c0, vec3 c1, vec3 c2, vec3 c3,
                               vec3 c4, vec3 c5, vec3 c6, vec3 c7,
                               vec4 color) {
    // Convention: (0..3) near, (4..7) far, matching quad order.
    // 12 edges => 24 vertices
    const uint v = debug_reserve_vertices_3d(24u);
    if (v == ~0u) { return; }

    const uint cu = vec4_to_rgba(color);
    uint o = 0u;

    // near
    debug_write_line_vertices(v + o, c0, c1, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c1, c2, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c2, c3, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c3, c0, cu, cu); o += 2u;

    // far
    debug_write_line_vertices(v + o, c4, c5, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c5, c6, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c6, c7, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c7, c4, cu, cu); o += 2u;

    // sides
    debug_write_line_vertices(v + o, c0, c4, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c1, c5, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c2, c6, cu, cu); o += 2u;
    debug_write_line_vertices(v + o, c3, c7, cu, cu);
}

// 2D pixel-space API (stored in 2Dpx slice) /////////////////////////////
void debug_draw_line_2d_px_coloru(vec2 start_px, vec2 end_px, uint start_color, uint end_color) {
    const uint v = debug_reserve_vertices_2d_px(2u);
    if (v == ~0u) {
        return;
    }

    debug_lines_sb.vertices[v + 0u].position = vec3(start_px, 0.0);
    debug_lines_sb.vertices[v + 0u].color    = start_color;

    debug_lines_sb.vertices[v + 1u].position = vec3(end_px, 0.0);
    debug_lines_sb.vertices[v + 1u].color    = end_color;
}

void debug_draw_line_2d_px(vec2 start_px, vec2 end_px, vec4 start_color, vec4 end_color) {
    debug_draw_line_2d_px_coloru(start_px, end_px, vec4_to_rgba(start_color), vec4_to_rgba(end_color));
}

void debug_draw_box_2d_px(vec2 min_px, vec2 max_px, vec4 color) {
    const uint c = vec4_to_rgba(color);

    // 4 lines => 8 vertices, single atomic
    const uint v = debug_reserve_vertices_2d_px(8u);
    if (v == ~0u) {
        return;
    }

    uint o = 0u;

    debug_write_line_vertices(v + o, vec3(min_px.x, min_px.y, 0), vec3(max_px.x, min_px.y, 0), c, c);
    o += 2u;
    debug_write_line_vertices(v + o, vec3(max_px.x, min_px.y, 0), vec3(max_px.x, max_px.y, 0), c, c);
    o += 2u;
    debug_write_line_vertices(v + o, vec3(max_px.x, max_px.y, 0), vec3(min_px.x, max_px.y, 0), c, c);
    o += 2u;
    debug_write_line_vertices(v + o, vec3(min_px.x, max_px.y, 0), vec3(min_px.x, min_px.y, 0), c, c);
}

#endif // RAPTOR_GLSL_DEBUG_RENDERING_H

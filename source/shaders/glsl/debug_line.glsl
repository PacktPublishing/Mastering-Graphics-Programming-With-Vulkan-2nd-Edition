#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#if defined(VERTEX_DEBUG_LINE) || defined (FRAGMENT_DEBUG_LINE)
struct PhysicsVertex {
    vec3 position;
    vec3 start_position;
    vec3 previous_position;
    vec3 normal;
    uint joint_count;
    vec3 velocity;
    float mass;
    vec3 force; // TODO(marco): maybe we can remove this
    uint joints[ 12 ];
};

layout ( set = MATERIAL_SET, binding = 1 ) buffer PhysicsMesh {
    uint index_count;
    uint vertex_count;

    PhysicsVertex physics_vertices[];
};

#endif

#if defined(VERTEX_DEBUG_LINE)

void main() {
    uint vertex_index = 0;
    if ( gl_VertexIndex == 0 ) {
        vertex_index = gl_DrawIDARB;
    } else {
        vertex_index = physics_vertices[ gl_DrawIDARB ].joints[ gl_InstanceIndex ];
    }
    vec3 position = physics_vertices[ vertex_index ].position;
    gl_Position = frame.view_projection * vec4(position, 1.0);
}

#endif // VERTEX


#if defined (FRAGMENT_DEBUG_LINE)

layout (location = 0) out vec4 colour;

void main() {

    colour = vec4( 0.7, 0.2, 0.2, 1.0 );
}

#endif // FRAGMENT

#if defined (COMPUTE_COMMANDS_FINALIZE)

#include "debug_rendering.h"

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main() {

    // Calculate instance count for indirect drawing.
    const uint slot = debug_frame_slot();

    const uint base_vertex_3d    = debug_base_vertex_3d(slot);
    const uint base_vertex_2d_px = debug_base_vertex_2d_px(slot);

    const uint base_instance_3d    = base_vertex_3d / 2u;
    const uint base_instance_2d_px = base_vertex_2d_px / 2u;

    const uint vertices_3d    = debug_line_counts_sb.counts[ slot ].vertex_count_3d;
    const uint vertices_2d_px = debug_line_counts_sb.counts[ slot ].vertex_count_2d_px;

    // Line shader draws 1 thick line as 2 triangles => 6 vertices per instance.
    debug_lines_commands_sb.draw_3d.vertex_count   = 6u;
    debug_lines_commands_sb.draw_3d.instance_count = vertices_3d / 2u;
    debug_lines_commands_sb.draw_3d.first_vertex   = 0u;
    debug_lines_commands_sb.draw_3d.first_instance = base_instance_3d;

    debug_lines_commands_sb.draw_2d_px.vertex_count   = 6u;
    debug_lines_commands_sb.draw_2d_px.instance_count = vertices_2d_px / 2u;
    debug_lines_commands_sb.draw_2d_px.first_vertex   = 0u;
    debug_lines_commands_sb.draw_2d_px.first_instance = base_instance_2d_px;
}

#endif // COMPUTE_DEBUG_COMMANDS_FINALIZE

#if defined (VERTEX_DEBUG_LINE_GPU) || defined (VERTEX_DEBUG_LINE_CPU) || defined (VERTEX_DEBUG_LINE_2D_GPU) || defined (VERTEX_DEBUG_LINE_2D_CPU)
// Common vertex code.
// Could be written in a separate file, but chosing reading locality

// Quad made of two triangles. z selects endpoint: 0 -> A, 1 -> B.
// x,y select side extrusion direction.
const vec3 k_segment_quad[6] = {
    vec3(-0.5, -0.5, 0.0),
    vec3( 0.5, -0.5, 1.0),
    vec3( 0.5,  0.5, 1.0),
    vec3(-0.5, -0.5, 0.0),
    vec3( 0.5,  0.5, 1.0),
    vec3(-0.5,  0.5, 0.0)
};

const vec2 k_quad_uv[6] = {
    vec2(0, 0),
    vec2(1, 0),
    vec2(1, 1),
    vec2(0, 0),
    vec2(1, 1),
    vec2(0, 1)
};

vec2 clip_to_ndc(vec4 clip_pos) {
    // Assume clip_pos.w != 0
    return clip_pos.xy / clip_pos.w;
}

vec2 pixel_to_ndc(vec2 pixels, vec2 resolution) {
    // NDC spans [-1, 1], so 1 pixel = 2 / resolution in NDC.
    return pixels * (2.0 / resolution);
}

vec2 pixel_to_ndc_point(vec2 p, vec2 resolution) {
    // origin top-left -> NDC: x [0..w] -> [-1..1], y [0..h] -> [1..-1]
    vec2 ndc = (p / resolution) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    return ndc;
}

// Based on "Antialised volumetric lines using shader based extrusion" by Sebastien Hillaire, OpenGL Insights Chapter 11.
vec4 extrude_line_clip_space(vec4 clip0, vec4 clip1, vec3 quad_vertex, float half_width_px, vec2 resolution) {
    // Compute direction in NDC
    vec2 ndc0 = clip_to_ndc(clip0);
    vec2 ndc1 = clip_to_ndc(clip1);

    vec2 dir = ndc1 - ndc0;
    float len2 = dot(dir, dir);

    // Handle degenerate line (avoid NaNs)
    if (len2 < 1e-12) {
        // Still output something stable: just return endpoint with no extrusion.
        return mix(clip0, clip1, quad_vertex.z);
    }

    dir *= inversesqrt(len2); // normalize

    // Perpendicular in NDC
    vec2 n = vec2(-dir.y, dir.x);

    // Convert pixel width -> NDC offset
    vec2 half_width_ndc = pixel_to_ndc(vec2(half_width_px), resolution);

    // Offset in NDC. quad_vertex.y in [-0.5, 0.5] so multiply by 2 to get [-1, 1].
    float side = quad_vertex.y * 2.0;
    vec2 ndc_offset = n * half_width_ndc * side;

    // Interpolate endpoint in clip space, then apply offset in clip space:
    // clip.xy = (ndc + offset) * clip.w
    vec4 clip = mix(clip0, clip1, quad_vertex.z);
    clip.xy += ndc_offset * clip.w;

    return clip;
}

#endif // VERTEX_DEBUG_*

#if defined (VERTEX_DEBUG_LINE_GPU)

#include "frame.h"
#include "debug_rendering.h"

layout (location = 0) out vec4 Frag_Color;
layout(location = 1) out float v_edge;

void main()
{
    vec3 position = k_segment_quad[gl_VertexIndex % 6];

    uint line_id = gl_InstanceIndex;

    uint v0 = line_id * 2u;
    uint v1 = v0 + 1u;

    DebugLineVertex a = debug_lines_sb.vertices[v0];
    DebugLineVertex b = debug_lines_sb.vertices[v1];

    vec4 clip0 = frame.view_projection * vec4(a.position, 1.0);
    vec4 clip1 = frame.view_projection * vec4(b.position, 1.0);

    float half_width_px = k_line_width_px;
    gl_Position = extrude_line_clip_space(clip0, clip1, position, half_width_px, frame.resolution * 1.f);

    Frag_Color = mix(unpack_color_rgba(a.color), unpack_color_rgba(b.color), position.z);

    v_edge = position.y;
}

#endif // VERTEX_DEBUG_LINE_GPU


#if defined (VERTEX_DEBUG_LINE_CPU)

#include "frame.h"
#include "debug_rendering.h"

layout (location = 0) in vec3 point_a;
layout (location = 1) in uvec4 color_a;
layout (location = 2) in vec3 point_b;
layout (location = 3) in uvec4 color_b;

layout (location = 0) out vec4 Frag_Color;
layout(location = 1) out float v_edge;

void main()
{
    vec3 position = k_segment_quad[gl_VertexIndex % 6];

    vec4 clip0 = frame.view_projection * vec4(point_a, 1.0);
    vec4 clip1 = frame.view_projection * vec4(point_b, 1.0);

    float half_width_px = k_line_width_px;
    gl_Position = extrude_line_clip_space(clip0, clip1, position, half_width_px, frame.resolution * 1.0f);

    Frag_Color = mix(color_a, color_b, position.z) / 255.f;

    v_edge = position.y;
}

#endif // VERTEX_DEBUG_LINE_CPU


#if defined (VERTEX_DEBUG_LINE_2D_GPU)

#include "frame.h"
#include "debug_rendering.h"

layout (location = 0) out vec4 Frag_Color;
layout(location = 1) out float v_edge;

void main()
{
    vec3 position = k_segment_quad[gl_VertexIndex % 6];

    uint line_id = gl_InstanceIndex;

    uint v0 = line_id * 2u;
    uint v1 = v0 + 1u;

    DebugLineVertex a = debug_lines_sb.vertices[v0];
    DebugLineVertex b = debug_lines_sb.vertices[v1];

    vec2 a_ndc = pixel_to_ndc_point(a.position.xy, frame.resolution * 1.f);
    vec2 b_ndc = pixel_to_ndc_point(b.position.xy, frame.resolution * 1.f);

    vec4 clip0 = vec4(a_ndc, 0.0, 1.0);
    vec4 clip1 = vec4(b_ndc, 0.0, 1.0);

    float half_width_px = k_line_width_px;
    gl_Position = extrude_line_clip_space(clip0, clip1, position, half_width_px, frame.resolution * 1.f);

    Frag_Color = mix(unpack_color_rgba(a.color), unpack_color_rgba(b.color), position.z);

    v_edge = position.y;
}

#endif // VERTEX_DEBUG_LINE_GPU

#if defined (VERTEX_DEBUG_LINE_2D_CPU)

#include "frame.h"
#include "debug_rendering.h"

layout (location = 0) in vec2 point_a;
layout (location = 1) in uvec4 color_a;
layout (location = 2) in vec2 point_b;
layout (location = 3) in uvec4 color_b;

layout (location = 0) out vec4 Frag_Color;
layout(location = 1) out float v_edge;

void main()
{
    vec2 position = k_segment_quad[gl_VertexIndex % 6];

    vec2 a_ndc = pixel_to_ndc_point(point_a, frame.resolution);
    vec2 b_ndc = pixel_to_ndc_point(point_b, frame.resolution);

    vec4 clip0 = vec4(a_ndc, 0.0, 1.0);
    vec4 clip1 = vec4(b_ndc, 0.0, 1.0);

    float half_width_px = k_line_width_px;
    gl_Position = extrude_line_clip_space(clip0, clip1, position, half_width_px, frame.resolution * 1.f);

    // Colors are stored in a Uint decompressed to 4 floats, but they range from 0 to 255.
    Frag_Color = mix(color_a, color_b, position.x) / 255.f;

    v_edge = position.y;
}

#endif // VERTEX_DEBUG_LINE_CPU

#if defined (FRAGMENT_DEBUG_LINE_GPU) || defined (FRAGMENT_DEBUG_LINE_2D_GPU) || defined (FRAGMENT_DEBUG_LINE_CPU) || defined (FRAGMENT_DEBUG_LINE_2D_CPU)

layout (location = 0) in vec4 Frag_Color;
layout(location = 1) in float v_edge;

layout (location = 0) out vec4 Out_Color;

void main()
{
    vec4 col = Frag_Color;
    float dist = abs(v_edge);
    float aa = fwidth(dist); 

    float alpha = 1.0 - smoothstep(0.5 - aa, 0.5 + aa, dist);

    Out_Color = vec4(Frag_Color.rgb * alpha, Frag_Color.a * alpha);
}

#endif // FRAGMENT_DEBUG_LINE_GPU

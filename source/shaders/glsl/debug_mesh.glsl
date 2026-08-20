#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "meshlet.h"

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

#if defined(VERTEX_DEBUG_MESH)

layout ( set = MATERIAL_SET, binding = 10 ) readonly buffer SphereTransforms {
    mat4 transforms[];
};

layout(location=0) in vec3 position;

layout(location=0) flat out uint draw_id;

void main() {
    draw_id = gl_DrawIDARB;
    gl_Position = frame.view_projection * vec4( transforms[gl_DrawIDARB] * vec4( position, 1.0 ) );
}

#endif // VERTEX


#if defined (FRAGMENT_DEBUG_MESH)

layout (location = 0) flat in uint draw_id;

layout (location = 0) out vec4 colour;

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

void main() {

    uint mhash = hash(draw_id);
    colour = vec4( vec3(float(mhash & 255), float((mhash >> 8) & 255), float((mhash >> 16) & 255)) / 255.0, 0.6 );
}

#endif // FRAGMENT

#if defined(COMPUTE_DEBUG_UPDATE_SPHERE_MATRICES)

layout(set = MATERIAL_SET, binding = 1) buffer Meshlets
{
    Meshlet meshlets[];
};

layout ( set = MATERIAL_SET, binding = 10 ) buffer SphereTransforms {
    mat4 transforms[];
};

layout( push_constant ) uniform PushConstants
{
    uint mesh_index;
};

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

    mat4 transform = mat4( 1.0f );

    transform[3][0] = meshlet.center.x;
    transform[3][1] = meshlet.center.y;
    transform[3][2] = meshlet.center.z;

    float s = meshlet.radius * 0.5;
    transform[0][0] = s;
    transform[1][1] = s;
    transform[2][2] = s;

    transforms[ global_meshlet_index ] = transform;
}

#endif // COMPUTE_DEBUG_UPDATE_SPHERE_MATRICES

#if defined(COMPUTE_DEBUG_UPDATE_CONE_MATRICES)

layout(set = MATERIAL_SET, binding = 1) buffer Meshlets
{
    Meshlet meshlets[];
};

layout ( set = MATERIAL_SET, binding = 10 ) buffer ConeTransforms {
    mat4 transforms[];
};

layout( push_constant ) uniform PushConstants
{
    uint mesh_index;
};

// Adapted from Tom Duff, James Burgess, Per Christensen, Christophe Hery, Andrew Kensler, Max Liani, and Ryusuke Villemin,
// Building an Orthonormal Basis, Revisited, Journal of Computer Graphics Techniques (JCGT), vol. 6, no. 1, 1-8, 2017
// Modified to use Y-axis instead of Z-axis
void branchlessONB( in vec3 n, out vec3 b1, out vec3 b2 )
{
    float s = sign( n.y );
    float a = -1.0 / (s + n.y );
    float b = n.x * n.z * a;
    b1 = vec3( 1.0 + s * n.x * n.x * a, -s * n.x, s * b );
    b2 = vec3( b, -n.z, s + n.z * n.z * a );
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

    float i8_inverse = 1.0 / 127.0;
    vec3 cone = vec3( meshlet.cone_axis[ 0 ] * i8_inverse,
                      meshlet.cone_axis[ 1 ] * i8_inverse,
                      meshlet.cone_axis[ 2 ] * i8_inverse );

    vec3 forward = normalize(cone);

#if 0
    // Choose a reference vector that's not parallel to forward
    bool parallel = abs( forward.y ) >= 0.999;
    vec3 reference = parallel ? vec3( 1, 0, 0 ) : vec3( 0, 1, 0 );

    // Build orthonormal basis
    vec3 right = normalize( cross( reference, forward ) );
    vec3 up = cross( forward, right );
#else
    vec3 right;
    vec3 up;
    branchlessONB( forward, right, up );
#endif

    // Construct rotation matrix (assuming cone points along Y-axis in local space)
    // If your cone model points along Y-axis:
    mat4 rotation = mat4(
        vec4( right, 0.0) ,
        vec4( forward, 0.0) ,
        vec4( up, 0.0) ,
        vec4( 0.0, 0.0, 0.0, 1.0 )
    );

    mat4 translate = mat4( 1.0f );
    translate[3][0] = meshlet.center.x;
    translate[3][1] = meshlet.center.y;
    translate[3][2] = meshlet.center.z;

    // Extract cone cutoff and convert from int8
    float cone_cutoff = float( meshlet.cone_cutoff ) * i8_inverse; // This is cos(half_angle)

    // Compute tan(half_angle) from cos(half_angle)
    // tan(x) = sqrt(1 - cos^2(x)) / cos(x)
    float cos_squared = cone_cutoff * cone_cutoff;
    float sin_half_angle = sqrt(max(0.0, 1.0 - cos_squared));
    float tan_half_angle = sin_half_angle / max(0.001, abs(cone_cutoff)); // Avoid division by zero

    // Use meshlet radius as cone height
    float cone_height = meshlet.radius * 0.5;
    float cone_base_radius = cone_height * tan_half_angle;

    // Build the transform matrix
    // Assuming your cone mesh points along the Y-axis with base at origin
    mat4 scale = mat4(
        vec4(cone_base_radius, 0, 0, 0),  // X: base radius
        vec4(0, cone_height, 0, 0),        // Y: height
        vec4(0, 0, cone_base_radius, 0),  // Z: base radius
        vec4(0, 0, 0, 1)
    );

    transforms[ global_meshlet_index ] = translate * rotation * scale;
}

#endif // COMPUTE_DEBUG_UPDATE_CONE_MATRICES

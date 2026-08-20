
#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#include "frame.h"
#include "mesh.h"
#include "meshlet.h"

#if defined(VERTEX) || defined(VERTEX_TRANSPARENT_SKINNING_NO_CULL) || defined(VERTEX_GBUFFER_SKINNING)

layout(location=0) in vec3 position;
layout(location=1) in vec4 tangent;
layout(location=2) in vec3 normal;
layout(location=3) in vec2 texCoord0;
layout (location = 4) in uvec4 jointIndices;
layout (location = 5) in vec4 jointWeights;

layout (location = 0) out vec3 vTexcoord0_W;
layout (location = 1) out vec4 vNormal_BiTanX;
layout (location = 2) out vec4 vTangent_BiTanY;
layout (location = 3) out vec4 vPosition_BiTanZ;
layout (location = 4) out flat uint mesh_draw_index;
#if DEBUG
layout (location = 5) out vec4 vColour;
layout (location = 6) out flat uint triangle_index;
#endif // DEBUG

layout(std430, set = MATERIAL_SET, binding = 3) readonly buffer JointMatrices {
	mat4 joint_matrices[];
};

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

	MeshInstanceDraw mesh_draw = mesh_instance_draws[gl_InstanceIndex];
    MeshDraw mesh = mesh_draws[ mesh_draw.mesh_draw_index ];
    mesh_draw_index = mesh_draw.mesh_draw_index;

	mat4 skinning_transform =
		jointWeights.x * joint_matrices[(jointIndices.x)] +
		jointWeights.y * joint_matrices[(jointIndices.y)] +
		jointWeights.z * joint_matrices[(jointIndices.z)] +
		jointWeights.w * joint_matrices[(jointIndices.w)];

	// Better to separate multiplications to minimize precision issues, visible as Z-Fighting.
    vec4 worldPosition = mesh_draw.model * skinning_transform * vec4( position, 1.0 );

    gl_Position = frame.view_projection * worldPosition;

    bool has_tangents = (mesh.flags & DrawFlags_HasTangents) != 0;

    vPosition_BiTanZ.xyz = worldPosition.xyz / worldPosition.w;
    vTexcoord0_W = vec3( texCoord0.x, texCoord0.y, worldPosition.w );

    // TODO(marco): we should use the inverse transpose of the model matrix
    vec3 normal_ws = normalize( mat3( mesh_draw.model ) * normal );
    vNormal_BiTanX.xyz = normal_ws;

    if ( has_tangents ) {
        vec3 tangent_ws = normalize( mat3( mesh_draw.model ) * tangent.xyz );
        vec3 biTangent_ws = cross( normal_ws, tangent_ws ) * tangent.w;

        vTangent_BiTanY.xyz = tangent_ws;
        vNormal_BiTanX.w   = biTangent_ws.x;
        vTangent_BiTanY.w  = biTangent_ws.y;
        vPosition_BiTanZ.w = biTangent_ws.z;
    }

#if DEBUG
    vec3 mcolor = hash_color(gl_InstanceIndex);
    vColour = vec4(mcolor, 1.0);
    // NOTE(marco): we can't really get the triangle id in a vertex shader, but this variable is not used for now
    triangle_index = gl_VertexIndex;
#endif // DEBUG
}

#endif // VERTEX

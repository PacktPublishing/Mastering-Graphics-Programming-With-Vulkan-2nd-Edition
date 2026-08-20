#version 450

uint DrawFlags_HasTangents = 1 << 6;

struct DrawData {
    mat4        model;
    mat4        model_inverse;

    // x = diffuse index, y = roughness index, z = normal index, w = occlusion index.
    uvec4       textures;
    vec4        base_color_factor;
    vec4        metallic_roughness_occlusion_factor;
    float       alpha_cutoff;
    uint        flags;
    uint        padding_0;
    uint        padding_1;
};

layout ( std430, set = 1, binding = 0 ) readonly buffer SceneBuffer {
    mat4        view_projection;
    vec4        eye;
    vec4        light;
    float       light_range;
    float       light_intensity;
    uint        padding_0;
    uint        padding_1;
    DrawData    draws[];
} scene;

layout(location=0) in vec3 position;
layout(location=1) in vec4 tangent;
layout(location=2) in vec3 normal;
layout(location=3) in vec2 texCoord0;

layout (location = 0) out vec2 vTexcoord0;
layout (location = 1) out vec3 vNormal;
layout (location = 2) out vec3 vTangent;
layout (location = 3) out vec3 vBiTangent;
layout (location = 4) out vec3 vPosition;
layout (location = 5) flat out uint vDrawIndex;

void main() {
    uint draw_index = uint( gl_InstanceIndex );
    DrawData draw_data = scene.draws[ draw_index ];

    vec4 worldPosition = draw_data.model * vec4(position, 1.0);
    gl_Position = scene.view_projection * worldPosition;
    vPosition = worldPosition.xyz / worldPosition.w;
    vTexcoord0 = texCoord0;
    vNormal = normalize( mat3( draw_data.model_inverse ) * normal );

    if ( ( draw_data.flags & DrawFlags_HasTangents ) != 0 ) {
        vTangent = normalize( mat3( draw_data.model ) * tangent.xyz );
        vBiTangent = cross( vNormal, vTangent ) * tangent.w;
    }

    vDrawIndex = draw_index;
}

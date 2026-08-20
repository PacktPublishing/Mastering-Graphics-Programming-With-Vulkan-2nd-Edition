
#ifndef RAPTOR_CH4_SCENE_MESH_DATA_H
#define RAPTOR_CH4_SCENE_MESH_DATA_H

layout ( std140, set = MATERIAL_SET, binding = 0 ) uniform LocalConstants {
    mat4        view_projection;
    vec4        eye;
    vec4        light;
    float       light_range;
    float       light_intensity;
};

struct MeshDraw {

    mat4        model;
    mat4        model_inverse;

    // x = diffuse index, y = roughness index, z = normal index, w = occlusion index.
    // Occlusion and roughness are encoded in the same texture
    uvec4       textures;
    vec4        base_color_factor;
    vec4        metallic_roughness_occlusion_factor;
    float       alpha_cutoff;
    uint        flags;
};

layout ( std430, set = MATERIAL_SET, binding = 1 ) readonly buffer MeshData {

    MeshDraw    mesh_draws[];
};

#endif // RAPTOR_CH4_SCENE_MESH_DATA_H
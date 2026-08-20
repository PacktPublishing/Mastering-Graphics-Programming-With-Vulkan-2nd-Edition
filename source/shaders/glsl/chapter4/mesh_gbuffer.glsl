
#if defined(VERTEX)

layout(location=0) in vec3 position;
layout(location=1) in vec4 tangent;
layout(location=2) in vec3 normal;
layout(location=3) in vec2 texCoord0;

layout (location = 0) out vec2 vTexcoord0;
layout (location = 1) out vec3 vNormal;
layout (location = 2) out vec3 vTangent;
layout (location = 3) out vec3 vBiTangent;
layout (location = 4) out vec3 vPosition;
layout (location = 5) flat out uint out_instance_id;

void main() {
    MeshDraw mesh_draw = mesh_draws[gl_InstanceIndex];
    mat4 model = mesh_draw.model;
    mat4 model_inverse = mesh_draw.model_inverse;
    uint flags = mesh_draw.flags;

    gl_Position = view_projection * model * vec4(position, 1.0);

    vec4 worldPosition = model * vec4(position, 1.0);
    vPosition = worldPosition.xyz / worldPosition.w;

    vTexcoord0 = texCoord0;
    vNormal = normalize( mat3(model_inverse) * normal );

    if ( ( flags & DrawFlags_HasTangents ) != 0 ) {
        vTangent = normalize( mat3(model) * tangent.xyz );
        vBiTangent = cross( vNormal, vTangent ) * tangent.w;
    }

    out_instance_id = gl_InstanceIndex;
}

#endif // VERTEX

#if defined (FRAGMENT)

layout (location = 0) in vec2 vTexcoord0;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec3 vTangent;
layout (location = 3) in vec3 vBiTangent;
layout (location = 4) in vec3 vPosition;
layout (location = 5) flat in uint instance_id;

layout (location = 0) out vec4 color_out;
layout (location = 1) out vec4 normal_out;
layout (location = 2) out vec4 metallic_roughness_occlusion_out;
layout (location = 3) out vec4 position_out;

void main() {
    MeshDraw mesh_draw = mesh_draws[instance_id];
    uvec4 textures = mesh_draw.textures;
    vec4 base_color_factor = mesh_draw.base_color_factor;
    vec4 metallic_roughness_occlusion_factor = mesh_draw.metallic_roughness_occlusion_factor;
    float alpha_cutoff = mesh_draw.alpha_cutoff;
    uint flags = mesh_draw.flags;
    mat4 model = mesh_draw.model;

    vec4 base_colour = base_color_factor;
    if (textures.x != INVALID_TEXTURE_INDEX) {
        base_colour *= texture(global_textures[nonuniformEXT(textures.x)], vTexcoord0);
    }

    bool useAlphaMask = (flags & DrawFlags_AlphaMask) != 0;
    if (useAlphaMask && base_colour.a < alpha_cutoff) {
        base_colour.a = 0.0;
    }

    vec3 normal = normalize( vNormal );
    vec3 tangent = normalize( vTangent );
    vec3 bitangent = normalize( vBiTangent );

    if ( ( flags & DrawFlags_HasTangents ) == 0 ) {
        // NOTE(marco): taken from https://community.khronos.org/t/computing-the-tangent-space-in-the-fragment-shader/52861
        vec3 Q1 = dFdx( vPosition.xyz );
        vec3 Q2 = dFdy( vPosition.xyz );
        vec2 st1 = dFdx( vTexcoord0 );
        vec2 st2 = dFdy( vTexcoord0 );

        tangent = normalize(  Q1 * st2.t - Q2 * st1.t );
        tangent = normalize( mat3( model ) * tangent );
        bitangent = normalize( -Q1 * st2.s + Q2 * st1.s );
    }

    if (gl_FrontFacing == false)
    {
        tangent *= -1.0;
        bitangent *= -1.0;
        normal *= -1.0;
    }

    if (textures.z != INVALID_TEXTURE_INDEX) {
        // NOTE(marco): normal textures are encoded to [0, 1] but need to be mapped to [-1, 1] value
        vec3 bump_normal = normalize( texture(global_textures[nonuniformEXT(textures.z)], vTexcoord0).rgb * 2.0 - 1.0 );
        mat3 TBN = mat3(
            tangent,
            bitangent,
            normal
        );

        normal = normalize(TBN * normalize(bump_normal));
    }

    normal_out.rgb = normal;

    float roughness = metallic_roughness_occlusion_factor.x;
    float metalness = metallic_roughness_occlusion_factor.y;

    if (textures.y != INVALID_TEXTURE_INDEX) {
        vec4 rm = texture(global_textures[nonuniformEXT(textures.y)], vTexcoord0);

        // Green channel contains roughness values
        roughness *= rm.g;

        // Blue channel contains metalness
        metalness *= rm.b;
    }


    float occlusion = metallic_roughness_occlusion_factor.z;
    if (textures.w != INVALID_TEXTURE_INDEX) {
        vec4 o = texture(global_textures[nonuniformEXT(textures.w)], vTexcoord0);
        // Red channel for occlusion value
        occlusion *= o.r;
    }

    metallic_roughness_occlusion_out.rgb = vec3( occlusion, roughness, metalness );

    color_out.rgb = decode_srgb( base_colour.rgb );
    color_out.a = base_colour.a;

    position_out = vec4( vPosition, 0.0 );
}

#endif // FRAGMENT

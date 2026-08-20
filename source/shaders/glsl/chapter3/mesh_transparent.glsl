
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

    vec4 worldPosition = model * vec4(position, 1.0);
    gl_Position = view_projection * worldPosition;

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

layout (location = 0) out vec4 frag_color;

void main() {
    MeshDraw mesh_draw = mesh_draws[instance_id];
    uvec4 textures = mesh_draw.textures;
    vec4 base_color_factor = mesh_draw.base_color_factor;
    vec4 metallic_roughness_occlusion_factor = mesh_draw.metallic_roughness_occlusion_factor;
    float alpha_cutoff = mesh_draw.alpha_cutoff;
    uint flags = mesh_draw.flags;
    mat4 model = mesh_draw.model;

    vec4 base_colour = texture(global_textures[nonuniformEXT(textures.x)], vTexcoord0) * base_color_factor;

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

    vec3 V = normalize( eye.xyz - vPosition );
    vec3 L = normalize( light.xyz - vPosition );
    vec3 N = normal;
    vec3 H = normalize( L + V );

    float roughness = metallic_roughness_occlusion_factor.x;
    float metalness = metallic_roughness_occlusion_factor.y;

    if (textures.y != INVALID_TEXTURE_INDEX) {
        vec4 rm = texture(global_textures[nonuniformEXT(textures.y)], vTexcoord0);

        // Green channel contains roughness values
        roughness *= rm.g;

        // Blue channel contains metalness
        metalness *= rm.b;
    }

    float alpha = pow(roughness, 2.0);

    float occlusion = metallic_roughness_occlusion_factor.z;
    if (textures.w != INVALID_TEXTURE_INDEX) {
        vec4 o = texture(global_textures[nonuniformEXT(textures.w)], vTexcoord0);
        // Red channel for occlusion value
        occlusion *= o.r;
    }

    base_colour.rgb = decode_srgb( base_colour.rgb );

    // https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#specular-brdf
    float alpha_squared = alpha * alpha;

    float NdotL = dot(N, L);
    float HdotV = clamp( dot(H, L), 0, 1 );

    float distance = length(light.xyz - vPosition);
    float intensity = light_intensity * max(min(1.0 - pow(distance / light_range, 4.0), 1.0), 0.0) / pow(distance, 2.0);

    vec3 material_colour = vec3(0, 0, 0);
    if (NdotL > 0.0)
    {
        vec3 specular = intensity * NdotL * specular_brdf( alpha_squared, N, L, H, V );
        float coeff = pow( 1 - abs( HdotV ), 5 );
        vec3 metal_brdf =  specular * ( base_colour.rgb + ( 1 - base_colour.rgb ) * coeff );

        // assumes IOR = 1.5
        float fresnel_mix = 0.04 + ( 1 - 0.04 ) * coeff;
        vec3 diffuse = intensity * NdotL * diffuse_brdf( base_colour.rgb );
        vec3 dielectric_brdf = mix( diffuse, specular, fresnel_mix );

        material_colour = mix( dielectric_brdf, metal_brdf, metalness );
    }

    frag_color = vec4( encode_srgb( material_colour ), base_colour.a );
}

#endif // FRAGMENT

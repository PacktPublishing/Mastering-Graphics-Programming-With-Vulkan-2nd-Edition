#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "lighting.h"


#if defined(VERTEX_DEFERRED_LIGHTING_PIXEL)

layout(location=0) in vec3 position;

layout (location = 0) out vec2 vTexcoord0;

void main() {

    vTexcoord0.xy = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vTexcoord0.xy * 2.0f - 1.0f, 0.0f, 1.0f);
    gl_Position.y = -gl_Position.y;
}

#endif // VERTEX_DEFERRED_LIGHTING_PIXEL

#if defined (FRAGMENT_DEFERRED_LIGHTING_PIXEL)

#extension GL_EXT_fragment_shading_rate : enable

layout (location = 0) in vec2 vTexcoord0;

layout (location = 0) out vec4 frag_color;

// TODO(marco): refactor
layout ( std140, set = MATERIAL_SET, binding = 1 ) uniform LightingConstants {

    // x = albedo index, y = roughness index, z = normal index, w = position index.
    // Occlusion and roughness are encoded in the same texture
    uvec4       textures;

    uint        output_index;   // Used by compute
    uint        output_width;
    uint        output_height;
    uint        emissive_index;
};

void main() {
    vec4 base_colour = texture(global_textures[nonuniformEXT(textures.x)], vTexcoord0);
    vec3 orm = texture(global_textures[nonuniformEXT(textures.y)], vTexcoord0).rgb;
    vec2 encoded_normal = texture(global_textures[nonuniformEXT(textures.z)], vTexcoord0).rg;
    vec3 normal = octahedral_decode(encoded_normal);
    vec3 emissive = texture(global_textures[nonuniformEXT(emissive_index)], vTexcoord0).rgb;

    vec4 color = vec4(0);

    const vec2 screen_uv = uv_nearest(ivec2( gl_FragCoord.xy ), frame.resolution);
    const float raw_depth = texelFetch(global_textures[nonuniformEXT(textures.w)], ivec2( gl_FragCoord.xy ), 0).r;
    if ( raw_depth == 1.0f ) {
        color = vec4( base_colour.rgb, 1 );
    }
    else {
        const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

        uvec2 position = uvec2(gl_FragCoord.x - 0.5, gl_FragCoord.y - 0.5);
        position.y = uint( frame.resolution.y ) - position.y;

        color = calculate_lighting( base_colour, orm, normal, emissive, pixel_world_position, position, screen_uv, false );
    }

    color.rgb = apply_volumetric_fog( screen_uv, raw_depth, color.rgb );
    color.rgb = encode_srgb( color.rgb );

#if 0
    vec3 sr_color = vec3( 0.5, 0, 0 );

    bool has_2vert_rate = ( gl_ShadingRateEXT & gl_ShadingRateFlag2VerticalPixelsEXT ) != 0;
    bool has_2hor_rate = ( gl_ShadingRateEXT & gl_ShadingRateFlag2HorizontalPixelsEXT ) != 0;

    if ( has_2vert_rate && has_2hor_rate )
        sr_color = vec3( 0, 0.5, 0 );

    if ( has_2hor_rate )
        sr_color = vec3( 0, 0, 0.5 );

    if ( has_2vert_rate )
        sr_color = vec3( 0.5, 0.5, 0 );

    color.rgb += ( sr_color * 0.2 );
#endif
    frag_color = color;
}

#endif // FRAGMENT_DEFERRED_LIGHTING_PIXEL

#if defined(COMPUTE_DEFERRED_LIGHTING_COMPUTE)

// TODO(marco): refactor
layout ( std140, set = MATERIAL_SET, binding = 1 ) uniform LightingConstants {

    // x = albedo index, y = roughness index, z = normal index, w = position index.
    // Occlusion and roughness are encoded in the same texture
    uvec4       textures;

    uint        output_index;   // Used by compute
    uint        output_width;
    uint        output_height;
    uint        emissive_index;

    uint        gi_index;
    uint        reflection_index;
    uint        pad000;
    uint        pad001;
};

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    vec4 base_colour = texelFetch(global_textures[nonuniformEXT(textures.x)], pos.xy, 0);
    vec3 orm = texelFetch(global_textures[nonuniformEXT(textures.y)], pos.xy, 0).rgb;
    vec2 encoded_normal = texelFetch(global_textures[nonuniformEXT(textures.z)], pos.xy, 0).rg;
    vec3 normal = octahedral_decode(encoded_normal);
    vec3 emissive = texelFetch(global_textures[nonuniformEXT(emissive_index)], pos.xy, 0).rgb;

    vec3 gi = gi_index != INVALID_TEXTURE_INDEX ? texelFetch(global_textures[gi_index], ivec2(pos.xy) / 2, 0).rgb : vec3(1,1,1);

    vec4 color = vec4(0);

    const vec2 screen_uv = uv_nearest(pos.xy, frame.resolution);
    const float raw_depth = texelFetch(global_textures[nonuniformEXT(textures.w)], pos.xy, 0).r;
    if ( raw_depth == 1.0f ) {
        color = vec4(base_colour.rgb, 1);
    }
    else {
        const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

        uvec2 position = gl_GlobalInvocationID.xy;
        color = calculate_lighting( base_colour, orm, normal, emissive, pixel_world_position, position, screen_uv, false );
    }

    color.rgb *= gi;
    color.rgb = apply_volumetric_fog( screen_uv, raw_depth, color.rgb );
    color.rgb = encode_srgb( color.rgb );

    // DEBUG:
    if ( debug_modes > 0 ) {

        if ( debug_modes == 1 ) {
            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(pixel_world_position, 1));
        }
        else if (debug_modes == 2) {

            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            vec3 position_to_light = light.world_position - pixel_world_position;
            const float current_depth = length(position_to_light) / light.radius;
            const float closest_depth = texture(global_textures_cubemaps[nonuniformEXT(cubemap_shadows_index)], vec3(position_to_light)).r;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(current_depth, closest_depth, vector_to_depth_value(position_to_light, light.radius, light.rcp_n_minus_f), 1));
        }
        else if (debug_modes == 3) {

            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            const vec3 position_to_light = light.world_position - pixel_world_position;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(normalize(position_to_light), 1));
        }
        else if (debug_modes == 4) {

            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            const vec3 position_to_light = pixel_world_position - light.world_position;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(normalize(position_to_light), 1));
        }
        else if (debug_modes == 5) {

            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            vec3 position_to_light = pixel_world_position - light.world_position;
            const float current_depth = length(position_to_light) / light.radius;
            float current_depth2 = vector_to_depth_value(position_to_light, light.radius, light.rcp_n_minus_f);
            const float closest_depth = texture(global_textures_cubemaps[nonuniformEXT(cubemap_shadows_index)], vec3(position_to_light)).r;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(current_depth, current_depth2, closest_depth, 1));

            color.rgb = current_depth2 > closest_depth ? vec3(1) : vec3(0);
        }
        else if (debug_modes == 6) {

            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            vec3 position_to_light = light.world_position - pixel_world_position;
            const float current_depth = length(position_to_light) / light.radius;
            float current_depth2 = vector_to_depth_value(position_to_light, light.radius, light.rcp_n_minus_f);
            const float closest_depth = texture(global_textures_cubemaps[nonuniformEXT(cubemap_shadows_index)], vec3(position_to_light)).r;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(current_depth, current_depth2, closest_depth, 1));
        }
        else if ( debug_modes == 7 ) {
            const vec3 pixel_world_position = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);

            Light light = lights[ 0 ];
            const vec3 position_to_light = pixel_world_position - light.world_position;

            // // Doing the max of the components tells us 2 things: which cubemap face we're going to use,
            // // and also what the projected distance is onto the major axis for that face.
            float projectedDistance = max(max(abs(position_to_light.x), abs(position_to_light.y)), abs(position_to_light.z));

            // fn = 1.0f / (nearZ - farZ);
            // dest[2][2] = (nearZ + farZ) * fn;
            // dest[3][2] = 2.0f * nearZ * farZ * fn;
            float fn = 1.0f / (0.01f - light.radius);
            float a = (0.01f + light.radius) * fn;
            float b = 2.0f * 0.01f * light.radius * fn;

            float z = projectedDistance * a + b;
            float dbDistance = z / projectedDistance;

            const float closest_depth = texture(global_textures_cubemaps[nonuniformEXT(cubemap_shadows_index)], vec3(position_to_light)).r;

            imageStore(global_images_2d[debug_texture_index], pos.xy, vec4(dbDistance, closest_depth, 0, 1));

        }
        else if (debug_modes == 8) {

            vec3 indirect_irradiance = textureLod(global_textures[nonuniformEXT(indirect_lighting_texture_index)], screen_uv, 0).rgb;
            color.rgb = indirect_irradiance;
        }
        else if (debug_modes == 9) {

            vec3 indirect_irradiance = textureLod(global_textures[nonuniformEXT(indirect_lighting_texture_index)], screen_uv, 0).rgb;
            color.rgb = indirect_irradiance * base_colour.rgb;
        }
    }

    imageStore(global_images_2d[output_index], pos.xy, color);
}

#endif // COMPUTE_DEFERRED_LIGHTING_COMPUTE

#if defined(COMPUTE_EDGE_DETECTION)

// TODO(marco): this should changed based on VkPhysicalDeviceFragmentShadingRatePropertiesKHR::minFragmentShadingRateAttachmentTexelSize
#define GROUP_SIZE 16
#define LOCAL_DATA_SIZE ( GROUP_SIZE + 2 )

layout ( local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1 ) in;

layout( set = MATERIAL_SET, binding = 2 ) readonly buffer FragmentShadingRateImage {
    uint color_image_index;
    uint fsr_image_index;
};

shared float local_image_data[ LOCAL_DATA_SIZE ][ LOCAL_DATA_SIZE ];
shared uint min_rate;

void main() {
    ivec2 iresolution = ivec2( frame.resolution ) - 1;
    if ( gl_GlobalInvocationID.x > iresolution.x || gl_GlobalInvocationID.y > iresolution.y )
        return;

    ivec2 local_index = ivec2( gl_LocalInvocationID.xy ) + ivec2( 1, 1 );
    ivec2 global_index = ivec2( gl_GlobalInvocationID.xy );

    local_image_data[ local_index.y ][ local_index.x ] = luminance( texelFetch( global_textures[ color_image_index ], global_index, 0 ).rgb );

    if ( local_index.x == 1 && local_index.y == 1 ) {
        local_image_data[ local_index.y - 1 ][ local_index.x - 1 ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( -1, -1 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    if ( local_index.x == 1 ) {
        local_image_data[ local_index.y ][ local_index.x - 1 ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( -1, 0 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    if ( local_index.y == 1 ) {
        local_image_data[ local_index.y - 1 ][ local_index.x ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( 0, -1 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    if ( local_index.x == GROUP_SIZE && local_index.y == GROUP_SIZE ) {
        local_image_data[ local_index.y + 1 ][ local_index.x + 1 ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( 1, 1 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    if ( local_index.x == GROUP_SIZE ) {
        local_image_data[ local_index.y ][ local_index.x + 1 ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( 1, 0 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    if ( local_index.y == GROUP_SIZE ) {
        local_image_data[ local_index.y + 1 ][ local_index.x ] = luminance( texelFetch( global_textures[ color_image_index ], clamp( global_index + ivec2( 0, 1 ), ivec2( 0 ), iresolution ), 0 ).rgb );
    }

    barrier();

    float normalization = 1.0; // 0.125;

    // Horizontal filter
    float dx =     local_image_data[ local_index.y - 1 ][ local_index.x - 1 ] -
                   local_image_data[ local_index.y - 1 ][ local_index.x + 1 ] +
               2 * local_image_data[ local_index.y     ][ local_index.x - 1 ] -
               2 * local_image_data[ local_index.y     ][ local_index.x + 1 ] +
                   local_image_data[ local_index.y + 1 ][ local_index.x - 1 ] -
                   local_image_data[ local_index.y + 1 ][ local_index.x + 1 ];

    dx *= normalization;

    // Vertical filter
    float dy =     local_image_data[ local_index.y - 1 ][ local_index.x - 1 ] +
               2 * local_image_data[ local_index.y - 1 ][ local_index.x     ] +
                   local_image_data[ local_index.y - 1 ][ local_index.x + 1 ] -
                   local_image_data[ local_index.y + 1 ][ local_index.x - 1 ] -
               2 * local_image_data[ local_index.y + 1 ][ local_index.x     ] -
                   local_image_data[ local_index.y + 1 ][ local_index.x + 1 ];

    dy *= normalization;

    float d = pow( dx, 2 ) + pow( dy, 2 );

    // NOTE(marco): 2x2 rate
    uint rate = 1 << 2 | 1;

    if ( d > 0.1 ) {
        // NOTE(marco): 1x1 rate
        rate = 0;
    }

    // TODO(marco): also use 1x2 and 2x1 rates

    atomicMin( min_rate, rate );

    barrier();

    if ( gl_LocalInvocationID.xy == uvec2( 0, 0 ) ) {
        imageStore( global_uimages_2d[ fsr_image_index ], ivec2( gl_GlobalInvocationID.xy / GROUP_SIZE ), uvec4( rate, 0, 0, 0 ) );
    }
}

#endif // EDGE_DETECTION

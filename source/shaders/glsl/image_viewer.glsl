#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "../shared_structs.h"

// Debug tool: reads any bindless 2D view and converts it to a displayable RGBA8 image.
// Used by ImageViewerRenderingFeature, the result is shown with ImGui::Image.

#if defined(COMPUTE_IMAGE_VIEWER)

layout( push_constant ) uniform ImageViewerPushConstants {
    GpuImageViewerConstants viewer;
};

layout( rgba8, set = GLOBAL_SET, binding = BINDLESS_IMAGES ) uniform writeonly image2D global_images_2d_rgba8[];

vec3 select_channels( vec4 value, uint mode ) {
    if ( mode == k_image_viewer_channel_r )         return value.rrr;
    if ( mode == k_image_viewer_channel_g )         return value.ggg;
    if ( mode == k_image_viewer_channel_b )         return value.bbb;
    if ( mode == k_image_viewer_channel_a )         return value.aaa;
    if ( mode == k_image_viewer_channel_luminance ) return vec3( luminance( value.rgb ) );
    return value.rgb;
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );

    if ( pixel.x >= int( viewer.display_size.x ) || pixel.y >= int( viewer.display_size.y ) ) {
        return;
    }

    // Display pixel -> input texel, nearest. Zoom and pan only change view_center and texels_per_pixel.
    const vec2 texel_position = viewer.view_center + ( vec2( pixel ) + 0.5 - viewer.display_size * 0.5 ) * viewer.texels_per_pixel;
    const ivec2 texel = ivec2( floor( texel_position ) );

    vec3 color;

    if ( any( lessThan( texel, ivec2( 0 ) ) ) || any( greaterThanEqual( texel, ivec2( viewer.input_size ) ) ) ) {
        // Outside the image: checkerboard, so the borders of the image are visible.
        const bool odd = ( ( ( pixel.x >> 3 ) + ( pixel.y >> 3 ) ) & 1 ) != 0;
        color = vec3( odd ? 0.18 : 0.12 );
    } else {

        // Read using the correct sampler        
        vec4 value;
        if ( ( viewer.options & k_image_viewer_input_uint ) != 0 ) {
            value = vec4( texelFetch( global_utextures[ nonuniformEXT( viewer.input_index ) ], texel, int( viewer.mip ) ) );
        } else if ( ( viewer.options & k_image_viewer_input_sint ) != 0 ) {
            value = vec4( texelFetch( global_itextures[ nonuniformEXT( viewer.input_index ) ], texel, int( viewer.mip ) ) );
        } else {
            value = texelFetch( global_textures[ nonuniformEXT( viewer.input_index ) ], texel, int( viewer.mip ) );
        }

        color = select_channels( value, viewer.channel_mode );

        const bool invalid = any( isnan( color ) ) || any( isinf( color ) );

        if ( invalid && ( viewer.options & k_image_viewer_show_nan_inf ) != 0 ) {
            color = vec3( 1, 0, 1 );
        } else {
            color = ( color * viewer.exposure_scale - viewer.range_min ) * viewer.range_rcp_size;

            const bool over  = any( greaterThan( color, vec3( 1 ) ) );
            const bool under = all( lessThan( color, vec3( 0 ) ) );

            if ( ( viewer.options & k_image_viewer_show_out_of_range ) != 0 && ( over || under ) ) {
                color = over ? vec3( 1, 0, 0 ) : vec3( 0, 0, 1 );
            } else {
                color = clamp( color, vec3( 0 ), vec3( 1 ) );

                // The swapchain is UNORM: ImGui writes the texture value as it is.
                if ( ( viewer.options & k_image_viewer_encode_srgb ) != 0 ) {
                    color = encode_srgb( color );
                }
            }
        }
    }

    imageStore( global_images_2d_rgba8[ nonuniformEXT( viewer.output_index ) ], pixel, vec4( color, 1 ) );
}

#endif // COMPUTE_IMAGE_VIEWER

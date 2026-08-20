#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable

layout( set = MATERIAL_SET, binding = 26 ) uniform accelerationStructureEXT as;

#include "frame.h"

#include "lighting.h"


layout ( std140, set = MATERIAL_SET, binding = 30 ) uniform ShadowVisibilityConstants {
    uint previous_visibility_cache_texture_index;
    uint current_visibility_cache_texture_index;
    uint previous_variation_cache_texture_index;
    uint current_variation_cache_texture_index;

    uint previous_samples_count_cache_texture_index;
    uint current_samples_count_cache_texture_index;
    uint variation_texture_index;
    uint motion_vectors_texture_index;

    uint normals_texture_index;
    uint filtered_visibility_texture;
    uint filetered_variation_texture;
    uint frame_index; // NOTE(marco): [0-3]

    float resolution_scale;
    float resolution_scale_rcp;
    uint previous_depth_texture_index;
    uint current_depth_texture_index;

    uint upscaled_visibility_texture;
    uint pad000_svc;
    uint pad001_svc;
    uint pad002_svc;
};


ivec2 shadow_visibility_resolution() {
    return ivec2( resolution * resolution_scale );
}

bool is_inside_uv( vec2 uv ) {
    return all( greaterThanEqual( uv, vec2( 0.0 ) ) ) &&
           all( lessThan( uv, vec2( 1.0 ) ) );
}

ivec2 cache_to_full_resolution_pixel( ivec2 cache_pixel ) {
    const ivec2 cache_resolution = shadow_visibility_resolution();
    const ivec2 full_resolution = ivec2( resolution );
    const vec2 cache_uv = ( vec2( cache_pixel ) + 0.5 ) / vec2( cache_resolution );

    ivec2 full_pixel = ivec2( cache_uv * vec2( full_resolution ) );
    return clamp( full_pixel, ivec2( 0 ), full_resolution - ivec2( 1 ) );
}

#if defined(COMPUTE_SHADOW_CACHE_REPROJECTION)

// Reuse some ideas from TAA
float sample_current_depth_point( ivec2 pos ) {
    return texelFetch(global_textures[ nonuniformEXT( current_depth_texture_index ) ], pos, 0 ).r;
}

float sample_previous_depth_point( ivec2 pos ) {
    return texelFetch(global_textures[ nonuniformEXT( previous_depth_texture_index ) ], pos, 0 ).r;
}

float compute_disocclusion_factor( float expected_previous_depth,
                                   vec2 previous_uv, ivec2 full_resolution ) {
    if ( !is_inside_uv( previous_uv ) ) {
        return 0.0;
    }

    ivec2 previous_pixel = ivec2( previous_uv * vec2( full_resolution ) );

    previous_pixel = clamp( previous_pixel, ivec2( 0 ), full_resolution - ivec2( 1 ) );

    float sampled_previous_depth = sample_previous_depth_point( previous_pixel );
    float depth_delta = abs( expected_previous_depth - sampled_previous_depth );

    return 1.0 - smoothstep( 0.0015, 0.02, depth_delta );
}

bool validate_previous_depth(
    float expected_previous_depth,
    vec2 previous_uv,
    ivec2 full_resolution
) {
    ivec2 previous_pixel = ivec2( previous_uv * vec2( full_resolution ) );

    if ( any( lessThan( previous_pixel, ivec2( 0 ) ) ) ||
         any( greaterThanEqual( previous_pixel, full_resolution ) ) ) {
        return false;
    }

    float sampled_previous_depth =
        texelFetch(
            global_textures[
                nonuniformEXT( previous_depth_texture_index )
            ],
            previous_pixel,
            0
        ).r;

    float depth_delta =
        abs( sampled_previous_depth - expected_previous_depth );

    // Start strict. Relax later if it invalidates too much.
    return depth_delta < 0.0015;
}


layout ( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;

void main() {
    const ivec2 cache_resolution = ivec2( resolution * resolution_scale );
    const ivec2 cache_pixel = ivec2( gl_GlobalInvocationID.xy );

    if ( any( greaterThanEqual( cache_pixel, cache_resolution ) ) ) {
        return;
    }

    const ivec2 full_resolution = ivec2( resolution );
    const ivec2 motion_pixel = cache_to_full_resolution_pixel( cache_pixel );
    const vec2 current_uv = uv_nearest( motion_pixel, resolution );
    const vec2 motion_ndc = texelFetch( global_textures[ motion_vectors_texture_index ], motion_pixel, 0 ).rg;

    bool valid_motion = !all( equal( motion_ndc, vec2( -1.0 ) ) );

    vec2 previous_uv = reproject_uv_from_ndc_motion( current_uv, motion_ndc );

    float current_raw_depth = sample_current_depth_point( motion_pixel );
    vec3 world_position = world_position_from_depth( current_uv, current_raw_depth, inverse_view_projection );

    vec4 previous_position = previous_view_projection * vec4( world_position, 1.0 );
    previous_position.xyz /= previous_position.w;

    float expected_previous_depth = previous_position.z;

    ivec2 previous_cache_pixel =
    ivec2( previous_uv * vec2( cache_resolution ) );

previous_cache_pixel =
    clamp( previous_cache_pixel,
           ivec2( 0 ),
           cache_resolution - ivec2( 1 ) );

ivec2 previous_representative_pixel =
    cache_to_full_resolution_pixel( previous_cache_pixel );

float sampled_previous_depth =
    texelFetch(
        global_textures[
            nonuniformEXT( previous_depth_texture_index )
        ],
        previous_representative_pixel,
        0
    ).r;

float depth_delta =
    abs( sampled_previous_depth - expected_previous_depth );

bool valid_depth = depth_delta < 0.0015;

    float disocclusion_factor = compute_disocclusion_factor( expected_previous_depth, previous_uv, full_resolution );

    const bool inside_screen = all( greaterThanEqual( previous_uv, vec2( 0.0 ) ) ) &&
                               all( lessThan( previous_uv, vec2( 1.0 ) ) );

    const bool valid_history = valid_motion && inside_screen && valid_depth;//disocclusion_factor > 0.9f;
    const ivec3 current_coord = ivec3( cache_pixel, gl_GlobalInvocationID.z );

    vec4 visibility_history = vec4( 0.0 );
    vec4 variation_history = vec4( -1.0 );
    uvec4 sample_count_history = uvec4( MAX_SHADOW_VISIBILITY_SAMPLE_COUNT );

    if ( valid_history ) {
        ivec2 previous_cache_pixel = ivec2( previous_uv * vec2( cache_resolution ) );

        previous_cache_pixel = clamp( previous_cache_pixel, ivec2( 0 ), cache_resolution - ivec2( 1 ) );

        const ivec3 previous_coord = ivec3( previous_cache_pixel, gl_GlobalInvocationID.z );

        visibility_history = texelFetch( global_texture_arrays[ previous_visibility_cache_texture_index ], previous_coord, 0 );
        variation_history = texelFetch( global_texture_arrays[ previous_variation_cache_texture_index ], previous_coord, 0 );
        sample_count_history = texelFetch( global_utexture_arrays[ previous_samples_count_cache_texture_index ], previous_coord, 0 );
    }

    imageStore( global_image_arrays_2d[ current_visibility_cache_texture_index ], current_coord, visibility_history );
    imageStore( global_image_arrays_2d[ current_variation_cache_texture_index ], current_coord, variation_history );
    imageStore( global_uimages_arrays_2d[ current_samples_count_cache_texture_index ], current_coord, sample_count_history );
}

#endif // COMPUTE_SHADOW_CACHE_REPROJECTION

#if defined(COMPUTE_SHADOW_VISIBILITY_VARIANCE)

layout ( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;

void main() {
    ivec2 iresolution = shadow_visibility_resolution();
    if ( gl_GlobalInvocationID.x >= iresolution.x || gl_GlobalInvocationID.y >= iresolution.y )
        return;

    ivec3 tex_coord = ivec3( gl_GlobalInvocationID.xyz );

    vec4 last_visibility_values = texelFetch( global_texture_arrays[ current_visibility_cache_texture_index ], tex_coord, 0 );

    float max_v = max( max( max( last_visibility_values.x, last_visibility_values.y ), last_visibility_values.z ), last_visibility_values.w );
    float min_v = min( min( min( last_visibility_values.x, last_visibility_values.y ), last_visibility_values.z ), last_visibility_values.w );

    float delta = max_v - min_v;

    imageStore( global_image_arrays_2d[ variation_texture_index ], tex_coord, vec4( delta, 0, 0, 0 ) );
}

#endif // COMPUTE_SHADOW_VISIBILITY_VARIANCE

#if defined(COMPUTE_SHADOW_VISIBILITY)

// Light visibility functions

// NOTE(marco): thanks to https://github.com/bartwronski/PoissonSamplingGenerator
#define SAMPLE_NUM 32
vec2 POISSON_SAMPLES[SAMPLE_NUM] =
{
    vec2( 0.39963964752463255f, 0.8910925368990373f ),
    vec2( -0.4940572704167889f, -0.8620650241721987f ),
    vec2( 0.8075570857119035f, -0.5440713505497983f ),
    vec2( -0.9116635046112362f, 0.2639502616182513f ),
    vec2( 0.05343036802745114f, 0.021474316209819044f ),
    vec2( 0.8499579311323042f, 0.27318537130618137f ),
    vec2( -0.3403992818902896f, 0.7063573920801801f ),
    vec2( 0.2101073022086032f, -0.8129909357248446f ),
    vec2( -0.9005900483859263f, -0.391550837884129f ),
    vec2( -0.19587476659917602f, -0.3981303634779107f ),
    vec2( -0.4648065562502342f, 0.02105911800771148f ),
    vec2( 0.35934411533835076f, 0.4121051098766807f ),
    vec2( 0.5065318505687553f, -0.10705978878497402f ),
    vec2( -0.7602340603847367f, 0.6493924352633489f ),
    vec2( -0.019782992429490595f, 0.8925406666774142f ),
    vec2( 0.3983473193951535f, -0.4801357934668924f ),
    vec2( 0.9869656537989692f, -0.09638640479894947f ),
    vec2( -0.25015603010828763f, 0.2972338092340553f ),
    vec2( -0.13317277640560815f, -0.9143508644248124f ),
    vec2( 0.6996155560882538f, 0.6876222716775685f ),
    vec2( -0.6345508708611187f, -0.24002497065722314f ),
    vec2( 0.07481225966233056f, 0.6194024571949546f ),
    vec2( -0.5795518698024703f, 0.35706998381720817f ),
    vec2( 0.10538818335743431f, -0.5072259616736443f ),
    vec2( 0.5901520300517671f, -0.8055715970062381f ),
    vec2( 0.4997349661429248f, 0.18391430091175387f ),
    vec2( -0.8936441537563113f, -0.09018813624787847f ),
    vec2( -0.49099986787705147f, -0.5534594920185129f ),
    vec2( 0.7883035678609505f, -0.2850303445322458f ),
    vec2( 0.20190051133128753f, -0.2287805625191621f ),
    vec2( 0.10095624109822983f, 0.356329397671627f ),
    vec2( 0.5999403247649068f, 0.4733652413019988f ),
};

float get_directional_light_visibility( vec3 light_position, uint sample_count, vec3 world_position, vec3 normal, uint frame_index ) {

    const vec3 l = normalize( light_position );
    const float NoL = dot(normal, l);

    vec3 x_axis =  l.y == 1.0f ? normalize(cross(l, vec3(0.0f, 0.0f, 1.0f))) : normalize(cross(l, vec3(0.0f, 1.0f, 0.0f)));
    vec3 y_axis = normalize(cross(x_axis, l));

    float visiblity = 0.0;

    if ( NoL > 0.001f ) {

        const float k_poisson_size = 0.01;
        for ( uint s = 0; s < sample_count; ++s ) {

#if 1
            vec2 poisson_sample = POISSON_SAMPLES[ (s * FRAME_HISTORY_COUNT + frame_index) % SAMPLE_NUM ];
            vec3 random_x = x_axis * poisson_sample.x * k_poisson_size;
            vec3 random_y = y_axis * poisson_sample.y * k_poisson_size;
            vec3 random_dir = normalize(l + random_x + random_y);
#else
            vec3 random_dir = l;
#endif

            rayQueryEXT rayQuery;
            rayQueryInitializeEXT(rayQuery,
                                  as,
                                  gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                                  0xff,
                                  world_position,
                                  0.01,
                                  random_dir,
                                  100.0f);
            rayQueryProceedEXT( rayQuery );

            if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
                visiblity += 1.0f;
            }

        }
    }

    return visiblity / float( sample_count );
}

float get_point_light_visibility( uint light_index, uint sample_count, vec3 world_position, vec3 normal, uint frame_index ) {
    const vec3 position_to_light = raytraced_shadow_light_position - world_position;
    const vec3 l = normalize( position_to_light );
    const float NoL = dot(normal, l);
    float d = sqrt( dot( position_to_light, position_to_light ) );

    vec3 x_axis = normalize(cross(l, vec3(0.0f, 1.0f, 0.0f)));
    vec3 y_axis = normalize(cross(x_axis, l));

    float visiblity = 0.0;

    const float r = raytraced_shadow_light_radius;
    float attenuation = attenuation_square_falloff(position_to_light, 1.0f / r);

    const float scaled_distance = r / d;
    if ( (NoL > 0.001f) && (d <= r) && (attenuation > 0.001f) ) {
        for ( uint s = 0; s < sample_count; ++s ) {

#if 1
            vec2 poisson_sample = POISSON_SAMPLES[ (s * FRAME_HISTORY_COUNT + frame_index) % SAMPLE_NUM ];
            vec3 random_x = x_axis * poisson_sample.x * (scaled_distance) * 0.01;
            vec3 random_y = y_axis * poisson_sample.y * (scaled_distance) * 0.01;
            vec3 random_dir = normalize(l + random_x + random_y);
#else
            vec3 random_dir = l;
#endif

            rayQueryEXT rayQuery;
            rayQueryInitializeEXT(rayQuery,
                                  as,
                                  gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                                  0xff,
                                  world_position,
                                  0.01,
                                  random_dir,
                                  d);
            rayQueryProceedEXT( rayQuery );

            if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
                visiblity += rayQueryGetIntersectionTEXT(rayQuery, true) < d ? 0.0f : 1.0f;
            }
            else {
                visiblity += 1.0f;
            }
        }
    }

    return visiblity / float( sample_count );
}

#define GROUP_SIZE 8

#define MAX_FILTER_RADIUS 2
#define TENT_FILTER_RADIUS 6
#define TOTAL_FILTER_RADIUS ( MAX_FILTER_RADIUS + TENT_FILTER_RADIUS )

#define LOCAL_DATA_SIZE ( GROUP_SIZE + TOTAL_FILTER_RADIUS * 2 )
#define LOCAL_MAX_DATA_SIZE ( GROUP_SIZE + TENT_FILTER_RADIUS * 2 )

layout ( local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1 ) in;

shared float local_image_data[ LOCAL_DATA_SIZE ][ LOCAL_DATA_SIZE ];
shared float local_max_image_data[ LOCAL_DATA_SIZE ][ LOCAL_DATA_SIZE ];

float read_variation_value( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const bool invalid_index = any( lessThan( index.xy, ivec2( 0 ) ) ) ||
                               any( greaterThanEqual( index.xy, image_resolution ) );

    if ( invalid_index ) {
        return 0.0;
    }

    return texelFetch( global_texture_arrays[ variation_texture_index ], index, 0 ).r;
}

float max_filter( ivec2 shared_index ) {
    float max_value = 0.0;

    for ( int y = -MAX_FILTER_RADIUS; y <= MAX_FILTER_RADIUS; ++y ) {
        for ( int x = -MAX_FILTER_RADIUS; x <= MAX_FILTER_RADIUS; ++x ) {

            const ivec2 sample_index = shared_index + ivec2( x, y );

            max_value = max( max_value, local_image_data[ sample_index.y ][ sample_index.x ] );
        }
    }

    return max_value;
}

float tent_kernel[ 13 ][ 13 ] = {
    { 0.00041649, 0.00083299, 0.00124948, 0.00166597, 0.00208247, 0.00249896, 0.00291545, 0.00249896, 0.00208247, 0.00166597, 0.00124948, 0.00083299, 0.00041649 },
    { 0.00083299, 0.00166597, 0.00249896, 0.00333195, 0.00416493, 0.00499792, 0.00583090, 0.00499792, 0.00416493, 0.00333195, 0.00249896, 0.00166597, 0.00083299 },
    { 0.00124948, 0.00249896, 0.00374844, 0.00499792, 0.00624740, 0.00749688, 0.00874636, 0.00749688, 0.00624740, 0.00499792, 0.00374844, 0.00249896, 0.00124948 },
    { 0.00166597, 0.00333195, 0.00499792, 0.00666389, 0.00832986, 0.00999584, 0.01166181, 0.00999584, 0.00832986, 0.00666389, 0.00499792, 0.00333195, 0.00166597 },
    { 0.00208247, 0.00416493, 0.00624740, 0.00832986, 0.01041233, 0.01249479, 0.01457726, 0.01249479, 0.01041233, 0.00832986, 0.00624740, 0.00416493, 0.00208247 },
    { 0.00249896, 0.00499792, 0.00749688, 0.00999584, 0.01249479, 0.01499375, 0.01749271, 0.01499375, 0.01249479, 0.00999584, 0.00749688, 0.00499792, 0.00249896 },
    { 0.00291545, 0.00583090, 0.00874636, 0.01166181, 0.01457726, 0.01749271, 0.02040816, 0.01749271, 0.01457726, 0.01166181, 0.00874636, 0.00583090, 0.00291545 },
    { 0.00249896, 0.00499792, 0.00749688, 0.00999584, 0.01249479, 0.01499375, 0.01749271, 0.01499375, 0.01249479, 0.00999584, 0.00749688, 0.00499792, 0.00249896 },
    { 0.00208247, 0.00416493, 0.00624740, 0.00832986, 0.01041233, 0.01249479, 0.01457726, 0.01249479, 0.01041233, 0.00832986, 0.00624740, 0.00416493, 0.00208247 },
    { 0.00166597, 0.00333195, 0.00499792, 0.00666389, 0.00832986, 0.00999584, 0.01166181, 0.00999584, 0.00832986, 0.00666389, 0.00499792, 0.00333195, 0.00166597 },
    { 0.00124948, 0.00249896, 0.00374844, 0.00499792, 0.00624740, 0.00749688, 0.00874636, 0.00749688, 0.00624740, 0.00499792, 0.00374844, 0.00249896, 0.00124948 },
    { 0.00083299, 0.00166597, 0.00249896, 0.00333195, 0.00416493, 0.00499792, 0.00583090, 0.00499792, 0.00416493, 0.00333195, 0.00249896, 0.00166597, 0.00083299 },
    { 0.00041649, 0.00083299, 0.00124948, 0.00166597, 0.00208247, 0.00249896, 0.00291545, 0.00249896, 0.00208247, 0.00166597, 0.00124948, 0.00083299, 0.00041649 }
};

void main() {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const ivec2 global_pixel = ivec2( gl_GlobalInvocationID.xy );
    const bool valid_pixel = all( lessThan( global_pixel, image_resolution ) );

    const uint local_thread_index = gl_LocalInvocationID.y * GROUP_SIZE + gl_LocalInvocationID.x;
    const uint local_thread_count = GROUP_SIZE * GROUP_SIZE;

    const ivec2 group_origin = ivec2( gl_WorkGroupID.xy ) * GROUP_SIZE;

    // Load the complete variation tile, including the halo required
    // by both the max filter and the following tent filter.
    for ( uint i = local_thread_index; i < LOCAL_DATA_SIZE * LOCAL_DATA_SIZE; i += local_thread_count ) {

        const ivec2 shared_coord = ivec2( i % LOCAL_DATA_SIZE, i / LOCAL_DATA_SIZE );

        const ivec2 source_pixel = group_origin + shared_coord - ivec2( TOTAL_FILTER_RADIUS );

        local_image_data[ shared_coord.y ][ shared_coord.x ] = read_variation_value( ivec3( source_pixel, gl_GlobalInvocationID.z ) );
    }

    memoryBarrierShared();
    barrier();

    // The tent filter needs max-filtered values in a region with
    // a six-pixel halo. Each of these values reads an additional
    // two-pixel halo from local_image_data.
    for ( uint i = local_thread_index; i < LOCAL_MAX_DATA_SIZE * LOCAL_MAX_DATA_SIZE; i += local_thread_count ) {

        const ivec2 max_region_coord = ivec2( i % LOCAL_MAX_DATA_SIZE, i / LOCAL_MAX_DATA_SIZE );

        // local_image_data has an eight-pixel halo, while the
        // max-filtered region begins two pixels inside that tile.
        const ivec2 shared_coord = max_region_coord + ivec2( MAX_FILTER_RADIUS );

        local_max_image_data[ shared_coord.y ][ shared_coord.x ] = max_filter( shared_coord );
    }

    memoryBarrierShared();
    barrier();

    // Every invocation must reach both barriers. Invalid invocations
    // can return only after the shared-memory work is complete.
    if ( !valid_pixel ) {
        return;
    }

    const ivec3 global_index = ivec3( gl_GlobalInvocationID.xyz );

    const ivec2 local_index = ivec2( gl_LocalInvocationID.xy ) + ivec2( TOTAL_FILTER_RADIUS );

    // 13x13 tent filter over the max-filtered variation.
    float spatial_filtered_value = 0.0;

    for ( int y = -TENT_FILTER_RADIUS; y <= TENT_FILTER_RADIUS; ++y ) {
        for ( int x = -TENT_FILTER_RADIUS; x <= TENT_FILTER_RADIUS; ++x ) {

            const ivec2 sample_index = local_index + ivec2( x, y );

            const float variation = local_max_image_data[ sample_index.y ][ sample_index.x ];

            const float weight = tent_kernel[ y + TENT_FILTER_RADIUS ][ x + TENT_FILTER_RADIUS ];

            spatial_filtered_value += variation * weight;
        }
    }

    vec4 last_variation_values = texelFetch( global_texture_arrays[ current_variation_cache_texture_index ], global_index, 0 );

    // This value still contains the result of the reprojection pass.
    // If reprojection failed, the reprojection pass initialized it to -1.
    const bool valid_history = last_variation_values.x >= 0.0;
    //float filtered_value = 0.5 * ( spatial_filtered_value + 0.25 * ( last_variation_values.x + last_variation_values.y + last_variation_values.z + last_variation_values.w ) );

    const vec4 valid_variation_values = valid_history ? last_variation_values : vec4( 0.0 );

    float filtered_value = 0.5 * ( spatial_filtered_value + 0.25 * ( valid_variation_values.x + valid_variation_values.y + valid_variation_values.z + valid_variation_values.w ) );
    // last_variation_values.w = last_variation_values.z;
    // last_variation_values.z = last_variation_values.y;
    // last_variation_values.y = last_variation_values.x;
    // last_variation_values.x = texelFetch( global_texture_arrays[ variation_texture_index ], global_index, 0 ).r;

    const ivec2 full_resolution_pixel = cache_to_full_resolution_pixel( global_index.xy );

    //const vec2 current_uv = uv_nearest( full_resolution_pixel, resolution );
    //const vec2 motion_ndc = texelFetch( global_textures[ motion_vectors_texture_index ], full_resolution_pixel, 0 ).rg;
    //const vec2 previous_uv = reproject_uv_from_ndc_motion( current_uv, motion_ndc );
    //const bool valid_history = is_inside_uv( previous_uv );


    uvec4 sample_count_history = texelFetch( global_utexture_arrays[ current_samples_count_cache_texture_index ], global_index, 0 );
    const float raw_depth = texelFetch( global_textures[ current_depth_texture_index ], full_resolution_pixel, 0 ).r;

    uint sample_count = MAX_SHADOW_VISIBILITY_SAMPLE_COUNT;
    if ( raw_depth == 1.0f ) {
        sample_count = 0;
    } else if ( valid_history ) {
        sample_count = sample_count_history.x;

        bool stable_sample_count = ( sample_count_history.x == sample_count_history.y ) && ( sample_count_history.x == sample_count_history.z ) && ( sample_count_history.x == sample_count_history.w );

        float delta = 0.2;
            if ( filtered_value > delta && sample_count < MAX_SHADOW_VISIBILITY_SAMPLE_COUNT ) {
                sample_count += 1;
            } else if ( stable_sample_count && sample_count >= 1 && (filtered_value < delta) ) {
                sample_count -= 1;
            }

#if 0
            // NOTE(marco): this is the implementation described in the book. If we don't
            // sample for one frame, the output is very noisy and blocky. Always forcing at
            // least one sample reduces this issue. Needs further investigation.
            uvec4 new_sample_count_history = uvec4( sample_count, sample_count_history.x, sample_count_history.y, sample_count_history.z );
            bool zeroSampleHistory = all( lessThan( new_sample_count_history, uvec4( 1 ) ) );
            if ( zeroSampleHistory ) {
                // NOTE(marco): force this frame to have at least one sample
                sample_count = 1;
            }
#else
            if ( sample_count <= 0 ) {
                sample_count = 1;
            }
#endif
    }

    float visibility = 0.0;
    if ( sample_count > 0 ) {
        const vec2 screen_uv = uv_nearest( full_resolution_pixel, resolution );
        //const vec2 unjittered_uv = screen_uv - jitter_xy;
        vec3 pixel_world_position = world_position_from_depth( screen_uv, raw_depth, inverse_view_projection );
        //pixel_world_position = world_position_from_depth( unjittered_uv, raw_depth, inverse_unjittered_view_projection );

        vec2 encoded_normal = texelFetch( global_textures[ normals_texture_index ], full_resolution_pixel, 0 ).rg;
        vec3 normal = octahedral_decode( encoded_normal );

        // Point light
        if ( is_raytrace_shadow_point_light() ) {
            // NOTE(marco): this is the code in the book
            visibility = get_point_light_visibility( gl_GlobalInvocationID.z, sample_count, pixel_world_position, normal, frame_index );
        }
        else {
            visibility = get_directional_light_visibility( raytraced_shadow_light_position, sample_count, pixel_world_position, normal, frame_index );
        }
    }

    vec4 last_visibility_values = vec4( visibility );
    if ( valid_history ) {
        last_visibility_values = texelFetch( global_texture_arrays[ current_visibility_cache_texture_index ], global_index, 0 );

        last_visibility_values.w = last_visibility_values.z;
        last_visibility_values.z = last_visibility_values.y;
        last_visibility_values.y = last_visibility_values.x;
    }

    last_visibility_values.x = visibility;

    sample_count_history.w = sample_count_history.z;
    sample_count_history.z = sample_count_history.y;
    sample_count_history.y = sample_count_history.x;
    sample_count_history.x = sample_count;


    last_variation_values.w = last_variation_values.z;
    last_variation_values.z = last_variation_values.y;
    last_variation_values.y = last_variation_values.x;
    last_variation_values.x = texelFetch( global_texture_arrays[ variation_texture_index ], global_index, 0 ).r;


    imageStore( global_image_arrays_2d[ current_visibility_cache_texture_index ], global_index, last_visibility_values );
    imageStore( global_image_arrays_2d[ filetered_variation_texture ], global_index, vec4( spatial_filtered_value, 0, 0, 0 ) );
    imageStore( global_image_arrays_2d[ current_variation_cache_texture_index ], global_index, last_variation_values );
    imageStore( global_uimages_arrays_2d[ current_samples_count_cache_texture_index ], global_index, sample_count_history );
}

#endif // COMPUTE_SHADOW_VISIBILITY

#if defined(COMPUTE_SHADOW_VISIBILITY_FILTERING)

#define GROUP_SIZE 8
#define FILTER_RADIUS 2
#define LOCAL_DATA_SIZE ( GROUP_SIZE + FILTER_RADIUS * 2 )

layout ( local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1 ) in;

shared float local_image_data[ LOCAL_DATA_SIZE ][ LOCAL_DATA_SIZE ];
shared vec4 local_normal_data[ LOCAL_DATA_SIZE ][ LOCAL_DATA_SIZE ];


// NOTE(marco): computed with script from https://stackoverflow.com/questions/29731726/how-to-calculate-a-gaussian-kernel-matrix-efficiently-in-numpy
float gaussian_kernel[5][5] = {
    { 0.00296902, 0.01330621, 0.02193823, 0.01330621, 0.00296902 },
    { 0.01330621, 0.0596343,  0.09832033, 0.0596343,  0.01330621 },
    { 0.02193823, 0.09832033, 0.16210282, 0.09832033, 0.02193823 },
    { 0.01330621, 0.0596343,  0.09832033, 0.0596343,  0.01330621 },
    { 0.00296902, 0.01330621, 0.02193823, 0.01330621, 0.00296902 }
};

float visibility_temporal_filter_orig( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const bool invalid_index = any( lessThan( index.xy, ivec2( 0 ) ) ) ||
                               any( greaterThanEqual( index.xy, image_resolution ) );

    if ( invalid_index ) {
        return 0.0;
    }

    const vec4 history = texelFetch( global_texture_arrays[ current_visibility_cache_texture_index ], index, 0 );

    float temporal_visibility = 0.25 * ( history.x + history.y + history.z + history.w );
    temporal_visibility = clamp( temporal_visibility, history.x - 0.50, history.x + 0.50 );

    return temporal_visibility;
}

float visibility_temporal_filter3( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const bool invalid_index = any( lessThan( index.xy, ivec2( 0 ) ) ) || any( greaterThanEqual( index.xy, image_resolution ) );

    if ( invalid_index ) {
        return 0.0;
    }

    const vec4 history = texelFetch( global_texture_arrays[ nonuniformEXT( current_visibility_cache_texture_index ) ], index, 0 );

    const float current_visibility = history.x;
    const float temporal_visibility = 0.25 * ( history.x + history.y + history.z + history.w );

    // If history disagrees too much with the current value,
    // trust current more. This protects shadow edges.
    const float history_delta = max( max( abs( history.y - current_visibility ), abs( history.z - current_visibility ) ), abs( history.w - current_visibility ) );

    const float history_weight = 1.0 - smoothstep( 0.10, 0.35, history_delta );

    return mix( current_visibility, temporal_visibility, history_weight );
}

float visibility_temporal_filter4( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const bool invalid_index = any( lessThan( index.xy, ivec2( 0 ) ) ) || any( greaterThanEqual( index.xy, image_resolution ) );

    if ( invalid_index ) {
        return 0.0;
    }

    const vec4 history = texelFetch( global_texture_arrays[ nonuniformEXT( current_visibility_cache_texture_index ) ], index, 0 );

    const float current_visibility = history.x;

    vec4 weights = vec4( 1.0 );

    float rejection_min = 0.25;
    float rejection_max = 0.55;

    weights.y = 1.0 - smoothstep( rejection_min, rejection_max, abs( history.y - current_visibility ) );
    weights.z = 1.0 - smoothstep( rejection_min, rejection_max, abs( history.z - current_visibility ) );
    weights.w = 1.0 - smoothstep( rejection_min, rejection_max, abs( history.w - current_visibility ) );

    // Prefer recent samples.
    weights *= vec4( 1.0, 0.75, 0.50, 0.25 );

    const float weight_sum = max( dot( weights, vec4( 1.0 ) ), 1e-5 );

    return dot( history, weights ) / weight_sum;
}

float visibility_temporal_filter( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const bool invalid_index =
        any( lessThan( index.xy, ivec2( 0 ) ) ) ||
        any( greaterThanEqual( index.xy, image_resolution ) );

    if ( invalid_index ) {
        return 0.0;
    }

    const vec4 history = texelFetch(
        global_texture_arrays[
            nonuniformEXT( current_visibility_cache_texture_index )
        ],
        index,
        0
    );

    const float current_visibility = history.x;

    // Use a short stable estimate instead of trusting only history.x.
    const float history_mean =
        0.25 * ( history.x + history.y + history.z + history.w );

    vec4 weights = vec4( 1.0 );

    weights.x = 1.0;

    weights.y = 1.0 - smoothstep(
        0.20, 0.60, abs( history.y - history_mean )
    );

    weights.z = 1.0 - smoothstep(
        0.20, 0.60, abs( history.z - history_mean )
    );

    weights.w = 1.0 - smoothstep(
        0.20, 0.60, abs( history.w - history_mean )
    );

    // Keep recent samples more important, but do not kill older ones too much.
    weights *= vec4( 1.0, 0.85, 0.70, 0.55 );

    const float weight_sum = max( dot( weights, vec4( 1.0 ) ), 1e-5 );

    float temporal_visibility = dot( history, weights ) / weight_sum;

    // Final safety clamp around current value.
    // This avoids the "extra pixel" from old history leaking too far.
    const float max_deviation = 0.35;

    return clamp(
        temporal_visibility,
        current_visibility - max_deviation,
        current_visibility + max_deviation
    );
}

vec4 get_normal( ivec3 index ) {
    const ivec2 image_resolution = shadow_visibility_resolution();

    if ( any( lessThan( index.xy, ivec2( 0 ) ) ) ||
         any( greaterThanEqual( index.xy, image_resolution ) ) ) {
        return vec4( 0, 0, 0, z_far );
    }

    const ivec2 full_resolution_pixel = cache_to_full_resolution_pixel( index.xy );

    const float raw_depth = texelFetch( global_textures[ nonuniformEXT( current_depth_texture_index ) ], full_resolution_pixel, 0 ).r;
    if ( raw_depth == 1.0 ) {
        return vec4( 0, 0, 0, z_far );
    }

    const vec2 encoded_normal = texelFetch( global_textures[ nonuniformEXT( normals_texture_index ) ], full_resolution_pixel, 0 ).rg;

    // Encode linear depth
    float linear_depth = raw_depth_to_linear_depth( raw_depth, z_near, z_far );
    return vec4( octahedral_decode( encoded_normal ), linear_depth );
}

void main() {
    const ivec2 image_resolution = shadow_visibility_resolution();

    const ivec2 global_pixel = ivec2( gl_GlobalInvocationID.xy );

    const bool valid_pixel = all( lessThan( global_pixel, image_resolution ) );

    const uint local_thread_index = gl_LocalInvocationID.y * GROUP_SIZE + gl_LocalInvocationID.x;
    const uint local_thread_count = GROUP_SIZE * GROUP_SIZE;

    const ivec2 group_origin = ivec2( gl_WorkGroupID.xy ) * GROUP_SIZE;

    // Load the complete tile and all four halo corners.
    for ( uint i = local_thread_index; i < LOCAL_DATA_SIZE * LOCAL_DATA_SIZE; i += local_thread_count ) {

        const ivec2 shared_coord = ivec2( i % LOCAL_DATA_SIZE, i / LOCAL_DATA_SIZE );

        const ivec2 source_pixel = group_origin + shared_coord - ivec2( FILTER_RADIUS );
        const ivec3 source_index = ivec3( source_pixel, gl_GlobalInvocationID.z );

        local_image_data[ shared_coord.y ][ shared_coord.x ] = visibility_temporal_filter_orig( source_index );
        local_normal_data[ shared_coord.y ][ shared_coord.x ] = get_normal( source_index );
    }

    memoryBarrierShared();
    barrier();

    // All invocations must reach the barrier.
    if ( !valid_pixel ) {
        return;
    }

    const ivec2 local_index = ivec2( gl_LocalInvocationID.xy ) + ivec2( FILTER_RADIUS );
    const ivec3 global_index = ivec3( gl_GlobalInvocationID.xyz ); 

    const float center_visibility = local_image_data[ local_index.y ][ local_index.x ];

    const vec4 center_normal_depth = local_normal_data[ local_index.y ][ local_index.x ];

    float filtered_visibility = 0.0;
    float total_weight = 0.0;

    for ( int y = -FILTER_RADIUS; y <= FILTER_RADIUS; ++y ) {
        for ( int x = -FILTER_RADIUS; x <= FILTER_RADIUS; ++x ) {

            const float kernel_weight = gaussian_kernel[ y + FILTER_RADIUS ][ x + FILTER_RADIUS ];
            if ( x == 0 && y == 0 ) {
                filtered_visibility += center_visibility * kernel_weight;
                total_weight += kernel_weight;
                continue;
            }

            const ivec2 sample_index = local_index + ivec2( x, y );

            const vec4 sample_normal_depth = local_normal_data[ sample_index.y ][ sample_index.x ];

            const float normal_weight = dot( center_normal_depth.xyz, sample_normal_depth.xyz ) > 0.9 ? 1.0 : 0.0;
            const float depth_delta = abs( center_normal_depth.w - sample_normal_depth.w );
            const float depth_threshold = max( 0.01, center_normal_depth.w * 0.002 );
            const float depth_weight = depth_delta <= depth_threshold ? 1.0 : 0.0;

            const float weight = kernel_weight * normal_weight * depth_weight;

            const float visibility = local_image_data[ sample_index.y ][ sample_index.x ];

            filtered_visibility += visibility * weight;

            total_weight += weight;
        }
    }

    if ( total_weight > 1e-6 ) {
        filtered_visibility /= total_weight;
    } else {
        filtered_visibility = center_visibility;
    }

    imageStore( global_image_arrays_2d[ filtered_visibility_texture ], global_index, vec4( filtered_visibility, center_normal_depth.w, 0.0, 0.0 ) );
}

#endif // COMPUTE_SHADOW_VISIBILITY_FILTERING


#if defined(COMPUTE_SHADOW_VISIBILITY_UPSCALING)

uint min_index4( vec4 values ) {
    const bool x_wins = values.x <= values.z;
    const bool y_wins = values.y <= values.w;

    const vec2 pair_min = min( values.xy, values.zw );

    if ( pair_min.x <= pair_min.y ) {
        return x_wins ? 0u : 2u;
    }

    return y_wins ? 1u : 3u;
}

float select_component( vec4 values, uint index ) {
    switch ( index ) {
        case 0u: {
            return values.x;
        }

        case 1u: {
            return values.y;
        }

        case 2u: {
            return values.z;
        }

        default: {
            return values.w;
        }
    }
}

#define GROUP_SIZE 8

layout (local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1 ) in;

void main() {
    const ivec2 full_resolution = ivec2( resolution );
    const ivec2 full_pixel = ivec2( gl_GlobalInvocationID.xy );
    const int shadow_layer = int( gl_GlobalInvocationID.z );

    if ( any( greaterThanEqual( full_pixel, full_resolution ) ) ) {
        return;
    }

    const float raw_depth = texelFetch( global_textures[ nonuniformEXT( current_depth_texture_index ) ], full_pixel, 0 ).r;

    if ( raw_depth == 1.0 ) {
        imageStore( global_image_arrays_2d[ nonuniformEXT( upscaled_visibility_texture ) ], ivec3( full_pixel, shadow_layer ), vec4( 1.0, 0.0, 0.0, 0.0 ) );

        return;
    }

    const float current_linear_depth = raw_depth_to_linear_depth( raw_depth, z_near, z_far );

    const vec2 full_uv = ( vec2( full_pixel ) + vec2( 0.5 ) ) / vec2( full_resolution );

    const vec3 shadow_uv_layer = vec3( full_uv, float( shadow_layer ) );

    // R channel: filtered visibility.
    const vec4 visibility_samples = textureGather( global_texture_arrays[ nonuniformEXT( filtered_visibility_texture ) ], shadow_uv_layer, 0 );

    // G channel: representative linear depth.
    const vec4 guide_depth_samples = textureGather( global_texture_arrays[ nonuniformEXT( filtered_visibility_texture ) ], shadow_uv_layer, 1 );

    const vec4 depth_delta = abs( guide_depth_samples - vec4( current_linear_depth ) );

    const uint best_index = min_index4( depth_delta );

    const float visibility = select_component( visibility_samples, best_index );

    imageStore( global_image_arrays_2d[ nonuniformEXT( upscaled_visibility_texture ) ], ivec3( full_pixel, shadow_layer ), vec4( visibility, 0.0, 0.0, 0.0 ) );
}

#endif // COMPUTE_SHADOW_VISIBILITY_UPSCALING


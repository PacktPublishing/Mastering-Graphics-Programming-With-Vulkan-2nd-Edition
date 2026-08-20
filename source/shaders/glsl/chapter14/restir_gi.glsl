#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable

#include "platform.glslh"
#include "frame.h"
#include "mesh.h"
#include "lighting.h"
#include "sampling.h"

#define MAX_TEMPORAL_M 30
#define MAX_SPATIAL_M 500
#define MAX_ITER_LOW 3
#define MAX_ITER_HIGH 9

#define MAX_DEPTH_DIFFERENCE 0.05
#define MAX_NORMAL_DIFFERENCE ( cos( radians( 25 ) ) )
#define MIN_SAMPLE_NORMAL_COS 0.01
#define MIN_RESAMPLING_JACOBIAN 0.01
#define MAX_RESAMPLING_JACOBIAN 100.0

// Adapted from the ReSTIR GI: Path Resampling for Real-Time Path Tracing paper
struct ReservoirSample {
    vec3 xv, nv; // visible point and surface normal
    vec3 xs;
    float s_roughness;
    vec3 ns; // sample point and surface normal
    float s_metalness;
    vec3 s_albedo;
    vec3 lo; // outgoing radiance at sample point

    vec3 wi; // sampled direction

    vec3 Fs; // BRDF at xv
    float p_wi; // pdf of sampled direction

    float p_hat; // target function
    float cos_theta;  // dot(nv, wi)
    uint  pad[2];
};

struct Reservoir
{
    ReservoirSample z;

    // From Algorithm 1, Weighted Reservoir Sampling
    float w_sum;   // line 3
    float W;       // line 5
    uint  M;       // line 4
    uint pad;
};

struct RayPayload {
    int instance_id;
    int geometry_id;
    int primitive_id;
    vec2 barycentric_weights;
    mat4x3 object_to_world;
    uint triangle_facing;
    float t;
};

struct GpuReSTIRGIConstants {
    uint                albedo_texture_index;
    uint                normal_texture_index;
    uint                roughness_texture_index;
    uint                depth_texture_index;

    uint                sbt_offset; // shader binding table offset
    uint                sbt_stride; // shader binding table stride
    uint                miss_index;
    uint                output_texture_index;

    vec4                light;

    float               light_range;
    float               light_intensity;
    ivec2               resolution;

    uint                linear_depth_index;
    uint                linear_depth_history_index;
    uint                normal_history_index;
    uint                motion_vectors_index;

    uint                mesh_id_index;
    uint                mesh_id_history_index;
    uint                output_history_texture_index;
    uint                output_indirect_texture_index;
};

layout(std140, set = MATERIAL_SET, binding = 1) uniform restir_gi_locals {
    GpuReSTIRGIConstants   restir_gi;
};

layout ( std430, set = MATERIAL_SET, binding = 30 ) buffer spatial_reservoir_buffer {
    Reservoir spatial_reservoirs[];
};

layout ( std430, set = MATERIAL_SET, binding = 31 ) buffer temporal_reservoir_buffer_read {
    Reservoir temporal_reservoirs_read[];
};

layout ( std430, set = MATERIAL_SET, binding = 32 ) buffer temporal_reservoir_buffer_write {
    Reservoir temporal_reservoirs_write[];
};

layout( set = MATERIAL_SET, binding = 26 ) uniform accelerationStructureEXT as;

bool check_visibility( vec3 pos, vec3 dir, float max_dist ) {
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery,
                          as,
                          gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0xff,
                          pos,
                          0.01,
                          dir,
                          max_dist - 0.02);
    while ( rayQueryProceedEXT( rayQuery ) ) { }

    bool visible = false;
    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        visible = true;
    }

    return visible;
}

void reservoir_update(
    inout Reservoir R,
    ReservoirSample reservoir_sample,
    float wnew,
    float rnd)
{
    if ( ( wnew < 0.0 ) || isinf( wnew ) || isnan( wnew ) ) return;

    R.w_sum += wnew;
    R.M++;

    if ( rnd < ( wnew / R.w_sum ) )
    {
        R.z = reservoir_sample;
    }
}

void merge_reservoir( inout Reservoir Rs, in Reservoir Rq, float p_hat, float rnd ) {
    if ( Rq.M == 0u || Rq.W <= 0.0 || p_hat <= 0.0 || isnan(p_hat) || isinf(p_hat) ) return;

    uint M0 = Rs.M;
    reservoir_update( Rs, Rq.z, p_hat * Rq.W * Rq.M, rnd );
    Rs.M = M0 + Rq.M;
}

Reservoir empty_reservoir() {
    Reservoir empty;

    empty.z.xv = vec3( 0 );
    empty.z.nv = vec3( 0 );
    empty.z.xs = vec3( 0 );
    empty.z.s_roughness = 0;
    empty.z.ns = vec3( 0 );
    empty.z.s_metalness = 0;
    empty.z.s_albedo = vec3( 0 );
    empty.z.lo = vec3( 0 );
    empty.z.wi = vec3( 0 );
    empty.z.Fs = vec3( 0 );
    empty.z.p_wi = 0;
    empty.z.p_hat = 0;
    empty.z.cos_theta = 0;

    empty.w_sum = 0;
    empty.W = 0;
    empty.M = 0;

    return empty;
}

vec3 diffuse_brdf( vec3 color ) {
    return ( 1.0 / PI ) * color;
}

vec3 specular_brdf( float alpha_squared, vec3 N, vec3 L, vec3 H, vec3 V ) {
    float NdotH = clamp( dot( N, H ), 0, 1.0 );
    float NdotL = clamp( dot( N, L ), 0, 1.0 );
    float NdotV = clamp( dot( N, V ), 0, 1.0 );
    float HdotL = clamp( dot( H, L ), 0, 1.0 );
    float HdotV = clamp( dot( H, V ), 0, 1.0 );
    float ggx = ( alpha_squared * heaviside( NdotH ) ) /
        ( PI * (
            pow( ( ( NdotH * NdotH ) * ( alpha_squared - 1.0 ) + 1 ), 2 )
        ) );

    float visiblity = ( heaviside( HdotL ) / ( abs( NdotL ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotL * NdotL ) ) ) ) *
                      ( heaviside( HdotV ) / ( abs( NdotV ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotV * NdotV ) ) ) );

    return vec3( visiblity * ggx );
}

vec3 evaluate_brdf( vec3 wo, vec3 wi, vec3 albedo, vec3 position, vec3 normal, float metalness, float roughness ) {
    vec3 V = wo;
    vec3 L = wi;
    vec3 N = normal;
    vec3 H = normalize( L + V );

    roughness = max(roughness, 0.045);
    float alpha = pow(roughness, 2.0);

    // https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#specular-brdf
    float alpha_squared = alpha * alpha;

    float HdotV = clamp( dot( H, V ), 0, 1 );

    vec3 material_colour = vec3(0, 0, 0);
    vec3 specular = specular_brdf( alpha_squared, N, L, H, V );
    float coeff = pow( 1 - abs( HdotV ), 5 );
    vec3 metal_brdf =  specular * ( albedo + ( 1 - albedo ) * coeff );

    // assumes IOR = 1.5
    float fresnel_mix = 0.04 + ( 1 - 0.04 ) * coeff;
    vec3 diffuse = diffuse_brdf( albedo );
    vec3 dielectric_brdf = mix( diffuse, specular, fresnel_mix );

    material_colour = mix( dielectric_brdf, metal_brdf, metalness );

    return material_colour;
}

// vec3 evaluate_brdf( vec3 wo, vec3 wi, vec3 albedo, vec3 position, vec3 normal, float metalness, float roughness ) {
//     return ( 1.0 - metalness ) * albedo * INV_PI;
// }

vec3 estimate_outgoing_radiance(
    vec3 xs,
    vec3 ns,
    vec3 wo,
    vec3 albedo,
    float metalness,
    float roughness
) {
    vec3 Lo = vec3(0.0);

    vec3 wi = normalize( restir_gi.light.xyz );
    float dist = 10000;

    float NoL = max(dot(ns, wi), 0.0);
    if ( NoL <= 0.0 )
        return Lo;

    bool visible = check_visibility( xs, wi, dist );

    if ( !visible )
        return Lo;

    // vec3 f = evaluate_brdf( wo, wi, albedo, xs, ns, metalness, roughness );
    // The paper suggests to "Instead, the scattered radiance in the direction to the original visible point
    // is used for all directions, corresponding to Lambertian scattering (Section 5)
    vec3 f = ( 1.0 - metalness ) * albedo * INV_PI;

    float light_pdf = 1.0f; // Assumes we only have one light
    Lo = restir_gi.light_intensity * f * NoL / light_pdf;

    return Lo;
}

float compute_phat( vec3 lo, vec3 f, float cos_theta ) {
    return luminance( lo * f * cos_theta );
    // return luminance( lo );
}

Reservoir validate_reservoir(
    Reservoir r,
    vec3 xv,
    vec3 nv,
    vec3 wo,
    vec3 albedo,
    float roughness,
    float metalness ) {
    if ( r.M == 0u || r.W <= 0.0 || r.z.p_hat <= 0.0 )
        return empty_reservoir();

    vec3 to_sample = r.z.xs - xv;
    float dist = length( to_sample );

    if ( dist <= 1e-4 )
        return empty_reservoir();

    vec3 wi = to_sample / dist;
    float cos_theta = max( dot( nv, wi ), 0.0 );

    if ( cos_theta <= 0.0 )
        return empty_reservoir();

    bool visible = check_visibility( xv, wi, dist );

    if ( !visible ) return empty_reservoir();

    vec3 sun_dir = normalize( restir_gi.light.xyz );
    float sun_dist = 10000;

    float NoL = max( dot( r.z.ns, sun_dir ), 0.0);
    if (NoL <= 0.0) return empty_reservoir();

    visible = check_visibility( r.z.xs, sun_dir, sun_dist );
    if ( !visible ) return empty_reservoir();

    vec3 Fs = evaluate_brdf( wo, wi, albedo, xv, nv, metalness, roughness );

    vec3 bounce_wo = normalize( xv - r.z.xs );

    vec3 bounce_lo = estimate_outgoing_radiance(
        r.z.xs,
        r.z.ns,
        bounce_wo,
        r.z.s_albedo,
        r.z.s_metalness,
        r.z.s_roughness
    );

    // float oldLum = luminance( r.z.lo );
    // float newLum = luminance( bounce_lo );

    // float relativeDifference =
    //     abs( newLum - oldLum ) /
    //     max( max( oldLum, newLum ), 1e-4);

    // if (relativeDifference > 0.2)
    //     return empty_reservoir();

    float p_hat = compute_phat( bounce_lo, Fs, cos_theta );

    if ( p_hat <= 0.0 || isnan( p_hat ) || isinf( p_hat ) )
        return empty_reservoir();

    r.z.lo = bounce_lo;
    r.z.wi = wi;
    r.z.Fs = Fs;
    r.z.cos_theta = cos_theta;
    r.z.p_hat = p_hat;

    r.W = r.w_sum / ( float( r.M ) * p_hat );

    return r;
}

bool reevaluate_selected_target(
    inout Reservoir r,
    vec3 xv,
    vec3 nv,
    vec3 wo,
    vec3 albedo,
    float metalness,
    float roughness)
{
    if ( r.M == 0u )
        return false;

    vec3 delta = r.z.xs - xv;
    float distance2 = dot( delta, delta );

    if ( distance2 <= 1e-8 )
        return false;

    vec3 wi = delta * inversesqrt( distance2 );
    float cos_theta = max( dot( nv, wi ), 0.0);

    if (cos_theta <= 0.0)
        return false;

    vec3 Fs = evaluate_brdf(
        wo,
        wi,
        albedo,
        xv,
        nv,
        metalness,
        roughness
    );

    float pHat = compute_phat(r.z.lo, Fs, cos_theta);

    if ( ( pHat <= 0.0 ) || isnan( pHat ) || isinf( pHat ) )
        return false;

    r.z.xv = xv;
    r.z.nv = nv;
    r.z.wi = wi;
    r.z.Fs = Fs;
    r.z.cos_theta = cos_theta;
    r.z.p_hat = pHat;

    return true;
}

bool check_temporal_similarity( ivec2 pos, ivec2 reprojected_uv, vec3 n ) {
    bool reprojectionFailed = reprojected_uv.x < 0.0 || reprojected_uv.x >= restir_gi.resolution.x || reprojected_uv.y < 0.0 || reprojected_uv.y >= restir_gi.resolution.y;
    bool depthMismatch = true;
    bool normalMismatch = true;
    bool meshMismatch = true;

    if ( !reprojectionFailed ) {
        float linear_depth = texelFetch( global_textures[ restir_gi.linear_depth_index ], pos, 0 ).r;
        float reprojected_depth = texelFetch( global_textures[ restir_gi.linear_depth_history_index ], reprojected_uv, 0 ).r;
        vec2 encoded_reprojected_normal = texelFetch( global_textures[ restir_gi.normal_history_index ], reprojected_uv, 0 ).rg;
        vec3 reprojected_normal = octahedral_decode( encoded_reprojected_normal );

        uint mesh_id = texelFetch( global_utextures[ restir_gi.mesh_id_index ], pos, 0 ).r;
        uint reprojected_mesh_id = texelFetch( global_utextures[ restir_gi.mesh_id_history_index ], reprojected_uv, 0 ).r;

        depthMismatch = abs( reprojected_depth - linear_depth ) > MAX_DEPTH_DIFFERENCE;
        normalMismatch = dot( reprojected_normal, n ) < MAX_NORMAL_DIFFERENCE;
        meshMismatch = mesh_id != reprojected_mesh_id;
    }

    return !( reprojectionFailed || depthMismatch || normalMismatch || meshMismatch );
}

bool check_spatial_similarity( ivec2 p, ivec2 q )
{
    float depthP =
        texelFetch(global_textures[restir_gi.linear_depth_index], p, 0).r;

    float depthQ =
        texelFetch(global_textures[restir_gi.linear_depth_index], q, 0).r;

    vec3 normalP = octahedral_decode(
        texelFetch(
            global_textures[restir_gi.normal_texture_index],
            p,
            0
        ).rg
    );

    vec3 normalQ = octahedral_decode(
        texelFetch(
            global_textures[restir_gi.normal_texture_index],
            q,
            0
        ).rg
    );

    uint meshP =
        texelFetch(global_utextures[restir_gi.mesh_id_index], p, 0).r;

    uint meshQ =
        texelFetch(global_utextures[restir_gi.mesh_id_index], q, 0).r;

    bool depthMismatch =
        abs(depthP - depthQ) > MAX_DEPTH_DIFFERENCE;
        // abs(depthP - depthQ) < max(0.02, depthP * 0.01);

    bool normalMismatch =
        dot(normalP, normalQ) < MAX_NORMAL_DIFFERENCE;

    bool meshMismatch = meshP != meshQ;

    return !(depthMismatch || normalMismatch || meshMismatch);
}

ivec2 compute_checker_board( ivec2 pos ) {
    uint mask = uint( frame.current_frame ) & 0x3;

    ivec2 offset = ivec2( mask & 0x1, ( mask & 0x2 ) >> 1 );

    return pos * 2 + offset;
}

#if defined(RAYGEN_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadEXT RayPayload payload;

void main() {
    ivec2 pos = ivec2( gl_LaunchIDEXT.xy );
    pos = compute_checker_board( pos );

    if (any(greaterThanEqual(pos, restir_gi.resolution))) {
        return;
    }

    const vec3 albedo = texelFetch(global_textures[(restir_gi.albedo_texture_index)], pos, 0).rgb;
    const vec2 encoded_normal = texelFetch(global_textures[(restir_gi.normal_texture_index)], pos, 0).rg;
    const float raw_depth = texelFetch(global_textures[(restir_gi.depth_texture_index)], pos, 0).r;
    const vec3 orm = texelFetch(global_textures[(restir_gi.roughness_texture_index)], pos, 0).rgb;

    const vec2 screen_uv = uv_nearest( pos, restir_gi.resolution );

    vec3 nv = octahedral_decode(encoded_normal);
    vec3 xv = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);
    vec3 wo = normalize( frame.camera_position.xyz - xv );

    // vec2 U = vec2( pcg2d( uvec2( pos ) + uvec2( frame.current_frame ) ) ) * RND_NORMALIZER;
    vec2 U = animated_blue_noise_golden_ratio( pos, frame.current_frame, frame.blue_noise_128_rg_texture_index );
    uint rng_state = seed( uvec2( pos ) ) + frame.current_frame;

    BrdfSample brdf_sample = sampleVNDFOrDiffuse( wo, albedo, nv, orm.b, orm.g, orm.g, rng_state, U.x, U.y );
    // BrdfSample brdf_sample = sample_unform_diffuse( U );

    const int reservoir_index = int( pos.y * restir_gi.resolution.x + pos.x );
    float cos_theta = max( dot( nv, brdf_sample.wi ), 0.0 );
    if ( brdf_sample.pdf <= 0.0 || cos_theta <= 0.0 ) {
        temporal_reservoirs_write[ reservoir_index ] = empty_reservoir();
        return;
    }

    ReservoirSample reservoirSample;
    reservoirSample.nv = nv;
    reservoirSample.xv = xv;
    reservoirSample.wi = brdf_sample.wi;
    reservoirSample.p_wi = brdf_sample.pdf;
    reservoirSample.cos_theta = cos_theta;

    traceRayEXT( as, // topLevel
        gl_RayFlagsOpaqueEXT, // rayFlags
        0xff, // cullMask
        restir_gi.sbt_offset, // sbtRecordOffset
        restir_gi.sbt_stride, // sbtRecordStride
        restir_gi.miss_index, // missIndex
        reservoirSample.xv, // origin
        0.05, // Tmin
        reservoirSample.wi, // direction
        1000.0, // Tmax
        0 // payload index
    );

    if ( payload.instance_id != -1 ) {
        uint mesh_instance_index = payload.instance_id + payload.geometry_id;
        MeshInstanceDraw instance = mesh_instance_draws[ mesh_instance_index ];
        uint mesh_index = instance.mesh_draw_index;
        MeshDraw mesh = mesh_draws[ mesh_index ];

        int_array_type index_buffer = int_array_type( mesh.index_buffer );
        int i0 = index_buffer[ payload.primitive_id * 3 ].v;
        int i1 = index_buffer[ payload.primitive_id * 3 + 1 ].v;
        int i2 = index_buffer[ payload.primitive_id * 3 + 2 ].v;

        float_array_type vertex_buffer = float_array_type( mesh.position_buffer );
        vec4 p0 = vec4(
            vertex_buffer[ i0 * 3 + 0 ].v,
            vertex_buffer[ i0 * 3 + 1 ].v,
            vertex_buffer[ i0 * 3 + 2 ].v,
            1.0
        );
        vec4 p1 = vec4(
            vertex_buffer[ i1 * 3 + 0 ].v,
            vertex_buffer[ i1 * 3 + 1 ].v,
            vertex_buffer[ i1 * 3 + 2 ].v,
            1.0
        );
        vec4 p2 = vec4(
            vertex_buffer[ i2 * 3 + 0 ].v,
            vertex_buffer[ i2 * 3 + 1 ].v,
            vertex_buffer[ i2 * 3 + 2 ].v,
            1.0
        );

        vec2_array_type uv_buffer = vec2_array_type( mesh.uv_buffer );
        vec2 uv0 = uv_buffer[ i0 ].v;
        vec2 uv1 = uv_buffer[ i1 ].v;
        vec2 uv2 = uv_buffer[ i2 ].v;

        vec4 p0_world = vec4( payload.object_to_world * p0, 1.0 );
        vec4 p1_world = vec4( payload.object_to_world * p1, 1.0 );
        vec4 p2_world = vec4( payload.object_to_world * p2, 1.0 );

        vec4 p0_screen = frame.view_projection * p0_world;
        vec4 p1_screen = frame.view_projection * p1_world;
        vec4 p2_screen = frame.view_projection * p2_world;

        ivec2 texture_size = textureSize( global_textures[ nonuniformEXT( mesh.textures.x ) ], 0 );
        float texel_area = texture_size.x * texture_size.y * abs( ( uv1.x - uv0.x ) * ( uv2.y - uv0.y ) - ( uv2.x - uv0.x ) * ( uv1.y - uv0.y ) );
        float triangle_area = abs( ( p1_screen.x - p0_screen.x ) * ( p2_screen.y - p0_screen.y ) - ( p2_screen.x - p0_screen.x ) * ( p1_screen.y - p0_screen.y ) );
        float lod = floor( 0.5 * log2( texel_area / triangle_area ) );

        float b = payload.barycentric_weights.x;
        float c = payload.barycentric_weights.y;
        float a = 1 - b - c;
        vec2 uv = ( a * uv0 + b * uv1 + c * uv2 );

        reservoirSample.xs = reservoirSample.xv + reservoirSample.wi * payload.t;
        vec3 bounce_wo = normalize( reservoirSample.xv - reservoirSample.xs );

        vec3 triangle_normal = normalize( cross( p1_world.xyz - p0_world.xyz, p2_world.xyz - p0_world.xyz ) );
        if ( dot( triangle_normal, bounce_wo ) < 0.0 )
            triangle_normal = -triangle_normal;

        reservoirSample.ns = triangle_normal;
        vec4 bounce_albedo = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod );
        vec3 bounce_orm = calculate_pbr_parameters( mesh.metallic_roughness_occlusion_factor.x,
                                             mesh.metallic_roughness_occlusion_factor.y,
                                             mesh.textures.y,
                                             mesh.metallic_roughness_occlusion_factor.z,
                                             mesh.textures.w, uv );

        // NOTE(marco): calculate the outgoing radiance at the "bounce" point. We calculate visibility and the BRDF at that point
        reservoirSample.s_roughness = bounce_orm.g;
        reservoirSample.s_metalness = bounce_orm.b;
        reservoirSample.s_albedo = bounce_albedo.rgb;
        reservoirSample.lo = estimate_outgoing_radiance( reservoirSample.xs, reservoirSample.ns, bounce_wo, bounce_albedo.rgb, bounce_orm.b, bounce_orm.g );
        // NOTE(marco): calculate the BRDF at the visible point
        reservoirSample.Fs = evaluate_brdf( wo, reservoirSample.wi, albedo, reservoirSample.xv, reservoirSample.nv, orm.b, orm.g );

        reservoirSample.p_hat = compute_phat( reservoirSample.lo, reservoirSample.Fs, reservoirSample.cos_theta );

    } else {
        // TODO: sample sky
        temporal_reservoirs_write[reservoir_index] = empty_reservoir();
        spatial_reservoirs[reservoir_index] = empty_reservoir();
        return;
    }

    vec2 motion_vector = texelFetch( global_textures[ restir_gi.motion_vectors_index ], pos, 0 ).rg;
    vec2 reprojected_uv = reproject_uv_from_ndc_motion( screen_uv, motion_vector );
    ivec2 reprojected_uv_screen = ivec2( reprojected_uv * vec2( restir_gi.resolution ) - 0.5 );
    bool similar_samples = check_temporal_similarity( pos, reprojected_uv_screen, nv );

    Reservoir temporal_reservoir = empty_reservoir();
    if ( similar_samples )
    {
        int previous_index =
            reprojected_uv_screen.y * restir_gi.resolution.x + reprojected_uv_screen.x;
        Reservoir previour_reservoir = temporal_reservoirs_read[previous_index];

        vec3 wi_q = normalize( previour_reservoir.z.xs - xv );
        float cos_q = max( dot ( nv, wi_q ), 0.0 );

        vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );

        float p_hat_q = compute_phat( previour_reservoir.z.lo, Fs_q, cos_q );

        float rnd = rand( rng_state );
        merge_reservoir( temporal_reservoir, previour_reservoir, p_hat_q, rnd );
    }

    bool validation_frame = ( frame.current_frame % 6u ) == 0u;
    if ( validation_frame ) {
        temporal_reservoir = validate_reservoir(
            temporal_reservoir,
            xv,
            nv,
            wo,
            albedo.rgb,
            orm.g,
            orm.b
        );
    }

    float wnew = reservoirSample.p_hat / reservoirSample.p_wi;
    reservoir_update( temporal_reservoir, reservoirSample, wnew, rand( rng_state ) );

    if ( !reevaluate_selected_target( temporal_reservoir, xv, nv, wo, albedo, orm.b, orm.g ) )
    {
        temporal_reservoir = empty_reservoir();
    }

    if (temporal_reservoir.M > MAX_TEMPORAL_M) {
        float scale = float( MAX_TEMPORAL_M ) / float( temporal_reservoir.M );
        temporal_reservoir.w_sum *= scale;
        temporal_reservoir.M = MAX_TEMPORAL_M;
    }

    // Recompute reservoir contribution weight.
    if (temporal_reservoir.M > 0u && temporal_reservoir.z.p_hat > 0.0)
        temporal_reservoir.W = temporal_reservoir.w_sum / (float(temporal_reservoir.M) * temporal_reservoir.z.p_hat);
    else
        temporal_reservoir.W = 0.0;

    temporal_reservoirs_write[ reservoir_index ] = temporal_reservoir;
}

#endif // RAYGEN_SAMPLE_GENERATION

#if defined (CLOSEST_HIT_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;
hitAttributeEXT vec2 barycentric_weights;

void main() {

    payload.instance_id = gl_InstanceCustomIndexEXT;
    payload.geometry_id = gl_GeometryIndexEXT;
    payload.primitive_id = gl_PrimitiveID;
    payload.barycentric_weights = barycentric_weights;
    payload.object_to_world = gl_ObjectToWorldEXT;
    payload.t = gl_HitTEXT;
    payload.triangle_facing = gl_HitKindEXT;
}

#endif // CLOSEST_HIT_SAMPLE_GENERATION

#if defined (MISS_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;

void main() {
    payload.instance_id = -1;
    payload.geometry_id = -1;
    payload.primitive_id = -1;
}

#endif // MISS_SAMPLE_GENERATION

#if defined (COMPUTE_SPATIAL_SAMPLING)

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

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 pos = ivec2( gl_GlobalInvocationID.xy );
    pos = compute_checker_board( pos );

    if (any(greaterThanEqual(pos, restir_gi.resolution))) {
        return;
    }

    const int reservoir_index = int( pos.y * restir_gi.resolution.x + pos.x );

    // Algorithm 4 Spatial Resampling
    Reservoir spatial_reservoir = temporal_reservoirs_write[ reservoir_index ];

    // NOTE(marco): keep to debug temporal accumulation only
    // vec3 indirect_contribution =
    //     spatial_reservoir.z.lo *
    //     spatial_reservoir.z.Fs *
    //     spatial_reservoir.z.cos_theta *
    //     spatial_reservoir.W;

    // imageStore( global_images_2d[ restir_gi.output_indirect_texture_index ], pos, vec4( indirect_contribution, 1.0 ) );

    // return;

    uint start_M = spatial_reservoir.M;

    uint num_iterations = spatial_reservoir.M < ( MAX_SPATIAL_M / 2 ) ? MAX_ITER_HIGH : MAX_ITER_LOW;
    uint rng_state = seed( pos ) + frame.current_frame;

    const vec3 albedo = texelFetch(global_textures[(restir_gi.albedo_texture_index)], pos, 0).rgb;
    const vec2 encoded_normal = texelFetch(global_textures[(restir_gi.normal_texture_index)], pos, 0).rg;
    const float raw_depth = texelFetch(global_textures[(restir_gi.depth_texture_index)], pos, 0).r;
    const vec3 orm = texelFetch(global_textures[(restir_gi.roughness_texture_index)], pos, 0).rgb;

    const vec2 screen_uv = uv_nearest( pos, restir_gi.resolution );

    vec3 xv = world_position_from_depth(screen_uv, raw_depth, frame.inverse_view_projection);
    vec3 nv = octahedral_decode(encoded_normal);
    vec3 wo = normalize( frame.camera_position.xyz - xv );

    uint Q[ MAX_ITER_HIGH ];
    ivec2 Qpos[ MAX_ITER_HIGH ];
    uint q_count = 0;
    for ( uint s = 0; s < num_iterations; s++ ) {
        // generate random direction
        uint next_rnd = rand_pcg( rng_state ) % SAMPLE_NUM;
        // The paper uses 10% of the image size as the radius but that seems too large. We use a fixed radius of 30 pixels
        // ivec2 qn = pos + ivec2( POISSON_SAMPLES[next_rnd] * vec2( restir_gi.resolution ) * vec2( 0.1 ) );
        ivec2 qn = pos + ivec2( POISSON_SAMPLES[next_rnd] * 30.0 );
        if ( qn.x < 0 || qn.x >= restir_gi.resolution.x || qn.y < 0 || qn.y >= restir_gi.resolution.y ) continue;

        int q_index = qn.y * restir_gi.resolution.x + qn.x;
        Reservoir rn = temporal_reservoirs_write[ q_index ];

        if ( rn.M == 0u || rn.W <= 0.0 || rn.z.p_hat <= 0.0 ) continue;

        bool similar_samples = check_spatial_similarity( pos, qn );

        if ( !similar_samples ) continue;

        float q_depth =
            texelFetch(
                global_textures[restir_gi.depth_texture_index],
                qn,
                0
            ).r;

        vec2 q_uv = uv_nearest(qn, restir_gi.resolution);

        vec3 q_xv = world_position_from_depth(
            q_uv,
            q_depth,
            frame.inverse_view_projection
        );

        // float dist = length( q_xv - xv );
        // float linear_depth = texelFetch( global_textures[restir_gi.linear_depth_index], qn, 0 ).r;
        // bool dist_valid = dist < max( 0.02, 0.01 * linear_depth );
        // if ( !dist_valid ) continue;

        vec3 qq_dir = q_xv - rn.z.xs;
        vec3 rq_dir = xv - rn.z.xs;

        float qq_len_sq = dot( qq_dir, qq_dir );
        float rq_len_sq = dot( rq_dir, rq_dir );

        qq_dir = normalize( qq_dir );
        rq_dir = normalize( rq_dir );

        float cos_phi_q = abs( dot( qq_dir, rn.z.ns ) );
        float cos_phi_r = abs( dot( rq_dir, rn.z.ns ) );

        if ( cos_phi_q <= MIN_SAMPLE_NORMAL_COS || cos_phi_r <= MIN_SAMPLE_NORMAL_COS || rq_len_sq <= 1e-8 || qq_len_sq < 1e-8 ) continue;

        float jq = ( cos_phi_r  / cos_phi_q ) * ( qq_len_sq / rq_len_sq );
        if ( jq < MIN_RESAMPLING_JACOBIAN || jq > MAX_RESAMPLING_JACOBIAN ) continue;

        vec3 ray_dir = rn.z.xs - xv;
        float max_dist = sqrt( dot( ray_dir, ray_dir ) );

        bool visible = check_visibility( xv, normalize( ray_dir ), max_dist );

        float p_hat_prime = 0.0;
        if ( visible ) {
            vec3 wi_q = normalize( rn.z.xs - xv );
            float cos_q = max( dot ( nv, wi_q ), 0.0 );

            vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );

            float p_hat_q = compute_phat( rn.z.lo, Fs_q, cos_q );
            p_hat_prime = p_hat_q / abs( jq );
        }

        if ( p_hat_prime < 0.0 || isnan( p_hat_prime ) || isinf( p_hat_prime ) ) continue;

        float rnd = rand( rng_state );
        merge_reservoir( spatial_reservoir, rn, p_hat_prime, rnd );

        Qpos[ q_count ] = qn;
        Q[ q_count++ ] = q_index;
    }

    if (spatial_reservoir.M > MAX_SPATIAL_M) {
        float scale = float(MAX_SPATIAL_M) / float(spatial_reservoir.M);
        spatial_reservoir.w_sum *= scale;
        spatial_reservoir.M = MAX_SPATIAL_M;
    }

    float Z = 0;
    // Small optimization to avoid re-reading the material data for the current pixel
    {
        vec3 wi_dir = spatial_reservoir.z.xs - xv;
        float max_dist = sqrt( dot( wi_dir, wi_dir ) );

        bool valid = ( max_dist > 0.025 );

        vec3 wi_q = wi_dir / max_dist;
        float cos_q = max( dot ( nv, wi_q ), 0.0 );

        vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );

        float p_hat_qn = compute_phat( spatial_reservoir.z.lo, Fs_q, cos_q );

        valid = valid && p_hat_qn  > 0;

        bool visible = check_visibility( xv, wi_q, max_dist );

        valid = valid && visible;

        if ( valid ) Z += start_M;
    }

    for ( uint qn = 0; qn < q_count; ++qn ) {
        Reservoir rn = temporal_reservoirs_write[ Q[ qn ] ];
        ivec2 qnp = Qpos[ qn ];

        float q_depth =
            texelFetch(
                global_textures[restir_gi.depth_texture_index],
                qnp,
                0
            ).r;

        vec2 q_uv = uv_nearest( qnp, restir_gi.resolution );

        vec3 q_xv = world_position_from_depth(
            q_uv,
            q_depth,
            frame.inverse_view_projection
        );

        vec3 q_nv = octahedral_decode(
            texelFetch(
                global_textures[restir_gi.normal_texture_index],
                qnp,
                0
            ).rg
        );

        vec3 wi_dir = spatial_reservoir.z.xs - q_xv;
        float max_dist = sqrt( dot( wi_dir, wi_dir ) );

        if ( max_dist <= 0.025 ) continue;

        vec3 wo_q = normalize( frame.camera_position.xyz - q_xv );
        vec3 wi_q = wi_dir / max_dist;
        float cos_q = max( dot ( q_nv, wi_q ), 0.0 );

        const vec3 q_albedo = texelFetch(global_textures[(restir_gi.albedo_texture_index)], qnp, 0).rgb;
        const vec3 q_orm = texelFetch(global_textures[(restir_gi.roughness_texture_index)], qnp, 0).rgb;

        vec3 Fs_q = evaluate_brdf( wo_q, wi_q, q_albedo, q_xv, q_nv, q_orm.b, q_orm.g );

        float p_hat_qn = compute_phat( spatial_reservoir.z.lo, Fs_q, cos_q );
        if ( p_hat_qn <= 0 ) continue;

        bool visible = check_visibility( q_xv, wi_q, max_dist );
        if ( !visible ) continue;

        Z += rn.M;
    }

    vec3 wi = normalize( spatial_reservoir.z.xs - xv );
    float cosTheta = max( dot( nv, wi ), 0.0 );
    vec3 Fs = evaluate_brdf( wo, wi, albedo, xv, nv, orm.b, orm.g );
    float p_hat_selected = compute_phat( spatial_reservoir.z.lo, Fs, cosTheta );

    if ( Z > 0 && p_hat_selected > 0 )
        spatial_reservoir.W = spatial_reservoir.w_sum / ( Z * p_hat_selected );
    else
        spatial_reservoir.W = 0;

    spatial_reservoirs[ reservoir_index ] = spatial_reservoir;

    vec3 indirect_contribution =
        spatial_reservoir.z.lo *
        // Fs *
        cosTheta *
        spatial_reservoir.W;

    imageStore( global_images_2d[ restir_gi.output_indirect_texture_index ], pos, vec4( indirect_contribution, 1.0 ) );
}

#endif // COMPUTE_SPATIAL_SAMPLING

#if defined(COMPUTE_TEMPORAL_ACCUMULATION)

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 pos = ivec2( gl_GlobalInvocationID.xy );
    pos = compute_checker_board( pos );

    if (any(greaterThanEqual(pos, restir_gi.resolution))) {
        return;
    }
    const vec2 screen_uv = uv_nearest( pos, restir_gi.resolution );

    vec3 indirect_contribution = texelFetch( global_textures[ restir_gi.output_indirect_texture_index ], pos, 0 ).rgb;
    vec2 motion_vector = texelFetch( global_textures[ restir_gi.motion_vectors_index ], pos, 0 ).rg;
    vec2 reprojected_uv = reproject_uv_from_ndc_motion( screen_uv, motion_vector );
    ivec2 reprojected_uv_screen = ivec2( reprojected_uv * vec2( restir_gi.resolution ) - 0.5 );

    const vec2 encoded_normal = texelFetch(global_textures[(restir_gi.normal_texture_index)], pos, 0).rg;
    vec3 nv = octahedral_decode(encoded_normal);
    bool similar_samples = check_temporal_similarity( pos, reprojected_uv_screen, nv );

    if ( similar_samples && frame.current_frame > 0u ) {
        vec4 history_value = texelFetch( global_textures[ restir_gi.output_history_texture_index ], reprojected_uv_screen, 0 );

        vec3 nmin = vec3(1e20);
        vec3 nmax = vec3(-1e20);

        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec2 q = clamp( pos + ivec2(x, y), ivec2(0), restir_gi.resolution - ivec2(1) );
                vec3 c = texelFetch( global_textures[restir_gi.output_indirect_texture_index], q, 0).rgb;
                nmin = min(nmin, c);
                nmax = max(nmax, c);
            }
        }

        vec3 history_gi = history_value.rgb;
        history_gi = clamp( history_gi, nmin, nmax );
        uint old_count = uint( clamp( history_value.a, 0.0, 256.0 ) );

        uint new_count = min( old_count + 1, 256 );
        float current_weight = 1.0 / float( new_count );
        vec3 accumulated_gi = mix( history_gi, indirect_contribution, 0.2 );

        imageStore( global_images_2d[ restir_gi.output_texture_index ], pos, vec4( accumulated_gi, float( new_count ) ) );
    } else {
        imageStore( global_images_2d[ restir_gi.output_texture_index ], pos, vec4( indirect_contribution, 1.0 ) );
    }
}

#endif // COMPUTE_TEMPORAL_ACCUMULATION

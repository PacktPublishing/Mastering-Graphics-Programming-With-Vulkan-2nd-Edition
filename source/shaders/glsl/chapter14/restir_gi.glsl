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
#define MAX_ITER_HIGH 6

#define MAX_NORMAL_DIFFERENCE ( cos( radians( 25 ) ) )
#define MIN_SAMPLE_NORMAL_COS 0.01
#define MIN_RESAMPLING_JACOBIAN 0.01
#define MAX_RESAMPLING_JACOBIAN 100.0

#define RESTIR_DEBUG_NONE                   0

// Shows the temporal reservoir contribution before spatial reuse.
#define RESTIR_DEBUG_TEMPORAL_ONLY          1
// Visualizes temporal reservoir age/sample count (M).
#define RESTIR_DEBUG_TEMPORAL_M             2
// Shows whether temporal reprojection passes depth/normal/mesh similarity tests.
#define RESTIR_DEBUG_TEMPORAL_SIMILARITY    3
// Visualizes which stage rejects temporal reservoir reuse.
#define RESTIR_DEBUG_TEMPORAL_REUSE         4
// Visualizes the energy of the temporal reservoir before spatial reuse.
#define RESTIR_DEBUG_TEMPORAL_WEIGHT        5
// No reuse, show only the current frame's candidate sample.
#define RESTIR_DEBUG_SAMPLE_ONLY            6

// RESTIR_DEBUG_TEMPORAL_REUSE color coding
// black = invalid previous reservoir
// red = valid previous but failed visibility
// yellow = valid visibility, p_hat failed
// white = all valid, reusing reservoir

#ifndef RESTIR_DEBUG_MODE
    #define RESTIR_DEBUG_MODE RESTIR_DEBUG_NONE
#endif

struct ReservoirPacked {
    vec3  xs;                // 0   punto di bounce
    uint  ns_oct;            // 12  normale al bounce, octahedral 16:16
    uint  lo_rgb9e5;         // 16  radianza uscente
    uint  s_albedo_rgb9e5;   // 20  albedo al bounce
    uint  s_rough_metal;     // 24  packHalf2x16( roughness, metalness )
    float p_hat;             // 28
    float W;                 // 32
    float w_sum;             // 36
    uint  M;                 // 40
    uint  debug;             // 44
};

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
    float cos_theta; // dot(nv, wi)
    uint pad[2];
};

struct Reservoir {
    ReservoirSample z;

    // From Algorithm 1, Weighted Reservoir Sampling
    float w_sum; // line 3
    float W;     // line 5
    uint M;      // line 4
    uint debug;
};

struct RayPayload {
    int instance_id;
    int geometry_id;
    int primitive_id;
    vec2 barycentric_weights;
    uint triangle_facing;
    float t;
};

struct GpuReSTIRGIConstants {
    uint albedo_texture_index;
    uint normal_texture_index;
    uint roughness_texture_index;
    uint depth_texture_index;

    uint sbt_offset;
    uint sbt_stride;
    uint miss_index;
    uint output_texture_index;

    vec4 light_old;

    float light_range;
    float light_intensity;
    ivec2 resolution;

    uint linear_depth_index;
    uint linear_depth_history_index;
    uint normal_history_index;
    uint motion_vectors_index;

    uint mesh_id_index;
    uint mesh_id_history_index;
    uint output_history_texture_index;
    uint output_indirect_texture_index;
};

const ivec2 representative_offsets[4] = ivec2[](
    ivec2( 0, 0 ),
    ivec2( 1, 0 ),
    ivec2( 0, 1 ),
    ivec2( 1, 1 )
);

layout( std140, set = MATERIAL_SET, binding = 1 ) uniform restir_gi_locals {
    GpuReSTIRGIConstants restir_gi;
};

layout( std430, set = MATERIAL_SET, binding = 30 ) buffer spatial_reservoir_buffer {
    ReservoirPacked spatial_reservoirs[];
};

layout( std430, set = MATERIAL_SET, binding = 31 ) buffer temporal_reservoir_buffer_read {
    ReservoirPacked temporal_reservoirs_read[];
};

layout( std430, set = MATERIAL_SET, binding = 32 ) buffer temporal_reservoir_buffer_write {
    ReservoirPacked temporal_reservoirs_write[];
};

layout( set = MATERIAL_SET, binding = 26 ) uniform accelerationStructureEXT as;

// Packing/unpacking
uint pack_rgb9e5( vec3 c ) {
    c = clamp( c, vec3( 0.0 ), vec3( 65408.0 ) );
    float max_c = max( max( c.r, c.g ), c.b );

    int e = ( max_c > 1e-30 ) ? int( floor( log2( max_c ) ) ) + 1 : -15;
    e = clamp( e, -15, 16 );
    float denom = exp2( float( e ) - 9.0 );

    // l'arrotondamento può portare la mantissa a 512: alza l'esponente
    if ( int( floor( max_c / denom + 0.5 ) ) >= 512 ) {
        e     += 1;
        denom *= 2.0;
    }

    uvec3 q = uvec3( clamp( floor( c / denom + 0.5 ), vec3( 0.0 ), vec3( 511.0 ) ) );
    return ( uint( e + 15 ) << 27 ) | ( q.b << 18 ) | ( q.g << 9 ) | q.r;
}

vec3 unpack_rgb9e5( uint v ) {
    float denom = exp2( float( int( v >> 27 ) - 15 ) - 9.0 );
    return denom * vec3( float(   v        & 0x1FFu ),
                         float( ( v >>  9 ) & 0x1FFu ),
                         float( ( v >> 18 ) & 0x1FFu ) );
}

uint pack_reservoir_normal( vec3 n )  { return packSnorm2x16( octahedral_encode( n ) ); }
vec3 unpack_reservoir_normal( uint v ) { return octahedral_decode( unpackSnorm2x16( v ) ); }

Reservoir unpack_reservoir( ReservoirPacked p ) {
    Reservoir r;

    r.z.xs          = p.xs;
    r.z.ns          = unpack_reservoir_normal( p.ns_oct );
    r.z.lo          = unpack_rgb9e5( p.lo_rgb9e5 );
    r.z.s_albedo    = unpack_rgb9e5( p.s_albedo_rgb9e5 );

    vec2 rm         = unpackHalf2x16( p.s_rough_metal );
    r.z.s_roughness = rm.x;
    r.z.s_metalness = rm.y;
    r.z.p_hat       = p.p_hat;

    // Campi non persistiti: ogni consumatore li ricalcola prima dell'uso
    // (reevaluate_selected_target / validate_reservoir).
    r.z.xv = vec3( 0.0 );
    r.z.nv = vec3( 0.0 );
    r.z.wi = vec3( 0.0 );
    r.z.Fs = vec3( 0.0 );
    r.z.p_wi      = 0.0;
    r.z.cos_theta = 0.0;

    r.w_sum = p.w_sum;
    r.W     = p.W;
    r.M     = p.M;
    r.debug = p.debug;

    return r;
}

ReservoirPacked pack_reservoir( Reservoir r ) {
    ReservoirPacked p;

    p.xs              = r.z.xs;
    p.ns_oct          = pack_reservoir_normal( r.z.ns );
    p.lo_rgb9e5       = pack_rgb9e5( max( r.z.lo, vec3( 0.0 ) ) );
    p.s_albedo_rgb9e5 = pack_rgb9e5( clamp( r.z.s_albedo, vec3( 0.0 ), vec3( 1.0 ) ) );
    p.s_rough_metal   = packHalf2x16( vec2( r.z.s_roughness, r.z.s_metalness ) );
    p.p_hat           = r.z.p_hat;
    p.W               = r.W;
    p.w_sum           = r.w_sum;
    p.M               = r.M;
    p.debug           = r.debug;

    return p;
}


bool check_visibility( vec3 pos, vec3 dir, float max_dist ) {
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT( rayQuery, as,
                           gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                           0xff, pos, 0.01, dir, max_dist - 0.02 );

    while ( rayQueryProceedEXT( rayQuery ) ) {}

    return rayQueryGetIntersectionTypeEXT(
        rayQuery, true ) == gl_RayQueryCommittedIntersectionNoneEXT;
}

void reservoir_update(
    inout Reservoir R,
    ReservoirSample reservoir_sample,
    float wnew,
    float rnd ) {

    if ( wnew < 0.0 || isinf( wnew ) || isnan( wnew ) )
        return;

    R.w_sum += wnew;
    R.M++;

    if ( rnd < ( wnew / R.w_sum ) )
        R.z = reservoir_sample;
}

void merge_reservoir( inout Reservoir Rs, in Reservoir Rq, float p_hat, float rnd ) {
    if ( Rq.M == 0u || Rq.W <= 0.0 || p_hat <= 0.0 || isnan( p_hat ) || isinf( p_hat ) )
        return;

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
    empty.debug = 0;

    return empty;
}

vec3 diffuse_brdf( vec3 color ) {
    return INV_PI * color;
}

vec3 specular_brdf( float alpha_squared, vec3 N, vec3 L, vec3 H, vec3 V ) {
    float NdotH = clamp( dot( N, H ), 0, 1.0 );
    float NdotL = clamp( dot( N, L ), 0, 1.0 );
    float NdotV = clamp( dot( N, V ), 0, 1.0 );
    float HdotL = clamp( dot( H, L ), 0, 1.0 );
    float HdotV = clamp( dot( H, V ), 0, 1.0 );

    float ggx = ( alpha_squared * heaviside( NdotH ) ) /
        ( PI * pow( NdotH * NdotH * ( alpha_squared - 1.0 ) + 1.0, 2.0 ) );

    float visibility = ( heaviside( HdotL ) / ( abs( NdotL ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * NdotL * NdotL ) ) ) *
                       ( heaviside( HdotV ) / ( abs( NdotV ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * NdotV * NdotV ) ) );

    return vec3( visibility * ggx );
}

vec3 evaluate_brdf_with_reflection( vec3 wo, vec3 wi, vec3 albedo, vec3 position,
                                    vec3 normal, float metalness, float roughness ) {

    vec3 V = wo;
    vec3 L = wi;
    vec3 N = normal;
    vec3 H = normalize( L + V );

    float alpha = roughness * roughness;
    float alpha_squared = alpha * alpha;

    float HdotV = clamp( dot( H, V ), 0, 1 );
    vec3 specular = specular_brdf( alpha_squared, N, L, H, V );

    float coeff = pow( 1 - abs( HdotV ), 5 );
    vec3 metal_brdf = specular * ( albedo + ( 1 - albedo ) * coeff );

    float fresnel_mix = 0.04 + ( 1 - 0.04 ) * coeff;
    vec3 diffuse = diffuse_brdf( albedo );
    vec3 dielectric_brdf = mix( diffuse, specular, fresnel_mix );

    return mix( dielectric_brdf, metal_brdf, metalness );
}

vec3 evaluate_brdf_diffuse( vec3 wo, vec3 wi, vec3 albedo, vec3 position,
                            vec3 normal, float metalness, float roughness ) {

    return ( 1.0 - metalness ) * albedo * INV_PI;
}

#define evaluate_brdf evaluate_brdf_diffuse


vec3 estimate_outgoing_radiance( vec3 xs, vec3 ns, vec3 wo, vec3 albedo,
                                 float metalness, float roughness ) {

    vec3 Lo = vec3( 0.0 );

    // Regular point light.
    {
        Light light = lights[0];

        vec3 position_to_light = light.world_position - xs;
        float dist = length( position_to_light );
        vec3 wi = position_to_light / max( dist, 1e-4 );

        float attenuation = attenuation_square_falloff( position_to_light, 1.0 / light.radius );

        float NoL = max( dot( ns, wi ), 0.0 );

        if ( NoL > 1e-4 && attenuation > 1e-4 && check_visibility( xs, wi, dist ) ) {
            vec3 f = evaluate_brdf( wo, wi, albedo, xs, ns, metalness, roughness );

            Lo += light.intensity * light.color * attenuation * f * NoL;
        }
    }

    // Raytraced directional light.
    if ( !is_raytrace_shadow_point_light() ) {
        vec3 wi = normalize( light_cb.raytraced_shadow_light_position );
        float NoL = max( dot( ns, wi ), 0.0 );

        if ( NoL > 1e-4 && check_visibility( xs, wi, 100.0 ) ) {
            vec3 f = evaluate_brdf( wo, wi, albedo, xs, ns, metalness, roughness );
            vec3 color = unpack_color_rgba( light_cb.raytraced_shadow_light_color_type ).rgb;

            Lo += light_cb.raytraced_shadow_light_intensity * color * f * NoL;
        }
    }

    return Lo;
}

float compute_phat( vec3 lo, vec3 f, float cos_theta ) {
    return luminance( lo * f * cos_theta );
}

bool depth_similar( float depth_a, float depth_b ) {
    float threshold = max( 0.02, max( abs( depth_a ), abs( depth_b ) ) * 0.01 );
    return abs( depth_a - depth_b ) <= threshold;
}

Reservoir validate_reservoir( Reservoir r, vec3 xv, vec3 nv, vec3 wo,
                              vec3 albedo, float roughness, float metalness ) {

    // r.W is not initialized still, use w_sum
    if ( r.M == 0u || r.w_sum <= 0.0 || r.z.p_hat <= 0.0 ) {
        return empty_reservoir();
    }

    vec3 to_sample = r.z.xs - xv;
    float sample_dist = length( to_sample );

    if ( sample_dist <= 1e-4 ) {
        return empty_reservoir();
    }

    vec3 wi_sample = to_sample / sample_dist;
    float cos_theta = max( dot( nv, wi_sample ), 0.0 );

    if ( cos_theta <= 0.0 ) {
        return empty_reservoir();
    }

    // The caller already checked visibility from xv to this sample
    // before merging temporal history. No new candidate has been
    // inserted yet, so repeating the same visibility query is unnecessary.
    // if ( !check_visibility( xv, wi_sample, sample_dist ) ) {
    //     return empty_reservoir();
    // }

    vec3 Fs = evaluate_brdf( wo, wi_sample, albedo, xv, nv, metalness, roughness );
    vec3 bounce_wo = normalize( xv - r.z.xs );

    vec3 bounce_lo = estimate_outgoing_radiance( r.z.xs, r.z.ns, bounce_wo, r.z.s_albedo, r.z.s_metalness, r.z.s_roughness );

    float p_hat = compute_phat( bounce_lo, Fs, cos_theta );

    if ( p_hat <= 0.0 || isnan( p_hat ) || isinf( p_hat ) ) {
        return empty_reservoir();
    }

    r.z.lo = bounce_lo;
    r.z.wi = wi_sample;
    r.z.Fs = Fs;
    r.z.cos_theta = cos_theta;
    r.z.p_hat = p_hat;

    r.W = r.w_sum / ( float( r.M ) * p_hat );

    return r;
}

bool reevaluate_selected_target( inout Reservoir r, vec3 xv, vec3 nv, vec3 wo,
                                 vec3 albedo, float metalness, float roughness ) {

    if ( r.M == 0u ) {
        return false;
    }

    vec3 delta = r.z.xs - xv;
    float distance2 = dot( delta, delta );

    if ( distance2 <= 1e-8 ) {
        return false;
    }

    vec3 wi = delta * inversesqrt( distance2 );
    float cos_theta = max( dot( nv, wi ), 0.0 );

    if ( cos_theta <= 0.0 ) {
        return false;
    }

    vec3 Fs = evaluate_brdf( wo, wi, albedo, xv, nv, metalness, roughness );
    float pHat = compute_phat( r.z.lo, Fs, cos_theta );

    if ( pHat <= 0.0 || isnan( pHat ) || isinf( pHat ) ) {
        return false;
    }

    r.z.xv = xv;
    r.z.nv = nv;
    r.z.wi = wi;
    r.z.Fs = Fs;
    r.z.cos_theta = cos_theta;
    r.z.p_hat = pHat;

    return true;
}

bool check_temporal_similarity( ivec2 pos, ivec2 reprojected_uv, vec3 n ) {
    bool reprojectionFailed = reprojected_uv.x < 0 || reprojected_uv.x >= restir_gi.resolution.x ||
                              reprojected_uv.y < 0 || reprojected_uv.y >= restir_gi.resolution.y;

    bool depthMismatch = true;
    bool normalMismatch = true;
    bool meshMismatch = true;

    if ( !reprojectionFailed ) {
        //float linear_depth = texelFetch( global_textures[restir_gi.linear_depth_index], pos, 0 ).r;
        //float reprojected_depth = texelFetch( global_textures[restir_gi.linear_depth_history_index], reprojected_uv, 0 ).r;
        // Reconstruct the same representative point used by ReSTIR.
        vec4 guide = texelFetch( global_textures[restir_gi.normal_texture_index], pos, 0 );

        uint representative_index = uint( round( guide.a ) );
        ivec2 fullres_pos = pos * 2 + representative_offsets[representative_index];
        vec2 fullres_uv = uv_nearest( fullres_pos, frame.resolution );

        float raw_depth = guide.b;
        // Read depth at the same representative pixel, at its original precision.
        raw_depth = texelFetch( global_textures[restir_gi.depth_texture_index], fullres_pos, 0 ).r;
        vec3 world_position = world_position_from_depth( fullres_uv, raw_depth, frame.inverse_view_projection );

        // Predict this static surface point's depth in the previous frame.
        vec4 previous_clip = frame.previous_view_projection * vec4( world_position, 1.0 );

        if ( previous_clip.w <= 0.0 ) {
            return false;
        }

        // The GBuffer stores gl_FragCoord.z / gl_FragCoord.w,
        // which equals clip-space Z for viewport depth [0, 1].
        float expected_previous_depth = previous_clip.z;
        float reprojected_depth = texelFetch( global_textures[restir_gi.linear_depth_history_index], reprojected_uv, 0 ).r;

        vec2 encoded_reprojected_normal = texelFetch( global_textures[restir_gi.normal_history_index], reprojected_uv, 0 ).rg;
        vec3 reprojected_normal = octahedral_decode( encoded_reprojected_normal );

        uint mesh_id = texelFetch( global_utextures[restir_gi.mesh_id_index], pos, 0 ).r;
        uint reprojected_mesh_id = texelFetch( global_utextures[restir_gi.mesh_id_history_index], reprojected_uv, 0 ).r;

        //depthMismatch = !depth_similar( reprojected_depth, linear_depth );
        depthMismatch = !depth_similar( reprojected_depth, expected_previous_depth );
        normalMismatch = dot( reprojected_normal, n ) < MAX_NORMAL_DIFFERENCE;
        meshMismatch = mesh_id != reprojected_mesh_id;
    }

    return !( reprojectionFailed || depthMismatch || normalMismatch || meshMismatch );
}

bool check_spatial_similarity( ivec2 p, ivec2 q ) {

    float depthP = texelFetch( global_textures[restir_gi.linear_depth_index], p, 0 ).r;
    float depthQ = texelFetch( global_textures[restir_gi.linear_depth_index], q, 0 ).r;

    vec3 normalP = octahedral_decode( texelFetch( global_textures[restir_gi.normal_texture_index], p, 0 ).rg );
    vec3 normalQ = octahedral_decode( texelFetch( global_textures[restir_gi.normal_texture_index], q, 0 ).rg );

    uint meshP = texelFetch( global_utextures[restir_gi.mesh_id_index], p, 0 ).r;
    uint meshQ = texelFetch( global_utextures[restir_gi.mesh_id_index], q, 0 ).r;

    bool depthMismatch = !depth_similar( depthP, depthQ );
    bool normalMismatch = dot( normalP, normalQ ) < MAX_NORMAL_DIFFERENCE;
    bool meshMismatch = meshP != meshQ;

    return !( depthMismatch || normalMismatch || meshMismatch );
}

ivec2 compute_checker_board( ivec2 pos ) {

    uint mask = uint( frame.current_frame ) & 0x3;
    ivec2 offset = ivec2( mask & 0x1, ( mask & 0x2 ) >> 1 );

    return pos * 2 + offset;
}


#if defined(RAYGEN_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadEXT RayPayload payload;

void main() {

    ivec2 restir_xy = ivec2( gl_LaunchIDEXT.xy );

    if ( any( greaterThanEqual( restir_xy, restir_gi.resolution ) ) ) {
        return;
    }

    const vec4 guide = texelFetch( global_textures[restir_gi.normal_texture_index], restir_xy, 0 );

    vec2 encoded_normal = guide.rg;
    float raw_depth = guide.b;
    uint representative_index = uint( round( guide.a ) );
    ivec2 pos = restir_xy * 2 + representative_offsets[representative_index];

    //raw_depth = texelFetch( global_textures[restir_gi.depth_texture_index], pos, 0 ).r;

    const vec3 orm = texelFetch( global_textures[restir_gi.roughness_texture_index], pos, 0 ).rgb;
    const vec3 albedo = texelFetch( global_textures[restir_gi.albedo_texture_index], pos, 0 ).rgb;

    const vec2 restir_screen_uv = uv_nearest( restir_xy, restir_gi.resolution );
    const vec2 full_screen_uv = uv_nearest( pos, frame.resolution );

    vec3 nv = octahedral_decode( encoded_normal );
    vec3 xv = world_position_from_depth( full_screen_uv, raw_depth, frame.inverse_view_projection );
    vec3 wo = normalize( frame.camera_position.xyz - xv );

    vec2 U = animated_blue_noise_golden_ratio( restir_xy, frame.current_frame, frame.blue_noise_128_rg_texture_index );

    uint rng_state = seed( uvec2( restir_xy ) ) + frame.current_frame;
    BrdfSample brdf_sample = sample_diffuse( U, nv );

    const int reservoir_index = int( restir_xy.y * restir_gi.resolution.x + restir_xy.x );

    float cos_theta = max( dot( nv, brdf_sample.wi ), 0.0 );
    bool candidate_valid = brdf_sample.pdf > 0.0 && cos_theta > 0.0;

    ReservoirSample reservoirSample;

    if ( candidate_valid ) {
        reservoirSample.nv = nv;
        reservoirSample.xv = xv;
        reservoirSample.wi = brdf_sample.wi;
        reservoirSample.p_wi = brdf_sample.pdf;
        reservoirSample.cos_theta = cos_theta;

        traceRayEXT(
            as,
            gl_RayFlagsOpaqueEXT,
            0xff,
            restir_gi.sbt_offset,
            restir_gi.sbt_stride,
            restir_gi.miss_index,
            reservoirSample.xv,
            0.05,
            reservoirSample.wi,
            1000.0,
            0
        );

        if ( payload.instance_id == -1 ) {
            candidate_valid = false;
        }
    }

    if ( candidate_valid ) {
        uint mesh_instance_index = payload.instance_id + payload.geometry_id;
        MeshInstanceDraw instance = mesh_instance_draws[mesh_instance_index];
        uint mesh_index = instance.mesh_draw_index;
        MeshDraw mesh = mesh_draws[mesh_index];

        int_array_type index_buffer = int_array_type( mesh.index_buffer );
        int i0 = index_buffer[payload.primitive_id * 3].v;
        int i1 = index_buffer[payload.primitive_id * 3 + 1].v;
        int i2 = index_buffer[payload.primitive_id * 3 + 2].v;

        float_array_type vertex_buffer = float_array_type( mesh.position_buffer );

        vec4 p0 = vec4(
            vertex_buffer[i0 * 3 + 0].v,
            vertex_buffer[i0 * 3 + 1].v,
            vertex_buffer[i0 * 3 + 2].v,
            1.0
        );

        vec4 p1 = vec4(
            vertex_buffer[i1 * 3 + 0].v,
            vertex_buffer[i1 * 3 + 1].v,
            vertex_buffer[i1 * 3 + 2].v,
            1.0
        );

        vec4 p2 = vec4(
            vertex_buffer[i2 * 3 + 0].v,
            vertex_buffer[i2 * 3 + 1].v,
            vertex_buffer[i2 * 3 + 2].v,
            1.0
        );

        vec2_array_type uv_buffer = vec2_array_type( mesh.uv_buffer );
        vec2 uv0 = uv_buffer[i0].v;
        vec2 uv1 = uv_buffer[i1].v;
        vec2 uv2 = uv_buffer[i2].v;

        vec4 p0_world = vec4( instance.model * p0 );
        vec4 p1_world = vec4( instance.model * p1 );
        vec4 p2_world = vec4( instance.model * p2 );

        vec4 p0_screen = frame.view_projection * p0_world;
        vec4 p1_screen = frame.view_projection * p1_world;
        vec4 p2_screen = frame.view_projection * p2_world;

        ivec2 texture_size = textureSize( global_textures[nonuniformEXT( mesh.textures.x )], 0 );

        float texel_area = texture_size.x * texture_size.y *
            abs( ( uv1.x - uv0.x ) * ( uv2.y - uv0.y ) -
                 ( uv2.x - uv0.x ) * ( uv1.y - uv0.y ) );

        float triangle_area = abs( ( p1_screen.x - p0_screen.x ) * ( p2_screen.y - p0_screen.y ) -
                                   ( p2_screen.x - p0_screen.x ) * ( p1_screen.y - p0_screen.y ) );

        float lod = floor( 0.5 * log2( texel_area / triangle_area ) );

        float b = payload.barycentric_weights.x;
        float c = payload.barycentric_weights.y;
        float a = 1 - b - c;
        vec2 uv = a * uv0 + b * uv1 + c * uv2;

        reservoirSample.xs = reservoirSample.xv + reservoirSample.wi * payload.t;
        vec3 bounce_wo = normalize( reservoirSample.xv - reservoirSample.xs );

        float_array_type normals_buffer = float_array_type( mesh.normals_buffer );
        vec3 n0 = vec3(normals_buffer[ i0 * 3 + 0 ].v,
                       normals_buffer[ i0 * 3 + 1 ].v,
                       normals_buffer[ i0 * 3 + 2 ].v );

        vec3 n1 = vec3(normals_buffer[ i1 * 3 + 0 ].v,
                       normals_buffer[ i1 * 3 + 1 ].v,
                       normals_buffer[ i1 * 3 + 2 ].v );

        vec3 n2 = vec3(normals_buffer[ i2 * 3 + 0 ].v,
                       normals_buffer[ i2 * 3 + 1 ].v,
                       normals_buffer[ i2 * 3 + 2 ].v );

        vec3 object_normal = a * n0 + b * n1 + c * n2;

        //vec3 triangle_normal = normalize( cross( p1_world.xyz - p0_world.xyz, p2_world.xyz - p0_world.xyz ) );

        const mat3 normal_transform = mat3(instance.model_inverse);
        vec3 shading_normal = normalize( normal_transform * object_normal );

        vec3 normal = shading_normal;
        if ( dot( normal, bounce_wo ) < 0.0 ) {
            normal = -normal;
        }

        reservoirSample.ns = normal;

        vec4 bounce_albedo = textureLod( global_textures[nonuniformEXT( mesh.textures.x )], uv, lod );

        vec3 bounce_orm = calculate_pbr_parameters( mesh.metallic_roughness_occlusion_factor.x, mesh.metallic_roughness_occlusion_factor.y,
                                                    mesh.textures.y, mesh.metallic_roughness_occlusion_factor.z, mesh.textures.w, uv );

        reservoirSample.s_roughness = bounce_orm.g;
        reservoirSample.s_metalness = bounce_orm.b;
        reservoirSample.s_albedo = bounce_albedo.rgb;

        reservoirSample.lo = estimate_outgoing_radiance( reservoirSample.xs, reservoirSample.ns, bounce_wo,
                                                         bounce_albedo.rgb, bounce_orm.b, bounce_orm.g );

        reservoirSample.Fs = evaluate_brdf( wo, reservoirSample.wi, albedo, reservoirSample.xv, reservoirSample.nv, orm.b, orm.g );
        reservoirSample.p_hat = compute_phat( reservoirSample.lo, reservoirSample.Fs, reservoirSample.cos_theta );

        candidate_valid = reservoirSample.p_hat > 0.0 && !isnan( reservoirSample.p_hat ) && !isinf( reservoirSample.p_hat );
    }

    vec2 motion_vector = texelFetch( global_textures[restir_gi.motion_vectors_index], restir_xy, 0 ).rg;
    vec2 reprojected_uv = reproject_uv_from_ndc_motion( restir_screen_uv, motion_vector );
    ivec2 reprojected_uv_screen = ivec2( floor( reprojected_uv * vec2( restir_gi.resolution ) ) );

    bool similar_samples = check_temporal_similarity( restir_xy, reprojected_uv_screen, nv );

    Reservoir temporal_reservoir = empty_reservoir();

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
    bool debug_previous_valid = false;
    bool debug_temporal_visible = false;
    bool debug_phat_valid = false;
#endif // RESTIR_DEBUG_TEMPORAL_REUSE

    if ( similar_samples ) {
        int previous_index = reprojected_uv_screen.y * restir_gi.resolution.x + reprojected_uv_screen.x;

        //Reservoir previous_reservoir = temporal_reservoirs_read[previous_index];
        Reservoir previous_reservoir = unpack_reservoir( temporal_reservoirs_read[previous_index] );

        bool previous_valid = previous_reservoir.M > 0u && previous_reservoir.W > 0.0 && previous_reservoir.z.p_hat > 0.0;

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
        debug_previous_valid = previous_valid;
#endif

        if ( previous_valid ) {

            vec3 to_sample = previous_reservoir.z.xs - xv;
            float sample_dist = length( to_sample );

            if ( sample_dist > 1e-4 ) {
                vec3 wi_q = to_sample / sample_dist;
                float cos_q = max( dot( nv, wi_q ), 0.0 );

                bool temporal_visible = cos_q > 0.0 && check_visibility( xv, wi_q, sample_dist );

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
                debug_temporal_visible = temporal_visible;
#endif

                if ( temporal_visible ) {
                    vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );
                    float p_hat_q = compute_phat( previous_reservoir.z.lo, Fs_q, cos_q );

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
                    bool phat_valid = p_hat_q > 0.0 && !isnan( p_hat_q ) && !isinf( p_hat_q );
                    debug_phat_valid = phat_valid;
#endif

#if RESTIR_DEBUG_MODE != RESTIR_DEBUG_SAMPLE_ONLY
                    merge_reservoir( temporal_reservoir, previous_reservoir, p_hat_q, rand( rng_state ) );
#endif // RESTIR_DEBUG_SAMPLE_ONLY
                }
            }
        }
    }

    // Pixel hashed validation period to distribute the signal across frames
#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_SAMPLE_ONLY
    bool validation_frame = false;
#else
    const uint validation_period = 6u;
    uint validation_phase = seed( uvec2( restir_xy ) ) % validation_period;
    bool validation_frame = ( frame.current_frame % validation_period ) == validation_phase;
    //validation_frame = ( frame.current_frame % 6u ) == 0u;
#endif

    if ( validation_frame ) {
        temporal_reservoir = validate_reservoir( temporal_reservoir, xv, nv, wo, albedo.rgb, orm.g, orm.b );
    }

    // A failed new candidate does not invalidate temporal history.
    if ( candidate_valid ) {
        float wnew = reservoirSample.p_hat / reservoirSample.p_wi;
        reservoir_update( temporal_reservoir, reservoirSample, wnew, rand( rng_state ) );
    }

#if RESTIR_DEBUG_MODE != RESTIR_DEBUG_SAMPLE_ONLY
    if ( temporal_reservoir.M > 0u ) {
        if ( !reevaluate_selected_target( temporal_reservoir, xv, nv, wo, albedo, orm.b, orm.g ) ) {
            temporal_reservoir = empty_reservoir();
        }
    }

    if ( temporal_reservoir.M > MAX_TEMPORAL_M ) {
        float scale = float( MAX_TEMPORAL_M ) / float( temporal_reservoir.M );
        temporal_reservoir.w_sum *= scale;
        temporal_reservoir.M = MAX_TEMPORAL_M;
    }

    if ( temporal_reservoir.M > 0u && temporal_reservoir.z.p_hat > 0.0 ) {
        temporal_reservoir.W = temporal_reservoir.w_sum / ( float( temporal_reservoir.M ) * temporal_reservoir.z.p_hat );
    } else {
        temporal_reservoir.W = 0.0;
    }
#endif


#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_SIMILARITY
    temporal_reservoir.debug = similar_samples ? 1u : 0u;
#endif // RESTIR_DEBUG_TEMPORAL_SIMILARITY

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
    temporal_reservoir.debug = ( debug_previous_valid  ? 1u : 0u ) | ( debug_temporal_visible ? 2u : 0u ) | ( debug_phat_valid ? 4u : 0u );
#endif // RESTIR_DEBUG_TEMPORAL_REUSE

    //temporal_reservoirs_write[reservoir_index] = temporal_reservoir;
    temporal_reservoirs_write[reservoir_index] = pack_reservoir( temporal_reservoir );
}

#endif // RAYGEN_SAMPLE_GENERATION


#if defined(CLOSEST_HIT_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;
hitAttributeEXT vec2 barycentric_weights;

void main() {
    payload.instance_id = gl_InstanceCustomIndexEXT;
    payload.geometry_id = gl_GeometryIndexEXT;
    payload.primitive_id = gl_PrimitiveID;
    payload.barycentric_weights = barycentric_weights;
    payload.t = gl_HitTEXT;
    payload.triangle_facing = gl_HitKindEXT;
}

#endif // CLOSEST_HIT_SAMPLE_GENERATION


#if defined(MISS_SAMPLE_GENERATION)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;

void main() {
    payload.instance_id = -1;
    payload.geometry_id = -1;
    payload.primitive_id = -1;
}

#endif // MISS_SAMPLE_GENERATION


#if defined(COMPUTE_SPATIAL_SAMPLING)

#define SAMPLE_NUM 32

vec2 POISSON_SAMPLES[SAMPLE_NUM] = {
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
    vec2( 0.07481225966283056f, 0.6194024571949546f ),
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
    ivec2 restir_xy = ivec2( gl_GlobalInvocationID.xy );

    if ( any( greaterThanEqual( restir_xy, restir_gi.resolution ) ) ) {
        return;
    }

    const int reservoir_index = int( restir_xy.y * restir_gi.resolution.x + restir_xy.x );

    Reservoir spatial_reservoir = unpack_reservoir( temporal_reservoirs_write[reservoir_index] );

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_SAMPLE_ONLY
    vec3 color = spatial_reservoir.z.lo;

    imageStore( global_images_2d[ restir_gi.output_indirect_texture_index ], restir_xy, vec4( color, 1.0 ) );

    return;
#endif

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_REUSE
    uint debug = spatial_reservoir.debug;
    vec3 color = vec3( ( debug & 1u ) != 0u ? 1.0 : 0.0, ( debug & 2u ) != 0u ? 1.0 : 0.0, ( debug & 4u ) != 0u ? 1.0 : 0.0 );

    imageStore( global_images_2d[ restir_gi.output_indirect_texture_index ], restir_xy, vec4( color, 1.0 ) );

    return;
#endif // RESTIR_DEBUG_TEMPORAL_REUSE

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_SIMILARITY

    float valid = spatial_reservoir.debug != 0u ? 1.0 : 0.0;

    imageStore( global_images_2d[ restir_gi.output_indirect_texture_index ], restir_xy, vec4( vec3( valid ), 1.0 ) );

    return;

#endif // RESTIR_DEBUG_TEMPORAL_SIMILARITY

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_M
    float history = float( min( spatial_reservoir.M, MAX_TEMPORAL_M ) ) / float( MAX_TEMPORAL_M );

    imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( vec3( history * 1.0 ), 1.0 ) );

    return;
#endif // RESTIR_DEBUG_TEMPORAL_M

#if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_WEIGHT

    float weight = 0.0;

    if ( spatial_reservoir.M > 0u && spatial_reservoir.W > 0.0 ) {

        vec3 contribution = spatial_reservoir.z.lo * spatial_reservoir.z.Fs *
                            spatial_reservoir.z.cos_theta * spatial_reservoir.W;

        weight = luminance( contribution );
        weight = log2( 1.0 + weight );
    }

    imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( vec3( weight ), 1.0 ) );

    return;

#endif

    uint start_M = spatial_reservoir.M;

    uint num_iterations = spatial_reservoir.M < ( MAX_TEMPORAL_M / 2 ) ? MAX_ITER_HIGH : MAX_ITER_LOW;

    uint rng_state = seed( restir_xy ) + frame.current_frame;

    const vec4 guide = texelFetch( global_textures[restir_gi.normal_texture_index], restir_xy, 0 );

    vec2 encoded_normal = guide.rg;
    float raw_depth = guide.b;
    uint representative_index = uint( round( guide.a ) );
    ivec2 pos = restir_xy * 2 + representative_offsets[representative_index];

    //raw_depth = texelFetch( global_textures[restir_gi.depth_texture_index], pos, 0 ).r;

    const vec3 orm = texelFetch( global_textures[restir_gi.roughness_texture_index], pos, 0 ).rgb;
    const vec3 albedo = texelFetch( global_textures[restir_gi.albedo_texture_index], pos, 0 ).rgb;

    const vec2 screen_uv = uv_nearest( pos, frame.resolution );

    vec3 xv = world_position_from_depth( screen_uv, raw_depth, frame.inverse_view_projection );
    vec3 nv = octahedral_decode( encoded_normal );
    vec3 wo = normalize( frame.camera_position.xyz - xv );

    #if RESTIR_DEBUG_MODE == RESTIR_DEBUG_TEMPORAL_ONLY
    vec3 contribution = vec3( 0.0 );

    if ( spatial_reservoir.M > 0u && spatial_reservoir.W > 0.0 ) {
        // Reconstruct transient fields omitted from the packed reservoir.
        // Preserve the stored reservoir weight W.
        bool valid = reevaluate_selected_target( spatial_reservoir, xv, nv, wo, albedo, orm.b, orm.g );

        if ( valid ) {
            contribution = spatial_reservoir.z.lo * spatial_reservoir.z.Fs * spatial_reservoir.z.cos_theta * spatial_reservoir.W;
        }
    }

    imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( contribution, 1.0 ) );

    return;
#endif // RESTIR_DEBUG_TEMPORAL_ONLY

    //uint Q[MAX_ITER_HIGH];
    uint  QM[MAX_ITER_HIGH];
    ivec2 Qpos[MAX_ITER_HIGH];
    uint q_count = 0;

    for ( uint s = 0; s < num_iterations; ++s ) {
        uint next_rnd = rand_pcg( rng_state ) % SAMPLE_NUM;

        ivec2 qn = restir_xy + ivec2( POISSON_SAMPLES[next_rnd] * 15.0 );

        if ( qn.x < 0 || qn.x >= restir_gi.resolution.x ||
             qn.y < 0 || qn.y >= restir_gi.resolution.y ) {
            continue;
        }

        int q_index = qn.y * restir_gi.resolution.x + qn.x;
        Reservoir rn = unpack_reservoir( temporal_reservoirs_write[q_index] );

        if ( rn.M == 0u || rn.W <= 0.0 || rn.z.p_hat <= 0.0 ) {
            continue;
        }

        if ( !check_spatial_similarity( restir_xy, qn ) ) {
            continue;
        }

        vec4 q_guide = texelFetch( global_textures[restir_gi.normal_texture_index], qn, 0 );

        float q_depth = q_guide.b;
        uint q_ri = uint( round( q_guide.a ) );
        ivec2 q_full_res = qn * 2 + representative_offsets[q_ri];
        //q_depth = texelFetch( global_textures[restir_gi.depth_texture_index], q_full_res, 0 ).r;
        vec2 q_uv = uv_nearest( q_full_res, frame.resolution );

        vec3 q_xv = world_position_from_depth( q_uv, q_depth, frame.inverse_view_projection );

        vec3 qq_dir = q_xv - rn.z.xs;
        vec3 rq_dir = xv - rn.z.xs;

        float qq_len_sq = dot( qq_dir, qq_dir );
        float rq_len_sq = dot( rq_dir, rq_dir );

        if ( rq_len_sq <= 1e-8 || qq_len_sq <= 1e-8 ) {
            continue;
        }

        qq_dir = normalize( qq_dir );
        rq_dir = normalize( rq_dir );

        float cos_phi_q = abs( dot( qq_dir, rn.z.ns ) );
        float cos_phi_r = abs( dot( rq_dir, rn.z.ns ) );

        if ( cos_phi_q <= MIN_SAMPLE_NORMAL_COS || cos_phi_r <= MIN_SAMPLE_NORMAL_COS ) {
            continue;
        }

        float jq = ( cos_phi_r / cos_phi_q ) * ( qq_len_sq / rq_len_sq );

        if ( jq < MIN_RESAMPLING_JACOBIAN || jq > MAX_RESAMPLING_JACOBIAN ) {
            continue;
        }

#if 1
        vec3 ray_dir = rn.z.xs - xv;
        float max_dist = length( ray_dir );

        if ( max_dist <= 1e-4 ) {
            continue;
        }

        vec3 wi_q = ray_dir / max_dist;
        float cos_q = max( dot( nv, wi_q ), 0.0 );

        vec3 Fs_q = evaluate_brdf(
            wo, wi_q, albedo, xv, nv, orm.b, orm.g
        );

        float p_hat_q = compute_phat( rn.z.lo, Fs_q, cos_q );
        float p_hat_prime = p_hat_q / abs( jq );

        // Reject zero or invalid targets before tracing visibility.
        if ( p_hat_prime <= 0.0 || isnan( p_hat_prime ) || isinf( p_hat_prime ) ) {
            continue;
        }

        if ( !check_visibility( xv, wi_q, max_dist ) ) {
            continue;
        }
#else
        vec3 ray_dir = rn.z.xs - xv;
        float max_dist = length( ray_dir );

        if ( max_dist <= 1e-4 ) {
            continue;
        }

        bool visible = check_visibility( xv, ray_dir / max_dist, max_dist );

        float p_hat_prime = 0.0;

        if ( visible ) {
            vec3 wi_q = ray_dir / max_dist;
            float cos_q = max( dot( nv, wi_q ), 0.0 );

            vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );

            float p_hat_q = compute_phat( rn.z.lo, Fs_q, cos_q );
            p_hat_prime = p_hat_q / abs( jq );
        }

        if ( p_hat_prime <= 0.0 || isnan( p_hat_prime ) || isinf( p_hat_prime ) ) {
            continue;
        }
#endif

        merge_reservoir( spatial_reservoir, rn, p_hat_prime, rand( rng_state ) );

        Qpos[q_count] = qn;
        QM[q_count++] = rn.M;//q_index;
    }

    if ( spatial_reservoir.M > MAX_SPATIAL_M ) {
        float scale = float( MAX_SPATIAL_M ) / float( spatial_reservoir.M );
        spatial_reservoir.w_sum *= scale;
        spatial_reservoir.M = MAX_SPATIAL_M;
    }

    if ( spatial_reservoir.M == 0u ) {
        //spatial_reservoirs[reservoir_index] = pack_reservoir( empty_reservoir() );

        imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( 0.0 ) );
        return;
    }

    float Z = 0.0;

    {
        vec3 wi_dir = spatial_reservoir.z.xs - xv;
        float max_dist = length( wi_dir );

        bool valid = max_dist > 0.025;

        if ( valid ) {
            vec3 wi_q = wi_dir / max_dist;
            float cos_q = max( dot( nv, wi_q ), 0.0 );

            vec3 Fs_q = evaluate_brdf( wo, wi_q, albedo, xv, nv, orm.b, orm.g );

            float p_hat_qn = compute_phat( spatial_reservoir.z.lo, Fs_q, cos_q );

            valid = p_hat_qn > 0.0;

            if ( valid ) {
                valid = check_visibility( xv, wi_q, max_dist );
            }
        }

        if ( valid ) {
            Z += start_M;
        }
    }

    for ( uint qn = 0; qn < q_count; ++qn ) {
        //Reservoir rn = temporal_reservoirs_write[Q[qn]];
        uint rn_M = QM[qn];
        ivec2 qnp = Qpos[qn];

        vec4 q_guide = texelFetch( global_textures[restir_gi.normal_texture_index], qnp, 0 );

        float q_depth = q_guide.b;
        uint q_ri = uint( round( q_guide.a ) );
        ivec2 q_full_res = qnp * 2 + representative_offsets[q_ri];
        //q_depth = texelFetch( global_textures[restir_gi.depth_texture_index], q_full_res, 0 ).r;
        vec2 q_uv = uv_nearest( q_full_res, frame.resolution );

        vec3 q_xv = world_position_from_depth( q_uv, q_depth, frame.inverse_view_projection );
        vec3 q_nv = octahedral_decode( texelFetch( global_textures[restir_gi.normal_texture_index], qnp, 0 ).rg );

        vec3 wi_dir = spatial_reservoir.z.xs - q_xv;
        float max_dist = length( wi_dir );

        if ( max_dist <= 0.025 ) {
            continue;
        }

        vec3 wo_q = normalize( frame.camera_position.xyz - q_xv );
        vec3 wi_q = wi_dir / max_dist;
        float cos_q = max( dot( q_nv, wi_q ), 0.0 );

        const vec3 q_albedo = texelFetch( global_textures[restir_gi.albedo_texture_index], q_full_res, 0 ).rgb;
        const vec3 q_orm = texelFetch( global_textures[restir_gi.roughness_texture_index], q_full_res, 0 ).rgb;

        vec3 Fs_q = evaluate_brdf( wo_q, wi_q, q_albedo, q_xv, q_nv, q_orm.b, q_orm.g );

        float p_hat_qn = compute_phat( spatial_reservoir.z.lo, Fs_q, cos_q );

        if ( p_hat_qn <= 0.0 ) {
            continue;
        }

        if ( !check_visibility( q_xv, wi_q, max_dist ) ) {
            continue;
        }

        Z += rn_M;
    }

    vec3 wi_dir = spatial_reservoir.z.xs - xv;
    float wi_dist = length( wi_dir );

    if ( wi_dist <= 1e-4 ) {
        spatial_reservoir = empty_reservoir();
        //spatial_reservoirs[reservoir_index] = pack_reservoir( spatial_reservoir );

        imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( 0.0 ) );
        return;
    }

    vec3 wi = wi_dir / wi_dist;
    float cosTheta = max( dot( nv, wi ), 0.0 );

    vec3 Fs = evaluate_brdf( wo, wi, albedo, xv, nv, orm.b, orm.g );

    float p_hat_selected = compute_phat( spatial_reservoir.z.lo, Fs, cosTheta );

    if ( Z > 0.0 && p_hat_selected > 0.0 ) {
        spatial_reservoir.W = spatial_reservoir.w_sum / ( Z * p_hat_selected );
    } else {
        spatial_reservoir.W = 0.0;
    }

    //spatial_reservoirs[reservoir_index] = pack_reservoir( spatial_reservoir );

    // Fs is part of the final estimator.
    vec3 indirect_contribution = spatial_reservoir.z.lo * Fs * cosTheta * spatial_reservoir.W;

    imageStore( global_images_2d[restir_gi.output_indirect_texture_index], restir_xy, vec4( indirect_contribution, 1.0 ) );
}

#endif // COMPUTE_SPATIAL_SAMPLING


#if defined(COMPUTE_TEMPORAL_ACCUMULATION)

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;

void main() {
    ivec2 pos = ivec2( gl_GlobalInvocationID.xy );

    if ( any( greaterThanEqual( pos, restir_gi.resolution ) ) ) {
        return;
    }

    const vec2 screen_uv = uv_nearest( pos, restir_gi.resolution );

    vec3 indirect_contribution = texelFetch( global_textures[restir_gi.output_indirect_texture_index], pos, 0 ).rgb;
    vec2 motion_vector = texelFetch( global_textures[restir_gi.motion_vectors_index], pos, 0 ).rg;

    vec2 reprojected_uv = reproject_uv_from_ndc_motion( screen_uv, motion_vector );
    ivec2 reprojected_uv_screen = ivec2( floor( reprojected_uv * vec2( restir_gi.resolution ) ) );

    const vec2 encoded_normal = texelFetch( global_textures[restir_gi.normal_texture_index], pos, 0 ).rg;
    vec3 nv = octahedral_decode( encoded_normal );

    bool similar_samples = check_temporal_similarity( pos, reprojected_uv_screen, nv );

    if ( similar_samples && frame.current_frame > 0u ) {
        vec4 history_value = texelFetch( global_textures[restir_gi.output_history_texture_index], reprojected_uv_screen, 0 );

        vec3 nmin = vec3( 1e20 );
        vec3 nmax = vec3( -1e20 );

        for ( int y = -1; y <= 1; ++y ) {
            for ( int x = -1; x <= 1; ++x ) {
                ivec2 q = clamp( pos + ivec2( x, y ), ivec2( 0 ), restir_gi.resolution - ivec2( 1 ) );

                vec3 c = texelFetch( global_textures[restir_gi.output_indirect_texture_index], q, 0 ).rgb;

                nmin = min( nmin, c );
                nmax = max( nmax, c );
            }
        }

        vec3 history_gi = clamp( history_value.rgb, nmin, nmax );

        uint old_count = uint( clamp( history_value.a, 0.0, 256.0 ) );
        uint new_count = min( old_count + 1, 256u );
        float alpha = max( 1.0 / float( new_count ), 0.025 );

        vec3 accumulated_gi = mix( history_gi, indirect_contribution, alpha );

        imageStore( global_images_2d[restir_gi.output_texture_index], pos, vec4( accumulated_gi, float( new_count ) ) );

    } else {
        imageStore( global_images_2d[restir_gi.output_texture_index], pos, vec4( indirect_contribution, 1.0 ) );
    }
}

#endif // COMPUTE_TEMPORAL_ACCUMULATION

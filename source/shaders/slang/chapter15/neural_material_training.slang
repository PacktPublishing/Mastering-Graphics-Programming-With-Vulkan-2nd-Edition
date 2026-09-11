#ifndef NEURAL_MATERIAL_TRAINING_SLANG
#define NEURAL_MATERIAL_TRAINING_SLANG

#include "platform.slangh"

#include "neural_material_brdf.slangh"
#include "neural_material_structs.h"
#include "neural_material_common.slangh"
#include "neural_material_layers.slangh"

#define WORKGROUP_COUNT_X 32

// Random number generators //////////////////////////////////////////////
struct LCG
{
    uint state;

    __init(uint seed) { state = seed; }

    [mutating]
    uint next_uint()
    {
        const uint A = 1664525u;
        const uint C = 1013904223u;
        state = (A * state + C);
        return state;
    }

    [mutating]
    float next_float()
    {
        // Convert to float in range [0, 1)
        return (next_uint() >> 8) * 0x1p-24;
    }
};

float radical_inverse_vdc(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 hammersley(uint i, uint n) {
    return float2(float(i) / float(n), radical_inverse_vdc(i));
}

// Utilities /////////////////////////////////////////////////////////////

void tangents(float3 n, out float3 s, out float3 t)
{
    const float3 u = abs(n.x) < 0.95 ? float3(1, 0, 0) : float3(0, 1, 0);
    s = normalize(u - n * dot(n, u));
    t = cross(n, s);
}

// Mollification /////////////////////////////////////////////////////////

float3 sample_uniform_cone(float3 axis, float cone_angle_rad, float2 u) {
    axis = safe_normalize(axis);

    float3 tangent;
    float3 bitangent;
    build_tangent_frame(axis, tangent, bitangent);

    const float cos_theta_max = cos(cone_angle_rad);

    // Uniform sampling over cone solid angle.
    const float cos_theta = lerp(cos_theta_max, 1.0f, u.x);
    const float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = 2.0f * PI * u.y;

    return safe_normalize(tangent * (sin_theta * cos(phi)) +
                          bitangent * (sin_theta * sin(phi)) +
                          axis * cos_theta);
}

float get_mollification_cone_angle(uint epoch) {
    const float max_angle_rad = radians(10.0f);
    const float mollification_steps = 20000.0f;

    const float t = saturate(float(epoch) / mollification_steps);

    // 10 degrees -> 0 degrees with cosine decay.
    return 0.5f * max_angle_rad * (1.0f + cos(t * PI));
}

float get_mollification_roughness(uint epoch, float target_roughness) {
    const float mollification_steps = 20000.0f;

    const float t = saturate(float(epoch) / mollification_steps);

    return lerp(0.6f, target_roughness, t);
}

float3 eval_disney_brdf_mollified(float3 albedo, float3 view_dir_local,
                                  float3 light_dir_local, float3 normal,
                                  float roughness, float metallic, float specular,
                                  uint epoch, inout LCG lcg) {
    const float cone_angle = get_mollification_cone_angle(epoch);

    roughness = get_mollification_roughness(epoch, roughness);

    // When cone is basically closed, use the true BRDF.
    if (cone_angle <= radians(0.01f)) {
        return eval_disney_brdf(albedo, light_dir_local, view_dir_local,
                                normal, roughness, metallic, specular);
    }

    // NVIDIA uses 256 samples.
    const uint sample_count = 64u;

    float3 sum = 0.0f.xxx;

    float2 cone_jitter = float2(lcg.next_float(), lcg.next_float());

    [loop]
    for (uint i = 0u; i < sample_count; ++i) {
        float2 uv = frac(hammersley(i, sample_count) + cone_jitter);

        float3 perturbed_light_dir = sample_uniform_cone(light_dir_local, cone_angle, uv);

        sum += eval_disney_brdf(albedo, perturbed_light_dir, view_dir_local,
                                normal, roughness, metallic, specular);
    }

    return sum / float(sample_count);
}

// Shaders ///////////////////////////////////////////////////////////////
#if defined(COMPUTE_COMPUTE_RANDOM_BATCH) || defined(COMPUTE_TRAIN_BRDF_WITH_ENCODER) || defined(COMPUTE_TRAIN_BRDF_WITH_LATENT_TEXTURE)
ConstantBuffer<NMRandomBatchConstants> g_random_batch_constants : register(b0, space1);
#endif

#if defined(COMPUTE_COMPUTE_RANDOM_BATCH)
[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    const uint batch_index = dispatch_thread_id.x;
    if (batch_index >= g_random_batch_constants.batch_size)
    {
        return;
    }

    RWStructuredBuffer<float> output_buffer = global_buffers_rw_f32[g_random_batch_constants.output_buffer_index];
    Texture2D<float4> normal_map = texture_2d(g_random_batch_constants.normal_map_index);
    Texture2D<float> roughness_map = global_textures_f32[g_random_batch_constants.roughness_map_index];

    const uint sample_seed = wang_hash(g_random_batch_constants.seed ^ batch_index);
    LCG lcg = LCG(sample_seed);

    uint width, height;
    normal_map.GetDimensions(width, height);

    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(g_random_batch_constants.batch_size);
    uv = float2(lcg.next_float(), lcg.next_float());

    int2 pixel = int2(uv * float2(width, height));

    float3 n = normal_map.Load(int3(pixel.x, pixel.y, 0)).xyz;
    n = safe_normalize(n * 2.0f - 1.0f);

    const float roughness = roughness_map.Load(int3(pixel.x, pixel.y, 0));

    float3 t;
    float3 b;
    build_tangent_frame(n, t, b);

    const uint base_index = batch_index * k_neural_material_input_size;

    output_buffer[base_index + 0] = n.x;
    output_buffer[base_index + 1] = n.y;
    output_buffer[base_index + 2] = n.z;

    output_buffer[base_index + 3] = t.x;
    output_buffer[base_index + 4] = t.y;
    output_buffer[base_index + 5] = t.z;

    // Placeholder albedo.
    float3 base_paint_color = float3(0.5843139, 0.19215536, 0.000000);
    output_buffer[base_index + 6] = base_paint_color.x;
    output_buffer[base_index + 7] = base_paint_color.y;
    output_buffer[base_index + 8] = base_paint_color.z;

    output_buffer[base_index + 9] = roughness;
    output_buffer[base_index + 10] = 1.0f;
}
#endif

#if defined(COMPUTE_TRAIN_BRDF_WITH_ENCODER)
ConstantBuffer<NMBrdfTrainingConstants> g_brdf_training_constants : register(b1, space1);

// float3 eval_reference_brdf(float3 n, float roughness, float2 uv)
// {
//     // Placeholder baseline until the full Disney/MaterialX target is ported.
//     const float3 base_color = float3(0.5843139, 0.19215536, 0.000000);
//     const float ndotl = saturate(dot(n, safe_normalize(float3(0.3f, 0.4f, 0.85f))));
//     return base_color * (0.1f + 0.9f * ndotl);
// }

[Differentiable]
float3 compute_grads_for_brdf_with_encoder(no_diff float input_values[11], no_diff float3 wi, no_diff float3 wo, no_diff float3 target) {
    float latent_values[8];

    encoder_eval(g_brdf_training_constants.encoder, input_values, latent_values);

    float3 predicted = eval_neural_brdf(latent_values, wi, wo,
                                        g_brdf_training_constants.frame_decoder,
                                        g_brdf_training_constants.brdf_decoder);

    float3 loss = l1_log_loss3(predicted, target);

    return loss;
}

[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint batch_index = dispatch_thread_id.x;
    if (batch_index >= g_brdf_training_constants.batch_size) {
        return;
    }

    const uint global_batch_index = g_brdf_training_constants.batch_offset + batch_index;

    // Baseline stub:
    // forward path is already available and differentiable.
    // Full backward/training logic can be filled in next.
    StructuredBuffer<float> input_buffer = global_buffers_f32[g_brdf_training_constants.input_buffer_index];

    float input_values[11];

    const uint base_index = global_batch_index * 11;
    [unroll]
    for (uint i = 0; i < 11; ++i) {
        input_values[i] = input_buffer[base_index + i];
    }

    const uint sample_seed = wang_hash(g_random_batch_constants.seed ^ global_batch_index);
    LCG lcg = LCG(sample_seed);
    float2 uv = (global_batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(g_brdf_training_constants.total_batch_size);
    uv = float2(lcg.next_float(), lcg.next_float());

    const float3 normal = float3(input_values[0], input_values[1], input_values[2]);
    const float3 tangent = float3(input_values[3], input_values[4], input_values[5]);
    const float3 bitangent = normalize(cross(normal, tangent));
    const float roughness = input_values[9];
    const float3 albedo = float3(input_values[6], input_values[7], input_values[8]);

    float phi_h = lcg.next_float() * 2.0 * PI;
    float theta_h = lcg.next_float() * 0.5 * PI;
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d = lcg.next_float() * 2.0 * PI;
    float theta_d = lcg.next_float() * 0.5 * PI;
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame);
    float3 wo = 2 * dot(wi, h) * h - wi;

    float3 target = eval_disney_brdf_mollified(albedo, wo, wi, normal, roughness,
                                               0.0 /* metallic */, 1.0 /* specular */,
                                               g_random_batch_constants.step, lcg);

    no_diff float3 loss = compute_grads_for_brdf_with_encoder(input_values, wi, wo, target);
    global_buffers_rw_f32[g_brdf_training_constants.loss_buffer_index][global_batch_index * 3 + 0] = loss.x;
    global_buffers_rw_f32[g_brdf_training_constants.loss_buffer_index][global_batch_index * 3 + 1] = loss.y;
    global_buffers_rw_f32[g_brdf_training_constants.loss_buffer_index][global_batch_index * 3 + 2] = loss.z;

    const float loss_grad_scale = 1.0f / float(g_brdf_training_constants.total_batch_size);
    bwd_diff(compute_grads_for_brdf_with_encoder)(input_values, wi, wo, target, loss_grad_scale);
}
#endif

#if defined(COMPUTE_TRAIN_BRDF_WITH_LATENT_TEXTURE)
ConstantBuffer<NMBrdfLatentTrainingConstants> g_brdf_latent_training_constants : register(b1, space1);

[Differentiable]
float3 compute_grads_for_brdf_with_latent_texture(no_diff float input_values[11], no_diff float2 uv, no_diff float3 wi, no_diff float3 wo, no_diff uint lod, no_diff float3 target) {

    float latent_values[8];
    latent_eval(g_brdf_latent_training_constants.latent_pyramid, uv, lod, latent_values);

    float3 predicted = eval_neural_brdf(
        latent_values, wi, wo,
        g_brdf_latent_training_constants.frame_decoder,
        g_brdf_latent_training_constants.brdf_decoder);

    float3 loss = l1_log_loss3(predicted, target);

    return loss;
}

[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    const uint batch_index = dispatch_thread_id.x;
    if (batch_index >= g_brdf_latent_training_constants.batch_size) {
        return;
    }

    const uint global_batch_index = g_brdf_latent_training_constants.batch_offset + batch_index;

    StructuredBuffer<float> input_buffer = global_buffers_f32[g_brdf_latent_training_constants.input_buffer_index];
    float input_values[11];

    const uint base_index = global_batch_index * 11;
    [unroll]
    for (uint i = 0; i < 11; ++i) {
        input_values[i] = input_buffer[base_index + i];
    }

    const uint sample_seed = wang_hash(g_random_batch_constants.seed ^ global_batch_index);
    LCG lcg = LCG(sample_seed);
    float2 uv = (global_batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(g_brdf_latent_training_constants.total_batch_size);
    uv = float2(lcg.next_float(), lcg.next_float());

    const float3 normal = float3(input_values[0], input_values[1], input_values[2]);
    const float3 tangent = float3(input_values[3], input_values[4], input_values[5]);
    const float3 bitangent = normalize(cross(normal, tangent));
    const float roughness = input_values[9];
    const float3 albedo = float3(input_values[6], input_values[7], input_values[8]);

    float phi_h = lcg.next_float() * 2.0 * PI;
    float theta_h = lcg.next_float() * 0.5 * PI;
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d = lcg.next_float() * 2.0 * PI;
    float theta_d = lcg.next_float() * 0.5 * PI;
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame);
    float3 wo = 2 * dot(wi, h) * h - wi;

    Texture2D<float4> normal_map = texture_2d(g_random_batch_constants.normal_map_index);
    uint width, height;
    normal_map.GetDimensions(width, height);
    int2 pixel = int2(uv * float2(width, height));

    float3 target = eval_disney_brdf_mollified(albedo, wo, wi, normal, roughness,
                                               0.0 /* metallic */, 1.0 /* specular */,
                                               g_brdf_latent_training_constants.mollification_step, lcg);

    uint lod = 0; // uint(lcg.next_float() * g_brdf_latent_training_constants.latent_pyramid.level_count);

    no_diff float3 loss = compute_grads_for_brdf_with_latent_texture(input_values, uv, wi, wo, lod, target);
    global_buffers_rw_f32[g_brdf_latent_training_constants.loss_buffer_index][global_batch_index * 3 + 0] = loss.x;
    global_buffers_rw_f32[g_brdf_latent_training_constants.loss_buffer_index][global_batch_index * 3 + 1] = loss.y;
    global_buffers_rw_f32[g_brdf_latent_training_constants.loss_buffer_index][global_batch_index * 3 + 2] = loss.z;

    const float loss_grad_scale = 1.0f / float(g_brdf_latent_training_constants.total_batch_size);
    bwd_diff(compute_grads_for_brdf_with_latent_texture)(input_values, uv, wi, wo, lod, target, loss_grad_scale);
}
#endif

#if defined(COMPUTE_TRAIN_SAMPLER_WITH_ENCODER)
ConstantBuffer<NMSamplerTrainingConstants> g_sampler_training_constants : register(b0, space1);

[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint batch_index = dispatch_thread_id.x;
    if (batch_index >= g_sampler_training_constants.batch_size) {
        return;
    }

    StructuredBuffer<float> input_buffer = global_buffers_f32[g_sampler_training_constants.input_buffer_index];
    float input_values[11];
    float latent_values[8];
    float sampler_output[9];

    const uint base_index = batch_index * 11;
    [unroll]
    for (uint i = 0; i < 11; ++i) {
        input_values[i] = input_buffer[base_index + i];
    }

    encoder_eval(g_sampler_training_constants.encoder, input_values, latent_values);
    sampler_decoder_eval(g_sampler_training_constants.sampler_decoder, input_values, sampler_output);
}
#endif

#if defined(COMPUTE_TRAIN_SAMPLER_WITH_LATENT_TEXTURE)
ConstantBuffer<NMSamplerLatentTrainingConstants> g_sampler_latent_training_constants : register(b0, space1);

[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint batch_index = dispatch_thread_id.x;
    if (batch_index >= g_sampler_latent_training_constants.batch_size) {
        return;
    }

    StructuredBuffer<float> input_buffer = global_buffers_f32[g_sampler_latent_training_constants.input_buffer_index];
    float input_values[11];
    float sampler_output[9];

    const uint base_index = batch_index * 11;
    [unroll]
    for (uint i = 0; i < 11; ++i) {
        input_values[i] = input_buffer[base_index + i];
    }

    sampler_decoder_eval(g_sampler_latent_training_constants.sampler_decoder, input_values, sampler_output);
}
#endif

#if defined(COMPUTE_COPY_ENCODER_TO_LATENT_TEXTURE) || defined(COMPUTE_DOWNSAMPLE_LATENT_PYRAMID)
ConstantBuffer<NMCopyEncoderToLatentConstants> g_copy_encoder_to_latent_constants : register(b0, space1);
#endif

#if defined(COMPUTE_COPY_ENCODER_TO_LATENT_TEXTURE)
[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint x = dispatch_thread_id.x;
    const uint y = dispatch_thread_id.y;

    if (x >= g_copy_encoder_to_latent_constants.width ||
        y >= g_copy_encoder_to_latent_constants.height) {
        return;
    }

    Texture2D<float4> normal_map = texture_2d(g_copy_encoder_to_latent_constants.normal_map_index);
    Texture2D<float> roughness_map = global_textures_f32[g_copy_encoder_to_latent_constants.roughness_map_index];

    RWTexture2D<float4> latent0 = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures0_uav.get(0)];
    RWTexture2D<float4> latent1 = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures1_uav.get(0)];

    float3 n = normal_map.Load(int3(x, y, 0)).xyz;
    n = safe_normalize(n * 2.0f - 1.0f);

    const float roughness = roughness_map.Load(int3(x, y, 0));

    float3 t;
    float3 b;
    build_tangent_frame(n, t, b);

    float3 base_paint_color = float3(0.5843139, 0.19215536, 0.000000);

    float input_values[11];
    input_values[0] = n.x;
    input_values[1] = n.y;
    input_values[2] = n.z;
    input_values[3] = t.x;
    input_values[4] = t.y;
    input_values[5] = t.z;
    input_values[6] = base_paint_color.x;
    input_values[7] = base_paint_color.y;
    input_values[8] = base_paint_color.z;
    input_values[9] = roughness;
    input_values[10] = 1.0f;

    float latent_values[8];
    encoder_eval(g_copy_encoder_to_latent_constants.encoder, input_values, latent_values);

    latent0[int2(x, y)] = float4(latent_values[0], latent_values[1], latent_values[2], latent_values[3]);
    latent1[int2(x, y)] = float4(latent_values[4], latent_values[5], latent_values[6], latent_values[7]);
}
#endif

#if defined(COMPUTE_DOWNSAMPLE_LATENT_PYRAMID)
[[vk::push_constant]]
ConstantBuffer<uint2> g_latent_texture_src_dst;

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id: SV_DispatchThreadID)
{
    // 2x2 source texel positions
    int2 s00 = dispatch_thread_id.xy * 2;
    int2 s10 = s00 + int2(1, 0);
    int2 s01 = s00 + int2(0, 1);
    int2 s11 = s00 + int2(1, 1);

    uint src_level = g_latent_texture_src_dst.x;
    uint dst_level = g_latent_texture_src_dst.y;

    RWTexture2D<float4> latent0_src = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures0_uav.get(src_level)];
    RWTexture2D<float4> latent1_src = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures1_uav.get(src_level)];

    // Box-filter latents from src_level
    float4 avg0 = (latent0_src[s00] +
                   latent0_src[s10] +
                   latent0_src[s01] +
                   latent0_src[s11]) * 0.25f;

    float4 avg1 = (latent1_src[s00] +
                   latent1_src[s10] +
                   latent1_src[s01] +
                   latent1_src[s11]) * 0.25f;

    RWTexture2D<float4> latent0_dst = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures0_uav.get(dst_level)];
    RWTexture2D<float4> latent1_dst = global_images_2d[g_copy_encoder_to_latent_constants.latent_pyramid.textures1_uav.get(dst_level)];

    latent0_dst[int2(dispatch_thread_id.xy)] = avg0;
    latent1_dst[int2(dispatch_thread_id.xy)] = avg1;
}
#endif

#if defined(COMPUTE_OPTIMIZER_STEP_BUFFER)
ConstantBuffer<NMOptimizerBufferConstants> g_optimizer_buffer_constants : register(b0, space1);

[numthreads(WORKGROUP_COUNT_X, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint index = dispatch_thread_id.x;
    if (index >= g_optimizer_buffer_constants.element_count) {
        return;
    }

#if defined(RAPTOR_USE_COOPVEC)
    RWStructuredBuffer<half> values = global_buffers_rw_f16[g_optimizer_buffer_constants.value_buffer_index];
    RWStructuredBuffer<half> grads = global_buffers_rw_f16[g_optimizer_buffer_constants.grad_buffer_index];
    RWStructuredBuffer<float> exp_avg = global_buffers_rw_f32[g_optimizer_buffer_constants.exp_avg_buffer_index];
    RWStructuredBuffer<float> exp_avg_sq = global_buffers_rw_f32[g_optimizer_buffer_constants.exp_avg_sq_buffer_index];
#else
    RWStructuredBuffer<float> values = global_buffers_rw_f32[g_optimizer_buffer_constants.value_buffer_index];
    RWStructuredBuffer<float> grads = global_buffers_rw_f32[g_optimizer_buffer_constants.grad_buffer_index];
    RWStructuredBuffer<float> exp_avg = global_buffers_rw_f32[g_optimizer_buffer_constants.exp_avg_buffer_index];
    RWStructuredBuffer<float> exp_avg_sq = global_buffers_rw_f32[g_optimizer_buffer_constants.exp_avg_sq_buffer_index];
#endif

    float value = float(values[index]);
    float grad = float(grads[index]);
    float m = float(exp_avg[index]);
    float v = float(exp_avg_sq[index]);

    optimizer_step( value, grad, m, v,
        g_optimizer_buffer_constants.learning_rate,
        g_optimizer_buffer_constants.optimize_counter);

    values[index] = half(value);
    grads[index] = half(grad);
    exp_avg[index] = m;
    exp_avg_sq[index] = v;
}
#endif

#if defined(COMPUTE_OPTIMIZER_STEP_TEXTURE)
ConstantBuffer<NMOptimizerTextureConstants> g_optimizer_texture_constants : register(b0, space1);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {

    const uint x = dispatch_thread_id.x;
    const uint y = dispatch_thread_id.y;

    if (x >= g_optimizer_texture_constants.width ||
        y >= g_optimizer_texture_constants.height) {
        return;
    }

    RWTexture2D<float4> values = global_images_2d[g_optimizer_texture_constants.values_texture_index];
    RWTexture2D<float4> exp_avg = global_images_2d[g_optimizer_texture_constants.exp_avg_texture_index];
    RWTexture2D<float4> exp_avg_sq = global_images_2d[g_optimizer_texture_constants.exp_avg_sq_texture_index];
    RWStructuredBuffer<float> grads = global_buffers_rw_f32[g_optimizer_texture_constants.grads_texture_index];

    const int2 pixel = int2(x, y);
    const uint grad_index = (y * g_optimizer_texture_constants.width + x) * 4;

    float4 value = values[pixel];
    float4 grad = float4(
        grads[grad_index + 0],
        grads[grad_index + 1],
        grads[grad_index + 2],
        grads[grad_index + 3]);
    float4 m = exp_avg[pixel];
    float4 v = exp_avg_sq[pixel];

    optimizer_step(value.x, grad.x, m.x, v.x, g_optimizer_texture_constants.learning_rate, g_optimizer_texture_constants.optimize_counter);
    optimizer_step(value.y, grad.y, m.y, v.y, g_optimizer_texture_constants.learning_rate, g_optimizer_texture_constants.optimize_counter);
    optimizer_step(value.z, grad.z, m.z, v.z, g_optimizer_texture_constants.learning_rate, g_optimizer_texture_constants.optimize_counter);
    optimizer_step(value.w, grad.w, m.w, v.w, g_optimizer_texture_constants.learning_rate, g_optimizer_texture_constants.optimize_counter);

    values[pixel] = value;
    grads[grad_index + 0] = grad.x;
    grads[grad_index + 1] = grad.y;
    grads[grad_index + 2] = grad.z;
    grads[grad_index + 3] = grad.w;
    exp_avg[pixel] = m;
    exp_avg_sq[pixel] = v;
}
#endif

#endif // NEURAL_MATERIAL_TRAINING_SLANG


#if !defined(__NEURAL_MATERIAL_STRUCTS_H__)
#define __NEURAL_MATERIAL_STRUCTS_H__

#include "shared_converter.h"

#ifdef __SLANG__
#include "platform.slangh"

#if defined(RAPTOR_USE_COOPVEC)
// Adapted from Slang Siggraph 2025 course
static const CoopVecComponentType k_neural_material_coopvec_component_type = CoopVecComponentType::Float16;

public struct NeuralMaterialCoopVec<let N : int> : IDifferentiable {
    CoopVec<float, N> data;
    typealias Differential = NeuralMaterialCoopVec<N>;

    static NeuralMaterialCoopVec<N> from_array(const float values[N]) {
        NeuralMaterialCoopVec<N> result;
        [ForceUnroll]
        for (int i = 0; i < N; ++i) {
            result.data[i] = values[i];
        }
        return result;
    }

    static float[N] to_array(NeuralMaterialCoopVec<N> vec) {
        float result[N];
        [ForceUnroll]
        for (int i = 0; i < N; ++i) {
            result[i] = float(vec.data[i]);
        }
        return result;
    }

    [BackwardDerivativeOf(from_array)]
    static void from_array_bwd(inout DifferentialPair<float[N]> values, NeuralMaterialCoopVec<N> result_grad) {
        values = diffPair(values.p, to_array(result_grad));
    }

    [BackwardDerivativeOf(to_array)]
    static void to_array_bwd(inout DifferentialPair<NeuralMaterialCoopVec<N>> vec, float result_grad[N]) {
        vec = diffPair(vec.p, from_array(result_grad));
    }

    [Differentiable]
    float[N] to_array() {
        return to_array(this);
    }

    public override static Differential dadd(Differential left, Differential right) {
        Differential result;
        result.data = left.data + right.data;
        return result;
    }

    public override static Differential dmul<U : __BuiltinRealType>(U scalar, Differential value) {
        Differential result;
        result.data = value.data * __realCast<float>(scalar);
        return result;
    }

    public override static Differential dzero() {
        Differential result;
        [ForceUnroll]
        for (int i = 0; i < N; ++i) {
            result.data[i] = 0.0f;
        }
        return result;
    }
};

static CoopVec<half, N> neural_material_to_half_coopvec<let N : int>(NeuralMaterialCoopVec<N> input_vec) {
    CoopVec<half, N> result;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        result[i] = half(input_vec.data[i]);
    }
    return result;
}

static NeuralMaterialCoopVec<N> neural_material_from_half_coopvec<let N : int>(CoopVec<half, N> input_vec) {
    NeuralMaterialCoopVec<N> result;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        result.data[i] = float(input_vec[i]);
    }
    return result;
}

[Differentiable]
NeuralMaterialCoopVec<N> neural_material_relu<let N : int>(NeuralMaterialCoopVec<N> input_vec) {
    NeuralMaterialCoopVec<N> result;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        result.data[i] = relu(input_vec.data[i]);
    }
    return result;
}

[BackwardDerivativeOf(neural_material_relu)]
void neural_material_relu_bwd<let N : int>(
    inout DifferentialPair<NeuralMaterialCoopVec<N>> input_vec,
    NeuralMaterialCoopVec<N> result_grad) {
    let forward = neural_material_relu(input_vec.p);
    NeuralMaterialCoopVec<N> input_grad = result_grad;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        if (forward.data[i] <= 0.0f) {
            input_grad.data[i] = 0.0f;
        }
    }
    input_vec = diffPair(input_vec.p, input_grad);
}

[Differentiable]
NeuralMaterialCoopVec<N> neural_material_exp_offset<let N : int>(
    NeuralMaterialCoopVec<N> input_vec,
    no_diff float offset) {
    NeuralMaterialCoopVec<N> result;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        result.data[i] = exp(input_vec.data[i] + offset);
    }
    return result;
}

[BackwardDerivativeOf(neural_material_exp_offset)]
void neural_material_exp_offset_bwd<let N : int>(
    inout DifferentialPair<NeuralMaterialCoopVec<N>> input_vec,
    no_diff float offset,
    NeuralMaterialCoopVec<N> result_grad) {
    let forward = neural_material_exp_offset(input_vec.p, offset);
    NeuralMaterialCoopVec<N> input_grad;
    [ForceUnroll]
    for (int i = 0; i < N; ++i) {
        input_grad.data[i] = result_grad.data[i] * forward.data[i];
    }
    input_vec = diffPair(input_vec.p, input_grad);
}
#endif
#endif

// Neural Material Layers ////////////////////////////////////////////////
struct NeuralWeightBiasLayer {
    uint weights_index;
    uint biases_index;
    uint weights_grad_index;
    uint biases_grad_index;

    uint weights_grad_training_index;
    uint weights_exp_avg_index;
    uint biases_exp_avg_index;
    uint weights_exp_avg_sq_index;
    uint biases_exp_avg_sq_index;

    uint inputs;
    uint outputs;
    uint padding000;
#ifdef __SLANG__
    [Differentiable]
    float get_weight(uint output_index, uint input_index) {
        StructuredBuffer<float> weights = global_buffers_f32[weights_index];
        const uint weight_index = output_index * inputs + input_index;
        return weights[weight_index];
    }

    [Differentiable]
    float get_bias(uint output_index) {
        StructuredBuffer<float> biases = global_buffers_f32[biases_index];
        return biases[output_index];
    }

    [BackwardDerivativeOf(get_bias)]
    void get_bias_bwd(uint output_index, float grad)
    {
        RWStructuredBuffer<float> biases_grad = global_buffers_rw_f32[biases_grad_index];
        InterlockedAdd(biases_grad[output_index], grad);
    }

    [BackwardDerivativeOf(get_weight)]
    void get_weight_bwd(uint output_index, uint input, float grad)
    {
        RWStructuredBuffer<float> weights_grad = global_buffers_rw_f32[weights_grad_index];
        const uint weight_index = output_index * inputs + input;
        InterlockedAdd(weights_grad[weight_index], grad);
    }

    [Differentiable]
    float eval<let N : int>(
        uint output_index, const float input_values[N],
        uint input_count) {
        float sum = get_bias(output_index);

        [MaxIters(N)]
        for (uint input_index = 0; input_index < N; ++input_index) {
            sum += get_weight(output_index, input_index) * input_values[input_index];
        }

        return sum;
    }

#if defined(RAPTOR_USE_COOPVEC)
    [Differentiable]
    NeuralMaterialCoopVec<OutputCount> eval_coop<let InputCount : int, let OutputCount : int>(
        NeuralMaterialCoopVec<InputCount> input_values) {
        StructuredBuffer<half> weights = global_buffers_f16[weights_index];
        StructuredBuffer<half> biases = global_buffers_f16[biases_index];
        let input_values_half = neural_material_to_half_coopvec(input_values);

        let output = coopVecMatMulAdd<half, OutputCount, InputCount>(
            input_values_half,
            k_neural_material_coopvec_component_type,
            weights,
            0,
            k_neural_material_coopvec_component_type,
            biases,
            0,
            k_neural_material_coopvec_component_type,
            CoopVecMatrixLayout::RowMajor,
            false,
            InputCount * sizeof(half));

        return neural_material_from_half_coopvec(output);
    }

    [BackwardDerivativeOf(eval_coop)]
    void eval_coop_bwd<let InputCount : int, let OutputCount : int>(
        inout DifferentialPair<NeuralMaterialCoopVec<InputCount>> input_values,
        NeuralMaterialCoopVec<OutputCount> result_grad) {
        RWStructuredBuffer<half> biases_grad = global_buffers_rw_f16[biases_grad_index];
        RWStructuredBuffer<half> weights_grad = global_buffers_rw_f16[weights_grad_training_index];
        StructuredBuffer<half> weights = global_buffers_f16[weights_index];
        let result_grad_half = neural_material_to_half_coopvec(result_grad);
        let input_values_half = neural_material_to_half_coopvec(input_values.p);

        coopVecOuterProductAccumulate(
            result_grad_half,
            input_values_half,
            weights_grad,
            0,
            0,
            CoopVecMatrixLayout::TrainingOptimal,
            k_neural_material_coopvec_component_type);

        coopVecReduceSumAccumulate(
            result_grad_half,
            biases_grad,
            0);

        let d_input = coopVecMatMul<half, InputCount, OutputCount>(
            result_grad_half,
            k_neural_material_coopvec_component_type,
            weights,
            0,
            k_neural_material_coopvec_component_type,
            CoopVecMatrixLayout::ColumnMajor,
            false,
            InputCount * sizeof(half));

        input_values = diffPair(input_values.p, neural_material_from_half_coopvec(d_input));
    }
#endif
#endif
};

struct NeuralWeightLayer {
    uint weights_index;
    uint weights_grad_index;
    uint weights_grad_training_index;
    uint weights_exp_avg_index;
    uint weights_exp_avg_sq_index;

    uint inputs;
    uint outputs;
    uint padding000;

#ifdef __SLANG__
    [Differentiable]
    float get_weight(uint output_index, uint input_index) {
        StructuredBuffer<float> weights = global_buffers_f32[weights_index];
        const uint weight_index = output_index * inputs + input_index;
        return weights[weight_index];
    }

    [BackwardDerivativeOf(get_weight)]
    void get_weight_bwd(uint output_index, uint input, float grad)
    {
        RWStructuredBuffer<float> weights_grad = global_buffers_rw_f32[weights_grad_index];
        const uint weight_index = output_index * inputs + input;
        InterlockedAdd(weights_grad[weight_index], grad);
    }

    [Differentiable]
    float eval<let N : int>(
        uint output_index, const float input_values[N],
        uint input_count) {

        float sum = 0;
        [MaxIters(N)]
        for (uint input_index = 0; input_index < N; ++input_index) {
            sum += get_weight(output_index, input_index) * input_values[input_index];
        }

        return sum;
    }

#if defined(RAPTOR_USE_COOPVEC)
    [Differentiable]
    NeuralMaterialCoopVec<OutputCount> eval_coop<let InputCount : int, let OutputCount : int>(
        NeuralMaterialCoopVec<InputCount> input_values) {
        StructuredBuffer<half> weights = global_buffers_f16[weights_index];
        let input_values_half = neural_material_to_half_coopvec(input_values);

        let output = coopVecMatMul<half, OutputCount, InputCount>(
            input_values_half,
            k_neural_material_coopvec_component_type,
            weights,
            0,
            k_neural_material_coopvec_component_type,
            CoopVecMatrixLayout::RowMajor,
            false,
            InputCount * sizeof(half));

        return neural_material_from_half_coopvec(output);
    }

    [BackwardDerivativeOf(eval_coop)]
    void eval_coop_bwd<let InputCount : int, let OutputCount : int>(
        inout DifferentialPair<NeuralMaterialCoopVec<InputCount>> input_values,
        NeuralMaterialCoopVec<OutputCount> result_grad) {
        RWStructuredBuffer<half> weights_grad = global_buffers_rw_f16[weights_grad_training_index];
        StructuredBuffer<half> weights = global_buffers_f16[weights_index];
        let result_grad_half = neural_material_to_half_coopvec(result_grad);
        let input_values_half = neural_material_to_half_coopvec(input_values.p);

        coopVecOuterProductAccumulate(
            result_grad_half,
            input_values_half,
            weights_grad,
            0,
            0,
            CoopVecMatrixLayout::TrainingOptimal,
            k_neural_material_coopvec_component_type);

        let d_input = coopVecMatMul<half, InputCount, OutputCount>(
            result_grad_half,
            k_neural_material_coopvec_component_type,
            weights,
            0,
            k_neural_material_coopvec_component_type,
            CoopVecMatrixLayout::ColumnMajor,
            false,
            InputCount * sizeof(half));

        input_values = diffPair(input_values.p, neural_material_from_half_coopvec(d_input));
    }
#endif
#endif
};

struct NeuralEncoder {
    NeuralWeightBiasLayer l0;
    NeuralWeightBiasLayer l1;
    NeuralWeightBiasLayer l2;
};

struct NeuralFrameDecoder {
    NeuralWeightLayer l0;
};

struct NeuralBrdfDecoder {
    NeuralWeightBiasLayer l0;
    NeuralWeightBiasLayer l1;
    NeuralWeightBiasLayer l2;
};

struct NeuralImportanceSamplingDecoder {
    NeuralWeightBiasLayer l0;
    NeuralWeightBiasLayer l1;
    NeuralWeightBiasLayer l2;
    NeuralWeightBiasLayer l3;
};

struct PackedUintArray16 {
    uint4 values[4];

    uint get( uint index ) {
        return values[ index >> 2u ][ index & 3u ];
    }
#ifndef __SLANG__
    void set( uint index, uint value ) {
        values[ index >> 2u ][ index & 3u ] = value;
    }
#endif
};


struct LatentTexturePyramid {
    uint level_count;
    uint width;
    uint height;
    uint padding002;

    PackedUintArray16 textures0_srv;
    PackedUintArray16 textures1_srv;

    PackedUintArray16 textures0_uav;
    PackedUintArray16 textures1_uav;

    PackedUintArray16 grads0_uav;
    PackedUintArray16 grads1_uav;

    PackedUintArray16 exp_avg0_uav;
    PackedUintArray16 exp_avg1_uav;

    PackedUintArray16 exp_avg_sq0_uav;
    PackedUintArray16 exp_avg_sq1_uav;

#ifdef __SLANG__
    [Differentiable]
    float4 get_latent0(int x, int y, uint lod)
    {
        Texture2D<float4> tex = global_textures[NonUniformResourceIndex(textures0_srv.get(lod))];
        return tex.Load(int3(x, y, 0));
    }

    [BackwardDerivativeOf(get_latent0)]
    void get_latent_bwd0(int x, int y, uint lod, float4 latent_grad)
    {
        RWStructuredBuffer<float> tex_grads = global_buffers_rw_f32[NonUniformResourceIndex(grads0_uav.get(lod))];
        uint lod_width = width >> lod;
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 0 ], latent_grad.x);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 1 ], latent_grad.y);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 2 ], latent_grad.z);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 3 ], latent_grad.w);
    }

    [Differentiable]
    float4 get_latent1(int x, int y, uint lod)
    {
        Texture2D<float4> tex = global_textures[NonUniformResourceIndex(textures1_srv.get(lod))];
        return tex.Load(int3(x, y, 0));
    }

    [BackwardDerivativeOf(get_latent1)]
    void get_latent_bwd1(int x, int y, uint lod, float4 latent_grad)
    {
        RWStructuredBuffer<float> tex_grads = global_buffers_rw_f32[NonUniformResourceIndex(grads1_uav.get(lod))];
        uint lod_width = width >> lod;
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 0 ], latent_grad.x);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 1 ], latent_grad.y);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 2 ], latent_grad.z);
        InterlockedAdd(tex_grads[ y * lod_width * 4 + x * 4 + 3 ], latent_grad.w);
    }

    [Differentiable]
    float[8] sample(no_diff float2 uv, no_diff uint lod)
    {
        uint x = uint(uv.x * float(width >> lod));
        uint y = uint(uv.y * float(height >> lod));

        float4 result0 = get_latent0( x, y, lod );
        float4 result1 = get_latent1( x, y, lod );

        float result[8];
        result[0] = result0.x;
        result[1] = result0.y;
        result[2] = result0.z;
        result[3] = result0.w;
        result[4] = result1.x;
        result[5] = result1.y;
        result[6] = result1.z;
        result[7] = result1.w;

        return result;
    }
#endif
};

// Shader structs ////////////////////////////////////////////////////////
struct NMRandomBatchConstants
{
    uint step;
    uint seed;
    uint batch_size;
    uint padding000;

    uint output_buffer_index;
    uint normal_map_index;
    uint roughness_map_index;
    uint padding001;
};

struct NMBrdfTrainingConstants
{
    uint batch_size;
    uint batch_offset;
    uint total_batch_size;
    uint padding000;

    uint input_buffer_index;
    uint loss_buffer_index;
    uint padding001;
    uint padding002;

    NeuralEncoder encoder;
    NeuralFrameDecoder frame_decoder;
    NeuralBrdfDecoder brdf_decoder;
};

struct NMBrdfLatentTrainingConstants
{
    uint batch_size;
    uint batch_offset;
    uint total_batch_size;
    uint padding000;

    uint input_buffer_index;
    uint loss_buffer_index;
    uint mollification_step;
    uint padding002;

    LatentTexturePyramid latent_pyramid;
    NeuralFrameDecoder frame_decoder;
    NeuralBrdfDecoder brdf_decoder;
};

struct NMSamplerTrainingConstants
{
    uint seed;
    uint batch_size;

    uint input_buffer_index;
    uint padding000;

    NeuralEncoder encoder;
    NeuralBrdfDecoder brdf_decoder;
    NeuralImportanceSamplingDecoder sampler_decoder;
};

struct NMSamplerLatentTrainingConstants
{
    uint seed;
    uint batch_size;

    uint input_buffer_index;
    uint padding000;

    LatentTexturePyramid latent_pyramid;
    NeuralBrdfDecoder brdf_decoder;
    NeuralImportanceSamplingDecoder sampler_decoder;
};

struct NMCopyEncoderToLatentConstants
{
    uint width;
    uint height;

    uint normal_map_index;
    uint roughness_map_index;

    NeuralEncoder encoder;
    LatentTexturePyramid latent_pyramid;
};

struct NMOptimizerBufferConstants
{
    uint value_buffer_index;
    uint grad_buffer_index;
    uint exp_avg_buffer_index;
    uint exp_avg_sq_buffer_index;

    uint element_count;
    uint optimize_counter;
    float learning_rate;
    uint padding000;
};

struct NMOptimizerTextureConstants
{
    uint values_texture_index;
    uint grads_texture_index;
    uint exp_avg_texture_index;
    uint exp_avg_sq_texture_index;

    uint width;
    uint height;
    uint optimize_counter;
    float learning_rate;
};

struct NMPreviewConstants
{
    uint output_texture_index;
    uint normal_map_index;
    uint roughness_map_index;
    uint use_latent_textures;

    // Preview texture resolution
    uint width;
    uint height;

    // Source material texture resolution
    uint material_width;
    uint material_height;

    NeuralEncoder encoder;
    NeuralFrameDecoder frame_decoder;
    NeuralBrdfDecoder brdf_decoder;
    LatentTexturePyramid latent_pyramid;
};


#if defined (__cplusplus)
static_assert( sizeof( uint4 ) == 16, "uint4 must match cbuffer layout" );
static_assert( sizeof( NeuralWeightBiasLayer ) == 48, "NeuralWeightBiasLayer must match cbuffer layout" );
static_assert( sizeof( NeuralWeightLayer ) == 32, "NeuralWeightLayer must match cbuffer layout" );
static_assert( sizeof( NeuralEncoder ) == 144, "NeuralEncoder must match cbuffer layout" );
static_assert( sizeof( NeuralFrameDecoder ) == 32, "NeuralFrameDecoder must match cbuffer layout" );
static_assert( sizeof( NeuralBrdfDecoder ) == 144, "NeuralBrdfDecoder must match cbuffer layout" );
static_assert( sizeof( NMBrdfTrainingConstants ) == 352, "NMBrdfTrainingConstants must match cbuffer layout" );
static_assert( sizeof( NMBrdfLatentTrainingConstants ) == 864, "NMBrdfLatentTrainingConstants must match cbuffer layout" );
static_assert( sizeof( NMPreviewConstants ) == 1008, "NMPreviewConstants must match cbuffer layout" );
static_assert( sizeof( PackedUintArray16 ) == 64, "PackedUintArray16 must match cbuffer layout" );
static_assert( sizeof( LatentTexturePyramid ) == 656, "LatentTexturePyramid must match cbuffer layout" );
#endif

#include "shared_converter_footer.h"

#endif // __NEURAL_MATERIAL_STRUCTS_H__

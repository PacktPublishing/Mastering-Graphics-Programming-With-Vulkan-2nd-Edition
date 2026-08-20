// Adapted from Neural Shading Course SIGGRAPH 2025, https://github.com/shader-slang/neural-shading-s25
// Adapted from Real-Time Neural Appearance Models - Supplemental material, https://doi.org/10.1145/3659577
module neural_materials;

import slangpy;
import brdf;

#define M_PI 3.14159265358979323846

public struct WeightBiasLayer<int Inputs, int Outputs>
{
    public Tensor<float, 1> biases;
    public Tensor<float, 2> weights;

    public AtomicTensor<float, 1> biases_grad;
    public AtomicTensor<float, 2> weights_grad;

    [Differentiable]
    float get_bias(int neuron)
    {
        return biases.get( { neuron });
    }
    [Differentiable]
    float get_weight(int neuron, int input)
    {
        return weights.get( { neuron, input });
    }

    [BackwardDerivativeOf(get_bias)]
    void get_bias_bwd(int neuron, float grad)
    {
        // .set on an atomic tensor atomically adds the result
        biases_grad.set( { neuron }, grad);
    }

    [BackwardDerivativeOf(get_weight)]
    void get_weight_bwd(int neuron, int input, float grad)
    {
        // .set on an atomic tensor atomically adds the result
        weights_grad.set( { neuron, input }, grad);
    }

    // Evaluate all neurons in the layer
    [Differentiable]
    float[Outputs] forward(float[Inputs] x)
    {
        float[Outputs] y;
        // For optimal runtime performance, using [ForceUnroll] here can be better. However, the downstream
        // compiler on some platforms (in particular, the DXC compiler on D3D12) may not cope well with
        // unrolled loops and compilation times grow exponentially. Using MaxIters on the outer loop
        // here helps avoid this.
        [MaxIters(Outputs)]
        for (int row = 0; row < Outputs; ++row)
        {
            // Sum up bias plus weighted inputs for 1 neuron
            var sum = get_bias(row);
            [ForceUnroll]
            for (int col = 0; col < Inputs; ++col)
                sum += get_weight(row, col) * x[col];
            y[row] = sum;
        }

        return y;
    }
}

public struct WeightLayer<int Inputs, int Outputs>
{
    public Tensor<float, 2> weights;

    public AtomicTensor<float, 2> weights_grad;

    [Differentiable]
    float get_weight(int neuron, int input)
    {
        return weights.get( { neuron, input });
    }

    [BackwardDerivativeOf(get_weight)]
    void get_weight_bwd(int neuron, int input, float grad)
    {
        // .set on an atomic tensor atomically adds the result
        weights_grad.set( { neuron, input }, grad);
    }

    // Evaluate all neurons in the layer
    [Differentiable]
    float[Outputs] forward(float[Inputs] x)
    {
        float[Outputs] y;
        // For optimal runtime performance, using [ForceUnroll] here can be better. However, the downstream
        // compiler on some platforms (in particular, the DXC compiler on D3D12) may not cope well with
        // unrolled loops and compilation times grow exponentially. Using MaxIters on the outer loop
        // here helps avoid this.
        [MaxIters(Outputs)]
        for (int row = 0; row < Outputs; ++row)
        {
            // Sum up bias plus weighted inputs for 1 neuron
            var sum = 0.0;
            [ForceUnroll]
            for (int col = 0; col < Inputs; ++col)
                sum += get_weight(row, col) * x[col];
            y[row] = sum;
        }

        return y;
    }
}

public struct LatentTexture {
    public RWTensor<float4, 2> texture;

    public AtomicTensor<float4, 2> texture_grads;

    [BackwardDerivative(get_latent_bwd)]
    float4 get_latent(int x, int y)
    {
        return texture[y, x];
    }

    void get_latent_bwd(int x, int y, float4 latent_grad)
    {
        texture_grads.set({y, x}, latent_grad);
    }

    [Differentiable]
    float4 sample(no_diff int2 uv)
    {
        float4 result = get_latent(uv.x, uv.y);

        return result;
    }
}

public struct LatentTexturePyramid<int Levels> {
    // This is the ideal implementation, not sure this will work
    public LatentTexture textures0[Levels];
    public LatentTexture textures1[Levels];

    [Differentiable]
    float[8] eval(no_diff int2 uv, no_diff uint lod)
    {
        float4 l0 = textures0[lod].sample(uv);
        float4 l1 = textures1[lod].sample(uv);
        return float[8](l0.x, l0.y, l0.z, l0.w, l1.x, l1.y, l1.z, l1.w);
    }
}

[Differentiable]
float relu(float x)
{
    return max(x, 0.0);
}

[Differentiable] float tanh_approx(float x) { return x / sqrt(1 + x * x); }
[Differentiable] float sinh_approx(float x) { return x * sqrt(1 + x * x); }
void tangents(float3 n, out float3 s, out float3 t)
{
    const float3 u = abs(n.x) < 0.95 ? float3(1, 0, 0) : float3(0, 1, 0);
    s = normalize(u - n * dot(n, u));
    t = cross(n, s);
}
float3 to_world(float3 w, float3 n)
{
    float3 s, t;
    tangents(n, s, t);
    return w.x * s + w.y * t + w.z * n;
}
float3 to_local(float3 w, float3 n)
{
    float3 s, t;
    tangents(n, s, t);
    return float3(dot(w, s), dot(w, t), dot(w, n));
}

float2 sample_disk(float2 u)
{
    float2 uoffset = 2 * u - 1;
    if (uoffset.x == 0 && uoffset.y == 0)
        return uoffset;

    float theta, r;
    if (abs(uoffset.x) > abs(uoffset.y))
    {
        r = uoffset.x;
        theta = (M_PI / 4) * (uoffset.y / uoffset.x);
    }
    else
    {
        r = uoffset.y;
        theta = (M_PI / 2) - (M_PI / 4) * (uoffset.x / uoffset.y);
    }

    return r * float2(cos(theta), sin(theta));
}

// Adapted from https://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations#Cosine-WeightedHemisphereSampling
float3 sample_cosine_distribution(float2 u)
{
    float2 d = sample_disk(u);
    float z = sqrt(max(0, 1 - d.x * d.x - d.y * d.y));
    return float3(d.x, d.y, z);
}

float3 sample_diffuse(float3 wi, float2 slope, float2 u)
{
    // Samle from cosine distribution.
    float3 wo = sample_cosine_distribution(u);
    // Apply normal mapping.
    float3 n = normalize(float3(-slope.x, -slope.y, 1));
    return to_world(wo, n);
}
[Differentiable]
float pdf_diffuse(no_diff float3 wi, no_diff float3 wo, float2 slope)
{
    // Normal from slope: n = normalize(-slope.x, -slope.y, 1).
    // pdf = dot(wo, n) / PI — inlined to avoid needing to_local to be differentiable.
    float3 n = normalize(float3(-slope.x, -slope.y, 1));
    return max(dot(wo, n), 0) / M_PI;
}
float3 sample_specular(float3 wi, float3 alpha, float2 slope, float2 u)
{
    float rho = alpha.z;
    float sqrtOneMinusRho = sqrt(1 - rho * rho);
    // Sample base configuration slope .
    float s = sqrt(u.x) / sqrt(1 - u.x);
    float phi = 2 * M_PI * u.y;
    float sxStd = s * cos(phi);
    float syStd = s * sin(phi);
    // Stretch slope based on elliptical anisotropy .
    float sx = alpha.x * sxStd;
    float sy = alpha.y * (rho * sxStd + sqrtOneMinusRho * syStd);
    // Apply the slope offset .
    sx += slope.x;
    sy += slope.y;
    // Convert slope to a half - vector .
    float3 wh = normalize(float3(-sx, -sy, 1));
    return 2 * dot(wi, wh) * wh - wi;
}
[Differentiable]
float pdf_specular(no_diff float3 wi, no_diff float3 wo, float3 alpha, float2 slope)
{
    float rho = alpha.z;
    float sqrtOneMinusRho = sqrt(1 - rho * rho);
    // Construct half - vector .
    float3 wh = normalize(wi + wo);
    wh *= sign(wh.z);
    float cosTheta = wh.z;
    if (cosTheta <= 1e - 4)
    {
        return 0;
    }
    // Convert to slope space .
    float sx = -wh.x / cosTheta;
    float sy = -wh.y / cosTheta;
    // Revert the slope offset .
    sx -= slope.x;
    sy -= slope.y;
    // Unstretch slope based on elliptical anisotropy .
    float sxStd = sx / alpha.x;
    float normalization = 1 / (alpha.x * alpha.y * sqrtOneMinusRho);
    float syStd = (alpha.x * sy - rho * alpha.y * sx) * normalization;
    // Evaluate basic slope density .
    float r2 = sxStd * sxStd + syStd * syStd;
    float p22Std = 1 / (M_PI * (1 + r2) * (1 + r2));
    float p22 = p22Std * normalization;
    float pdfH = p22 / (cosTheta * cosTheta * cosTheta);
    // Account for specular reflection Jacobian .
    float Jh = 4 * abs(dot(wi, wh));
    return pdfH / Jh;
}

// Samples the analytic BRDF model .
// The u parameter is a uniform random number in [0 ,1)^2.
// The function returns the sampled direction wo and pdf .
float3 sample_analytic(float3 alpha, float2 slopeSpec, float2 slopeDiff, float weightSpec,
                       float3 wi, float2 u, out float pdf)
{
    float3 wo;
    if (u.x < weightSpec) // specular lobe
    {
        u.x = u.x / weightSpec;
        wo = sample_specular(wi, alpha, slopeSpec, u);
    }
    else // diffuse lobe
    {
        u.x = (u.x - weightSpec) / (1 - weightSpec);
        wo = sample_diffuse(wi, slopeDiff, u);
    }
    pdf = 0;
    pdf += weightSpec * pdf_specular(wi, wo, alpha, slopeSpec);
    pdf += (1 - weightSpec) * pdf_diffuse(wi, wo, slopeDiff);
    return wo;
}

public struct Encoder {
    public WeightBiasLayer<11, 32> layer0;
    public WeightBiasLayer<32, 32> layer1;
    public WeightBiasLayer<32, 8> layer2;

    [Differentiable]
    float[8] eval(no_diff float inputs[11])
    {
        float output0[32] = layer0.forward(inputs);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output0[i] = relu(output0[i]);
        float output1[32] = layer1.forward(output0);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output1[i] = relu(output1[i]);
        float output2[8] = layer2.forward(output1);
        [ForceUnroll]
        for (int i = 0; i < 8; ++i)
            output2[i] = relu(output2[i]);
        return output2;
    }
}

public struct BrdfDecoder {
    public WeightBiasLayer<20, 32> layer0;
    public WeightBiasLayer<32, 32> layer1;
    public WeightBiasLayer<32, 3> layer2;

    [Differentiable]
    float3 eval(no_diff float inputs[20])
    {
        float output0[32] = layer0.forward(inputs);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output0[i] = relu(output0[i]);
        float output1[32] = layer1.forward(output0);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output1[i] = relu(output1[i]);
        float output2[3] = layer2.forward(output1);
        [ForceUnroll]
        for (int i = 0; i < 3; ++i)
            output2[i] = exp(output2[i] - 3.0);
        return float3(output2[0], output2[1], output2[2]);
    }
}

public struct ImportanceSamplingDecoder {
    public WeightBiasLayer<11, 32> layer0;
    public WeightBiasLayer<32, 32> layer1;
    public WeightBiasLayer<32, 32> layer2;
    public WeightBiasLayer<32, 9> layer3;

    [Differentiable]
    float[9] eval(no_diff float inputs[11])
    {
        float output0[32] = layer0.forward(inputs);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output0[i] = relu(output0[i]);
        float output1[32] = layer1.forward(output0);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output1[i] = relu(output1[i]);
        float output2[32] = layer2.forward(output1);
        [ForceUnroll]
        for (int i = 0; i < 32; ++i)
            output2[i] = relu(output2[i]);
        // Linear activation for the last layer
        float output[9] = layer3.forward(output2);
        return output;
    }
}

public struct FrameDecoder {
    public WeightLayer<8, 12> layer0;

    [Differentiable]
    float[12] eval(no_diff float inputs[8])
    {
        float output[12] = layer0.forward(inputs);
        return output;
    }
}

public void optimizer_step(inout float primal, inout float grad, inout float mean, inout float variance, float learning_rate, int iteration)
{
    // Default Adam values.
    const float ADAM_BETA_1 = 0.9;
    const float ADAM_BETA_2 = 0.999;
    const float ADAM_EPSILON = 1e-7;

    // Running average of gradient and square gradient (== variance)
    mean = lerp(grad, mean, ADAM_BETA_1);
    variance = lerp(grad * grad, variance, ADAM_BETA_2);

    // The running average is initialized with zeros, and underestimates
    // the true values at the beginning. Adam divides by a compensation factor
    float mHat = mean / (1.0 - pow(ADAM_BETA_1, iteration));
    float vHat = variance / (1.0 - pow(ADAM_BETA_2, iteration));
    // Compute the adam step by scaling the mean gradient by the learning rate and the variance estimate
    float step = learning_rate * mHat / (sqrt(vHat) + ADAM_EPSILON);

    // Take an adam step
    primal -= step;

    // Because we now accumulate gradients, we need to clear the gradient to zero before the next iteration
    grad = 0.0;
}

public void texture_optimizer_step(inout float4 primal, inout float4 grad, inout float4 mean, inout float4 variance, float learning_rate, int iteration)
{
    // Default Adam values.
    const float ADAM_BETA_1 = 0.9;
    const float ADAM_BETA_2 = 0.999;
    const float ADAM_EPSILON = 1e-7;

    // Running average of gradient and square gradient (== variance)
    mean = lerp(grad, mean, ADAM_BETA_1);
    variance = lerp(grad * grad, variance, ADAM_BETA_2);

    // The running average is initialized with zeros, and underestimates
    // the true values at the beginning. Adam divides by a compensation factor
    float4 mHat = mean / (1.0 - pow(ADAM_BETA_1, iteration));
    float4 vHat = variance / (1.0 - pow(ADAM_BETA_2, iteration));
    // Compute the adam step by scaling the mean gradient by the learning rate and the variance estimate
    float4 step = learning_rate * mHat / (sqrt(vHat) + ADAM_EPSILON);

    // Take an adam step
    primal -= step;

    // Because we now accumulate gradients, we need to clear the gradient to zero before the next iteration
    grad = 0.0;
}

Texture2D<float4> latentTextures[2]; // 8D latent code stored as 2 x RGBA textures
SamplerState bilinearSampler;

/* *
 * Fetch latent code (8 D ).
 * The gradients ( duvdx , duvdy ) represent a pixel / path footprint in texture space .
 * The u parameter is a random number in [0 ,1).
 */
void fetch_latent_code(float2 uv, float2 duvdx, float2 duvdy, float u, out float latents[8])
{
    // Compute LOD by approximating an isotropic footprint .
    float lod = 0.5 * log2(length(duvdx) * length(duvdy));
    // Scale by texture resolution to get fractional mip level .
    uint w, h;
    latentTextures[0].GetDimensions(w, h);
    lod += 0.5 * log2(w * h);
    // Pick between two nearest mip levels stochastically .
    int mip = frac(lod) < u ? floor(lod) : floor(lod) + 1;
    float4 l0 = latentTextures[0].SampleLevel(bilinearSampler, uv, mip);
    float4 l1 = latentTextures[1].SampleLevel(bilinearSampler, uv, mip);
    // Copy elements of l0 , l1 into latents ...
    for (int i = 0; i < 4; ++i) {
        latents[i] = l0[i];
        latents[4 + i] = l1[i];
    }
}

/**
* Evaluate the neural BRDF .
* The weights Wi,j and biases bi below are loaded from memory .
* Incident ( wi ) and outgoing direction ( wo ) are in the local frame .
* The function returns f(wi , wo ) * dot (n , wo ) ( the cosine term is baked into the MLP ).
*/
[Differentiable]
float3 eval_brdf( float latents [8], FrameDecoder frame_decoder, no_diff float3 wi , no_diff float3 wo, BrdfDecoder brdf_decoder )
{
    // TODO(marco): turn these back to halfs
    float input[20];
    // Copy latents into input [12] , input [13] , ...
    for (int i = 0; i < 8; ++i)
        input[12 + i] = latents[i];

    // Evaluate shading frame network to generate predicted shading frames (n =2).
    // Note that this is just a linear layer without biases or activations .
    // Input : latent code , output : array of (wi , wo ) transformed into the predicted frames.
    float sf[12] = frame_decoder.eval(latents);

    for (int i = 0; i < 12; i += 6)
    {
        float3 N = normalize(float3(sf[i + 0], sf[i + 1], sf[i + 2] + 1.0));
        float3 T = normalize(float3(sf[i + 3] + 1.0, sf[i + 4], sf[i + 5]));
        float3 B = cross(N, T); // Note (T ,B ,N) is not orthogonalized
        float3 wi_pred = float3(dot(wi, T), dot(wi, B), dot(wi, N));
        float3 wo_pred = float3(dot(wo, T), dot(wo, B), dot(wo, N));
        // Copy wi_pred , wo_pred into input [i], input [i +1] , ...
        input[i + 0] = wi_pred.x;
        input[i + 1] = wi_pred.y;
        input[i + 2] = wi_pred.z;
        input[i + 3] = wo_pred.x;
        input[i + 4] = wo_pred.y;
        input[i + 5] = wo_pred.z;
    }

    return brdf_decoder.eval(input);
}

[Differentiable]
void extract_differentiable_ggx(no_diff float[9] output, out float3 alpha, out float2 slopeSpec, out float2 slopeDiff, out float wSpec, out float wDiff) {
    int k = 0;
    float alphaX = 1e - 4 + 0.5 * (1 + tanh_approx(output[k++]));
    float alphaY = 1e - 4 + 0.5 * (1 + tanh_approx(output[k++]));
    float rho = tanh_approx(output[k++]);

    float slopeSpecX = sinh_approx(output[k++]);
    float slopeSpecY = sinh_approx(output[k++]);
    float slopeDiffX = sinh_approx(output[k++]);
    float slopeDiffY = sinh_approx(output[k++]);

    float wSpec = exp(output[k++]);
    float wDiff = exp(output[k]);
    float norm = 1 / (wSpec + wDiff);
    wSpec *= norm;
    wDiff *= norm;

    alpha = float3(alphaX, alphaY, rho);
    slopeSpec = float2(slopeSpecX, slopeSpecY);
    slopeDiff = float2(slopeDiffX, slopeDiffY);
}

/* *
 * Sample the neural BRDF .
 * The weights W_i,j and biases b_i below are loaded from memory .
 * Incident ( wi ) and sampled outgoing direction ( wo ) are in the local frame .
 * The function returns the weight : f(wi , wo ) * dot (n , wo ) / pdf .
 */
float3 sample_brdf(float latents[8], float3 wi, BrdfDecoder brdf_decoder, FrameDecoder frame_decoder, ImportanceSamplingDecoder is_decoder, out float3 wo, out float pdf)
{
    float input[11];
    // Copy wi into first three elements
    input[0] = wi.x;
    input[1] = wi.y;
    input[2] = wi.z;
    // Copy latents into input [3] , input [4] , ...
    for (int i = 0; i < 8; ++i)
        input[3 + i] = latents[i];

    // Evaluate BRDF sampling network.
    // Input : latent code and wi , output : sampling parameters .
    float[9] output = is_decoder.eval(input);

    // Compute sampling paramters based on network outputs.
    float3 alpha;
    float2 slopeSpec, slopeDiff;
    float wSpec, wDiff;
    extract_differentiable_ggx(output, alpha, slopeSpec, slopeDiff, wSpec, wDiff);

    // Sample analytic model to obtain wo , pdf.
    LCG lcg = LCG(wi.x);

    wo = sample_analytic(alpha, slopeSpec, slopeDiff, wSpec, wi, float2(lcg.next_float(), lcg.next_float()), pdf);
    // Return weight .
    return eval_brdf(latents, frame_decoder, wi, wo, brdf_decoder) / pdf;
}

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

public void compute_random_batch(uint epoch, uint seed, int batch_index, int batch_size, RWTensor<float, 1> output, Tensor<float4, 2> normal_map, Tensor<float, 2> roughness_map)
{
    // Pick a random pixel for computing the loss
    // First, sample a random UV within our cell of the training batch grid
    LCG lcg = LCG(seed);
    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(batch_size);
    // Then turn that into a pixel on the texture
    int2 resolution = { roughness_map.shape[1], roughness_map.shape[0] };
    int2 pixel = int2(uv * float2(resolution));

    float roughness = roughness_map.getv(pixel);
    float3 normal = normal_map.getv(pixel).xyz;
    // Derive a consistent tangent using the stable tangents() helper.
    float3 tangent, bitangent_unused;
    tangents(normal, tangent, bitangent_unused);

    float3 albedo = float3(0.5, 0.5, 0.5);

    uint offset = batch_index.x * 11;
    output.setv( offset + 0, normal.x );
    output.setv( offset + 1, normal.y );
    output.setv( offset + 2, normal.z );
    output.setv( offset + 3, tangent.x );
    output.setv( offset + 4, tangent.y );
    output.setv( offset + 5, tangent.z );
    output.setv( offset + 6, albedo.x );
    output.setv( offset + 7, albedo.y );
    output.setv( offset + 8, albedo.z );
    output.setv( offset + 9, roughness ); // TODO(marco): do we need metalness?
    output.setv( offset + 10, 1.0 ); // Assume a single layer for now with weight 1.0
}

[Differentiable]
float l1_log_loss(float3 predicted, float3 ground_truth)
{
    float eps = 1e-6;
    float3 error = abs(log(predicted + eps) - log(ground_truth + eps));
    // TODO(marco): test keeping the loss as a float3
    float loss = (error.x + error.y + error.z) / 3;

    return loss;
}

// KL divergence loss for the importance sampler: KL(p || f·cos).
// wi, wo and f_scalar are fixed constants (no_diff): wo was sampled from the proxy
// in the calling function and f_scalar is the reference BRDF evaluated at that wo.
// Gradients flow only into sampler_decoder weights; latents are detached via no_diff.
[Differentiable]
float compute_grads_sampler(no_diff float[8] latents, no_diff float3 wi, no_diff float3 wo,
    no_diff float f_scalar, ImportanceSamplingDecoder sampler_decoder)
{
    // Build sampler input: [wi (3), latents (8)]
    float input[11];
    input[0] = wi.x;
    input[1] = wi.y;
    input[2] = wi.z;
    [ForceUnroll]
    for (int i = 0; i < 8; ++i)
        input[3 + i] = latents[i];

    // Run sampler decoder — gradients flow through these parameters
    float[9] output = sampler_decoder.eval(input);

    float3 alpha;
    float2 slopeSpec, slopeDiff;
    float wSpec, wDiff;
    extract_differentiable_ggx(output, alpha, slopeSpec, slopeDiff, wSpec, wDiff);

    // Evaluate the analytic proxy PDF at the fixed wo (differentiable w.r.t. GGX params)
    float pdf = wSpec          * pdf_specular(wi, wo, alpha, slopeSpec)
              + (1 - wSpec)  * pdf_diffuse(wi, wo, slopeDiff);
    float log_p = log(max(pdf, 1e-6));

    // Reference target: log(f(wi,wo) * cos_theta) — treated as a constant
    float log_f = log(max(f_scalar, 1e-6));

    // KL = E_{wo~p}[log_p - log_f]
    return log_p - log_f;
}

[Differentiable]
float compute_grads_brdf_with_encoder(no_diff float[11] encoder_inputs, no_diff float3 ground_truth, no_diff float3 wi, no_diff float3 wo,
    Encoder encoder, FrameDecoder frame_decoder, BrdfDecoder brdf_decoder)
{
    float[8] latents = encoder.eval(encoder_inputs);
    float3 predicted = eval_brdf(latents, frame_decoder, wi, wo, brdf_decoder);

    return l1_log_loss(predicted, ground_truth);
}

[Differentiable]
float compute_grads_brdf_with_latent_texture<int Levels>(no_diff int2 uv, no_diff float3 ground_truth, no_diff float3 wi, no_diff float3 wo, no_diff uint lod,
                                             LatentTexturePyramid<Levels> latent_texture_pyramid, FrameDecoder frame_decoder, BrdfDecoder brdf_decoder)
{
    float[8] latents = latent_texture_pyramid.eval(uv, lod);
    float3 predicted = eval_brdf(latents, frame_decoder, wi, wo, brdf_decoder);

    return l1_log_loss(predicted, ground_truth);
}

void read_encoder_inputs(uint offset, float3 normal, float roughness, Tensor<float, 1> k,out float encoder_inputs[11]) {
    encoder_inputs[0] = normal.x;
    encoder_inputs[1] = normal.y;
    encoder_inputs[2] = normal.z;
    encoder_inputs[3] = k.getv(offset + 3);
    encoder_inputs[4] = k.getv(offset + 4);
    encoder_inputs[5] = k.getv(offset + 5);
    encoder_inputs[6] = k.getv(offset + 6);
    encoder_inputs[7] = k.getv(offset + 7);
    encoder_inputs[8] = k.getv(offset + 8);
    encoder_inputs[9] = roughness;
    encoder_inputs[10] = k.getv(offset + 10);
}

public void train_brdf_with_encoder(uint seed, Tensor<float, 1> k, int batch_index, int batch_size, Encoder encoder, FrameDecoder frame_decoder, BrdfDecoder brdf_decoder, RWTensor<float, 1> loss_output) {
    LCG lcg = LCG(seed);
    // NOTE(marco): need to be careful about the order of random number generation here, as it needs to be consistent with compute_random_batch
    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(batch_size);

    uint offset = batch_index * 11;
    float3 normal = float3(k.getv(offset + 0), k.getv(offset + 1), k.getv(offset + 2));
    float3 tangent = float3(k.getv(offset + 3), k.getv(offset + 4), k.getv(offset + 5));
    float3 bitangent = normalize( cross( normal, tangent ) );
    float roughness = k.getv(offset + 9);

    float phi_h = lcg.next_float() * 2.0 * M_PI;
    float theta_h = lcg.next_float() * 0.5 * M_PI;
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d = lcg.next_float() * 2.0 * M_PI;
    float theta_d = lcg.next_float() * 0.5 * M_PI;
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame);
    float3 wo = 2 * dot(wi, h) * h - wi;

    float encoder_inputs[11];
    read_encoder_inputs(offset, normal, roughness, k, encoder_inputs);

    float3 albedo = float3(k.getv(offset + 6), k.getv(offset + 7), k.getv(offset + 8));
    // TODO(marco): read metalness from texture?
    float3 ground_truth = eval_disney_brdf(albedo, wo, wi, normal, roughness, 0.0 /* metallic */, 1.0 /* specular */);

    loss_output.setv(batch_index, compute_grads_brdf_with_encoder(encoder_inputs, ground_truth, wi, wo, encoder, frame_decoder, brdf_decoder));

    bwd_diff(compute_grads_brdf_with_encoder)(encoder_inputs, ground_truth, wi, wo, encoder, frame_decoder, brdf_decoder, 1.0);
}

public void train_brdf_with_latent_texture<int Levels>(uint seed, Tensor<float, 1> k, int batch_index, int batch_size, LatentTexturePyramid<Levels> latent_texture_pyramid, FrameDecoder frame_decoder, BrdfDecoder brdf_decoder, RWTensor<float, 1> loss_output) {
    LCG lcg = LCG(seed);
    // NOTE(marco): need to be careful about the order of random number generation here, as it needs to be consistent with compute_random_batch
    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(batch_size);

    uint offset = batch_index * 11;
    float3 normal = float3(k.getv(offset + 0), k.getv(offset + 1), k.getv(offset + 2));
    float3 tangent = float3(k.getv(offset + 3), k.getv(offset + 4), k.getv(offset + 5));
    float3 bitangent = normalize(cross(normal, tangent));
    float roughness = k.getv(offset + 9);

    float phi_h = lcg.next_float() * 2.0 * M_PI;
    float theta_h = lcg.next_float() * 0.5 * M_PI;
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d = lcg.next_float() * 2.0 * M_PI;
    float theta_d = lcg.next_float() * 0.5 * M_PI;
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame);
    float3 wo = 2 * dot(wi, h) * h - wi;

    float3 albedo = float3(k.getv(offset + 6), k.getv(offset + 7), k.getv(offset + 8));
    // TODO(marco): read metalness from texture?
    float3 ground_truth = eval_disney_brdf(albedo, wo, wi, normal, roughness, 0.0 /* metallic */, 1.0 /* specular */);

    uint[2] shape = latent_texture_pyramid.textures0[0].texture.shape;
    int2 pixel = int2(uv * float2(shape[1], shape[0]));

    uint lod = uint(lcg.next_float() * Levels);

    loss_output.setv(batch_index, compute_grads_brdf_with_latent_texture<Levels>(pixel, ground_truth, wi, wo, lod, latent_texture_pyramid, frame_decoder, brdf_decoder));

    bwd_diff(compute_grads_brdf_with_latent_texture<Levels>)(pixel, ground_truth, wi, wo, lod, latent_texture_pyramid, frame_decoder, brdf_decoder, 1.0);
}

// Train the importance sampling decoder for one sample.
// wi is sampled via Rusinkiewicz parameterization (same as BRDF training).
// wo is drawn from the current analytic proxy — it is a constant from the perspective
// of the KL gradient, so we sample it in forward mode before calling bwd_diff.
// Gradients flow only into sampler_decoder; latents are detached inside compute_kl_loss.
void train_sampler_with_encoder(uint seed, Tensor<float, 1> k, int batch_index, int batch_size,
    Encoder encoder, BrdfDecoder brdf_decoder, ImportanceSamplingDecoder sampler_decoder)
{
    LCG lcg = LCG(seed);
    // NOTE(marco): random number order must match compute_random_batch
    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(batch_size);

    uint offset = batch_index * 11;
    float3 normal   = float3(k.getv(offset + 0), k.getv(offset + 1), k.getv(offset + 2));
    float roughness = k.getv(offset + 9);

    // --- Sample wi via Rusinkiewicz parameterization ---
    float phi_h   = lcg.next_float() * 2.0 * M_PI;
    float theta_h = acos(lcg.next_float());          // uniform on hemisphere
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d   = lcg.next_float() * 2.0 * M_PI;
    float theta_d = acos(lcg.next_float());
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame_h = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame_h);
    // TODO(marco): loop until we get a valid sample
    if (wi.z <= 0.0) return; // below horizon

    // --- Get latents from encoder (detached: no KL gradient flows back here) ---
    float encoder_inputs[11];
    read_encoder_inputs(offset, normal, roughness, k, encoder_inputs);
    float[8] latents = encoder.eval(encoder_inputs);

    // --- Run sampler decoder in primal mode to get GGX params for sampling wo ---
    float sampler_input[11];
    sampler_input[0] = wi.x;
    sampler_input[1] = wi.y;
    sampler_input[2] = wi.z;
    [ForceUnroll]
    for (int i = 0; i < 8; ++i)
        sampler_input[3 + i] = latents[i];
    float[9] so = sampler_decoder.eval(sampler_input);

    float3 alpha;
    float2 slopeSpec, slopeDiff;
    float wSpec, wDiff;
    extract_differentiable_ggx(so, alpha, slopeSpec, slopeDiff, wSpec, wDiff);

    // --- Sample wo from the analytic proxy ---
    float pdf_sample;
    float2 u = float2(lcg.next_float(), lcg.next_float());
    float3 wo = sample_analytic(
        float3(alpha.x, alpha.y, alpha.z),
        float2(slopeSpec.x, slopeSpec.y),
        float2(slopeDiff.x, slopeDiff.y),
        wSpec, wi, u, pdf_sample);

    // TODO(marco): loop until we get a valid sample
    if (wo.z <= 0.0 || pdf_sample <= 0.0) return; // degenerate sample

    // --- Evaluate reference BRDF at wo (fixed target, no gradient needed) ---
    float3 albedo = float3(k.getv(offset + 6), k.getv(offset + 7), k.getv(offset + 8));
    float3 brdf_val = eval_disney_brdf(albedo, wo, wi, normal, roughness, 0.0 /* metallic */, 1.0 /* specular */);
    float cos_theta = max(wo.z, 0.0);
    // Collapse RGB to luminance and apply cosine weight
    float f_scalar = (0.2126 * brdf_val.r + 0.7152 * brdf_val.g + 0.0722 * brdf_val.b) * cos_theta;

    // --- Backpropagate KL loss into sampler_decoder only ---
    // latents are passed as no_diff inside compute_kl_loss (detached)
    bwd_diff(compute_grads_sampler)(latents, wi, wo, f_scalar, sampler_decoder, 1.0);
}

void train_sampler_with_latent_texture<int Levels>(uint seed, Tensor<float, 1> k, int batch_index, int batch_size,
                                LatentTexturePyramid<Levels> latent_texture, BrdfDecoder brdf_decoder, ImportanceSamplingDecoder sampler_decoder)
{
    LCG lcg = LCG(seed);
    // NOTE(marco): random number order must match compute_random_batch
    float2 uv = (batch_index + float2(lcg.next_float(), lcg.next_float())) / float2(batch_size);

    uint offset = batch_index * 11;
    float3 normal = float3(k.getv(offset + 0), k.getv(offset + 1), k.getv(offset + 2));
    float roughness = k.getv(offset + 9);

    // --- Sample wi via Rusinkiewicz parameterization ---
    float phi_h = lcg.next_float() * 2.0 * M_PI;
    float theta_h = acos(lcg.next_float()); // uniform on hemisphere
    float3 h = float3(sin(theta_h) * cos(phi_h), sin(theta_h) * sin(phi_h), cos(theta_h));

    float phi_d = lcg.next_float() * 2.0 * M_PI;
    float theta_d = acos(lcg.next_float());
    float3 d = float3(sin(theta_d) * cos(phi_d), sin(theta_d) * sin(phi_d), cos(theta_d));

    float3 Th, Bh;
    tangents(h, Th, Bh);
    float3x3 frame_h = float3x3(Th, Bh, h);
    float3 wi = mul(d, frame_h);
    // TODO(marco): loop until we get a valid sample
    if (wi.z <= 0.0) return; // below horizon

    // --- Get latents from encoder (detached: no KL gradient flows back here) ---
    uint[2] shape = latent_texture.textures0[0].texture.shape;
    int2 pixel = int2(uv * float2(shape[1], shape[0]));
    float[8] latents = latent_texture.eval(pixel, 0); // TODO(marco): random LOD

    // --- Run sampler decoder in primal mode to get GGX params for sampling wo ---
    float sampler_input[11];
    sampler_input[0] = wi.x;
    sampler_input[1] = wi.y;
    sampler_input[2] = wi.z;
    [ForceUnroll]
    for (int i = 0; i < 8; ++i)
        sampler_input[3 + i] = latents[i];
    float[9] so = sampler_decoder.eval(sampler_input);

    float3 alpha;
    float2 slopeSpec, slopeDiff;
    float wSpec, wDiff;
    extract_differentiable_ggx(so, alpha, slopeSpec, slopeDiff, wSpec, wDiff);

    // --- Sample wo from the analytic proxy ---
    float pdf_sample;
    float2 u = float2(lcg.next_float(), lcg.next_float());
    float3 wo = sample_analytic(
        float3(alpha.x, alpha.y, alpha.z),
        float2(slopeSpec.x, slopeSpec.y),
        float2(slopeDiff.x, slopeDiff.y),
        wSpec, wi, u, pdf_sample);

    // TODO(marco): loop until we get a valid sample
    if (wo.z <= 0.0 || pdf_sample <= 0.0) return; // degenerate sample

    // --- Evaluate reference BRDF at wo (fixed target, no gradient needed) ---
    float3 albedo = float3(k.getv(offset + 6), k.getv(offset + 7), k.getv(offset + 8));
    float3 brdf_val = eval_disney_brdf(albedo, wo, wi, normal, roughness, 0.0 /* metallic */, 1.0 /* specular */);
    float cos_theta = max(wo.z, 0.0);
    // Collapse RGB to luminance and apply cosine weight
    float f_scalar = (0.2126 * brdf_val.r + 0.7152 * brdf_val.g + 0.0722 * brdf_val.b) * cos_theta;

    // --- Backpropagate KL loss into sampler_decoder only ---
    // latents are passed as no_diff inside compute_kl_loss (detached)
    bwd_diff(compute_grads_sampler)(latents, wi, wo, f_scalar, sampler_decoder, 1.0);
}

public void copy_encoder_to_latent_texture<int Levels>(Encoder encoder, LatentTexturePyramid<Levels> latent_texture_pyramid,
                                    int2 batch_index,
                                    Tensor<float4, 2> normal_map, Tensor<float, 2> roughness_map) {

    float roughness = roughness_map.getv(batch_index);
    float3 normal = normal_map.getv(batch_index).xyz;
    // Derive a consistent tangent using the stable tangents() helper.
    float3 tangent, bitangent_unused;
    tangents(normal, tangent, bitangent_unused);

    float3 albedo = float3(0.5, 0.5, 0.5);

    float encoder_inputs[11];
    encoder_inputs[0] = normal.x;
    encoder_inputs[1] = normal.y;
    encoder_inputs[2] = normal.z;
    encoder_inputs[3] = tangent.x;
    encoder_inputs[4] = tangent.y;
    encoder_inputs[5] = tangent.z;
    encoder_inputs[6] = albedo.x;
    encoder_inputs[7] = albedo.y;
    encoder_inputs[8] = albedo.z;
    encoder_inputs[9] = roughness;
    encoder_inputs[10] = 1.0;

    float[8] latents = encoder.eval(encoder_inputs);

    latent_texture_pyramid.textures0[0].texture.setv(batch_index, float4(latents[0], latents[1], latents[2], latents[3]));
    latent_texture_pyramid.textures1[0].texture.setv(batch_index, float4(latents[4], latents[5], latents[6], latents[7]));
}

// Downsamples one MIP level of the latent texture pyramid by 2x2 box filtering.
// Dispatch one thread per texel at the DESTINATION (coarser) level.
// batch_index ranges over [0, dst_width) x [0, dst_height).
// src_level is the finer level to read from; dst_level = src_level + 1.
public void downsample_latent_pyramid<int Levels>(LatentTexturePyramid<Levels> latent_texture_pyramid,
                                           int2 batch_index, int src_level, int dst_level)
{
    // 2x2 source texel positions
    int2 s00 = batch_index * 2;
    int2 s10 = s00 + int2(1, 0);
    int2 s01 = s00 + int2(0, 1);
    int2 s11 = s00 + int2(1, 1);

    // Box-filter latents from src_level
    float4 avg0 = (latent_texture_pyramid.textures0[src_level].get_latent(s00.x, s00.y) +
                   latent_texture_pyramid.textures0[src_level].get_latent(s10.x, s10.y) +
                   latent_texture_pyramid.textures0[src_level].get_latent(s01.x, s01.y) +
                   latent_texture_pyramid.textures0[src_level].get_latent(s11.x, s11.y)) * 0.25f;

    float4 avg1 = (latent_texture_pyramid.textures1[src_level].get_latent(s00.x, s00.y) +
                   latent_texture_pyramid.textures1[src_level].get_latent(s10.x, s10.y) +
                   latent_texture_pyramid.textures1[src_level].get_latent(s01.x, s01.y) +
                   latent_texture_pyramid.textures1[src_level].get_latent(s11.x, s11.y)) * 0.25f;

    latent_texture_pyramid.textures0[dst_level].texture.setv(batch_index, avg0);
    latent_texture_pyramid.textures1[dst_level].texture.setv(batch_index, avg1);
}

/* TODO(marco):
 * - LEAN to prefilter material parameters
 * - Mollification
 * - Filtering (section 5.2)
 * - Want to understand pdf_specular

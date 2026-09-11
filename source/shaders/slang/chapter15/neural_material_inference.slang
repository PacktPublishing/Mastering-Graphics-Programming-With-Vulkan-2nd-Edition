#ifndef NEURAL_MATERIAL_INFERENCE_SLANG
#define NEURAL_MATERIAL_INFERENCE_SLANG

#include "platform.slangh"

#include "neural_material_structs.h"
#include "neural_material_common.slangh"
#include "neural_material_layers.slangh"
#include "neural_material_brdf.slangh"

#if defined(COMPUTE_PREVIEW_BRDF)
ConstantBuffer<NMPreviewConstants> g_preview_constants : register(b0, space1);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    const uint x = dispatch_thread_id.x;
    const uint y = dispatch_thread_id.y;

    if (x >= g_preview_constants.width || y >= g_preview_constants.height)
    {
        return;
    }

    const uint panel_width = g_preview_constants.width / 3;
    if (panel_width == 0)
    {
        return;
    }

    const uint panel_index = min(x / panel_width, 2u);
    const uint local_x = x - panel_index * panel_width;

    RWTexture2D<float4> output_texture = global_images_2d[ g_preview_constants.output_texture_index ];
    Texture2D<float4> normal_map = texture_2d( g_preview_constants.normal_map_index );
    Texture2D<float> roughness_map = global_textures_f32[ g_preview_constants.roughness_map_index ];

    // Each panel visualizes the same underlying material pixel.
    float2 local_uv = (float2(local_x, y) + 0.5f.xx) / float2(panel_width, g_preview_constants.height);
    local_uv = clamp(local_uv, 0.0f.xx, 0.999999f.xx);

    // Convert from UV to material source dimensions
    const uint material_x = min( uint(local_uv.x * float(g_preview_constants.material_width)),
                                 g_preview_constants.material_width - 1u );

    const uint material_y = min( uint(local_uv.y * float(g_preview_constants.material_height)),
                                 g_preview_constants.material_height - 1u );

    float3 n = normal_map.Load(int3(material_x, material_y, 0)).xyz;
    n = safe_normalize(n * 2.0f - 1.0f);

    const float roughness = roughness_map.Load(int3(material_x, material_y, 0));

    float3 t;
    float3 b;
    build_tangent_frame(n, t, b);

    float3x3 tbn = float3x3(t, b, n);

    // Use the same placeholder albedo as training for consistency.
    const float3 albedo = float3(0.5843139, 0.19215536, 0.000000);

    float3 out_color = 0.0f.xxx;

    // Fixed light/view for the first preview.
    // Following convention wi = view and wo = light
    const float3 V = safe_normalize(float3(0.3f, 0.4f, 0.85f));
    const float3 L = safe_normalize(float3(0.0f, 0.0f, 1.0f));
    const float3 wi = mul(tbn, V);
    const float3 wo = mul(tbn, L);

    if (panel_index == 0) {
        const float3 target_brdf = eval_disney_brdf(albedo, wo, wi, n, roughness, 0.0f, 1.0f);

        out_color = target_brdf;
    }
    else {
        float latent_values[8];

        if (g_preview_constants.use_latent_textures != 0) {
            latent_eval(g_preview_constants.latent_pyramid, local_uv, 0, latent_values);
        }
        else {
            float input_values[11];

            input_values[0] = n.x;
            input_values[1] = n.y;
            input_values[2] = n.z;

            input_values[3] = t.x;
            input_values[4] = t.y;
            input_values[5] = t.z;

            input_values[6] = albedo.x;
            input_values[7] = albedo.y;
            input_values[8] = albedo.z;

            input_values[9] = roughness;
            input_values[10] = 1.0f;

            encoder_eval(g_preview_constants.encoder, input_values, latent_values);
        }

        const float3 neural_brdf = eval_neural_brdf(latent_values, wi, wo, g_preview_constants.frame_decoder, g_preview_constants.brdf_decoder);

        if (panel_index == 1) {
            out_color = neural_brdf;
        }
        else {
            const float3 target_brdf = eval_disney_brdf(albedo, wo, wi, n, roughness, 0.0f, 1.0f);

            const float error_scale = 8.0f;
            out_color = saturate(sqrt(target_brdf*target_brdf - neural_brdf*neural_brdf));
        }
    }


    output_texture[int2(x, y)] = float4(out_color, 1.0f);
}
#endif

#endif // NEURAL_MATERIAL_INFERENCE_SLANG

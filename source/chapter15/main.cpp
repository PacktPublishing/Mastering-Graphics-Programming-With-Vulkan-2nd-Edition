#include "application/game_camera.hpp"
#include "application/input.hpp"
#include "application/window.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/renderer.hpp"
#include "graphics/frame_graph.hpp"

#include "foundation/array.hpp"
#include "foundation/file.hpp"
#include "foundation/memory.hpp"
#include "foundation/numerics.hpp"
#include "foundation/resource_manager.hpp"
#include "foundation/static_array.hpp"
#include "foundation/static_string.hpp"
#include "foundation/string.hpp"
#include "foundation/string_view.hpp"
#include "foundation/time.hpp"

#include "external/cglm/struct/mat3.h"
#include "external/cglm/struct/vec2.h"
#include "external/imgui/imgui.h"
#include "external/stb_image.h"
#include "external/tracy/tracy/Tracy.hpp"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "generated/compute_random_batch.h"
#include "generated/compute_train_encoder.h"
#include "generated/compute_train_latent_texture.h"
#include "generated/copy_encoder_to_latent_texture.h"
#include "generated/optimizer_step_buffer.h"
#include "generated/optimizer_step_texture.h"
#include "generated/downsample_latent_pyramid.h"
#include "generated/preview_brdf.h"

#include "../shaders/slang/chapter15/neural_material_structs.h"

namespace idra {

using namespace raptor;

struct TextureHandle {
    ImageHandle       image;
    ImageViewHandle   view;

    u32 index() const { return view.index(); }
    operator ImageHandle() const { return image; }
};

struct TextureAsset {
    TextureHandle     texture;
    TextureResource*  resource = nullptr;
};

// Neural materials explanation //////////////////////////////////////////

// Neural networks are composed by different layers, often with additional bias to optimize the learning process.
//
// In the demo we use different networks:
//
// Encoder (3 layers):
//     l0 = 11 -> 32
//     l1 = 32 -> 32
//     l2 = 32 -> 8
//
// FrameDecoder (1 layer):
//     l0 = 8 -> 12
//
// BrdfDecoder (3 layers):
//     l0 = 20 -> 32
//     l1 = 32 -> 32
//     l2 = 32 -> 3
//
// ImportanceSamplingDecoder (4 layers):
//     l0 = 11 -> 32
//     l1 = 32 -> 32
//     l2 = 32 -> 32
//     l3 = 32 -> 9

static const u32 BATCH_SIZE              = 8192;
static const u32 MICROBATCH_SIZE         = 256;
static const u32 WORKGROUP_COUNT_X       = 32;
static const u32 LATENT_PYRAMID_LEVELS   = 11;
static const u32 LATENT_TEXTURE_SIZE     = 2048;
static const u32 ENCODER_TRAINING_EPOCHS = 200000;
static const f32 NM_LEARNING_RATE        = 1e-3f;
static const u32 LOSS_CHANNEL_COUNT      = 3;
static const u32 LOSS_HISTORY_CAPACITY   = 500;
static const u32 NEURAL_PREVIEW_PANEL_WIDTH = 1024;
static const u32 NEURAL_PREVIEW_PANEL_HEIGHT = 1024;
static const u32 NEURAL_PREVIEW_WIDTH = NEURAL_PREVIEW_PANEL_WIDTH * 3;
static const u32 NEURAL_PREVIEW_HEIGHT = NEURAL_PREVIEW_PANEL_HEIGHT;

static_assert( ( BATCH_SIZE % MICROBATCH_SIZE ) == 0, "Microbatch size must divide the full batch size" );

//
struct LatentPyramidResources {
    TextureHandle   textures0[ LATENT_PYRAMID_LEVELS ];
    TextureHandle   textures1[ LATENT_PYRAMID_LEVELS ];

    BufferHandle    grads0[ LATENT_PYRAMID_LEVELS ];
    BufferHandle    grads1[ LATENT_PYRAMID_LEVELS ];

    TextureHandle   exp_avg0[ LATENT_PYRAMID_LEVELS ];
    TextureHandle   exp_avg1[ LATENT_PYRAMID_LEVELS ];

    TextureHandle   exp_avg_sq0[ LATENT_PYRAMID_LEVELS ];
    TextureHandle   exp_avg_sq1[ LATENT_PYRAMID_LEVELS ];

    u32             level_count = 1;
    u32             width       = LATENT_TEXTURE_SIZE;
    u32             height      = LATENT_TEXTURE_SIZE;
}; // struct LatentPyramidResources

//
struct NeuralWeightBiasLayer {

    // Weights and biases for the layer
    BufferHandle        weights;
    BufferHandle        biases;

    // Gradients for weights and biases, used during training to update the parameters
    BufferHandle        weights_grad;
    BufferHandle        weights_grad_training;
    BufferHandle        biases_grad;

    // Weights and biases moments and variance for optimization algorithms like Adam,
    // which require maintaining running averages of gradients and squared gradients.
    BufferHandle        weights_exp_avg;      // Adam m
    BufferHandle        weights_exp_avg_sq;   // Adam v

    BufferHandle        biases_exp_avg;
    BufferHandle        biases_exp_avg_sq;

    u32                 inputs;
    u32                 outputs;
    u32                 weights_grad_training_size = 0;

}; // struct NeuralWeightBiasLayer

//
struct NeuralWeightLayer {

    BufferHandle        weights;

    BufferHandle        weights_grad;
    BufferHandle        weights_grad_training;

    BufferHandle        weights_exp_avg;
    BufferHandle        weights_exp_avg_sq;

    u32                 inputs;
    u32                 outputs;
    u32                 weights_grad_training_size = 0;

}; // struct NeuralWeightLayer

// NeuralMaterialTrainer
struct NeuralMaterialTrainer {

    void                    create_resources( Renderer* renderer, FrameGraph* frame_graph );
    void                    destroy_resources( Renderer* renderer );

    void                    create_wb_layer_resources( NeuralWeightBiasLayer& layer, u32 inputs, u32 outputs, cstring prefix );
    void                    create_w_layer_resources( NeuralWeightLayer& layer, u32 inputs, u32 outputs, cstring prefix );

    void                    destroy_wb_layer_resources( NeuralWeightBiasLayer& layer );
    void                    destroy_w_layer_resources( NeuralWeightLayer& layer );

    void                    step( CommandBuffer* cb );

    void                    initialize_wb_layer_buffers( NeuralWeightBiasLayer& layer );
    void                    initialize_w_layer_buffers( NeuralWeightLayer& layer );


    // Network model
    NeuralWeightBiasLayer   encoder[ 3 ];
    NeuralWeightLayer       frame_decoder;
    NeuralWeightBiasLayer   brdf_decoder[ 3 ];
    NeuralWeightBiasLayer   sampler_decoder[ 4 ];
    LatentPyramidResources  latent_pyramid;

    BufferHandle            batch_kx;
    BufferHandle            loss_gpu;
    BufferHandle            loss_cpu;
    f32                     current_loss[ LOSS_CHANNEL_COUNT ]{};
    f32                     loss_history[ LOSS_CHANNEL_COUNT ][ LOSS_HISTORY_CAPACITY ]{};
    u32                     loss_history_size = 0;
    u32                     loss_history_start = 0;
    u64                     loss_history_sample_count = 0;

    TextureAsset            material_normal;
    TextureAsset            material_roughness;

    ComputePipelineState    random_batch_pipeline;
    PipelineHandle          random_batch_pso;
    DescriptorSetLayoutHandle random_batch_dsl;
    DescriptorSetHandle     random_batch_ds;

    ComputePipelineState    brdf_encoder_train_pipeline;
    PipelineHandle          brdf_encoder_train_pso;
    DescriptorSetLayoutHandle brdf_encoder_train_dsl;
    DescriptorSetHandle     brdf_encoder_train_ds;

    ComputePipelineState    brdf_latent_texture_train_pipeline;
    PipelineHandle          brdf_latent_texture_train_pso;
    DescriptorSetLayoutHandle brdf_latent_texture_train_dsl;
    DescriptorSetHandle     brdf_latent_texture_train_ds;

    ComputePipelineState    optimizer_step_buffer_pipeline;
    PipelineHandle          optimizer_step_buffer_pso;
    DescriptorSetLayoutHandle optimizer_step_buffer_dsl;
    DescriptorSetHandle     optimizer_step_buffer_ds;

    // ShaderAsset*            sampler_encoder_train_shader;
    // PipelineHandle          sampler_encoder_train_pso;
    // DescriptorSetLayoutHandle sampler_encoder_train_dsl;
    // DescriptorSetHandle     sampler_encoder_train_ds;

    // ShaderAsset*            sampler_latent_texture_train_shader;
    // PipelineHandle          sampler_latent_texture_train_pso;
    // DescriptorSetLayoutHandle sampler_latent_texture_train_dsl;
    // DescriptorSetHandle     sampler_latent_texture_train_ds;

    ComputePipelineState    copy_encoder_to_latent_pipeline;
    PipelineHandle          copy_encoder_to_latent_pso;
    DescriptorSetLayoutHandle copy_encoder_to_latent_dsl;
    DescriptorSetHandle     copy_encoder_to_latent_ds;

    ComputePipelineState    optimizer_step_texture_pipeline;
    PipelineHandle          optimizer_step_texture_pso;
    DescriptorSetLayoutHandle optimizer_step_texture_dsl;
    DescriptorSetHandle     optimizer_step_texture_ds;

    ComputePipelineState    downsample_latent_pyramid_pipeline;
    PipelineHandle          downsample_latent_pyramid_pso;
    // DescriptorSetLayoutHandle downsample_latent_pyramid_dsl;
    // DescriptorSetHandle     downsample_latent_pyramid_ds;

    ComputePipelineState    preview_brdf_pipeline;
    PipelineHandle          preview_brdf_pso;
    DescriptorSetLayoutHandle preview_brdf_dsl;
    DescriptorSetHandle     preview_brdf_ds;
    TextureHandle           preview_texture;

    u32                     epoch       = 0;

    GpuDevice*              gpu         = nullptr;

    bool                    initialized = false;

}; // struct NeuralMaterialTrainer

static f32 get_loss_history_value( const NeuralMaterialTrainer& trainer, u32 channel_index, u32 history_index ) {
    return trainer.loss_history[ channel_index ][ ( trainer.loss_history_start + history_index ) % LOSS_HISTORY_CAPACITY ];
}

static f32 lerp_f32( f32 a, f32 b, f32 t ) {
    return a + ( b - a ) * t;
}

static void push_loss_history_sample( NeuralMaterialTrainer& trainer, const f32 loss_values[ LOSS_CHANNEL_COUNT ] ) {
    ++trainer.loss_history_sample_count;

    if ( trainer.loss_history_size < LOSS_HISTORY_CAPACITY ) {
        for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
            trainer.loss_history[ channel_index ][ trainer.loss_history_size ] = loss_values[ channel_index ];
        }
        ++trainer.loss_history_size;
    } else {
        for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
            trainer.loss_history[ channel_index ][ trainer.loss_history_start ] = loss_values[ channel_index ];
        }
        trainer.loss_history_start = ( trainer.loss_history_start + 1 ) % LOSS_HISTORY_CAPACITY;
    }
}

static void draw_training_loss_plot( const NeuralMaterialTrainer& trainer ) {
    if ( trainer.loss_history_size == 0 ) {
        ImGui::TextDisabled( "No loss samples yet." );
        return;
    }

    ImVec2 available_size = ImGui::GetContentRegionAvail();
    ImVec2 canvas_size = { max( available_size.x, 1.0f ), max( available_size.y, 220.0f ) };

    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_max = { canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y };
    ImGui::InvisibleButton( "##training_loss_plot", canvas_size );

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 frame_bg = ImGui::GetColorU32( ImGuiCol_FrameBg );
    const ImU32 border = ImGui::GetColorU32( ImGuiCol_Border );
    const ImU32 grid = ImGui::GetColorU32( ImVec4( 1.0f, 1.0f, 1.0f, 0.10f ) );
    const ImU32 axis = ImGui::GetColorU32( ImVec4( 1.0f, 1.0f, 1.0f, 0.35f ) );
    const ImU32 text = ImGui::GetColorU32( ImGuiCol_Text );
    const ImU32 line_colors[ LOSS_CHANNEL_COUNT ] = {
        ImGui::GetColorU32( ImVec4( 0.95f, 0.35f, 0.35f, 1.0f ) ),
        ImGui::GetColorU32( ImVec4( 0.35f, 0.85f, 0.45f, 1.0f ) ),
        ImGui::GetColorU32( ImVec4( 0.35f, 0.60f, 0.95f, 1.0f ) )
    };
    const ImU32 line_soft_colors[ LOSS_CHANNEL_COUNT ] = {
        ImGui::GetColorU32( ImVec4( 0.95f, 0.35f, 0.35f, 0.22f ) ),
        ImGui::GetColorU32( ImVec4( 0.35f, 0.85f, 0.45f, 0.22f ) ),
        ImGui::GetColorU32( ImVec4( 0.35f, 0.60f, 0.95f, 0.22f ) )
    };
    const char* channel_labels[ LOSS_CHANNEL_COUNT ] = { "R", "G", "B" };

    draw_list->AddRectFilled( canvas_min, canvas_max, frame_bg, 4.0f );
    draw_list->AddRect( canvas_min, canvas_max, border, 4.0f );

    const f32 left_margin = 58.0f;
    const f32 right_margin = 14.0f;
    const f32 top_margin = 18.0f;
    const f32 bottom_margin = 42.0f;
    const ImVec2 plot_min = { canvas_min.x + left_margin, canvas_min.y + top_margin };
    const ImVec2 plot_max = { canvas_max.x - right_margin, canvas_max.y - bottom_margin };

    if ( plot_max.x <= plot_min.x || plot_max.y <= plot_min.y ) {
        return;
    }

    draw_list->AddRectFilled( plot_min, plot_max, ImGui::GetColorU32( ImVec4( 0.08f, 0.08f, 0.08f, 0.65f ) ), 2.0f );
    draw_list->AddRect( plot_min, plot_max, axis, 2.0f );

    f32 min_loss = get_loss_history_value( trainer, 0, 0 );
    f32 max_loss = min_loss;
    for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
        const u32 start_index = channel_index == 0 ? 1u : 0u;
        for ( u32 i = start_index; i < trainer.loss_history_size; ++i ) {
            const f32 value = get_loss_history_value( trainer, channel_index, i );
            min_loss = value < min_loss ? value : min_loss;
            max_loss = value > max_loss ? value : max_loss;
        }
    }

    f32 loss_range = max_loss - min_loss;
    if ( loss_range <= 1e-6f ) {
        loss_range = max( fabsf( max_loss ) * 0.1f, 1e-4f );
        min_loss -= 0.5f * loss_range;
        max_loss += 0.5f * loss_range;
    } else {
        const f32 padding = loss_range * 0.1f;
        min_loss -= padding;
        max_loss += padding;
        loss_range = max_loss - min_loss;
    }

    const u32 y_tick_count = 5;
    char label_buffer[ 64 ];
    for ( u32 tick = 0; tick < y_tick_count; ++tick ) {
        const f32 ratio = y_tick_count > 1 ? f32( tick ) / f32( y_tick_count - 1 ) : 0.0f;
        const f32 y = lerp_f32( plot_max.y, plot_min.y, ratio );
        const f32 tick_value = lerp_f32( min_loss, max_loss, ratio );
        draw_list->AddLine( { plot_min.x, y }, { plot_max.x, y }, grid );
        snprintf( label_buffer, sizeof( label_buffer ), "%.4g", tick_value );
        const ImVec2 text_size = ImGui::CalcTextSize( label_buffer );
        draw_list->AddText( { plot_min.x - text_size.x - 8.0f, y - text_size.y * 0.5f }, text, label_buffer );
    }

    const u64 first_sample_index = trainer.loss_history_sample_count > trainer.loss_history_size
        ? trainer.loss_history_sample_count - trainer.loss_history_size
        : 0;
    const u32 x_tick_count = trainer.loss_history_size > 1 ? 6u : 1u;
    for ( u32 tick = 0; tick < x_tick_count; ++tick ) {
        const f32 ratio = x_tick_count > 1 ? f32( tick ) / f32( x_tick_count - 1 ) : 0.0f;
        const f32 x = lerp_f32( plot_min.x, plot_max.x, ratio );
        draw_list->AddLine( { x, plot_min.y }, { x, plot_max.y }, grid );

        const u32 sample_offset = trainer.loss_history_size > 1
            ? u32( ratio * f32( trainer.loss_history_size - 1 ) + 0.5f )
            : 0u;
        const unsigned long long sample_label_value = first_sample_index + sample_offset + 1;
        snprintf( label_buffer, sizeof( label_buffer ), "%llu", sample_label_value );
        const ImVec2 text_size = ImGui::CalcTextSize( label_buffer );
        draw_list->AddText( { x - text_size.x * 0.5f, plot_max.y + 6.0f }, text, label_buffer );
    }

    draw_list->AddText( { plot_min.x + 6.0f, canvas_min.y + 2.0f }, text, "Loss" );
    for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
        const ImVec2 label_size = ImGui::CalcTextSize( channel_labels[ channel_index ] );
        const f32 legend_x = plot_max.x - ( LOSS_CHANNEL_COUNT - channel_index ) * 22.0f;
        draw_list->AddText( { legend_x, canvas_min.y + 2.0f }, line_colors[ channel_index ], channel_labels[ channel_index ] );
        draw_list->AddLine( { legend_x - 14.0f, canvas_min.y + 10.0f }, { legend_x - 4.0f, canvas_min.y + 10.0f }, line_colors[ channel_index ], 2.0f );
    }
    const char* x_axis_label = "Epoch";
    const ImVec2 x_axis_label_size = ImGui::CalcTextSize( x_axis_label );
    draw_list->AddText( { plot_min.x + ( plot_max.x - plot_min.x - x_axis_label_size.x ) * 0.5f, canvas_max.y - x_axis_label_size.y - 6.0f }, text, x_axis_label );

    if ( trainer.loss_history_size == 1 ) {
        for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
            const f32 value = get_loss_history_value( trainer, channel_index, 0 );
            const f32 normalized = ( value - min_loss ) / loss_range;
            const f32 y = lerp_f32( plot_max.y, plot_min.y, normalized );
            draw_list->AddCircleFilled( { plot_min.x, y }, 3.0f, line_colors[ channel_index ] );
        }
        return;
    }

    ImVec2 points[ LOSS_HISTORY_CAPACITY ];
    const ImDrawListFlags previous_flags = draw_list->Flags;
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines;
    for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
        for ( u32 i = 0; i < trainer.loss_history_size; ++i ) {
            const f32 value = get_loss_history_value( trainer, channel_index, i );
            const f32 x_ratio = f32( i ) / f32( trainer.loss_history_size - 1 );
            const f32 y_ratio = ( value - min_loss ) / loss_range;
            points[ i ] = {
                lerp_f32( plot_min.x, plot_max.x, x_ratio ),
                lerp_f32( plot_max.y, plot_min.y, y_ratio )
            };
        }

        draw_list->AddPolyline( points, i32( trainer.loss_history_size ), line_soft_colors[ channel_index ], ImDrawFlags_None, 3.0f );
        draw_list->AddPolyline( points, i32( trainer.loss_history_size ), line_colors[ channel_index ], ImDrawFlags_None, 1.5f );
    }
    draw_list->Flags = previous_flags;
}


void NeuralMaterialTrainer::create_resources( Renderer* renderer, FrameGraph* frame_graph ) {

    gpu = renderer->gpu;

    create_wb_layer_resources( encoder[ 0 ], 11, 32, "encoder_l0" );
    create_wb_layer_resources( encoder[ 1 ], 32, 32, "encoder_l1" );
    create_wb_layer_resources( encoder[ 2 ], 32, 8, "encoder_l2" );

    create_w_layer_resources( frame_decoder, 8, 12, "frame_decoder_l0" );

    create_wb_layer_resources( brdf_decoder[ 0 ], 20, 32, "brdf_decoder_l0" );
    create_wb_layer_resources( brdf_decoder[ 1 ], 32, 32, "brdf_decoder_l1" );
    create_wb_layer_resources( brdf_decoder[ 2 ], 32, 3, "brdf_decoder_l2" );

    create_wb_layer_resources( sampler_decoder[ 0 ], 11, 32, "sampler_decoder_l0" );
    create_wb_layer_resources( sampler_decoder[ 1 ], 32, 32, "sampler_decoder_l1" );
    create_wb_layer_resources( sampler_decoder[ 2 ], 32, 32, "sampler_decoder_l2" );
    create_wb_layer_resources( sampler_decoder[ 3 ], 32, 9, "sampler_decoder_l3" );

    const VkBufferUsageFlags storage_buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VmaAllocationCreateFlags host_buffer_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    batch_kx = gpu->create_buffer( { .size = 11 * BATCH_SIZE * sizeof( f32 ), .usage = storage_buffer_usage, .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .allocation_flags = host_buffer_flags, .name = "batch_kx" } );
    gpu->add_buffer_to_bindless( batch_kx );

    loss_gpu = gpu->create_buffer( { .size = BATCH_SIZE * sizeof( f32 ) * 3, .usage = storage_buffer_usage, .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .name = "loss_gpu" } );
    gpu->add_buffer_to_bindless( loss_gpu );

    loss_cpu = gpu->create_buffer( { .size = BATCH_SIZE * sizeof( f32 ) * 3, .usage = storage_buffer_usage, .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .allocation_flags = host_buffer_flags, .name = "loss_cpu" } );
    gpu->add_buffer_to_bindless( loss_cpu );

    int normal_width = 0;
    int normal_height = 0;
    int normal_components = 0;
    u8* normal_data = stbi_load( RAPTOR_DATA_FOLDER "/Car_Paint_normal.png", &normal_width, &normal_height, &normal_components, 4 );
    RASSERT( normal_data != nullptr );
    material_normal.resource = renderer->create_texture( ImageCreation{
        .image_type   = VK_IMAGE_TYPE_2D,
        .format       = VK_FORMAT_R8G8B8A8_UNORM,
        .width        = ( u32 )normal_width, .height = ( u32 )normal_height, .depth = 1,
        .usage        = VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initial_data = normal_data,
        .name         = RAPTOR_DATA_FOLDER "/Car_Paint_normal.png" } );
    stbi_image_free( normal_data );
    material_normal.texture = { material_normal.resource->image, material_normal.resource->image_view };

    int roughness_width = 0;
    int roughness_height = 0;
    int roughness_components = 0;
    u8* roughness_data = stbi_load( RAPTOR_DATA_FOLDER "/Car_Paint_roughness.png", &roughness_width, &roughness_height, &roughness_components, 4 );
    RASSERT( roughness_data != nullptr );
    material_roughness.resource = renderer->create_texture( ImageCreation{
        .image_type   = VK_IMAGE_TYPE_2D,
        .format       = VK_FORMAT_R8G8B8A8_UNORM,
        .width        = ( u32 )roughness_width, .height = ( u32 )roughness_height, .depth = 1,
        .usage        = VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initial_data = roughness_data,
        .name         = RAPTOR_DATA_FOLDER "/Car_Paint_roughness.png" } );
    stbi_image_free( roughness_data );
    material_roughness.texture = { material_roughness.resource->image, material_roughness.resource->image_view };

    ShaderCompilationCreation random_batch_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "compute_random_batch",
    };
    if ( gpu->cooperative_vector_supported ) {
        random_batch_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( random_batch_shader, { .name = "compute_random_batch", .render_pass_name = "chapter15" }, "compute_random_batch", frame_graph, random_batch_pipeline );
    random_batch_pso = random_batch_pipeline.any();
    random_batch_dsl = compute_random_batch::set_1::create_descriptor_set_layout( gpu );
    compute_random_batch::set_1::dynamic_buffers( sizeof( gpu::NMRandomBatchConstants ) );
    random_batch_ds = compute_random_batch::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation brdf_encoder_train_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "train_brdf_with_encoder",
    };
    if ( gpu->cooperative_vector_supported ) {
        brdf_encoder_train_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( brdf_encoder_train_shader, { .name = "train_brdf_with_encoder", .render_pass_name = "chapter15" }, "train_brdf_with_encoder", frame_graph, brdf_encoder_train_pipeline );
    brdf_encoder_train_pso = brdf_encoder_train_pipeline.any();
    brdf_encoder_train_dsl = train_brdf_with_encoder::set_1::create_descriptor_set_layout( gpu );
    train_brdf_with_encoder::set_1::dynamic_buffers( sizeof( gpu::NMRandomBatchConstants ), sizeof( gpu::NMBrdfTrainingConstants ) );
    brdf_encoder_train_ds = train_brdf_with_encoder::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation brdf_latent_texture_train_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "train_brdf_with_latent_texture",
    };
    if ( gpu->cooperative_vector_supported ) {
        brdf_latent_texture_train_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( brdf_latent_texture_train_shader, { .name = "train_brdf_with_latent_texture", .render_pass_name = "chapter15" }, "train_brdf_with_latent_texture", frame_graph, brdf_latent_texture_train_pipeline );
    brdf_latent_texture_train_pso = brdf_latent_texture_train_pipeline.any();
    brdf_latent_texture_train_dsl = train_brdf_with_latent_texture::set_1::create_descriptor_set_layout( gpu );
    train_brdf_with_latent_texture::set_1::dynamic_buffers( sizeof( gpu::NMRandomBatchConstants ), sizeof( gpu::NMBrdfLatentTrainingConstants ) );
    brdf_latent_texture_train_ds = train_brdf_with_latent_texture::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation copy_encoder_to_latent_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "copy_encoder_to_latent_texture",
    };
    if ( gpu->cooperative_vector_supported ) {
        copy_encoder_to_latent_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( copy_encoder_to_latent_shader, { .name = "copy_encoder_to_latent_texture", .render_pass_name = "chapter15" }, "copy_encoder_to_latent_texture", frame_graph, copy_encoder_to_latent_pipeline );
    copy_encoder_to_latent_pso = copy_encoder_to_latent_pipeline.any();
    copy_encoder_to_latent_dsl = copy_encoder_to_latent_texture::set_1::create_descriptor_set_layout( gpu );
    copy_encoder_to_latent_texture::set_1::dynamic_buffers( sizeof( gpu::NMCopyEncoderToLatentConstants ) );
    copy_encoder_to_latent_ds = copy_encoder_to_latent_texture::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation optimizer_step_buffer_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "optimizer_step_buffer",
    };
    if ( gpu->cooperative_vector_supported ) {
        optimizer_step_buffer_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( optimizer_step_buffer_shader, { .name = "optimizer_step_buffer", .render_pass_name = "chapter15" }, "optimizer_step_buffer", frame_graph, optimizer_step_buffer_pipeline );
    optimizer_step_buffer_pso = optimizer_step_buffer_pipeline.any();
    optimizer_step_buffer_dsl = optimizer_step_buffer::set_1::create_descriptor_set_layout( gpu );
    optimizer_step_buffer::set_1::dynamic_buffers( sizeof( gpu::NMOptimizerBufferConstants ) );
    optimizer_step_buffer_ds = optimizer_step_buffer::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation optimizer_step_texture_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "optimizer_step_texture",
    };
    if ( gpu->cooperative_vector_supported ) {
        optimizer_step_texture_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( optimizer_step_texture_shader, { .name = "optimizer_step_texture", .render_pass_name = "chapter15" }, "optimizer_step_texture", frame_graph, optimizer_step_texture_pipeline );
    optimizer_step_texture_pso = optimizer_step_texture_pipeline.any();
    optimizer_step_texture_dsl = optimizer_step_texture::set_1::create_descriptor_set_layout( gpu );
    optimizer_step_texture::set_1::dynamic_buffers( sizeof( gpu::NMOptimizerTextureConstants ) );
    optimizer_step_texture_ds = optimizer_step_texture::set_1::create_descriptor_set( gpu );

    ShaderCompilationCreation downsample_latent_pyramid_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_training.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "downsample_latent_pyramid",
    };
    if ( gpu->cooperative_vector_supported ) {
        downsample_latent_pyramid_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( downsample_latent_pyramid_shader, { .name = "downsample_latent_pyramid", .render_pass_name = "chapter15" }, "downsample_latent_pyramid", frame_graph, downsample_latent_pyramid_pipeline );
    downsample_latent_pyramid_pso = downsample_latent_pyramid_pipeline.any();

    ShaderCompilationCreation preview_brdf_shader = {
        .stages = { { .source = { .slang = "slang/chapter15/neural_material_inference.slang" }, .type = VK_SHADER_STAGE_COMPUTE_BIT } },
        .name = "preview_brdf",
    };
    if ( gpu->cooperative_vector_supported ) {
        preview_brdf_shader.stages[ 0 ].defines.push( "RAPTOR_USE_COOPVEC" );
    }
    renderer->create_compute_pipeline_state( preview_brdf_shader, { .name = "preview_brdf", .render_pass_name = "chapter15" }, "preview_brdf", frame_graph, preview_brdf_pipeline );
    preview_brdf_pso = preview_brdf_pipeline.any();
    preview_brdf_dsl = preview_brdf::set_1::create_descriptor_set_layout( gpu );
    preview_brdf::set_1::dynamic_buffers( sizeof( gpu::NMPreviewConstants ) );
    preview_brdf_ds = preview_brdf::set_1::create_descriptor_set( gpu );

    TextureResource* preview_resource = renderer->create_texture( ImageCreation{
        .image_type = VK_IMAGE_TYPE_2D,
        .format     = VK_FORMAT_R16G16B16A16_SFLOAT,
        .width      = ( u32 )NEURAL_PREVIEW_WIDTH, .height = ( u32 )NEURAL_PREVIEW_HEIGHT, .depth = 1,
        .usage      = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .name       = "preview_texture" } );
    preview_texture = { preview_resource->image, preview_resource->image_view };

    ImageCreation latent_tc{
        .image_type        = VK_IMAGE_TYPE_2D,
        .format            = VK_FORMAT_R32G32B32A32_SFLOAT,
        .mip_level_count   = 1,
        .array_layer_count = 1,
        .usage             = VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT };

    BufferCreation latent_buffer_creation = { .usage = storage_buffer_usage, .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

    latent_pyramid.width = LATENT_TEXTURE_SIZE;
    latent_pyramid.height = LATENT_TEXTURE_SIZE;
    latent_pyramid.level_count = LATENT_PYRAMID_LEVELS;
    StaticString64 tex_name;
    for ( u32 level = 0; level < LATENT_PYRAMID_LEVELS; ++level ) {
        latent_tc.width  = ( u32 )( LATENT_TEXTURE_SIZE >> level );
        latent_tc.height = ( u32 )( LATENT_TEXTURE_SIZE >> level );
        latent_buffer_creation.size = latent_tc.width * latent_tc.height * sizeof( f32 ) * 4;

        tex_name.format( "latent0_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* latent0_resource = renderer->create_texture( latent_tc );
        latent_pyramid.textures0[ level ] = { latent0_resource->image, latent0_resource->image_view };

        tex_name.format( "latent1_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* latent1_resource = renderer->create_texture( latent_tc );
        latent_pyramid.textures1[ level ] = { latent1_resource->image, latent1_resource->image_view };

        tex_name.format( "latent_grad0_%u", level );
        latent_buffer_creation.name = tex_name.c_str();
        latent_pyramid.grads0[ level ] = gpu->create_buffer( latent_buffer_creation );
        gpu->add_buffer_to_bindless( latent_pyramid.grads0[ level ] );

        tex_name.format( "latent_grad1_%u", level );
        latent_buffer_creation.name = tex_name.c_str();
        latent_pyramid.grads1[ level ] = gpu->create_buffer( latent_buffer_creation );
        gpu->add_buffer_to_bindless( latent_pyramid.grads1[ level ] );

        tex_name.format( "latent_exp_avg0_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* exp_avg0_resource = renderer->create_texture( latent_tc );
        latent_pyramid.exp_avg0[ level ] = { exp_avg0_resource->image, exp_avg0_resource->image_view };

        tex_name.format( "latent_exp_avg1_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* exp_avg1_resource = renderer->create_texture( latent_tc );
        latent_pyramid.exp_avg1[ level ] = { exp_avg1_resource->image, exp_avg1_resource->image_view };

        tex_name.format( "latent_exp_avg_sq0_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* exp_avg_sq0_resource = renderer->create_texture( latent_tc );
        latent_pyramid.exp_avg_sq0[ level ] = { exp_avg_sq0_resource->image, exp_avg_sq0_resource->image_view };

        tex_name.format( "latent_exp_avg_sq1_%u", level );
        latent_tc.name = tex_name.c_str();
        TextureResource* exp_avg_sq1_resource = renderer->create_texture( latent_tc );
        latent_pyramid.exp_avg_sq1[ level ] = { exp_avg_sq1_resource->image, exp_avg_sq1_resource->image_view };
    }

    initialized = false;
}

void NeuralMaterialTrainer::destroy_resources( Renderer* renderer ) {

    renderer->destroy_compute_pipeline_state( random_batch_pipeline );
    renderer->destroy_compute_pipeline_state( brdf_encoder_train_pipeline );
    renderer->destroy_compute_pipeline_state( brdf_latent_texture_train_pipeline );
    renderer->destroy_compute_pipeline_state( copy_encoder_to_latent_pipeline );
    renderer->destroy_compute_pipeline_state( optimizer_step_buffer_pipeline );
    renderer->destroy_compute_pipeline_state( optimizer_step_texture_pipeline );
    renderer->destroy_compute_pipeline_state( downsample_latent_pyramid_pipeline );
    renderer->destroy_compute_pipeline_state( preview_brdf_pipeline );

    renderer->destroy_texture( material_normal.resource );
    renderer->destroy_texture( material_roughness.resource );

    gpu->destroy_buffer( batch_kx );
    gpu->destroy_buffer( loss_gpu );
    gpu->destroy_buffer( loss_cpu );

    gpu->destroy_descriptor_set_layout( random_batch_dsl );
    gpu->destroy_descriptor_set( random_batch_ds );

    gpu->destroy_descriptor_set_layout( brdf_encoder_train_dsl );
    gpu->destroy_descriptor_set( brdf_encoder_train_ds );

    gpu->destroy_descriptor_set_layout( brdf_latent_texture_train_dsl );
    gpu->destroy_descriptor_set( brdf_latent_texture_train_ds );

    gpu->destroy_descriptor_set_layout( copy_encoder_to_latent_dsl );
    gpu->destroy_descriptor_set( copy_encoder_to_latent_ds );

    gpu->destroy_descriptor_set_layout( optimizer_step_buffer_dsl );
    gpu->destroy_descriptor_set( optimizer_step_buffer_ds );

    gpu->destroy_descriptor_set_layout( optimizer_step_texture_dsl );
    gpu->destroy_descriptor_set( optimizer_step_texture_ds );

    gpu->destroy_descriptor_set_layout( preview_brdf_dsl );
    gpu->destroy_descriptor_set( preview_brdf_ds );
    gpu->destroy_image_view( preview_texture.view );
    gpu->destroy_image( preview_texture.image );

    for ( u32 i = 0; i < 3; ++i ) {
        destroy_wb_layer_resources( encoder[ i ] );
    }

    destroy_w_layer_resources( frame_decoder );

    for ( u32 i = 0; i < 3; ++i ) {
        destroy_wb_layer_resources( brdf_decoder[ i ] );
    }

    for ( u32 i = 0; i < 4; ++i ) {
        destroy_wb_layer_resources( sampler_decoder[ i ] );
    }

    for ( u32 level = 0; level < LATENT_PYRAMID_LEVELS; ++level ) {
        gpu->destroy_image_view( latent_pyramid.textures0[ level ].view );
        gpu->destroy_image( latent_pyramid.textures0[ level ].image );
        gpu->destroy_image_view( latent_pyramid.textures1[ level ].view );
        gpu->destroy_image( latent_pyramid.textures1[ level ].image );
        gpu->destroy_buffer( latent_pyramid.grads0[ level ] );
        gpu->destroy_buffer( latent_pyramid.grads1[ level ] );
        gpu->destroy_image_view( latent_pyramid.exp_avg0[ level ].view );
        gpu->destroy_image( latent_pyramid.exp_avg0[ level ].image );
        gpu->destroy_image_view( latent_pyramid.exp_avg1[ level ].view );
        gpu->destroy_image( latent_pyramid.exp_avg1[ level ].image );
        gpu->destroy_image_view( latent_pyramid.exp_avg_sq0[ level ].view );
        gpu->destroy_image( latent_pyramid.exp_avg_sq0[ level ].image );
        gpu->destroy_image_view( latent_pyramid.exp_avg_sq1[ level ].view );
        gpu->destroy_image( latent_pyramid.exp_avg_sq1[ level ].image );
    }
}

static void fill_buffer_random( GpuDevice* gpu, BufferHandle buffer, u32 count, f32 min_value, f32 max_value ) {
    f32* data = ( f32* )gpu->map_buffer( { buffer, 0, 0 } );
    if ( !data ) {
        return;
    }

    for ( u32 i = 0; i < count; ++i ) {
        data[ i ] = get_random_value( min_value, max_value );
    }

    gpu->unmap_buffer( { buffer, 0, 0 } );
}

static u16 float_to_half_bits( f32 value ) {
    u32 bits;
    memcpy( &bits, &value, sizeof( bits ) );

    const u32 sign = ( bits >> 16 ) & 0x8000u;
    u32 mantissa = bits & 0x007fffffu;
    int exponent = int( ( bits >> 23 ) & 0xffu ) - 127 + 15;

    if ( exponent <= 0 ) {
        if ( exponent < -10 ) {
            return u16( sign );
        }

        mantissa = ( mantissa | 0x00800000u ) >> ( 1 - exponent );
        if ( mantissa & 0x00001000u ) {
            mantissa += 0x00002000u;
        }
        return u16( sign | ( mantissa >> 13 ) );
    }

    if ( exponent >= 31 ) {
        if ( mantissa == 0 ) {
            return u16( sign | 0x7c00u );
        }

        mantissa >>= 13;
        return u16( sign | 0x7c00u | mantissa | ( mantissa == 0 ) );
    }

    if ( mantissa & 0x00001000u ) {
        mantissa += 0x00002000u;
        if ( mantissa & 0x00800000u ) {
            mantissa = 0;
            ++exponent;
            if ( exponent >= 31 ) {
                return u16( sign | 0x7c00u );
            }
        }
    }

    return u16( sign | ( u32( exponent ) << 10 ) | ( mantissa >> 13 ) );
}

static void fill_buffer_random_half( GpuDevice* gpu, BufferHandle buffer, u32 count, f32 min_value, f32 max_value ) {
    u16* data = ( u16* )gpu->map_buffer( { buffer, 0, 0 } );
    if ( !data ) {
        return;
    }

    for ( u32 i = 0; i < count; ++i ) {
        data[ i ] = float_to_half_bits( get_random_value( min_value, max_value ) );
    }

    gpu->unmap_buffer( { buffer, 0, 0 } );
}

static f32 compute_glorot_uniform_limit( u32 fan_in, u32 fan_out ) {
    const f32 denominator = f32( fan_in + fan_out );
    return denominator > 0.0f ? sqrtf( 6.0f / denominator ) : 0.0f;
}

static void fill_buffer_zero( GpuDevice* gpu, BufferHandle buffer, u32 count ) {
    void* data = gpu->map_buffer( { buffer, 0, 0 } );
    if ( !data ) {
        return;
    }

    memset( data, 0, count * sizeof( f32 ) );
    gpu->unmap_buffer( { buffer, 0, 0 } );
}

static void fill_buffer_zero_bytes( GpuDevice* gpu, BufferHandle buffer, u32 byte_size ) {
    void* data = gpu->map_buffer( { buffer, 0, 0 } );
    if ( !data ) {
        return;
    }

    memset( data, 0, byte_size );
    gpu->unmap_buffer( { buffer, 0, 0 } );
}

#if RAPTOR_USE_COOPVEC
static u32 get_coopvec_row_major_stride_bytes( u32 inputs ) {
    return inputs * sizeof( u16 );
}

static u32 query_coopvec_training_matrix_size( GpuDevice* gpu, u32 inputs, u32 outputs ) {
    size_t dst_size = 0;

    VkConvertCooperativeVectorMatrixInfoNV info{ VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV };
    info.srcSize = size_t( inputs ) * size_t( outputs ) * sizeof( u16 );
    info.srcData.hostAddress = nullptr;
    info.pDstSize = &dst_size;
    info.dstData.hostAddress = nullptr;
    info.srcComponentType = VK_COMPONENT_TYPE_FLOAT16_KHR;
    info.dstComponentType = VK_COMPONENT_TYPE_FLOAT16_KHR;
    info.numRows = outputs;
    info.numColumns = inputs;
    info.srcLayout = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_ROW_MAJOR_NV;
    info.srcStride = get_coopvec_row_major_stride_bytes( inputs );
    info.dstLayout = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_TRAINING_OPTIMAL_NV;
    info.dstStride = 0;

    const VkResult result = vkConvertCooperativeVectorMatrixNV( gpu->vulkan_device, &info );
    RASSERT( result == VK_SUCCESS );
    RASSERT( dst_size <= UINT32_MAX );
    return u32( dst_size );
}

static void convert_coopvec_training_matrix_to_row_major( CommandBuffer* cb, GpuDevice* gpu,
                                                          BufferHandle training_buffer, u32 training_buffer_size,
                                                          BufferHandle row_major_buffer,
                                                          u32 inputs, u32 outputs ) {
    size_t dst_size = gpu->get_buffer( row_major_buffer )->size;

    VkConvertCooperativeVectorMatrixInfoNV info{ VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV };
    info.srcSize = training_buffer_size;
    info.srcData.deviceAddress = gpu->get_buffer_device_address( training_buffer );
    info.pDstSize = &dst_size;
    info.dstData.deviceAddress = gpu->get_buffer_device_address( row_major_buffer );
    RASSERT( ( info.srcData.deviceAddress % gpu->cooperative_vector_matrix_alignment ) == 0 );
    RASSERT( ( info.dstData.deviceAddress % gpu->cooperative_vector_matrix_alignment ) == 0 );
    info.srcComponentType = VK_COMPONENT_TYPE_FLOAT16_KHR;
    info.dstComponentType = VK_COMPONENT_TYPE_FLOAT16_KHR;
    info.numRows = outputs;
    info.numColumns = inputs;
    info.srcLayout = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_TRAINING_OPTIMAL_NV;
    info.srcStride = 0;
    info.dstLayout = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_ROW_MAJOR_NV;
    info.dstStride = get_coopvec_row_major_stride_bytes( inputs );

    vkCmdConvertCooperativeVectorMatrixNV( cb->vk_command_buffer, 1, &info );
}
#endif

// Wang random numbers
static u32 wang_hash( u32 seed ) {
    seed = ( seed ^ 61u ) ^ ( seed >> 16u );
    seed *= 9u;
    seed = seed ^ ( seed >> 4u );
    seed *= 0x27d4eb2du;
    seed = seed ^ ( seed >> 15u );
    return seed;
}

static u32 wang_hash_warmup( u32 seed, u32 warmup_count ) {
    for ( u32 i = 0; i < warmup_count; ++i ) {
        seed = wang_hash( seed );
    }

    return seed;
}

// Fill a gpu::NeuralWeightBiasLayer from the C++ resource holder.
static gpu::NeuralWeightBiasLayer make_gpu_wb_layer( const NeuralWeightBiasLayer& layer ) {
    gpu::NeuralWeightBiasLayer result{};
    result.weights_index            = layer.weights.index();
    result.biases_index             = layer.biases.index();
    result.weights_grad_index       = layer.weights_grad.index();
    result.biases_grad_index        = layer.biases_grad.index();
    result.weights_grad_training_index = layer.weights_grad_training.index();
    result.weights_exp_avg_index    = layer.weights_exp_avg.index();
    result.biases_exp_avg_index     = layer.biases_exp_avg.index();
    result.weights_exp_avg_sq_index = layer.weights_exp_avg_sq.index();
    result.biases_exp_avg_sq_index  = layer.biases_exp_avg_sq.index();
    result.inputs                   = layer.inputs;
    result.outputs                  = layer.outputs;
    return result;
}

static gpu::NeuralWeightLayer make_gpu_w_layer( const NeuralWeightLayer& layer ) {
    gpu::NeuralWeightLayer result{};
    result.weights_index         = layer.weights.index();
    result.weights_grad_index    = layer.weights_grad.index();
    result.weights_grad_training_index = layer.weights_grad_training.index();
    result.weights_exp_avg_index = layer.weights_exp_avg.index();
    result.weights_exp_avg_sq_index = layer.weights_exp_avg_sq.index();
    result.inputs                = layer.inputs;
    result.outputs               = layer.outputs;
    return result;
}

static void convert_wb_layer_gradients_for_optimizer( CommandBuffer* cb, GpuDevice* gpu, const NeuralWeightBiasLayer& layer ) {
#if RAPTOR_USE_COOPVEC
    cb->add_buffer_barrier( layer.weights_grad_training, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();

    convert_coopvec_training_matrix_to_row_major( cb, gpu,
        layer.weights_grad_training, layer.weights_grad_training_size,
        layer.weights_grad,
        layer.inputs, layer.outputs );

    cb->fill_buffer( layer.weights_grad_training, 0, 0, 0 );
#endif
}

static void convert_w_layer_gradients_for_optimizer( CommandBuffer* cb, GpuDevice* gpu, const NeuralWeightLayer& layer ) {
#if RAPTOR_USE_COOPVEC
    cb->add_buffer_barrier( layer.weights_grad_training, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();

    convert_coopvec_training_matrix_to_row_major( cb, gpu,
        layer.weights_grad_training, layer.weights_grad_training_size,
        layer.weights_grad,
        layer.inputs, layer.outputs );

    cb->fill_buffer( layer.weights_grad_training, 0, 0, 0 );
#endif
}

// Fill a gpu::LatentTexturePyramid from the C++ resource holder.
// SRV and UAV indices are the same because Compute_mask textures are registered in both arrays.
static gpu::LatentTexturePyramid make_gpu_latent_pyramid( const LatentPyramidResources& res ) {
    gpu::LatentTexturePyramid pyramid{};
    pyramid.level_count = res.level_count;
    pyramid.width       = res.width;
    pyramid.height      = res.height;
    for ( u32 i = 0; i < res.level_count; ++i ) {
        pyramid.textures0_srv.set( i, res.textures0[ i ].index() );
        pyramid.textures1_srv.set( i, res.textures1[ i ].index() );
        pyramid.textures0_uav.set( i, res.textures0[ i ].index() );
        pyramid.textures1_uav.set( i, res.textures1[ i ].index() );
        pyramid.grads0_uav.set( i, res.grads0[ i ].index() );
        pyramid.grads1_uav.set( i, res.grads1[ i ].index() );
        pyramid.exp_avg0_uav.set( i, res.exp_avg0[ i ].index() );
        pyramid.exp_avg1_uav.set( i, res.exp_avg1[ i ].index() );
        pyramid.exp_avg_sq0_uav.set( i, res.exp_avg_sq0[ i ].index() );
        pyramid.exp_avg_sq1_uav.set( i, res.exp_avg_sq1[ i ].index() );
    }
    return pyramid;
}

// Dispatch Adam optimizer_step_buffer for weights and biases of one layer.
// Assumes optimizer_step_buffer_pso is already bound.
static void dispatch_optimizer_wb_layer( CommandBuffer* cb, GpuDevice* gpu,
                                         DescriptorSetHandle optimizer_ds,
                                         const NeuralWeightBiasLayer& layer,
                                         u32 epoch, f32 learning_rate ) {
    const u32 weights_count = layer.inputs * layer.outputs;
    const u32 biases_count  = layer.outputs;

    // Weights
    {
        u32 offset = 0;
        gpu::NMOptimizerBufferConstants* c = gpu->dynamic_buffer_allocate<gpu::NMOptimizerBufferConstants>( &offset );
        if ( c ) {
            c->value_buffer_index        = layer.weights.index();
            c->grad_buffer_index         = layer.weights_grad.index();
            c->exp_avg_buffer_index      = layer.weights_exp_avg.index();
            c->exp_avg_sq_buffer_index   = layer.weights_exp_avg_sq.index();
            c->element_count             = weights_count;
            c->optimize_counter          = epoch;
            c->learning_rate             = learning_rate;
        }
        cb->bind_descriptor_set( { gpu->bindless_descriptor_set, optimizer_ds }, { offset } );
        cb->dispatch( ( weights_count + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
    }

    // Biases
    {
        u32 offset = 0;
        gpu::NMOptimizerBufferConstants* c = gpu->dynamic_buffer_allocate<gpu::NMOptimizerBufferConstants>( &offset );
        if ( c ) {
            c->value_buffer_index        = layer.biases.index();
            c->grad_buffer_index         = layer.biases_grad.index();
            c->exp_avg_buffer_index      = layer.biases_exp_avg.index();
            c->exp_avg_sq_buffer_index   = layer.biases_exp_avg_sq.index();
            c->element_count             = biases_count;
            c->optimize_counter          = epoch;
            c->learning_rate             = learning_rate;
        }
        cb->bind_descriptor_set( { gpu->bindless_descriptor_set, optimizer_ds }, { offset } );
        cb->dispatch( ( biases_count + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
    }
}

static void dispatch_optimizer_w_layer( CommandBuffer* cb, GpuDevice* gpu,
                                        DescriptorSetHandle optimizer_ds,
                                        const NeuralWeightLayer& layer,
                                        u32 epoch, f32 learning_rate ) {
    const u32 weights_count = layer.inputs * layer.outputs;

    u32 offset = 0;
    gpu::NMOptimizerBufferConstants* c = gpu->dynamic_buffer_allocate<gpu::NMOptimizerBufferConstants>( &offset );
    if ( c ) {
        c->value_buffer_index      = layer.weights.index();
        c->grad_buffer_index       = layer.weights_grad.index();
        c->exp_avg_buffer_index    = layer.weights_exp_avg.index();
        c->exp_avg_sq_buffer_index = layer.weights_exp_avg_sq.index();
        c->element_count           = weights_count;
        c->optimize_counter        = epoch;
        c->learning_rate           = learning_rate;
    }
    cb->bind_descriptor_set( { gpu->bindless_descriptor_set, optimizer_ds }, { offset } );
    cb->dispatch( ( weights_count + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
}

static void transition_wb_layer_for_training( CommandBuffer* cb, const NeuralWeightBiasLayer& layer ) {
    cb->add_buffer_barrier( layer.weights, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT } );
    cb->add_buffer_barrier( layer.biases, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT } );
    cb->add_buffer_barrier( cb->gpu_device->cooperative_vector_supported ? layer.weights_grad_training : layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.biases_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();
}

static void transition_w_layer_for_training( CommandBuffer* cb, const NeuralWeightLayer& layer ) {
    cb->add_buffer_barrier( layer.weights, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT } );
    cb->add_buffer_barrier( cb->gpu_device->cooperative_vector_supported ? layer.weights_grad_training : layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();
}

static void transition_wb_layer_for_optimizer( CommandBuffer* cb, const NeuralWeightBiasLayer& layer ) {
    cb->add_buffer_barrier( layer.weights, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.biases, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.biases_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_exp_avg, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.biases_exp_avg, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_exp_avg_sq, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.biases_exp_avg_sq, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();
}

static void transition_w_layer_for_optimizer( CommandBuffer* cb, const NeuralWeightLayer& layer ) {
    cb->add_buffer_barrier( layer.weights, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_grad, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_exp_avg, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->add_buffer_barrier( layer.weights_exp_avg_sq, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();
}

static void transition_latent_pyramid_for_training( CommandBuffer* cb, const LatentPyramidResources& pyramid ) {
    for ( u32 level = 0; level < pyramid.level_count; ++level ) {
        cb->add_image_barrier( pyramid.textures0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        cb->add_image_barrier( pyramid.textures1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        cb->add_buffer_barrier( pyramid.grads0[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->add_buffer_barrier( pyramid.grads1[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->flush_barriers();
    }
}

static void transition_latent_pyramid_for_optimizer( CommandBuffer* cb, const LatentPyramidResources& pyramid ) {
    for ( u32 level = 0; level < pyramid.level_count; ++level ) {
        cb->add_image_barrier( pyramid.textures0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_image_barrier( pyramid.textures1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_image_barrier( pyramid.exp_avg0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_image_barrier( pyramid.exp_avg1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_image_barrier( pyramid.exp_avg_sq0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_image_barrier( pyramid.exp_avg_sq1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->add_buffer_barrier( pyramid.grads0[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->add_buffer_barrier( pyramid.grads1[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->flush_barriers();
    }
}

static void clear_latent_optimizer_resources( CommandBuffer* cb, const LatentPyramidResources& pyramid ) {
    const VkClearColorValue zero_clear = { { 0.0f, 0.0f, 0.0f, 0.0f } };

    for ( u32 level = 0; level < pyramid.level_count; ++level ) {
        cb->add_image_barrier( pyramid.exp_avg0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        cb->add_image_barrier( pyramid.exp_avg1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        cb->add_image_barrier( pyramid.exp_avg_sq0[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        cb->add_image_barrier( pyramid.exp_avg_sq1[ level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        cb->add_buffer_barrier( pyramid.grads0[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT } );
        cb->add_buffer_barrier( pyramid.grads1[ level ], 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT } );
        cb->flush_barriers();

        cb->clear_color_image( pyramid.exp_avg0[ level ].image, zero_clear );
        cb->clear_color_image( pyramid.exp_avg1[ level ].image, zero_clear );
        cb->clear_color_image( pyramid.exp_avg_sq0[ level ].image, zero_clear );
        cb->clear_color_image( pyramid.exp_avg_sq1[ level ].image, zero_clear );

        cb->fill_buffer( pyramid.grads0[ level ], 0, 0, 0 );
        cb->fill_buffer( pyramid.grads1[ level ], 0, 0, 0 );
    }
}

void NeuralMaterialTrainer::step( CommandBuffer* cb ) {

    if ( !initialized ) {

        for ( u32 i = 0; i < 3; ++i ) {
            initialize_wb_layer_buffers( encoder[ i ] );
        }

        initialize_w_layer_buffers( frame_decoder );

        for ( u32 i = 0; i < 3; ++i ) {
            initialize_wb_layer_buffers( brdf_decoder[ i ] );
        }

        for ( u32 i = 0; i < 4; ++i ) {
            initialize_wb_layer_buffers( sampler_decoder[ i ] );
        }

        fill_buffer_zero( gpu, batch_kx, 11 * BATCH_SIZE );
        fill_buffer_zero( gpu, loss_cpu, BATCH_SIZE * 3 );
        clear_latent_optimizer_resources( cb, latent_pyramid );

        initialized = true;
    }

    Buffer* loss_buffer = gpu->get_buffer( loss_cpu );
    f32* loss_data = ( f32* )loss_buffer->mapped_data;

    for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
        current_loss[ channel_index ] = 0.0f;
    }

    for ( u32 i = 0; i < BATCH_SIZE; ++i ) {
        for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
            current_loss[ channel_index ] += loss_data[ i * LOSS_CHANNEL_COUNT + channel_index ];
        }
    }

    for ( u32 channel_index = 0; channel_index < LOSS_CHANNEL_COUNT; ++channel_index ) {
        current_loss[ channel_index ] /= ( f32 )BATCH_SIZE;
    }

    push_loss_history_sample( *this, current_loss );

    const u32 seed = wang_hash_warmup( epoch, 2 );

    // Compute random batch of input vectors from the material textures
    u32 constants_offset = 0;
    gpu::NMRandomBatchConstants* constants = gpu->dynamic_buffer_allocate<gpu::NMRandomBatchConstants>( &constants_offset );
    {
        cb->push_marker( "Compute random batch" );
        cb->add_buffer_barrier( batch_kx, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->flush_barriers();
        cb->bind_pipeline( random_batch_pso );

        if ( constants ) {
            constants->batch_size          = BATCH_SIZE;
            constants->seed                = seed;
            constants->output_buffer_index = batch_kx.index();
            constants->normal_map_index    = material_normal.texture.index();
            constants->roughness_map_index = material_roughness.texture.index();
            constants->step                = epoch;
        }
        cb->bind_descriptor_set( { gpu->bindless_descriptor_set, random_batch_ds }, { constants_offset } );
        cb->dispatch( ( BATCH_SIZE + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
        cb->pop_marker();
    }

    // Flush random batch UAV writes before transitioning batch_kx to SRV
    cb->add_buffer_barrier( batch_kx, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT } );
    cb->flush_barriers();

    float learning_rate_scale = 0.1f + 0.5f * (1.0f - 0.1f ) * ( 1.0f + cosf( ( epoch * 3.14159265358979323846f ) / 300000.0f ) );
    float epoch_learning_rate = max( NM_LEARNING_RATE * learning_rate_scale, 1e-4f );

    if ( epoch < ENCODER_TRAINING_EPOCHS ) {

        cb->barrier_instant_compute_write_to_compute_read();
        for ( u32 i = 0; i < 3; ++i ) {
            transition_wb_layer_for_training( cb, encoder[ i ] );
            transition_wb_layer_for_training( cb, brdf_decoder[ i ] );
        }
        transition_w_layer_for_training( cb, frame_decoder );

        // Phase 1: forward + backward through encoder + brdf decoder
        {
            cb->push_marker( "Train brdf decoder with encoder" );
            cb->add_buffer_barrier( loss_gpu, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
            cb->flush_barriers();
            cb->bind_pipeline( brdf_encoder_train_pso );

            for ( u32 batch_offset = 0; batch_offset < BATCH_SIZE; batch_offset += MICROBATCH_SIZE ) {
                u32 offset = 0;
                gpu::NMBrdfTrainingConstants* c = gpu->dynamic_buffer_allocate<gpu::NMBrdfTrainingConstants>( &offset );
                if ( c ) {
                    c->batch_size         = MICROBATCH_SIZE;
                    c->batch_offset       = batch_offset;
                    c->total_batch_size   = BATCH_SIZE;
                    c->padding000         = 0;
                    c->input_buffer_index = batch_kx.index();
                    c->loss_buffer_index  = loss_gpu.index();
                    c->encoder.l0         = make_gpu_wb_layer( encoder[ 0 ] );
                    c->encoder.l1         = make_gpu_wb_layer( encoder[ 1 ] );
                    c->encoder.l2         = make_gpu_wb_layer( encoder[ 2 ] );
                    c->frame_decoder.l0   = make_gpu_w_layer( frame_decoder );
                    c->brdf_decoder.l0    = make_gpu_wb_layer( brdf_decoder[ 0 ] );
                    c->brdf_decoder.l1    = make_gpu_wb_layer( brdf_decoder[ 1 ] );
                    c->brdf_decoder.l2    = make_gpu_wb_layer( brdf_decoder[ 2 ] );
                }
                cb->bind_descriptor_set( { gpu->bindless_descriptor_set, brdf_encoder_train_ds }, { constants_offset, offset } );
                cb->dispatch( ( MICROBATCH_SIZE + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
                cb->barrier_instant_compute_write_to_compute_read();
            }
            cb->pop_marker();
        }

        // Flush writes from the training pass before the optimizer reads them
        cb->barrier_instant_compute_write_to_compute_read();

        if ( gpu->cooperative_vector_supported ) {
            cb->push_marker( "Convert coopvec gradients" );
            for ( u32 i = 0; i < 3; ++i ) {
                convert_wb_layer_gradients_for_optimizer( cb, gpu, encoder[ i ] );
                convert_wb_layer_gradients_for_optimizer( cb, gpu, brdf_decoder[ i ] );
            }
            convert_w_layer_gradients_for_optimizer( cb, gpu, frame_decoder );
            cb->global_debug_barrier();
            cb->pop_marker();
        }

        // Adam optimizer step for encoder and brdf decoder weights
        {
            cb->push_marker( "Optimizer: encoder + brdf decoder" );
            for ( u32 i = 0; i < 3; ++i ) {
                transition_wb_layer_for_optimizer( cb, encoder[ i ] );
                transition_wb_layer_for_optimizer( cb, brdf_decoder[ i ] );
            }
            transition_w_layer_for_optimizer( cb, frame_decoder );
            cb->bind_pipeline( optimizer_step_buffer_pso );

            for ( u32 i = 0; i < 3; ++i ) {
                dispatch_optimizer_wb_layer( cb, gpu, optimizer_step_buffer_ds, encoder[ i ], epoch, epoch_learning_rate );
                dispatch_optimizer_wb_layer( cb, gpu, optimizer_step_buffer_ds, brdf_decoder[ i ], epoch, epoch_learning_rate );
            }
            dispatch_optimizer_w_layer( cb, gpu, optimizer_step_buffer_ds, frame_decoder, epoch, epoch_learning_rate );
            // for ( u32 i = 0; i < 4; ++i ) {
            //     dispatch_optimizer_wb_layer( cb, gpu, optimizer_step_buffer_ds, sampler_decoder[ i ], epoch, epoch_learning_rate );
            // }

            cb->pop_marker();
        }

    } else {

        const u32 latent_mollification_step = epoch - ENCODER_TRAINING_EPOCHS;

        // Phase 2: switch to baked latent texture representation

        // On the first epoch of phase 2, bake the trained encoder into the latent pyramid level 0
        if ( epoch == ENCODER_TRAINING_EPOCHS ) {
            cb->push_marker( "Copy encoder to latent texture" );
            for ( u32 i = 0; i < 3; ++i ) {
                transition_wb_layer_for_training( cb, encoder[ i ] );
            }
            cb->add_image_barrier( latent_pyramid.textures0[ 0 ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
            cb->add_image_barrier( latent_pyramid.textures1[ 0 ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
            cb->flush_barriers();

            cb->bind_pipeline( copy_encoder_to_latent_pso );

            u32 offset = 0;
            gpu::NMCopyEncoderToLatentConstants* c = gpu->dynamic_buffer_allocate<gpu::NMCopyEncoderToLatentConstants>( &offset );
            if ( c ) {
                c->width          = latent_pyramid.width;
                c->height         = latent_pyramid.height;
                c->normal_map_index = material_normal.texture.index();
                c->roughness_map_index = material_roughness.texture.index();
                c->encoder.l0     = make_gpu_wb_layer( encoder[ 0 ] );
                c->encoder.l1     = make_gpu_wb_layer( encoder[ 1 ] );
                c->encoder.l2     = make_gpu_wb_layer( encoder[ 2 ] );
                c->latent_pyramid = make_gpu_latent_pyramid( latent_pyramid );
            }
            cb->bind_descriptor_set( { gpu->bindless_descriptor_set, copy_encoder_to_latent_ds }, { offset } );
            cb->dispatch( ( latent_pyramid.width + 7 ) / 8, ( latent_pyramid.height + 7 ) / 8, 1 );
            cb->pop_marker();

            // Downsample level 0 into the remaining pyramid levels
            if ( latent_pyramid.level_count > 1 ) {
                cb->push_marker( "Downsample latent pyramid" );
                cb->bind_pipeline( downsample_latent_pyramid_pso );

                for ( u32 dst_level = 1; dst_level < latent_pyramid.level_count; ++dst_level ) {
                    const u32 src_level = dst_level - 1;
                    const u32 dst_w     = latent_pyramid.width  >> dst_level;
                    const u32 dst_h     = latent_pyramid.height >> dst_level;

                    // Flush previous compute write before reading it as source
                    cb->barrier_instant_compute_write_to_compute_read();
                    cb->add_image_barrier( latent_pyramid.textures0[ dst_level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
                    cb->add_image_barrier( latent_pyramid.textures1[ dst_level ].image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
                    cb->flush_barriers();

                    // Reuse the constants uploaded for the copy pass (pyramid indices are all that is needed)
                    cb->bind_descriptor_set( { gpu->bindless_descriptor_set, copy_encoder_to_latent_ds }, { offset } );

                    u32 src_dst[ 2 ] = { src_level, dst_level };
                    cb->push_constants( downsample_latent_pyramid_pso, 0, sizeof( u32 ) * 2, src_dst );

                    cb->dispatch( ( dst_w + 7 ) / 8, ( dst_h + 7 ) / 8, 1 );
                }

                cb->pop_marker();
            }
        }

        // Forward + backward through brdf decoder against the latent texture
        {
            cb->push_marker( "Train brdf decoder with latent texture" );
            cb->barrier_instant_compute_write_to_compute_read();
            for ( u32 i = 0; i < 3; ++i ) {
                transition_wb_layer_for_training( cb, brdf_decoder[ i ] );
            }
            transition_w_layer_for_training( cb, frame_decoder );
            transition_latent_pyramid_for_training( cb, latent_pyramid );
            cb->add_buffer_barrier( loss_gpu, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT } );
            cb->flush_barriers();

            cb->bind_pipeline( brdf_latent_texture_train_pso );

            for ( u32 batch_offset = 0; batch_offset < BATCH_SIZE; batch_offset += MICROBATCH_SIZE ) {
                u32 latent_train_offset = 0;
                gpu::NMBrdfLatentTrainingConstants* c = gpu->dynamic_buffer_allocate<gpu::NMBrdfLatentTrainingConstants>( &latent_train_offset );
                if ( c ) {
                    c->batch_size = MICROBATCH_SIZE;
                    c->batch_offset = batch_offset;
                    c->total_batch_size = BATCH_SIZE;
                    c->padding000 = 0;
                    c->input_buffer_index = batch_kx.index();
                    c->loss_buffer_index = loss_gpu.index();
                    c->mollification_step = latent_mollification_step;
                    c->padding002 = 0;
                    c->latent_pyramid = make_gpu_latent_pyramid( latent_pyramid );
                    c->frame_decoder.l0 = make_gpu_w_layer( frame_decoder );
                    c->brdf_decoder.l0 = make_gpu_wb_layer( brdf_decoder[ 0 ] );
                    c->brdf_decoder.l1 = make_gpu_wb_layer( brdf_decoder[ 1 ] );
                    c->brdf_decoder.l2 = make_gpu_wb_layer( brdf_decoder[ 2 ] );
                }
                cb->bind_descriptor_set( { gpu->bindless_descriptor_set, brdf_latent_texture_train_ds }, { constants_offset, latent_train_offset } );
                cb->dispatch( ( MICROBATCH_SIZE + WORKGROUP_COUNT_X - 1 ) / WORKGROUP_COUNT_X, 1, 1 );
                cb->barrier_instant_compute_write_to_compute_read();
            }
            cb->pop_marker();
        }

        // Flush gradient writes from the training pass before the optimizer reads them
        cb->barrier_instant_compute_write_to_compute_read();

        if ( gpu->cooperative_vector_supported ) {
            cb->push_marker( "Convert coopvec gradients" );
            for ( u32 i = 0; i < 3; ++i ) {
                convert_wb_layer_gradients_for_optimizer( cb, gpu, brdf_decoder[ i ] );
            }
            convert_w_layer_gradients_for_optimizer( cb, gpu, frame_decoder );
            cb->global_debug_barrier();
            cb->pop_marker();
        }

        // Adam optimizer step for brdf decoder weights
        {
            cb->push_marker( "Optimizer: brdf decoder" );
            for ( u32 i = 0; i < 3; ++i ) {
                transition_wb_layer_for_optimizer( cb, brdf_decoder[ i ] );
            }
            transition_w_layer_for_optimizer( cb, frame_decoder );
            cb->bind_pipeline( optimizer_step_buffer_pso );

            for ( u32 i = 0; i < 3; ++i ) {
                dispatch_optimizer_wb_layer( cb, gpu, optimizer_step_buffer_ds, brdf_decoder[ i ], epoch, epoch_learning_rate );
            }
            dispatch_optimizer_w_layer( cb, gpu, optimizer_step_buffer_ds, frame_decoder, epoch, epoch_learning_rate );

            cb->pop_marker();
        }

        // Adam optimizer step for latent pyramid textures
        {
            cb->push_marker( "Optimizer: latent pyramid" );
            transition_latent_pyramid_for_optimizer( cb, latent_pyramid );

            cb->bind_pipeline( optimizer_step_texture_pso );

            for ( u32 level = 0; level < 1 /* latent_pyramid.level_count */; ++level ) {
                const u32 lw = latent_pyramid.width >> level;
                const u32 lh = latent_pyramid.height >> level;

                // textures0
                {
                    u32 offset = 0;
                    gpu::NMOptimizerTextureConstants* tc = gpu->dynamic_buffer_allocate<gpu::NMOptimizerTextureConstants>( &offset );
                    if ( tc ) {
                        tc->values_texture_index = latent_pyramid.textures0[ level ].index();
                        tc->grads_texture_index = latent_pyramid.grads0[ level ].index();
                        tc->exp_avg_texture_index = latent_pyramid.exp_avg0[ level ].index();
                        tc->exp_avg_sq_texture_index = latent_pyramid.exp_avg_sq0[ level ].index();
                        tc->width = lw;
                        tc->height = lh;
                        tc->optimize_counter = epoch - ENCODER_TRAINING_EPOCHS;
                        tc->learning_rate = epoch_learning_rate;
                    }
                    cb->bind_descriptor_set( { gpu->bindless_descriptor_set, optimizer_step_texture_ds }, { offset } );
                    cb->dispatch( ( lw + 7 ) / 8, ( lh + 7 ) / 8, 1 );
                }

                // textures1
                {
                    u32 offset = 0;
                    gpu::NMOptimizerTextureConstants* tc = gpu->dynamic_buffer_allocate<gpu::NMOptimizerTextureConstants>( &offset );
                    if ( tc ) {
                        tc->values_texture_index = latent_pyramid.textures1[ level ].index();
                        tc->grads_texture_index = latent_pyramid.grads1[ level ].index();
                        tc->exp_avg_texture_index = latent_pyramid.exp_avg1[ level ].index();
                        tc->exp_avg_sq_texture_index = latent_pyramid.exp_avg_sq1[ level ].index();
                        tc->width = lw;
                        tc->height = lh;
                        tc->optimize_counter = epoch - ENCODER_TRAINING_EPOCHS;
                        tc->learning_rate = epoch_learning_rate;
                    }
                    cb->bind_descriptor_set( { gpu->bindless_descriptor_set, optimizer_step_texture_ds }, { offset } );
                    cb->dispatch( ( lw + 7 ) / 8, ( lh + 7 ) / 8, 1 );
                }
            }

            cb->pop_marker();
        }
    }

    // TODO gabriel: too slow for now, reduce texture size probably.
    //if ( false )
    {
        cb->push_marker( "Preview BRDF" );

        cb->barrier_instant_compute_write_to_compute_read();
        for ( u32 i = 0; i < 3; ++i ) {
            transition_wb_layer_for_training( cb, brdf_decoder[ i ] );
        }
        transition_w_layer_for_training( cb, frame_decoder );
        if ( epoch >= ENCODER_TRAINING_EPOCHS ) {
            transition_latent_pyramid_for_training( cb, latent_pyramid );
        } else {
            for ( u32 i = 0; i < 3; ++i ) {
                transition_wb_layer_for_training( cb, encoder[ i ] );
            }
        }

        cb->add_image_barrier( preview_texture.image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL } );
        cb->flush_barriers();
        cb->bind_pipeline( preview_brdf_pso );

        u32 preview_constants_offset = 0;
        gpu::NMPreviewConstants* c = gpu->dynamic_buffer_allocate<gpu::NMPreviewConstants>( &preview_constants_offset );

        if ( c ) {
            c->output_texture_index = preview_texture.index();
            c->normal_map_index = material_normal.texture.index();
            c->roughness_map_index = material_roughness.texture.index();

            c->width = NEURAL_PREVIEW_WIDTH;
            c->height = NEURAL_PREVIEW_HEIGHT;

            Image* material_texture_data = gpu->get_image( material_normal.texture.image );

            c->material_width = material_texture_data->width;
            c->material_height = material_texture_data->height;

            c->use_latent_textures = epoch >= ENCODER_TRAINING_EPOCHS ? 1 : 0;

            c->encoder.l0 = make_gpu_wb_layer( encoder[ 0 ] );
            c->encoder.l1 = make_gpu_wb_layer( encoder[ 1 ] );
            c->encoder.l2 = make_gpu_wb_layer( encoder[ 2 ] );

            c->frame_decoder.l0 = make_gpu_w_layer( frame_decoder );

            c->brdf_decoder.l0 = make_gpu_wb_layer( brdf_decoder[ 0 ] );
            c->brdf_decoder.l1 = make_gpu_wb_layer( brdf_decoder[ 1 ] );
            c->brdf_decoder.l2 = make_gpu_wb_layer( brdf_decoder[ 2 ] );

            c->latent_pyramid = make_gpu_latent_pyramid( latent_pyramid );
        }

        cb->bind_descriptor_set( { gpu->bindless_descriptor_set, preview_brdf_ds }, { preview_constants_offset } );

        cb->dispatch( ( c->width + 7 ) / 8, ( c->height + 7 ) / 8, 1 );

        cb->barrier_instant_compute_write_to_compute_read();
        cb->add_image_barrier( preview_texture.image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ), { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        cb->flush_barriers();

        cb->pop_marker();
    }

    cb->add_buffer_barrier( loss_gpu, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT } );
    cb->add_buffer_barrier( loss_cpu, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT } );
    cb->flush_barriers();
    cb->copy_buffer( loss_gpu, 0, loss_cpu, 0, sizeof( f32 ) * BATCH_SIZE * 3 );
    cb->add_buffer_barrier( loss_cpu, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT } );
    cb->flush_barriers();

    ++epoch;
}

void NeuralMaterialTrainer::initialize_wb_layer_buffers( NeuralWeightBiasLayer& layer ) {

    const u32 weights_count = layer.inputs * layer.outputs;
    const u32 biases_count = layer.outputs;
    const f32 weight_init_limit = compute_glorot_uniform_limit( layer.inputs, layer.outputs );

#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        fill_buffer_random_half( gpu, layer.weights, weights_count, -weight_init_limit, weight_init_limit );
        fill_buffer_zero_bytes( gpu, layer.weights_grad_training, layer.weights_grad_training_size );

        fill_buffer_zero_bytes( gpu, layer.biases, biases_count * sizeof( u16 ) );

        fill_buffer_zero_bytes( gpu, layer.weights_grad, weights_count * sizeof( u16 ) );
        fill_buffer_zero_bytes( gpu, layer.biases_grad, biases_count * sizeof( u16 ) );
    } else
#endif
    {
    fill_buffer_random( gpu, layer.weights, weights_count, -weight_init_limit, weight_init_limit );

    fill_buffer_zero( gpu, layer.biases, biases_count );

    fill_buffer_zero( gpu, layer.weights_grad, weights_count );
    fill_buffer_zero( gpu, layer.biases_grad, biases_count );
    }
    fill_buffer_zero( gpu, layer.weights_exp_avg, weights_count );
    fill_buffer_zero( gpu, layer.biases_exp_avg, biases_count );

    fill_buffer_zero( gpu, layer.weights_exp_avg_sq, weights_count );
    fill_buffer_zero( gpu, layer.biases_exp_avg_sq, biases_count );
}

void NeuralMaterialTrainer::initialize_w_layer_buffers( NeuralWeightLayer& layer ) {

    const u32 weights_count = layer.inputs * layer.outputs;
    const f32 weight_init_limit = compute_glorot_uniform_limit( layer.inputs, layer.outputs );

#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        fill_buffer_random_half( gpu, layer.weights, weights_count, -weight_init_limit, weight_init_limit );
        fill_buffer_zero_bytes( gpu, layer.weights_grad_training, layer.weights_grad_training_size );
        fill_buffer_zero_bytes( gpu, layer.weights_grad, weights_count * sizeof( u16 ) );
    } else
#endif
    {
    fill_buffer_random( gpu, layer.weights, weights_count, -weight_init_limit, weight_init_limit );
    fill_buffer_zero( gpu, layer.weights_grad, weights_count );
    }
    fill_buffer_zero( gpu, layer.weights_exp_avg, weights_count );
    fill_buffer_zero( gpu, layer.weights_exp_avg_sq, weights_count );
}

void NeuralMaterialTrainer::create_wb_layer_resources( NeuralWeightBiasLayer& layer, u32 inputs, u32 outputs, cstring prefix ) {

    u32 type_size = sizeof( f32 );
#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        type_size = sizeof( u16 );
    }
#endif
    const u32 weight_size = inputs * outputs * type_size;
    const u32 bias_size = outputs * type_size;
    const u32 weights_moment_size = inputs * outputs * sizeof( f32 );
    const u32 biases_moment_size = outputs * sizeof( f32 );

    layer.inputs = inputs;
    layer.outputs = outputs;
#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        layer.weights_grad_training_size = query_coopvec_training_matrix_size( gpu, inputs, outputs );
    }
#endif

    BufferCreation buffer_creation = {
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .min_alignment = gpu->cooperative_vector_supported ? gpu->cooperative_vector_matrix_alignment : 0,
    };

    StaticString64 debug_name_buffer;

    debug_name_buffer.format( "%s_weights", prefix );
    buffer_creation.size = weight_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights );

    debug_name_buffer.format( "%s_weights_grad", prefix );
    buffer_creation.size = weight_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_grad = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_grad );

#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        debug_name_buffer.format( "%s_weights_grad_training", prefix );
        buffer_creation.size = layer.weights_grad_training_size;
        buffer_creation.name = debug_name_buffer.c_str();
        layer.weights_grad_training = gpu->create_buffer( buffer_creation );
        gpu->add_buffer_to_bindless( layer.weights_grad_training );
    }
#endif

    debug_name_buffer.format( "%s_weights_exp_avg", prefix );
    buffer_creation.size = weights_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_exp_avg = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_exp_avg );

    debug_name_buffer.format( "%s_weights_exp_avg_sq", prefix );
    buffer_creation.size = weights_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_exp_avg_sq = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_exp_avg_sq );

    debug_name_buffer.format( "%s_biases", prefix );
    buffer_creation.size = bias_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.biases = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.biases );

    debug_name_buffer.format( "%s_biases_grad", prefix );
    buffer_creation.size = bias_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.biases_grad = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.biases_grad );

    debug_name_buffer.format( "%s_biases_exp_avg", prefix );
    buffer_creation.size = biases_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.biases_exp_avg = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.biases_exp_avg );

    debug_name_buffer.format( "%s_biases_exp_avg_sq", prefix );
    buffer_creation.size = biases_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.biases_exp_avg_sq = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.biases_exp_avg_sq );
}

void NeuralMaterialTrainer::create_w_layer_resources( NeuralWeightLayer& layer, u32 inputs, u32 outputs, cstring prefix ) {

    u32 type_size = sizeof( f32 );
#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        type_size = sizeof( u16 );
    }
#endif
    const u32 weight_size = inputs * outputs * type_size;
    const u32 weights_moment_size = inputs * outputs * sizeof( f32 );

    layer.inputs = inputs;
    layer.outputs = outputs;
#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        layer.weights_grad_training_size = query_coopvec_training_matrix_size( gpu, inputs, outputs );
    }
#endif

    BufferCreation buffer_creation = {
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .min_alignment = gpu->cooperative_vector_supported ? gpu->cooperative_vector_matrix_alignment : 0,
    };

    StaticString64 debug_name_buffer;

    debug_name_buffer.format( "%s_weights", prefix );
    buffer_creation.size = weight_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights );

    debug_name_buffer.format( "%s_weights_grad", prefix );
    buffer_creation.size = weight_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_grad = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_grad );

#if RAPTOR_USE_COOPVEC
    if ( gpu->cooperative_vector_supported ) {
        debug_name_buffer.format( "%s_weights_grad_training", prefix );
        buffer_creation.size = layer.weights_grad_training_size;
        buffer_creation.name = debug_name_buffer.c_str();
        layer.weights_grad_training = gpu->create_buffer( buffer_creation );
        gpu->add_buffer_to_bindless( layer.weights_grad_training );
    }
#endif

    debug_name_buffer.format( "%s_weights_exp_avg", prefix );
    buffer_creation.size = weights_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_exp_avg = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_exp_avg );

    debug_name_buffer.format( "%s_weights_exp_avg_sq", prefix );
    buffer_creation.size = weights_moment_size;
    buffer_creation.name = debug_name_buffer.c_str();
    layer.weights_exp_avg_sq = gpu->create_buffer( buffer_creation );
    gpu->add_buffer_to_bindless( layer.weights_exp_avg_sq );
}

void NeuralMaterialTrainer::destroy_wb_layer_resources( NeuralWeightBiasLayer& layer ) {

    gpu->destroy_buffer( layer.biases );
    gpu->destroy_buffer( layer.biases_exp_avg );
    gpu->destroy_buffer( layer.biases_exp_avg_sq );
    gpu->destroy_buffer( layer.biases_grad );
    gpu->destroy_buffer( layer.weights );
    gpu->destroy_buffer( layer.weights_exp_avg );
    gpu->destroy_buffer( layer.weights_exp_avg_sq );
    gpu->destroy_buffer( layer.weights_grad );
    if ( layer.weights_grad_training.is_valid() ) {
        gpu->destroy_buffer( layer.weights_grad_training );
    }
}

void NeuralMaterialTrainer::destroy_w_layer_resources( NeuralWeightLayer& layer ) {

    gpu->destroy_buffer( layer.weights );
    gpu->destroy_buffer( layer.weights_exp_avg );
    gpu->destroy_buffer( layer.weights_exp_avg_sq );
    gpu->destroy_buffer( layer.weights_grad );
    if ( layer.weights_grad_training.is_valid() ) {
        gpu->destroy_buffer( layer.weights_grad_training );
    }
}

} // namespace idra

#if 0
// Devgames2026Demo //////////////////////////////////////////////////////
struct Devgames2026Demo {

    void                        create_resources( AssetManager* asset_manager, AssetCreationPhase::Enum phase );
    void                        destroy_resources( AssetManager* asset_manager, AssetDestructionPhase::Enum phase );

    void                        main_loop();

    CommandLineArguments        command_line_arguments;

    GpuDevice*                  gpu = nullptr;

    NeuralMaterialTrainer       neural_material_trainer;

    // Resources used by demo
    CGLTFAsset*                 gltf_scene = nullptr;
    SceneGraph                  scene_graph;

    ShaderAsset*                depth_shader;
    PipelineHandle              depth_pso;

    ShaderAsset*                raytracing_test_shader_hlsl;
    PipelineHandle              raytracing_test_pso_hlsl;

    ShaderAsset*                post_process_shader;
    PipelineHandle              post_process_pso;

    ShaderAsset*                composite_camera_motion_shader;
    PipelineHandle              composite_camera_motion_pso;


    BLASHandle                  blas;
    TLASHandle                  tlas;

    // Shared
    DescriptorSetLayoutHandle   shared_dsl;
    DescriptorSetHandle         shared_ds;

    // Post Process
    DescriptorSetLayoutHandle   post_dsl;
    DescriptorSetHandle         post_ds;

    DescriptorSetLayoutHandle   composite_camera_motion_dsl;
    DescriptorSetHandle         composite_camera_motion_ds;

    // Ray-tracing
    DescriptorSetLayoutHandle   raytrace_dsl;
    DescriptorSetHandle         raytrace_ds;
    BufferHandle                meshes_buffer;
    BufferHandle                mesh_instances_buffer;
    BufferHandle                materials_buffer;
    BufferHandle                transform_buffer;

    StaticArray<TextureAsset*, 64> blue_noise_textures;

}; // struct Devgames2026Demo

///////////////////////////////////////////////////////////////////////////

void Devgames2026Demo::create_resources( AssetManager* asset_manager, AssetCreationPhase::Enum phase ) {

    sizet base_marker = mem_temp_marker();

    if ( phase == AssetCreationPhase::Startup ) {

        // Ray-tracing
#if 0
        {
            //sizet temp_marker = g_memory->temp_allocator()->get_marker();

            BufferUsage::Mask buffer_usage_mask = ( BufferUsage::Mask )( BufferUsage::AccelerationStructureBuild_mask | BufferUsage::ShaderDeviceAddress_mask );
            transform_buffer = gpu->create_buffer( { .type = buffer_usage_mask, .usage = ResourceUsageType::Immutable,
                                .size = ( u32 )( sizeof( f32 ) * 48 * gltf_scene->mesh_instances.size ), .persistent = 0, .device_only = 0, .initial_data = nullptr,
                                .debug_name = "transform_buffer" } );

            VkTransformMatrixKHR* transforms = ( VkTransformMatrixKHR* )gpu->map_buffer( transform_buffer, 0, 0 );
            if ( transforms ) {
                for ( u32 i = 0; i < gltf_scene->mesh_instances.size; ++i ) {
                    MeshInstance& mesh_instance = gltf_scene->mesh_instances[ i ];

                    mat4s& local_transform = scene_graph.local_matrices[ mesh_instance.scene_graph_node_index ];
                    VkTransformMatrixKHR& transform = transforms[ i ];
                    for ( int y = 0; y < 3; ++y ) {
                        for ( int x = 0; x < 4; ++x ) {
                            transform.matrix[ y ][ x ] = local_transform.raw[ y ][ x ];
                        }
                    }
                }

                gpu->unmap_buffer( transform_buffer );
            }

            Array<BLASGeometry> geometries;
            geometries.init( mem_temp(), gltf_scene->meshes.size);

            for ( u32 i = 0; i < gltf_scene->mesh_instances.size; ++i ) {
                MeshInstance& mesh_instance = gltf_scene->mesh_instances[ i ];
                Mesh& mesh = gltf_scene->meshes[ mesh_instance.mesh_index ];
                BLASGeometry& geometry = geometries.push_use();

                geometry.index_buffer = mesh.index_buffer;
                geometry.index_buffer_offset = mesh.index_offset;
                geometry.index_type = IndexType::Uint16;
                geometry.max_vertex = mesh.primitive_count;
                geometry.position_format = TextureFormat::R32G32B32_FLOAT;
                geometry.primitive_count = mesh.primitive_count;
                geometry.vertex_buffer = mesh.position_buffer;
                geometry.vertex_buffer_offset = mesh.position_offset;
                geometry.vertex_stride = sizeof( f32 ) * 3;
                geometry.transform_index = i;
            }

            blas = gpu->create_blas( {
                .geometries = { geometries.data, geometries.size },
                .transform_buffer = transform_buffer } );

            tlas = gpu->create_tlas( {
                .instances = {{.blas = blas }} } );
        }
#endif

        ShaderAssetLoader* shader_loader = asset_manager->get_loader<ShaderAssetLoader>();

        depth_shader = shader_loader->compile_graphics( {}, { "platform.h" },
                                                        "devgames_2026/depth.vert", "main",
                                                        "devgames_2026/depth.frag" ,"main",
                                                        "depth", ShaderLanguage::Glsl );

        raytracing_test_shader_hlsl = shader_loader->compile_raytracing( {}, {},
                   "devgames_2026/raytrace.hlsl", "ray_gen",
                   "devgames_2026/raytrace.hlsl", "closest_hit",
                   "devgames_2026/raytrace.hlsl", "miss", {}, {}, {}, {},
                   "Raytrace_test_hlsl", ShaderLanguage::Hlsl );

        post_process_shader = shader_loader->compile_compute( {}, {}, "devgames_2026/post_process.hlsl",
                                                              "cs_main", "post_process", ShaderLanguage::Hlsl );

        composite_camera_motion_shader = shader_loader->compile_compute( {}, {}, "devgames_2026/composite_camera_motion.hlsl",
                                                              "cs_main", "composite_camera_motion", ShaderLanguage::Hlsl );

        TextureAssetLoader* texture_loader = asset_manager->get_loader<TextureAssetLoader>();
        Directory current_directory;
        os_directory_current( current_directory.path, 512 );

        {
            StringBuffer name_buffer;
            ScopedTempAllocator temp_allocator(false);
            name_buffer.init( 512, temp_allocator.allocator );
            for ( u32 i = 0; i < 64; ++i ) {
                name_buffer.clear();
                StringView path = name_buffer.append_use_f( "data:/textures/blue_noise/LDR_RGB1_%d.png", i );
                blue_noise_textures.push( texture_loader->load( path, true ) );
            }
        }

        shared_dsl = gpu->create_descriptor_set_layout( {
            .dynamic_buffer_bindings = { 0 },
            .debug_name = "common_dsl" } );

        shared_ds = gpu->create_descriptor_set( {
            .dynamic_buffer_bindings = {{.binding = 0, .size = 64 }},
            .layout = shared_dsl,
            .debug_name = "common_ds" } );

        raytrace_dsl = Raytrace_test_hlsl::set_1::create_descriptor_set_layout( gpu );

        // raytracing descriptor set needs meshes and mesh instances buffers.
#if 0
        BufferUsage::Mask buffer_type = BufferUsage::Structured_mask;
        ResourceUsageType::Enum buffer_usage = ResourceUsageType::Dynamic;

        // Create mesh and mesh instance buffers
        meshes_buffer = gpu->create_buffer( { .type = buffer_type, .usage = buffer_usage,
                                .size = ( u32 )( sizeof( GpuMesh ) * gltf_scene->meshes.size ), .persistent = 0, .device_only = 0, .initial_data = nullptr,
                                .debug_name = "meshes_buffer" } );

        mesh_instances_buffer = gpu->create_buffer( { .type = buffer_type, .usage = buffer_usage,
                                .size = ( u32 )( sizeof( GpuMeshInstance ) * gltf_scene->mesh_instances.size ), .persistent = 0, .device_only = 0, .initial_data = nullptr,
                                .debug_name = "mesh_instances_buffer" } );

        materials_buffer = gpu->create_buffer( { .type = buffer_type, .usage = buffer_usage,
                                .size = ( u32 )( sizeof( GpuMaterial ) * gltf_scene->materials.size ), .persistent = 0, .device_only = 0, .initial_data = nullptr,
                                .debug_name = "materials_buffer" } );

        // Descriptor set and layouts
        Raytrace_test_hlsl::set_1::ssbos( meshes_buffer, mesh_instances_buffer, materials_buffer, gltf_scene->buffers[ 0 ] );
        Raytrace_test_hlsl::set_1::dynamic_buffers( sizeof( GpuRaytraceConstants ) );
        Raytrace_test_hlsl::set_1::tlas( tlas );

        raytrace_ds = Raytrace_test_hlsl::set_1::create_descriptor_set( gpu );
#endif
        // Post-process
        post_dsl = post_process::set_1::create_descriptor_set_layout( gpu );

        post_process::set_1::dynamic_buffers( sizeof( GpuPostProcessConstants ) );
        post_ds = post_process::set_1::create_descriptor_set( gpu );

        // Composite Camera Motion
        composite_camera_motion_dsl = composite_camera_motion::set_1::create_descriptor_set_layout( gpu );

        composite_camera_motion::set_1::dynamic_buffers( sizeof( GpuCompositeCameraMotionConstants ) );
        composite_camera_motion_ds = composite_camera_motion::set_1::create_descriptor_set( gpu );

    }

    // Update dependent assets/resources
    // NOTE: shaders are already reloaded, and just the shader handle is modified.
    // Just need to create the pipelines.
    depth_pso = gpu->create_graphics_pipeline( { .rasterization = {.fill = FillMode::Solid },
            .depth_stencil = {.depth_comparison = ComparisonFunction::Less,
                                .depth_enable = 1, .depth_write_enable = 1 },
            .blend_state = {},
            .vertex_input = {
                .vertex_streams{ {.binding = 0, .stride = 12, .input_rate = VertexInputRate::PerVertex } },
                .vertex_attributes{ {.location = 0, .binding = 0, .offset = 0, .format = VertexComponentFormat::Float3 } }
            },
            .shader = depth_shader->shader,
            .descriptor_set_layouts = { gpu->bindless_descriptor_set_layout, shared_dsl },
            .viewport = {},
            .color_formats = { gpu->swapchain_format },
            .depth_format = TextureFormat::D32_FLOAT,
            .debug_name = "depth_pso" } );

    raytracing_test_pso_hlsl = gpu->create_raytracing_pipeline( {
        .shader = raytracing_test_shader_hlsl->shader,
        .descriptor_set_layouts = { gpu->bindless_descriptor_set_layout, raytrace_dsl },
        .debug_name = "raytrace_pso_hlsl" } );

    post_process_pso = gpu->create_compute_pipeline( {
        .shader = post_process_shader->shader,
        .descriptor_set_layouts = { gpu->bindless_descriptor_set_layout, post_dsl },
        .debug_name = "post_process_pso" });

    composite_camera_motion_pso = gpu->create_compute_pipeline( {
        .shader = composite_camera_motion_shader->shader,
        .descriptor_set_layouts = { gpu->bindless_descriptor_set_layout, composite_camera_motion_dsl },
        .debug_name = "composite_camera_motion_pso" } );

    mem_temp_restore( base_marker );

    neural_material_trainer.create_resources( asset_manager, phase );
}

void Devgames2026Demo::destroy_resources( AssetManager* asset_manager, AssetDestructionPhase::Enum phase ) {

    neural_material_trainer.destroy_resources( asset_manager, phase );

    // Destroy only the psos and return.
    gpu->destroy_pipeline( depth_pso );
    gpu->destroy_pipeline( raytracing_test_pso_hlsl );
    gpu->destroy_pipeline( post_process_pso );
    gpu->destroy_pipeline( composite_camera_motion_pso );

    if ( phase == AssetDestructionPhase::Reload ) {
        return;
    }

    gpu->destroy_blas( blas );
    gpu->destroy_tlas( tlas );

    //g_memory->push_allocator( g_memory->resident_allocator() );

    TextureAssetLoader* texture_loader = asset_manager->get_loader<TextureAssetLoader>();

    for ( u32 i = 0; i < blue_noise_textures.size; ++i ) {
        texture_loader->unload( blue_noise_textures[ i ] );
    }

    //g_memory->pop_allocator();

    ShaderAssetLoader* shader_loader = asset_manager->get_loader<ShaderAssetLoader>();
    shader_loader->unload( depth_shader );
    shader_loader->unload( raytracing_test_shader_hlsl );
    shader_loader->unload( post_process_shader );
    shader_loader->unload( composite_camera_motion_shader );

    gpu->destroy_buffer( transform_buffer );
    gpu->destroy_buffer( meshes_buffer );
    gpu->destroy_buffer( mesh_instances_buffer );
    gpu->destroy_buffer( materials_buffer );

    gpu->destroy_descriptor_set_layout( composite_camera_motion_dsl );
    gpu->destroy_descriptor_set( composite_camera_motion_ds );

    gpu->destroy_descriptor_set_layout( post_dsl );
    gpu->destroy_descriptor_set( post_ds );

    gpu->destroy_descriptor_set_layout( raytrace_dsl );
    gpu->destroy_descriptor_set( raytrace_ds );

    gpu->destroy_descriptor_set_layout( shared_dsl );
    gpu->destroy_descriptor_set( shared_ds );
}

f32                         halton( i32 i, i32 b );
f32                         interleaved_gradient_noise( vec2s pixel, i32 index );

vec2s                       halton23_sequence( i32 index );
vec2s                       m_robert_r2_sequence( i32 index );
vec2s                       interleaved_gradient_sequence( i32 index );
vec2s                       hammersley_sequence( i32 index, i32 num_samples );

f32 halton( i32 i, i32 b ) {
    // Creates a halton sequence of values between 0 and 1.
    // https://en.wikipedia.org/wiki/Halton_sequence
    // Used for jittering based on a constant set of 2D points.
    f32 f = 1.0f;
    f32 r = 0.0f;
    while ( i > 0 ) {
        f = f / f32( b );
        r = r + f * f32( i % b );
        i = i / b;
    }
    return r;
}

// https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
f32 interleaved_gradient_noise( vec2s pixel, i32 index ) {
    pixel = glms_vec2_adds( pixel, f32( index ) * 5.588238f );
    const f32 noise = fmodf( 52.9829189f * fmodf( 0.06711056f * pixel.x + 0.00583715f * pixel.y, 1.0f ), 1.0f );
    return noise;
}

vec2s halton23_sequence( i32 index ) {
    return vec2s{ halton( index, 2 ), halton( index, 3 ) };
}

// http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
vec2s m_robert_r2_sequence( i32 index ) {
    const f32 g = 1.32471795724474602596f;
    const f32 a1 = 1.0f / g;
    const f32 a2 = 1.0f / ( g * g );

    const f32 x = fmod( 0.5f + a1 * index, 1.0f );
    const f32 y = fmod( 0.5f + a2 * index, 1.0f );
    return vec2s{ x, y };
}

vec2s interleaved_gradient_sequence( i32 index ) {
    return vec2s{ interleaved_gradient_noise( {1.f, 1.f}, index ), interleaved_gradient_noise( {1.f, 2.f}, index ) };
}

// Computes a radical inverse with base 2 using crazy bit-twiddling from "Hacker's Delight"
inline f32 radical_inverse_base2( u32 bits ) {
    bits = ( bits << 16u ) | ( bits >> 16u );
    bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
    bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
    bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
    bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
    return f32( bits ) * 2.3283064365386963e-10f; // / 0x100000000
}

// Returns a single 2D point in a Hammersley sequence of length "numSamples", using base 1 and base 2
vec2s hammersley_sequence( i32 index, i32 num_samples ) {
    return vec2s{ index * 1.f / num_samples, radical_inverse_base2( u32( index ) ) };
}

namespace AOTechinques {
    enum Enum {
        ReferenceAmbientOcclusion, RaytracedAmbientOcclusion, Count
    };
} // namespace AOTechinques

namespace ChannelSelectors {
    enum Enum {
        RGBA, Red, Green, Blue, Alpha, Count
    };
} // namespace ChannelSelectors

namespace NoiseTypes {
    enum Enum {
        InterleavedGradient,
        White,
        Blue,
        VogelDisk,
        Count
    };
} // namespace NoiseTypes

void Devgames2026Demo::main_loop() {

    // Init services
    const sizet game01_size = imega( 40 );
    const sizet resident_size = game01_size + imega( 4 );
    const sizet application_size = imega( 128 );
    const sizet streaming_size = imega( 256 );

    // Init services
    HostServices host_services;
    host_services.init( application_size, streaming_size );
    host_services.memory_service.thread_temp_allocator_init( "Main Thread Temp Allocator", ikilo( 256 ) );

    vfs_mount_source( "data_source", "../engine/data", 0, true, "engine" );
    vfs_mount_source( "data_source", "../demos/devgames_2026/data", 1, true, "devgames2026" );
    vfs_mount( "data_output", "data", 0, false );

    vfs_mount( "data", "data/devgames2026", 0, false );
    vfs_mount( "data", "data/engine", 1, false );

    vfs_mount_source( "shaders", "../engine/shaders", 0, true, "engine" );
    vfs_mount_source( "shaders", "../demos/devgames_2026/shaders", 1, true, "devgames2026");

    vfs_mount( "shader_cache", "data/devgames2026/shaders", 0, false );
    vfs_mount( "generated", "../demos/devgames_2026/source/generated/", 0, false );

    // Asset compilation
    AssetCompilerExecutionContext asset_compiler_context{
        .host_services = &host_services,
        .total_memory_in_bytes = imega( 15 )
    };
    asset_compiler_main( { "data_source:/" }, "data_output:/", &asset_compiler_context );

    InputSystem* input = InputSystem::init_system();

    // Window creation
    Window window;
    window.init( 1600, 860, "DevGames 2026 demo", nullptr, input );

    idra::Allocator* app_allocator = mem_resident();

    Directory dir;
    os_directory_current( dir.path, 512 );

    shader_compiler_init( { "shaders:/" }, "shader_cache:/", &host_services, "generated:/" );

    // GPU Device initialization.
    GpuDeviceCreation gpu_creation{
        .system_allocator = app_allocator,
        .os_window_handle = window.platform_handle };

    gpu_creation.resource_pool_creation.buffers = 256;
    gpu_creation.resource_pool_creation.descriptor_set_bindings_2 = 16;

    gpu = idra::GpuDevice::init_system( gpu_creation );

    neural_material_trainer.gpu = gpu;

    // ImGui Service
    g_imgui->init( gpu, window.platform_handle, window.get_dpi_scale() );

    ImGui::ApplicationLogInit();
    ImGui::FPSInit();

    scene_graph.init( app_allocator, 8 );

    // Asset manager
    idra::AssetManager* asset_manager = idra::AssetManager::init_system( 192 );
    // Asset loaders
    idra::ShaderAssetLoader shader_loader;
    shader_loader.init( app_allocator, 32, asset_manager, gpu );

    idra::TextureAssetLoader texture_loader;
    texture_loader.init( app_allocator, 192, asset_manager, gpu );

    idra::TextureAtlasLoader atlas_loader;
    atlas_loader.init( app_allocator, 128, asset_manager, gpu );

    CGLTFAssetLoader gltf_loader;
    gltf_loader.init( app_allocator, 128, asset_manager );
    gltf_loader.gpu = gpu;
    gltf_loader.scene_graph = &scene_graph;

    // Assign loaders
    asset_manager->set_loader( idra::ShaderAssetLoader::k_loader_index, &shader_loader );
    asset_manager->set_loader( idra::TextureAssetLoader::k_loader_index, &texture_loader );
    asset_manager->set_loader( idra::TextureAtlasLoader::k_loader_index, &atlas_loader );
    asset_manager->set_loader( idra::CGLTFAssetLoader::k_loader_index, &gltf_loader );

    // Load assets!
    //g_memory->push_allocator( app_allocator );

#if 0
    StringView gltf_path;

    // Iterating and string comparing on a very small set of arguments is fast.
    // Syntax: -gltf %path%
    for ( u32 i = 0; i < command_line_arguments.arguments.size; ++i ) {
        const bool has_following_argument = i < command_line_arguments.arguments.size - 1;
        if ( strcmp( command_line_arguments.arguments[ i ], "-gltf" ) == 0 && has_following_argument ) {
            // Take the following parameter
            gltf_path = command_line_arguments.arguments[ i + 1 ];
            break;
        }
    }

    if ( gltf_path.size == 0 ) {

        ilog_error( "No GLTF scene was specified using command line -gltf (path). Quitting.\n" );
        return;
    }

    gltf_scene = gltf_loader.load( gltf_path );
    //g_memory->pop_allocator();
#endif

    ilog( "Done\n" );

    // First camera!
    idra::GameCamera game_camera;
    game_camera.camera.init_perpective( 0.1f, 1000.f, 60.f, gpu->swapchain_width * 1.f / gpu->swapchain_height );
    game_camera.camera.position = { 0.f, 0.f, 0.f };
    game_camera.init( true, 20.f, 3.f );

    // Render Systems
    idra::DebugRenderer debug_renderer;
    debug_renderer.pre_init( 2, 10000 );

    debug_renderer.init( gpu, app_allocator );
    debug_renderer.create_resources( asset_manager, idra::AssetCreationPhase::Startup );
    debug_renderer.scale = .01f;

    create_resources( asset_manager, idra::AssetCreationPhase::Startup );

#if 0
    void* mem = gpu->map_buffer( meshes_buffer, 0, 0 );
    if ( mem ) {
        GpuMesh* mesh_buffer = ( GpuMesh* )mem;
        for ( u32 i = 0; i < gltf_scene->meshes.size; ++i ) {

            Mesh& mesh = gltf_scene->meshes[ i ];
            GpuMesh& gpu_mesh = mesh_buffer[ i ];

            gpu_mesh.position_buffer_address = gpu->get_buffer_device_address( mesh.position_buffer ) + mesh.position_offset;
            gpu_mesh.normals_buffer_address = mesh.normal_buffer.is_valid() ? gpu->get_buffer_device_address( mesh.normal_buffer ) + mesh.normal_offset : u64_max;
            gpu_mesh.uv_buffer_address = mesh.uv0_buffer.is_valid() ? gpu->get_buffer_device_address( mesh.uv0_buffer ) + mesh.uv0_offset : u64_max;
            gpu_mesh.index_buffer_address = gpu->get_buffer_device_address( mesh.index_buffer ) + mesh.index_offset;

            gpu_mesh.position_buffer_offset = mesh.position_offset;
            gpu_mesh.normal_buffer_offset = mesh.normal_buffer.is_valid() ? mesh.normal_offset : u64_max;
            gpu_mesh.uv0_buffer_offset = mesh.uv0_buffer.is_valid() ? mesh.uv0_offset : u64_max;
            gpu_mesh.index_buffer_offset = mesh.index_offset;

        }

        gpu->unmap_buffer( meshes_buffer );
    }

    mem = gpu->map_buffer( mesh_instances_buffer, 0, 0 );
    if ( mem ) {
        GpuMeshInstance* mesh_buffer = ( GpuMeshInstance* )mem;
        for ( u32 i = 0; i < gltf_scene->mesh_instances.size; ++i ) {

            MeshInstance& mesh_instance = gltf_scene->mesh_instances[ i ];
            GpuMeshInstance& gpu_mesh = mesh_buffer[ i ];
            gpu_mesh.mesh_draw_index = mesh_instance.mesh_index;
            gpu_mesh.material_index = mesh_instance.material_index;

            mat4s& transform = scene_graph.world_matrices[ mesh_instance.scene_graph_node_index ];
            gpu_mesh.model = transform;
        }

        gpu->unmap_buffer( mesh_instances_buffer );
    }

    mem = gpu->map_buffer( materials_buffer, 0, 0 );
    if ( mem ) {
        // TODO:
        GpuMaterial* material_buffer = ( GpuMaterial* )mem;
        for ( u32 i = 0; i < gltf_scene->materials.size; ++i ) {

            Material& material = gltf_scene->materials[ i ];
            GpuMaterial& gpu_material = material_buffer[ i ];
            gpu_material.albedo = material.albedo ? material.albedo->texture.index : u32_max;
            gpu_material.normals = material.normals ? material.normals->texture.index : u32_max;
        }

        gpu->unmap_buffer( materials_buffer );
    }
#endif

    // Render targets
    TextureCreation texture_creation = {
        .width = ( u16 )gpu->swapchain_width, .height = ( u16 )gpu->swapchain_height, .depth = 1, .array_layer_count = 1,
        .mip_level_count = 1, .flags = idra::TextureFlags::Compute_mask | idra::TextureFlags::RenderTarget_mask,
        .format = gpu->swapchain_format, .type = idra::TextureType::Texture2D,
        .debug_name = "game_rt" };

    // Final render texture embedded in a ImGui Widget
    idra::TextureHandle game_rt = gpu->create_texture( texture_creation );

    // Textures used by various techniques.
    // It could be improved with texture aliasing.
    //
    // G-Buffer albedo RT
    texture_creation.debug_name = "raytrace_albedo_rt";
    idra::TextureHandle raytrace_albedo_rt = gpu->create_texture( texture_creation );

    texture_creation.debug_name = "camera_motion_vectors_rt";
    texture_creation.format = TextureFormat::R16G16_FLOAT;
    idra::TextureHandle camera_motion_vectors_rt = gpu->create_texture( texture_creation );

    // Current and previous frame normals and depth (used for filtering)
    texture_creation.debug_name = "raytrace_normals_rt";
    texture_creation.format = TextureFormat::R16G16_FLOAT;
    idra::TextureHandle raytrace_normals_rt0 = gpu->create_texture( texture_creation );
    idra::TextureHandle raytrace_normals_rt1 = gpu->create_texture( texture_creation );

    texture_creation.debug_name = "raytrace_depth_rt";
    texture_creation.format = TextureFormat::R32_FLOAT;
    idra::TextureHandle raytrace_depth_rt0 = gpu->create_texture( texture_creation );
    idra::TextureHandle raytrace_depth_rt1 = gpu->create_texture( texture_creation );

    texture_creation.format = TextureFormat::D32_FLOAT;
    texture_creation.flags = idra::TextureFlags::RenderTarget_mask;
    texture_creation.debug_name = "game_depth_rt";
    idra::TextureHandle game_depth_rt = gpu->create_texture( texture_creation );

    ImGui::ImGuiRenderView game_render_view;
    game_render_view.init( &game_camera, { game_rt, game_depth_rt }, gpu );

    GpuVisualProfiler gpu_profiler;
    gpu_profiler.init( app_allocator, 100, 60 );

    bool quit_application = false;

    u64 begin_frame_tick = time_ticks_now();
    u64 absolute_begin_frame_tick = begin_frame_tick;

    const u32 game_view_index = 0;
    const u32 debug_view_index = 1;

    f32 elapsed_time = 0.f;

    // Sun
    float sun_pitch = 0.45f, sun_yaw = 0;
    u32 current_frame = 0, noise_animation_frame = 0;

    // Used to check if camera has moved
    mat4s previous_view_matrix, previous_view_projection_matrix, current_view_matrix;
    bool apply_camera_jitter = false;
    bool advance_frame_manually = false;

    current_view_matrix = game_render_view.camera->camera.view_projection;

    // Options
    bool show_debug_rendering = false;

    // Render targets
    Span<const TextureHandle> render_targets = {
        raytrace_albedo_rt, raytrace_normals_rt0, raytrace_depth_rt0,
        raytrace_normals_rt1, raytrace_depth_rt1, camera_motion_vectors_rt };

    cstring render_target_names[] = { "raytrace_albedo_rt", "raytrace_normals_rt0", "raytrace_depth_rt0",
        "raytrace_normals_rt1", "raytrace_depth_rt1", "camera_motion_vectors_rt" };

    // Debug render target
    u32 debug_rt_index = 0;
    bool fullscreen_debug_rt = false;
    cstring channel_selector_names[] = { "RGBA", "Red", "Green", "Blue", "Alpha" };
    u32 channel_selector_index = 0;

    // Used to pause the training.
    bool advance_frame = false;

    // Main loop!
    while ( window.is_running && !quit_application ) {
        // Frame begin
        window.handle_os_messages();
        input->new_frame();

        if ( window.resized ) {

            game_camera.camera.set_aspect_ratio( window.width * 1.f / window.height );
            game_camera.camera.set_viewport_size( ( f32 )window.width, ( f32 )window.height );

            idra::SwapchainStatus::Enum swapchain_status = gpu->update_swapchain();
            if ( swapchain_status == idra::SwapchainStatus::NotReady ) {
                // TODO: imgui will need to be called here anyway
                continue;
            }

            window.resized = false;
        }

        gpu->wait_for_frames_in_flight();
        gpu->new_frame();
        g_imgui->new_frame();

        // Check for game window resize
        // Resize all the needed render_targets
        if ( game_render_view.check_resize( gpu, input ) ) {

            for ( u32 i = 0; i < render_targets.size; ++i ) {
                gpu->resize_texture( render_targets[ i ], ( u32 )game_render_view.texture_width, ( u32 )game_render_view.texture_height );
            }

        }

        static u32 jitter_index = 0;
        static u32 jitter_period = 2;
        vec2s jitter_values = halton23_sequence( jitter_index );

        jitter_index = ( jitter_index + 1 ) % jitter_period;

        vec2s jitter_offsets = vec2s{ jitter_values.x * 2 - 1.0f, jitter_values.y * 2 - 1.0f };

        if ( apply_camera_jitter ) {
            game_camera.apply_jittering( jitter_offsets.x / game_render_view.texture_width, jitter_offsets.y / game_render_view.texture_height );
        }

        previous_view_projection_matrix = current_view_matrix;

        const u64 current_tick = time_ticks_now();
        f32 delta_time = ( f32 )time_ticks_to_seconds( current_tick - begin_frame_tick );
        begin_frame_tick = current_tick;

        elapsed_time += delta_time;

        // Re-center mouse
        if ( game_render_view.focus ) {
            game_camera.update( input, window.width, window.height, delta_time );

            current_view_matrix = game_camera.camera.view_projection;
            // TODO: improve interface
            window.center_mouse( game_camera.mouse_dragging );
        }

        // Debug rendering test
        // View index is a way to dispatch line draws to different cameras
        debug_renderer.aabb( { -1,-1,-1 }, { 1,1,1 }, idra::Color::green(), game_view_index );

        // Frame update
        ImGui::DockSpaceOverViewport( 0, ImGui::GetMainViewport() );
        if ( ImGui::BeginMainMenuBar() ) {
            if ( ImGui::BeginMenu( "File" ) ) {
                ImGui::MenuItem( "Quit", nullptr, &quit_application );
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if ( ImGui::Begin( "DevGames 2026" ) ) {

            ImGui::Checkbox( "Advance Frame Training", &advance_frame );

            if ( ImGui::Button( "Reload shaders" ) ) {

                debug_renderer.destroy_resources( asset_manager, idra::AssetDestructionPhase::Reload );

                destroy_resources( asset_manager, idra::AssetDestructionPhase::Reload );

                asset_manager->get_loader<idra::ShaderAssetLoader>()->reload_assets();

                debug_renderer.create_resources( asset_manager, idra::AssetCreationPhase::Reload );

                create_resources( asset_manager, idra::AssetCreationPhase::Reload );
            }

            ImGui::Checkbox( "Show Debug Rendering", &show_debug_rendering );

            ImGui::Checkbox( "Camera Jitter", &apply_camera_jitter );
            ImGui::Text( "Camera position %f,%f,%f", game_camera.camera.position.x, game_camera.camera.position.y, game_camera.camera.position.z );

            if ( ImGui::Button( "Reset camera position" ) ) {
                game_camera.camera.position = { 0.f, 2.f, 0.f };
                game_camera.target_movement = game_camera.camera.position;
            }

            ImGui::Text( "Camera near %f far %f", game_camera.camera.near_plane, game_camera.camera.far_plane );

            if ( ImGui::SliderFloat( "Camera Near", &game_camera.camera.near_plane, 0.001f, 32000.f ) ) {
                game_camera.camera.update_projection = true;
            }

            if ( ImGui::SliderFloat( "Camera Far", &game_camera.camera.far_plane, 0.001f, 32000.f ) ) {
                game_camera.camera.update_projection = true;
            }

            ImGui::SliderFloat( "Camera Movement Speed", &game_camera.movement_speed, 0.1f, 10.0f );
            ImGui::SliderFloat("Camera Movement Smoothness", &game_camera.movement_smoothness, 0.01f, 1.0f);

            ImGui::Text( "Camera position %f,%f,%f", game_camera.camera.position.x, game_camera.camera.position.y, game_camera.camera.position.z );

            ImGui::Text( "Camera focal %f", game_camera.camera.projection.raw[ 0 ][ 0 ] );
            ImGui::Text( "Camera aspect %f", game_camera.camera.projection.raw[ 1 ][ 1 ] );

            ImGui::Text( "Camera View 0 %f, %f, %f", game_camera.camera.view.raw[ 0 ][ 0 ], game_camera.camera.view.raw[ 0 ][ 1 ], game_camera.camera.view.raw[ 0 ][ 2 ] );
            ImGui::Text( "Camera View 1 %f, %f, %f", game_camera.camera.view.raw[ 1 ][ 0 ], game_camera.camera.view.raw[ 1 ][ 1 ], game_camera.camera.view.raw[ 1 ][ 2 ] );
            ImGui::Text( "Camera View 2 %f, %f, %f", game_camera.camera.view.raw[ 2 ][ 0 ], game_camera.camera.view.raw[ 2 ][ 1 ], game_camera.camera.view.raw[ 2 ][ 2 ] );
            ImGui::Text( "Camera View 3 %f, %f, %f", game_camera.camera.view.raw[ 3 ][ 0 ], game_camera.camera.view.raw[ 3 ][ 1 ], game_camera.camera.view.raw[ 3 ][ 2 ] );

            mat3s rotation{
                game_camera.camera.view.raw[ 0 ][ 0 ], game_camera.camera.view.raw[ 0 ][ 1 ], game_camera.camera.view.raw[ 0 ][ 2 ],
                game_camera.camera.view.raw[ 1 ][ 0 ], game_camera.camera.view.raw[ 1 ][ 1 ], game_camera.camera.view.raw[ 1 ][ 2 ],
                game_camera.camera.view.raw[ 2 ][ 0 ], game_camera.camera.view.raw[ 2 ][ 1 ], game_camera.camera.view.raw[ 2 ][ 2 ]
            };
            vec3s camera_w{ game_camera.camera.view.raw[ 3 ][ 0 ], game_camera.camera.view.raw[ 3 ][ 1 ], game_camera.camera.view.raw[ 3 ][ 2 ] };

            vec3s camera_rotation = glms_mat3_mulv( rotation, camera_w );
            ImGui::Text( "Camera Rotation %f, %f, %f", camera_rotation.raw[ 0 ], camera_rotation.raw[ 1 ], camera_rotation.raw[ 2 ] );

            //ImGui::SliderFloat( "Sun Pitch", &sun_pitch, -3.14f, 3.14f );
            //ImGui::SliderFloat( "Sun Yaw", &sun_yaw, -3.14f, 3.14f );

            ImGui::Separator();

            ImVec2 rt_size = { game_render_view.texture_width, game_render_view.texture_height };

            ImGui::Checkbox( "Fullscreen debug RT", &fullscreen_debug_rt );
            if ( ImGui::BeginCombo( "Debug RT", render_target_names[ debug_rt_index ] ) ) {

                for ( u32 i = 0; i < render_targets.size; ++i ) {
                    bool is_selected = debug_rt_index == i;
                    if ( ImGui::Selectable( render_target_names[ i ], is_selected ) ) {
                        debug_rt_index = i;

                        break;
                    }
                }

                ImGui::EndCombo();
            }
            if ( ImGui::BeginCombo( "Debug RT Channel", channel_selector_names[ channel_selector_index ] ) ) {

                for ( u32 i = 0; i < 5; ++i ) {
                    bool is_selected = channel_selector_index == i;
                    if ( ImGui::Selectable( channel_selector_names[ i ], is_selected ) ) {
                        channel_selector_index = i;

                        break;
                    }
                }

                ImGui::EndCombo();
            }
            TextureHandle rt = render_targets[ debug_rt_index ];
            ImGui::Image( rt, rt_size );
        }
        ImGui::End();

        if ( ImGui::Begin( "GPU Profiler" ) ) {
            gpu_profiler.imgui_draw();
        }
        ImGui::End();

        gpu_profiler.update( *gpu );

        if ( ImGui::Begin( "Training Loss" ) ) {
            ImGui::TextColored( ImVec4( 0.95f, 0.35f, 0.35f, 1.0f ), "R: %0.5f", neural_material_trainer.current_loss[ 0 ] );
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.35f, 0.85f, 0.45f, 1.0f ), "G: %0.5f", neural_material_trainer.current_loss[ 1 ] );
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.35f, 0.60f, 0.95f, 1.0f ), "B: %0.5f", neural_material_trainer.current_loss[ 2 ] );

            draw_training_loss_plot( neural_material_trainer );
        }
        ImGui::End();

        if ( ImGui::Begin( "Neural Material Preview" ) ) {
            ImGui::Text( "Left: Target" );
            ImGui::Text( "Middle: Neural" );
            ImGui::Text( "Right: Error" );

            const f32 display_width = 1200.0f;
            const f32 display_height = display_width * f32( NEURAL_PREVIEW_HEIGHT ) / f32( NEURAL_PREVIEW_WIDTH );
            const ImVec2 size{ display_width, display_height };

            ImGui::Image( neural_material_trainer.preview_texture, size );
        }
        ImGui::End();

        ImGui::ApplicationLogDraw();

        game_render_view.draw( "Game View" );

        texture_loader.update();

        // Render
        idra::CommandBuffer* cb = gpu->acquire_new_command_buffer();

        cb->push_marker( "frame" );

        if ( advance_frame ) {
            neural_material_trainer.step( cb );
        }

#if 0
        PipelineHandle raytracing_pso = raytracing_test_pso_hlsl;

        TextureHandle current_depth_rt, current_normals_rt, previous_depth_rt, previous_normals_rt;

        if ( (current_frame % 2) == 0 ) {
            current_normals_rt = raytrace_normals_rt0;
            previous_normals_rt = raytrace_normals_rt1;

            current_depth_rt = raytrace_depth_rt0;
            previous_depth_rt = raytrace_depth_rt1;
        }
        else {
            current_normals_rt = raytrace_normals_rt1;
            previous_normals_rt = raytrace_normals_rt0;

            current_depth_rt = raytrace_depth_rt1;
            previous_depth_rt = raytrace_depth_rt0;
        }

        // Raytrace render
        cb->push_marker( "Raytrace render" );
        {
            cb->submit_barriers( { { raytrace_albedo_rt, idra::ResourceState::UnorderedAccess, 0, 1},
                                 { current_depth_rt, idra::ResourceState::UnorderedAccess, 0, 1},
                                 { current_normals_rt, idra::ResourceState::UnorderedAccess, 0, 1} }, {} );

            u32 data_offset;
            GpuRaytraceConstants* m = gpu->dynamic_buffer_allocate<GpuRaytraceConstants>( &data_offset );
            if ( m ) {
                m->camera_position = game_camera.camera.position;
                m->view_projection = game_camera.camera.view_projection;
                m->inverse_view_projection = glms_mat4_inv( game_camera.camera.view_projection );

                m->sbt_offset = 0;
                m->sbt_stride = gpu->ray_tracing_pipeline_properties.shaderGroupHandleAlignment;
                m->miss_index = 0;
                m->out_image_index = raytrace_albedo_rt.index;
                m->out_depth_index = current_depth_rt.index;
                m->out_normals_index = current_normals_rt.index;
                m->use_normal_maps = 1;// use_normal_maps ? 1 : 0;
            }

            cb->bind_pipeline( raytracing_pso );

            cb->bind_descriptor_set( { gpu->bindless_descriptor_set, raytrace_ds }, { data_offset } );
            cb->trace_rays( raytracing_pso, ( u32 )game_render_view.texture_width, ( u32 )game_render_view.texture_height, 1 );

            cb->submit_barriers( { {raytrace_albedo_rt, idra::ResourceState::ShaderResource, 0, 1},
                                 { current_depth_rt, idra::ResourceState::ShaderResource, 0, 1 },
                                 { current_normals_rt, idra::ResourceState::ShaderResource, 0, 1 }}, {} );
        }
        cb->pop_marker();

        cb->push_marker( "Composite Camera Motion" );
        {
            cb->submit_barriers( { {camera_motion_vectors_rt, idra::ResourceState::UnorderedAccess, 0, 1} }, {} );
            u32 data_offset;
            GpuCompositeCameraMotionConstants* m = gpu->dynamic_buffer_allocate<GpuCompositeCameraMotionConstants>( &data_offset );
            if ( m ) {
                m->inverse_view_projection = glms_mat4_inv( game_camera.camera.view_projection );
                m->view_projection = game_camera.camera.view_projection;
                m->previous_view_projection = previous_view_projection_matrix;
                m->input_image_index = current_depth_rt.index;
                m->output_width = ( u32 )game_render_view.texture_width;
                m->output_height = ( u32 )game_render_view.texture_height;
                m->output_image_index = camera_motion_vectors_rt.index;
                m->camera_position = game_camera.camera.position;
            }

            cb->bind_pipeline( composite_camera_motion_pso );
            cb->bind_descriptor_set( { gpu->bindless_descriptor_set, composite_camera_motion_ds }, { data_offset } );
            cb->dispatch_2d( ( u32 )game_render_view.texture_width, ( u32 )game_render_view.texture_height, 8, 8 );

            cb->submit_barriers( { {camera_motion_vectors_rt, idra::ResourceState::ShaderResource, 0, 1} }, {} );
        }
        cb->pop_marker();

        TextureHandle post_process_input = raytrace_albedo_rt;

        // Render game view
        cb->push_marker( "game render" );

        cb->submit_barriers( { {game_rt, idra::ResourceState::UnorderedAccess, 0, 1} }, {} );

        u32 data_offset;
        GpuPostProcessConstants* m = gpu->dynamic_buffer_allocate<GpuPostProcessConstants>( &data_offset );
        if ( m ) {

            if ( fullscreen_debug_rt ) {
                post_process_input = render_targets[ debug_rt_index ];
            }

            // Default to 0, in shader code will be outputting the raw texture
            vec4s channel_selector_vector = { 0,0,0,0 };
            switch ( channel_selector_index ) {
                case ChannelSelectors::Red:
                {
                    channel_selector_vector = { 1,0,0,0 };
                    break;
                }
                case ChannelSelectors::Green:
                {
                    channel_selector_vector = { 0,1,0,0 };
                    break;
                }
                case ChannelSelectors::Blue:
                {
                    channel_selector_vector = { 0,0,1,0 };
                    break;
                }
                case ChannelSelectors::Alpha:
                {
                    channel_selector_vector = { 0,0,0,1 };
                    break;
                }
            }

            m->single_channel_selector = channel_selector_vector;
            m->output_image_index = game_rt.index;
            m->output_width = ( u32 )game_render_view.texture_width;
            m->output_height = ( u32 )game_render_view.texture_height;
            m->input_image_index = post_process_input.index;
        }

        cb->bind_pipeline( post_process_pso );

        cb->bind_descriptor_set( { gpu->bindless_descriptor_set, post_ds }, { data_offset } );
        cb->dispatch_2d( ( u32 )game_render_view.texture_width, ( u32 )game_render_view.texture_height, 8, 8 );

        //cb->submit_barriers( { {game_rt, idra::ResourceState::ShaderResource, 0, 1} }, {} );

        cb->submit_barriers( { {game_rt, idra::ResourceState::RenderTarget, 0, 1},
                             {game_depth_rt, idra::ResourceState::RenderTarget, 0, 1} }, {} );

        cb->begin_pass( { game_rt }, { LoadOperation::Load }, { {0.1f,0.1f,0.1f,1.f} }, game_depth_rt, LoadOperation::Clear, { .depth_value = 1.0f } );
        cb->set_framebuffer_scissor();
        cb->set_framebuffer_viewport();

        // Scene render
        {
            /*u32 data_offset;
            mat4s* m = gpu->dynamic_buffer_allocate<mat4s>( &data_offset );
            if ( m ) {
                *m = game_camera.camera.view_projection;
            }

            cb->bind_pipeline( depth_pso );

            for ( u32 i = 0; i < gltf_scene->meshes.size; ++i ) {
                Mesh& mesh = gltf_scene->meshes[ i ];

                cb->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );
                cb->bind_index_buffer( mesh.index_buffer, mesh.index_offset, IndexType::Uint16 );
                cb->bind_descriptor_set( { gpu->bindless_descriptor_set, shared_ds }, { data_offset } );

                cb->draw_indexed( TopologyType::Triangle, mesh.primitive_count, 1, 0, 0, 0 );
            }*/
        }

        // Debug rendering
        if ( show_debug_rendering ) {
            debug_renderer.render( cb, &game_camera.camera, 0 );
        }

        cb->end_render_pass();

        cb->submit_barriers( { {game_rt, ResourceState::ShaderResource, 0, 1},
                             { game_depth_rt, ResourceState::ShaderResource, 0, 1 } }, {} );
        cb->pop_marker();
#endif

        // Swapchain rendering!
        idra::TextureHandle swapchain = gpu->get_current_swapchain_texture();

        // TODO: where should barriers be exposed ?
        cb->push_marker( "swapchain_pass" );

        cb->submit_barriers( { {swapchain, idra::ResourceState::RenderTarget, 0, 1} }, {} );
        cb->begin_pass( { swapchain }, { idra::LoadOperation::Clear }, { { 0, 0, 0, 1 } }, {}, idra::LoadOperation::DontCare, {} );

        cb->set_framebuffer_scissor();
        cb->set_framebuffer_viewport();

        // Imgui render
        g_imgui->render( *cb );

        cb->end_render_pass();

        cb->submit_barriers( { {swapchain, idra::ResourceState::Present, 0, 1} }, {} );
        cb->pop_marker();
        cb->pop_marker();

        gpu->enqueue_command_buffer( cb );
        gpu->present();

        // Always update current frame.
        // It is used to swap between render targets and other systems
        ++current_frame;

    }

    gpu->destroy_texture( game_rt );
    gpu->destroy_texture( game_depth_rt );
    gpu->destroy_texture( raytrace_albedo_rt );
    gpu->destroy_texture( raytrace_normals_rt0 );
    gpu->destroy_texture( raytrace_normals_rt1 );
    gpu->destroy_texture( raytrace_depth_rt0 );
    gpu->destroy_texture( raytrace_depth_rt1 );
    gpu->destroy_texture( camera_motion_vectors_rt );

    destroy_resources( asset_manager, idra::AssetDestructionPhase::Shutdown );

    gpu_profiler.shutdown();

    gltf_loader.unload_asset( gltf_scene );

    debug_renderer.destroy_resources( asset_manager, idra::AssetDestructionPhase::Shutdown );
    debug_renderer.shutdown();

    // Shutdown systems and services
    ImGui::ApplicationLogShutdown();
    ImGui::FPSShutdown();

    idra::AssetManager::shutdown_system( asset_manager );

    g_imgui->shutdown();
    InputSystem::shutdown_system( input );
    window.shutdown();
    GpuDevice::shutdown_system( gpu );

    shader_compiler_shutdown();

    host_services.shutdown();
}

} // namespace idra
#endif

// Main ///////////////////////////////////////////////////////////////////
int main( int argc, char** argv ) {
    ( void )argc;
    ( void )argv;

    using namespace raptor;

    time_service_init();

    MemoryServiceConfiguration memory_configuration;
    memory_configuration.maximum_dynamic_size = rgiga( 2ull );

    MemoryService::instance()->init( &memory_configuration );
    Allocator* allocator = &MemoryService::instance()->system_allocator;

    WindowConfiguration wconf{ 1280, 800, "Chapter 15: Neural Material Training", allocator };
    Window window;
    window.init( &wconf );

    GpuDeviceCreation dc;
    dc.debug_options.set_validation();
    dc.enable_bindless = true;
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( allocator );
    dc.resource_pool_creation.buffers = 1024;
    dc.resource_pool_creation.images = 256;
    dc.resource_pool_creation.image_views = 256;
    dc.resource_pool_creation.pipelines = 128;
    dc.resource_pool_creation.descriptor_set_layouts = 128;
    dc.resource_pool_creation.descriptor_sets = 256;
    dc.descriptor_pool_creation.storage_buffer = 512;
    dc.descriptor_pool_creation.uniform_buffer = 128;

    GpuDevice gpu;
    gpu.init( dc );

    ResourceManager rm;
    rm.init( allocator, nullptr );

    GpuVisualProfiler gpu_profiler;
    gpu_profiler.init( allocator, gpu.gpu_timestamp_frequency, 100, dc.gpu_time_queries_per_frame );

    RendererResourcePoolCreation rrpc{};
    rrpc.buffers = dc.resource_pool_creation.buffers;
    rrpc.textures = 256;

    Renderer renderer;
    renderer.init( { &gpu, allocator, rrpc } );
    renderer.set_loaders( &rm );

    FrameGraphBuilder frame_graph_builder;
    frame_graph_builder.init( &gpu );

    FrameGraph frame_graph;
    frame_graph.init( &frame_graph_builder );

    ImGuiService* imgui = ImGuiService::instance();
    ImGuiServiceConfiguration imgui_config{ &gpu, &renderer, window.platform_handle };
    imgui->init( &imgui_config );

    idra::NeuralMaterialTrainer trainer;
    trainer.create_resources( &renderer, &frame_graph );

    bool training_enabled = true;
    i64 begin_frame_tick = time_now();

    while ( !window.requested_exit ) {
        ZoneScopedN( "RenderLoop" );

        if ( !window.minimized ) {
            gpu.wait_for_previous_frame();
            VkResult result = gpu.acquire_next_swapchain_image();
            if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
                gpu.resize_swapchain();
            }
            else if ( result == VK_ERROR_DEVICE_LOST ) {
                gpu.dump_device_fault();
                break;
            }

            gpu.update_descriptors();
            gpu.reset_pools();
            gpu.update_bindless_resources();
        }

        window.handle_os_messages();

        if ( window.resized ) {
            gpu.resize( window.width, window.height );
            window.resized = false;
        }

        imgui->new_frame();

        const i64 current_tick = time_now();
        const f32 delta_time = ( f32 )time_delta_seconds( begin_frame_tick, current_tick );
        begin_frame_tick = current_tick;

        if ( ImGui::Begin( "Chapter 15: Neural Materials" ) ) {
            ImGui::Checkbox( "Training", &training_enabled );
            ImGui::Text( "Epoch: %u", trainer.epoch );
            ImGui::Text( "Frame time: %.3f ms", delta_time * 1000.0f );
            ImGui::Text( "Loss: %.6f %.6f %.6f", trainer.current_loss[ 0 ], trainer.current_loss[ 1 ], trainer.current_loss[ 2 ] );
        }
        ImGui::End();

        if ( ImGui::Begin( "Training Loss" ) ) {
            idra::draw_training_loss_plot( trainer );
        }
        ImGui::End();

        if ( ImGui::Begin( "Neural Material Preview" ) ) {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const f32 aspect = ( f32 )idra::NEURAL_PREVIEW_WIDTH / ( f32 )idra::NEURAL_PREVIEW_HEIGHT;
            ImVec2 image_size = { available.x, available.x / aspect };
            if ( image_size.y > available.y ) {
                image_size.y = available.y;
                image_size.x = image_size.y * aspect;
            }
            ImGui::Image( ( ImTextureID )&trainer.preview_texture.view, image_size );
        }
        ImGui::End();

        if ( ImGui::Begin( "GPU" ) ) {
            gpu_profiler.imgui_draw();
        }
        ImGui::End();

        imgui->finalize_draw_data();

        if ( !window.minimized ) {
            CommandBuffer* cb = gpu.allocate_command_buffer( 0, gpu.current_frame, CommandQueueType::Graphics );
            cb->begin();
            gpu.gpu_profiler->begin_command_buffer( cb );
            cb->push_marker( "Chapter15" );

            if ( training_enabled ) {
                trainer.step( cb );
            }

            cb->add_image_barrier( gpu.get_current_swapchain_image(), range_color_full(),
                                   { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
            cb->flush_barriers();

            cb->begin_render_pass( { gpu.get_current_swapchain_image_view() }, { VK_ATTACHMENT_LOAD_OP_CLEAR }, { { .3f, .3f, .3f, 1.f } },
                                   {}, VK_ATTACHMENT_LOAD_OP_DONT_CARE, {} );
            cb->set_fullscreen_scissor();
            cb->set_fullscreen_viewport();
            cb->set_depth_bias_enabled( false );
            imgui->render( *cb, false );
            cb->end_render_pass();

            cb->add_image_barrier( gpu.get_current_swapchain_image(), range_color_full(),
                                   { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     VK_ACCESS_2_NONE,
                                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR } );
            cb->flush_barriers();

            cb->pop_marker();
            gpu.gpu_profiler->end_command_buffer( cb );
            cb->end();

            StaticArray<CommandBuffer*, 4> cbs;
            cbs.push( cb );

            GpuSubmitSync sync = gpu.build_present_sync( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT );
            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(), sync.waits.as_span(), sync.signals.as_span() );
            gpu.present();
            gpu.resolve_timestamps();
            gpu.process_pending_resource_deletion();
            gpu_profiler.update( gpu );
        } else {
            ImGui::Render();
        }

        FrameMark;
    }

    vkDeviceWaitIdle( gpu.vulkan_device );

    trainer.destroy_resources( &renderer );

    imgui->shutdown();
    gpu_profiler.shutdown();
    frame_graph.shutdown();
    frame_graph_builder.shutdown();
    rm.shutdown();
    renderer.shutdown();
    window.shutdown();
    MemoryService::instance()->shutdown();

    return 0;
}

#pragma once

#include "foundation/memory.hpp"
#include "graphics/command_buffer.hpp"

struct ImVec2;

namespace raptor {

//
// A single timestamp query, containing indices for the pool, resolved time, name and color.
struct GPUTimeQuery {

    f64                             elapsed_ms;
    u64                             begin;
    u64                             end;

    u16                             start_query_index;  // Used to write timestamp in the query pool
    u16                             end_query_index;    // Used to write timestamp in the query pool

    u16                             parent_index;
    u16                             depth;

    u32                             color;
    CommandQueueType                queue_type;
    u64                             frame_index;

    cstring                         name;
}; // struct GPUTimeQuery

//
// Query tree used mainly per thread-frame to retrieve time data.
struct GpuTimeQueryTree {

    void                            reset();
    void                            set_queries( GPUTimeQuery* time_queries, u32 count );

    GPUTimeQuery*                   push( cstring name );
    GPUTimeQuery*                   pop();

    Span<GPUTimeQuery>              time_queries; // Allocated externally

    u16                             current_time_query   = 0;
    u16                             allocated_time_query = 0;
    u16                             depth                = 0;

}; // struct GpuTimeQueryTree

//
//
struct GpuPipelineStatistics {
    enum Statistics : u8 {
        VerticesCount,
        PrimitiveCount,
        VertexShaderInvocations,
        ClippingInvocations,
        ClippingPrimitives,
        FragmentShaderInvocations,
        ComputeShaderInvocations,
        Count
    };

    void                            reset();

    u64                             statistics[ Count ];
}; // struct GpuPipelineStatistics


//
struct GpuThreadFrameQueryPools {

    VkQueryPool                     vulkan_timestamp_query_pool = nullptr;
    VkQueryPool                     vulkan_pipeline_stats_query_pool = nullptr;

    StaticArray< GpuTimeQueryTree*, CommandBufferManager::k_num_cbs_per_thread> time_queries;

}; // struct 

//
//
struct GpuProfiler {

    void                            init( GpuDevice* gpu_device, Allocator* allocator, u16 queries_per_thread, u16 num_threads, u16 max_frames );
    void                            shutdown();

    void                            begin_command_buffer( CommandBuffer* cb );  // Reset query pools, begin pipeline stats
    void                            end_command_buffer( CommandBuffer* cb );    // End pipeline stats

    void                            push_timestamp( CommandBuffer* gpu_commands, cstring name );
    void                            pop_timestamp( CommandBuffer* gpu_commands );

    GpuThreadFrameQueryPools*       get_thread_frame_pools( u32 frame_index, u32 thread_index, CommandQueueType queue_type );

    u32                             pool_from_indices( u32 frame_index, u32 thread_index, CommandQueueType queue_type  );

    void                            reset_pools( u32 frame_index );
    void                            reset();

    void                            get_query_pool_results( ArenaAllocator* temp_allocator );
    u32                             resolve( u32 current_frame, GPUTimeQuery* timestamps_to_fill );    // Returns the total queries for this frame.

    GpuDevice*                      gpu_device                  = nullptr;
    Array<GpuThreadFrameQueryPools> thread_frame_query_pools;
    Array<GpuTimeQueryTree>         query_trees;

    Allocator*                      allocator                   = nullptr;
    Array<GPUTimeQuery>             timestamps;

    GpuPipelineStatistics           frame_pipeline_statistics;  // Per frame statistics as sum of per-frame ones.

    u32                             queries_per_thread          = 0;
    u32                             queries_per_frame           = 0;
    u32                             num_threads                 = 0;

    u32                             resolved_frame_index = u32_max;
    bool                            resolved_frame_valid = false;
    bool                            current_frame_resolved      = false;    // Used to query the GPU only once per frame if get_gpu_timestamps is called more than once per frame.

}; // struct GpuProfiler

// GpuVisualProfiler //////////////////////////////////////////////////////

//
// Collect per frame queries from GpuProfiler and create a visual representation.
struct GpuVisualProfiler {

    void                        init( Allocator* allocator, f64 gpu_timestamp_frequency, u32 max_frames, u32 max_queries_per_frame );
    void                        shutdown();

    void                        update( GpuDevice& gpu );

    void                        imgui_draw();

    void                        draw_vertical_graph( f64& new_average, i32& hovered_frame,
                                                     const ImVec2& origin, const ImVec2& size,
                                                     f32 row_h, f32 indent_px,
                                                     bool isolate_focused, u64 focused_marker_hash,
                                                     bool& out_clicked_frame, i32& out_clicked_frame_index );

    void                        draw_horizontal_graph( i32 frame_index, const ImVec2& origin, const ImVec2& size,
                                                       f32 row_h, u64 focused_marker_hash, bool isolate_focused );

    void                        reset_focused_marker_stats();
    void                        update_focused_marker_stats( f32 time_ms );

    Allocator*                  allocator;
    Array<GPUTimeQuery>         timestamps;     // Per frame timestamps collected from the profiler.
    Array<u16>                  per_frame_active;
    GpuPipelineStatistics*      pipeline_statistics;    // Per frame collected pipeline statistics.

    f64                         gpu_timestamp_frequency;

    u32                         max_frames;
    u32                         max_queries_per_frame;
    u32                         current_frame;
    u32                         max_visible_depth = 2;

    f32                         max_time;
    f32                         min_time;
    f32                         average_time;

    f32                         max_duration;
    bool                        paused;

    // Selected marker to focus on

    cstring                     focused_marker_name = nullptr;
    u64                         focused_marker_hash = 0;
    static const u32            k_focused_marker_max_samples = 100;
    StaticArray<f32, k_focused_marker_max_samples> focused_marker_times;
    u32                         focused_marker_times_write_index = 0;
    f32                         focused_marker_times_average = 0.f;
    f32                         focused_marker_min_time = 0.f;
    f32                         focused_marker_max_time = 0.f;

}; // struct GPUProfiler

} // namespace raptor

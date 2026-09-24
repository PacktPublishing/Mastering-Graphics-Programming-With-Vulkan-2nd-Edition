#pragma once

namespace raptor {

struct FrameGraph;
struct GpuVisualProfiler;
struct ImageViewDebugger;
struct MemoryService;

enum class DemoUiTab {
    Chapter,
    Scene,
    Renderer,
    GpuProfiler,
    FrameGraph,
    ImageViewer,
    Memory,
    Count
};

struct DemoUi {

    void                set_tools( GpuVisualProfiler* gpu_profiler = nullptr, ImageViewDebugger* image_viewer = nullptr,
                                   FrameGraph* frame_graph = nullptr, MemoryService* memory = nullptr );

    bool                begin( const char* chapter_title, const char* status_text = nullptr );
    void                end();

    bool                begin_outputs();
    void                end_outputs();
    void                common_outputs_ui();

    bool                begin_tab( DemoUiTab tab );
    void                end_tab();

    bool                begin_chapter_tab() { return begin_tab( DemoUiTab::Chapter ); }
    void                end_chapter_tab() { end_tab(); }

    bool                begin_scene_tab() { return begin_tab( DemoUiTab::Scene ); }
    bool                begin_renderer_tab() { return begin_tab( DemoUiTab::Renderer ); }

    void                open_tab( DemoUiTab tab );

    void                set_tab_available( DemoUiTab tab, bool available );

    bool                show_outputs = false;

    enum class Window { None, Raptor, Outputs };

    void                end_window();

    static constexpr int k_tab_count = static_cast< int >( DemoUiTab::Count );

    bool                tab_available[ k_tab_count ] = { true, true, true, true, true, true, true };
    bool                tab_open[ k_tab_count ] = { true, true, true, false, false, false, false };
    DemoUiTab           requested_control_tab = DemoUiTab::Chapter;
    DemoUiTab           requested_output_tab = DemoUiTab::Count;

    Window              active_window   = Window::None;
    bool                focus_outputs   = false;
    bool                window_begun    = false;
    bool                tab_bar_begun   = false;
    bool                tab_begun       = false;

    GpuVisualProfiler*  gpu_profiler    = nullptr;
    ImageViewDebugger*  image_viewer    = nullptr;
    FrameGraph*         frame_graph     = nullptr;
    MemoryService*      memory          = nullptr;
};

} // namespace raptor

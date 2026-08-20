#pragma once

#include "foundation/array.hpp"
#include "foundation/platform.hpp"
#include "foundation/color.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/renderer.hpp"
#include "graphics/gpu_resources.hpp"
#include "graphics/mesh.hpp"

#include "external/glm/gtc/quaternion.hpp"

static const char* kDefault3DModel = "../deps/src/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf";

#define InjectDefault3DModel() \
    if (raptor::file_exists(kDefault3DModel)) {\
        argc = 2;\
        argv[1] = const_cast<char*>(kDefault3DModel);\
    }\
    else {\
       printf("Unable to find default model. Please check the README in the root folder and make sure you've run `python ./bootstrap.py` to download all the additional assets for this project.\n");\
       exit(-1);\
    }

namespace enki { class TaskScheduler; }

namespace raptor {

    struct Allocator;
    struct AsynchronousLoader;
    struct FrameGraph;
    struct GpuVisualProfiler;
    struct ImGuiService;
    struct Renderer;
    struct RenderScene;
    struct SceneGraph;
    struct ArenaAllocator;
    struct GameCamera;
    struct FrameRenderer;
    struct UploadGpuDataContext;

    static const u16    k_invalid_scene_texture_index      = u16_max;
    static const u32    k_material_descriptor_set_index    = 1;
    static const u32    k_max_joint_count                  = 12;

    static const u32    k_num_lights                       = 256;

    static bool         recreate_per_thread_descriptors = false;
    static bool         use_secondary_command_buffers   = false;

    //
    //
    enum DrawFlags {
        DrawFlags_AlphaMask          = 1 << 0,
        DrawFlags_DoubleSided        = 1 << 1,
        DrawFlags_Transparent        = 1 << 2,
        DrawFlags_Phong              = 1 << 3,
        DrawFlags_HasNormals         = 1 << 4,
        DrawFlags_HasTexCoords       = 1 << 5,
        DrawFlags_HasTangents        = 1 << 6,
        DrawFlags_HasJoints          = 1 << 7,
        DrawFlags_HasWeights         = 1 << 8,
        DrawFlags_AlphaDither        = 1 << 9,
        DrawFlags_Cloth              = 1 << 10,
        DrawFlags_PBRSeparateTexture = 1 << 11,
    }; // enum DrawFlags

    struct glTFScene;

    //
    //
    struct PBRMaterial {

        BufferHandle            material_buffer = {};
        DescriptorSetHandle     descriptor_set_transparent = {};
        DescriptorSetHandle     descriptor_set_main = {};

        // Indices used for bindless textures.
        u16                     diffuse_texture_index   = u16_max;
        u16                     metalness_texture_index = u16_max;
        u16                     roughness_texture_index = u16_max;
        u16                     normal_texture_index    = u16_max;
        u16                     occlusion_texture_index = u16_max;
        u16                     emissive_texture_index  = u16_max;
        u16                     alpha_texture_index     = u16_max;

        // PBR
        glm::vec4               base_color_factor   = {1.f, 1.f, 1.f, 1.f};
        glm::vec3               emissive_factor     = {0.f, 0.f, 0.f};

        f32                     metallic            = 1.f;
        f32                     roughness           = 1.f;
        f32                     occlusion           = 0.f;
        f32                     alpha_cutoff        = 1.f;

        u32                     flags               = 0;
    }; // struct PBRMaterial

    //
    //
    struct PhysicsJoint {
        i32                     vertex_index = -1;

        // TODO(marco): for now this is only for cloth
        float                   stifness;
    };

    //
    //
    struct PhysicsVertex {
        void                    add_joint( u32 vertex_index );

        glm::vec3               start_position;
        glm::vec3               previous_position;
        glm::vec3               position;
        glm::vec3               normal;

        glm::vec3               velocity;
        glm::vec3               force;

        PhysicsJoint            joints[ k_max_joint_count ];
        u32                     joint_count;

        float                   mass;
        bool                    fixed;
    };

    //
    //
    struct PhysicsVertexGpuData {
        glm::vec3               position;
        f32                     pad0_;

        glm::vec3               start_position;
        f32                     pad1_;

        glm::vec3               previous_position;
        f32                     pad2_;

        glm::vec3               normal;
        u32                     joint_count;

        glm::vec3               velocity;
        f32                     mass;

        glm::vec3               force;

        // TODO(marco): better storage, values are never greater than 12
        u32                     joints[ k_max_joint_count ];
        u32                     pad3_;
    };

    //
    //
    struct PhysicsMeshGpuData {
        u32                     index_count;
        u32                     vertex_count;

        u32                     padding_[ 2 ];
    };

    //
    //
    struct PhysicsSceneData {
        glm::vec3               wind_direction;
        u32                     reset_simulation;

        f32                     air_density;
        f32                     spring_stiffness;
        f32                     spring_damping;
        f32                     padding_;
    };

    //
    //
    struct PhysicsMesh {
        u32                     mesh_index;

        Array<PhysicsVertex>    vertices;

        BufferHandle            gpu_buffer;
        BufferHandle            draw_indirect_buffer;
        DescriptorSetHandle     descriptor_set;
        DescriptorSetHandle     debug_mesh_descriptor_set;
    };

    //
    //
    struct Bone {
        u32 index = 0;
        glm::vec4 weights = { };
        glm::ivec2 joints = { };
    };

    //
    //
    struct Mesh {

        PBRMaterial             pbr_material;

        PhysicsMesh*            physics_mesh;

        // Vertex data
        BufferHandle            position_buffer;
        u32                     position_count;
        BufferHandle            tangent_buffer;
        BufferHandle            normal_buffer;
        BufferHandle            texcoord_buffer;
        // TODO: separate
        BufferHandle            joints_buffer;
        BufferHandle            weights_buffer;

        u32                     position_offset;
        u32                     tangent_offset;
        u32                     normal_offset;
        u32                     texcoord_offset;
        u32                     joints_offset;
        u32                     weights_offset;

        // Index data
        BufferHandle            index_buffer;
        VkIndexType             index_type;
        u32                     index_offset_bytes;
        u32                     index_count;

        u32                     meshlet_offset;
        u32                     meshlet_count;
        u32                     meshlet_vertex_offset;
        u32                     meshlet_index_count;

        u32                     gpu_mesh_index          = u32_max;
        i32                     skin_index              = i32_max;
        Array<Bone>             temp_bones;

        glm::vec4               bounding_sphere;
        glm::vec3               aabb[ 2 ];

        bool                    has_skinning() const    { return skin_index != i32_max;}
        bool                    is_transparent() const  { return ( pbr_material.flags & ( DrawFlags_AlphaMask | DrawFlags_Transparent ) ) != 0; }
        bool                    is_double_sided() const { return ( pbr_material.flags & DrawFlags_DoubleSided ) == DrawFlags_DoubleSided; }
        bool                    is_cloth() const { return ( pbr_material.flags & DrawFlags_Cloth ) == DrawFlags_Cloth; }
    }; // struct Mesh

    //
    //
    struct alignas( 16 ) GpuMeshlet {

        glm::vec3               center;
        f32                     radius;

        i8                      cone_axis[ 3 ];
        i8                      cone_cutoff;

        u32                     connectivity_data_offset;
        u32                     mesh_index;
        u8                      vertex_count;
        u8                      triangle_count;
        u16                     padding;
    }; // struct GpuMeshlet

    //
    //
    struct MeshletToMeshIndex {
        u32                     mesh_index;
        u32                     primitive_index;
    }; // struct MeshletToMeshIndex

    //
    //
    struct GpuMeshletVertexPosition {

        float                   position[3];
        float                   padding;
    }; // struct GpuMeshletVertexPosition


    //
    //
    struct GpuMeshletVertexData {

        u8                      normal[ 4 ];
        u8                      tangent[ 4 ];
        u16                     uv_coords[ 2 ];
        float                   padding;
    }; // struct GpuMeshletVertexData

    //
    //
    struct alignas( 16 ) GpuMaterialData {

        u32                     textures[ 4 ]; // diffuse, roughness, normal, occlusion
        // PBR
        glm::vec4               emissive; // emissive_color_factor + emissive texture index
        glm::vec4               base_color_factor;
        glm::vec4               metallic_roughness_occlusion_factor; // metallic, roughness, occlusion

        u32                     flags;
        f32                     alpha_cutoff;
        u32                     vertex_offset;
        u32                     mesh_index;

        u32                     meshlet_offset;
        u32                     meshlet_count;
        u32                     meshlet_index_count;
        u32                     alpha_texture_index;

        VkDeviceAddress         position_buffer;
        VkDeviceAddress         uv_buffer;
        VkDeviceAddress         index_buffer;
        VkDeviceAddress         normals_buffer;

        u32                     position_buffer_index;
        u32                     normal_buffer_index;
        u32                     uv0_buffer_index;
        u32                     index_buffer_index;

        u32                     position_buffer_offset;
        u32                     normal_buffer_offset;
        u32                     uv0_buffer_offset;
        u32                     index_buffer_offset;

        u32                     vertex_count;
        u32                     _pad[ 3 ];

    }; // struct GpuMaterialData

    //
    //
    struct alignas( 16 ) GpuMeshInstanceData {
        glm::mat4               world;
       // glm::mat4               inverse_world;

        u32                     mesh_index;
        u32                     pad000;
        u32                     pad001;
        u32                     pad002;
    }; // struct GpuMeshInstanceData

    //
    //
    struct alignas( 16 ) GpuMeshDrawCommand {
        u32                     drawId;
        u32                     taskOffset;
        VkDrawIndexedIndirectCommand indirect;          // 5 u32
        VkDrawMeshTasksIndirectCommandEXT indirectMS;    // 3 u32
    }; // struct GpuMeshDrawCommand

    //
    //
    struct alignas( 16 ) GpuMeshDrawCounts {
        u32                     opaque_mesh_visible_count;
        u32                     opaque_mesh_culled_count;
        u32                     transparent_mesh_visible_count;
        u32                     transparent_mesh_culled_count;

        u32                     total_count;
        u32                     depth_pyramid_texture_index;
        u32                     late_flag;
        u32                     meshlet_index_count;

        u32                     dispatch_task_x;
        u32                     dispatch_task_y;
        u32                     dispatch_task_z;
        u32                     pad001;

    }; // struct GpuMeshDrawCounts

    //
    //
    struct RenderItem {
        MeshInstance*           mesh_instance;
    }; // struct RenderItem


    //
    //
    struct RenderView {
        RenderScene*            scene;

        Array<RenderItem>       opaque_items;
        Array<RenderItem>       transparent_items;
    }; // struct RenderView

    // Animation structs //////////////////////////////////////////////////
    //
    //

    // Transform //////////////////////////////////////////////////////////
    //
    struct Transform {

        glm::vec3               scale;
        glm::quat               rotation;
        glm::vec3               translation;

        void                    reset();
        glm::mat4               calculate_matrix() const;

    }; // struct Transform

    struct AnimationChannel {

        enum TargetType {
            Invalid, Translation, Rotation, Scale, Weights, Count
        };

        i32                     sampler;
        i32                     target_node;
        TargetType              target_type;

    }; // struct AnimationChannel

    struct AnimationSampler {

        enum Interpolation {
            Linear, Step, CubicSpline, Count
        };

        Array<f32>              key_frames;
        glm::vec4*              data;       // Aligned-allocated data. Count is the same as key_frames.
        Interpolation           interpolation_type;

    }; // struct AnimationSampler

    //
    //
    struct Animation {

        f32                     time_start;
        f32                     time_end;

        Array<AnimationChannel> channels;
        Array<AnimationSampler> samplers;

        Array<Transform>        animated_transforms;

    }; // struct Animation

    //
    //
    struct AnimationInstance {
        Animation*              animation;
        f32                     current_time;
    }; // struct AnimationInstance

    //
    //
    struct AnimationViewer {

        void                    init( Allocator* allocator );
        void                    shutdown();

        void                    draw_imgui( Span<Animation> animations, SceneGraph& scene_graph, f32 animation_global_time );

        FlatHashMap<u32, bool>  channel_collapsed_state;
    };

    // Skinning ///////////////////////////////////////////////////////////
    //
    //
    struct Skin {

        u32                     skeleton_root_index;
        Array<i32>              joints;
        Array<glm::mat4>        inverse_bind_matrices;  // Align-allocated data. Count is same as joints.

        BufferHandle            joint_transforms[ k_max_frames ];

    }; // struct Skin

    struct AnimatedMesh {
        u32                     mesh_index;
        Array<u32>              animations;
    };

    // Light //////////////////////////////////////////////////////////////

    //
    struct alignas( 16 ) Light {

        glm::vec3               world_position;
        f32                     radius;

        glm::vec3               color;
        f32                     intensity;

        glm::vec4               aabb_min;
        glm::vec4               aabb_max;

        f32                     shadow_map_resolution;
        u32                     tile_x;
        u32                     tile_y;
        f32                     solid_angle;

    }; // struct Light

    // Separated from Light struct as it could contain unpacked data.
    struct alignas( 16 ) GpuLight {

        glm::vec3               world_position;
        f32                     radius;

        glm::vec3               color;
        f32                     intensity;

        f32                     shadow_map_resolution;
        f32                     rcp_n_minus_f;          // Calculation of 1 / (n - f) used to retrieve cubemap shadows depth value.
        f32                     pad1;
        f32                     pad2;

    }; // struct GpuLight

    // Render Passes //////////////////////////////////////////////////////

    //
    //
    struct MeshBuffers {
        Array<glm::vec3>        vertices;
        Array<glm::vec3>        normals;
        Array<glm::vec2>        tex_coords;
        Array<glm::vec4>        tangents;
        // TODO(marco): we might be wasting 3/4 of memory here if only one weight is used
        Array<glm::u16vec4>     joints;
        Array<glm::vec4>        weights;
        Array<u32>              indices; // For U16, each entry contains two indices.
    }; // struct MeshBuffers

    //
    //
    struct RenderModel
    {
        virtual                 ~RenderModel() { };

        virtual void            init( RenderScene* render_scene, SceneGraph* scene_graph, Allocator* resident_allocator, Renderer* renderer_ ) = 0;
        virtual void            load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) = 0;
        virtual void            add_animation( cstring filename, cstring path, ArenaAllocator* temp_allocator ) {
            RASSERT( false );
        }
        virtual void            shutdown( Renderer* renderer ) = 0;

        void                    build_meshlets( Mesh& mesh, Allocator* temp_allocator );

        Array<BufferHandle>     buffers;

        Allocator*              resident_allocator;
        Renderer*               renderer;
        SceneGraph*             scene_graph;
        RenderScene*            render_scene;

        glm::vec3                   mesh_aabb[2]; // 0 min, 1 max
    }; // struct RenderModel

    //
    //
    struct RenderScene final {

        void                    init( SceneGraph* scene_graph, Allocator* resident_allocator, Renderer* renderer_ );
        RenderModel*            add_and_load_model( char* filename, cstring path, ArenaAllocator* temp_allocator );
        void                    add_animation( char* filename, cstring path, ArenaAllocator* temp_allocator );
        void                    prepare_draws( Renderer* renderer, ArenaAllocator* scratch_allocator, SceneGraph* scene_graph );
        void                    shutdown( Renderer* renderer );

        // Geometry data
        void                    destroy_mesh_buffers();

        u32                     write_vertices_to_staging_buffer( glm::vec3* vertices, u32 vertex_count );
        u32                     write_indices_to_staging_buffer( u32* indices, u32 index_count );
        u32                     write_normals_to_staging_buffer( glm::vec3* normals, u32 normal_count );
        u32                     write_tangents_to_staging_buffer( glm::vec3* tangents, u32 tangent_count );
        u32                     write_tangents_to_staging_buffer( glm::vec4* tangents, u32 tangent_count );
        u32                     write_texcoords_to_staging_buffer( glm::vec2* texcoords, u32 texcoord_count );
        u32                     write_texcoords_to_staging_buffer( glm::vec3* texcoords, u32 texcoord_count, bool flip_y );
        u32                     write_joints_to_staging_buffer( glm::u16vec4* joints, u32 joint_count );
        u32                     write_joints_to_staging_buffer( u16* joints, u32 joint_count );
        u32                     write_weights_to_staging_buffer( glm::vec4* weights, u32 weight_count );

        void                    on_resize( GpuDevice& gpu, FrameGraph* frame_graph, u32 new_width, u32 new_height );

        CommandBuffer*          update_physics( f32 delta_time, f32 air_density, f32 spring_stiffness, f32 spring_damping, glm::vec3 wind_direction, bool reset_simulation );
        void                    update_animations( f32 delta_time );
        void                    update_joints( FrameRenderer* frame_renderer );

        glm::mat4               get_local_matrix( SceneGraph* scene_graph, u32 node_index );
        glm::mat4               get_node_transform( SceneGraph* scene_graph, u32 node_index );

        void                    draw_mesh_instance( CommandBuffer* gpu_commands, MeshInstance& mesh_instance, bool transparent );

        Array<RenderModel*>     models;

        // All graphics resources used by the scene
        Array<TextureResource>  images;
        Array<SamplerResource>  samplers;

        // Mesh and MeshInstances
        Array<Mesh>             meshes;
        Array<MeshInstance>     mesh_instances;
        Array<AnimatedMesh>     animated_meshes; // TODO(marco)
        MeshBuffers             mesh_buffers;

        // Meshlet data
        Array<GpuMeshlet>       meshlets;
        Array<GpuMeshletVertexPosition> meshlets_vertex_positions;
        Array<GpuMeshletVertexData> meshlets_vertex_data;
        Array<u32>              meshlets_position_only_data;
        Array<u32>              meshlets_connectivity_data;
        u32                     meshlets_index_count;

        // Animation and skinning data
        Array<Animation>        animations;
        Array<Skin>             skins;
        f32                     current_animation_time = 0.f;

        // Lights
        Array<Light>            lights;
        u32                     active_lights   = 1;
        bool                    shadow_constants_cpu_update = true;

        StringBuffer            names_buffer;   // Buffer containing all names of nodes, resources, etc.

        SceneGraph*             scene_graph;

        GpuMeshDrawCounts       mesh_draw_counts;

        Allocator*              resident_allocator;
        Renderer*               renderer;

    }; // struct RenderScene

} // namespace raptor

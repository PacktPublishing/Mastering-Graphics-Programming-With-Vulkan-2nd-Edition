
#if defined (RAYGEN_PROBE_RT_SLANG) || defined (CLOSEST_HIT_PROBE_RT_SLANG) || defined (MISS_PROBE_RT_SLANG)


struct RayPayload {
    float3      radiance;
    float       distance;
};


//layout( set = MATERIAL_SET, binding = 26 ) uniform accelerationStructureEXT as;
RaytracingAccelerationStructure as : register(t26, space1);

// TODO: put this in a common header.
struct Light {
    float3          world_position;
    float           radius;

    float3          color;
    float           intensity;

    float           shadow_map_resolution;
    float           padding_l_000;
    float           padding_l_001;
    float           padding_l_002;
};

StructuredBuffer<Light> lights : register(t21, space1);
StructuredBuffer<uint> probe_status : register(t43, space1);

#endif // #if defined (RAYGEN_PROBE_RT_SLANG) || defined (CLOSEST_HIT_PROBE_RT_SLANG) || defined (MISS_PROBE_RT_SLANG)


#if defined (RAYGEN_PROBE_RT_SLANG)

#include "platform.slangh"
#include "frame.slangh"
#include "mesh.slangh"
#include "ddgi.slangh"


[shader("raygeneration")]
void main() {
    uint3 pixel_coord = DispatchRaysIndex();

    const int probe_index = pixel_coord.y + ddgi.probe_update_offset;
    const int ray_index = pixel_coord.x;

    const bool skip_probe = (probe_status[probe_index] == PROBE_STATUS_OFF) || (probe_status[probe_index] == PROBE_STATUS_UNINITIALIZED);
    if ( use_probe_status() && skip_probe ) {
        return;
    }

    const int probe_counts = ddgi.probe_counts.x * ddgi.probe_counts.y * ddgi.probe_counts.z;
    if (probe_index >= probe_counts) {
        return;
    }

    int3 probe_grid_indices = probe_index_to_grid_indices( probe_index );
    float3 ray_origin = grid_indices_to_world( probe_grid_indices, probe_index );
    float3 direction = normalize( mul(float3x3(ddgi.random_rotation), spherical_fibonacci(ray_index, ddgi.probe_rays)) );

    RayPayload payload;
    payload.radiance = float3(0);
    payload.distance = 0;

    uint rayFlags = RAY_FLAG_FORCE_OPAQUE;

    RayDesc ray_desc;
    ray_desc.Origin = ray_origin;
    ray_desc.Direction = direction;
    ray_desc.TMin = 0.0f;
    ray_desc.TMax = 100.0f;

    TraceRay(as, rayFlags, 0xFF, 0, 0, 0, ray_desc, payload );

    //imageStore(global_images_2d[ radiance_output_index ], ivec2(ray_index, probe_index), vec4(payload.radiance, payload.distance));
    global_images_2d[ddgi.radiance_output_index][int2(ray_index, probe_index)] = float4(payload.radiance, payload.distance);
}

#endif // #if defined (RAYGEN_PROBE_RT_SLANG)

#if defined (CLOSEST_HIT_PROBE_RT_SLANG)

#include "platform.slangh"
#include "frame.slangh"
#include "mesh.slangh"
#include "ddgi.slangh"

float attenuation_square_falloff(float3 position_to_light, float light_inverse_radius) {
    const float distance_square = dot(position_to_light, position_to_light);
    const float factor = distance_square * light_inverse_radius * light_inverse_radius;
    const float smoothFactor = max(1.0 - factor * factor, 0.0);
    return (smoothFactor * smoothFactor) / max(distance_square, 1e-4);
}

[shader("closesthit")]
void main(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {

    float3 radiance = float3(0);
    float distance = 0.0f;

    if (HitKind() == HIT_KIND_TRIANGLE_BACK_FACE) {
        // Track backfacing rays with negative distance
        distance = RayTMin() + RayTCurrent();
        distance *= -0.2;
    }
    else {
        MeshInstanceDraw mesh_instance_draw = mesh_instance_draws[ GeometryIndex() ];
        uint mesh_index = mesh_instance_draw.mesh_draw_index;
        MeshDraw mesh = mesh_draws[ mesh_index ];

        // Load indices manually
        uint index_size_in_bytes = 2;
        uint indices_per_triangle = 3;
        uint triangle_index_stride = indices_per_triangle * index_size_in_bytes;
        uint base_index = PrimitiveIndex() * triangle_index_stride;

        ByteAddressBuffer index_buffer = buffer_get(mesh.index_buffer_index);

        uint3 indices = Load3x16BitIndices( index_buffer, mesh.index_buffer_offset + base_index);

        // Load positions
        ByteAddressBuffer position_buffer = buffer_get(mesh.position_buffer_index);
        uint position_size_in_bytes = 12;
        float3 position0 = position_buffer.Load<float3>( mesh.position_buffer_offset + (indices.x) * position_size_in_bytes);
        float3 position1 = position_buffer.Load<float3>( mesh.position_buffer_offset + (indices.y) * position_size_in_bytes);
        float3 position2 = position_buffer.Load<float3>( mesh.position_buffer_offset + (indices.z) * position_size_in_bytes);

        const float4x4 transform = mesh_instance_draw.model;
        float4 p0_world = mul(transform, float4(position0, 1));
        float4 p1_world = mul(transform, float4(position1, 1));
        float4 p2_world = mul(transform, float4(position2, 1));

        // Calculate barycentrics
        float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);

        // Load uv
        ByteAddressBuffer uv_buffer = buffer_get(mesh.uv0_buffer_index);
        uint uv_size_in_bytes = 8;
        float2 uv0 = uv_buffer.Load<float2>( mesh.uv0_buffer_offset + (indices.x * uv_size_in_bytes));
        float2 uv1 = uv_buffer.Load<float2>( mesh.uv0_buffer_offset + (indices.y * uv_size_in_bytes));
        float2 uv2 = uv_buffer.Load<float2>( mesh.uv0_buffer_offset + (indices.z * uv_size_in_bytes));

        float2 uv = ( barycentrics.x * uv0 + barycentrics.y * uv1 + barycentrics.z * uv2 );

        // Load normals
        ByteAddressBuffer normal_buffer = buffer_get(mesh.normal_buffer_index);
        uint normals_size_in_bytes = 12;
        float3 normal0 = normal_buffer.Load<float3>( mesh.normal_buffer_offset + (indices.x * normals_size_in_bytes ));
        float3 normal1 = normal_buffer.Load<float3>( mesh.normal_buffer_offset + (indices.y * normals_size_in_bytes ));
        float3 normal2 = normal_buffer.Load<float3>( mesh.normal_buffer_offset + (indices.z * normals_size_in_bytes ));

        float3 vertex_normal = ( barycentrics.x * normal0 + barycentrics.y * normal1 + barycentrics.z * normal2 );

        vertex_normal = mul( (float3x3)mesh_instance_draw.model_inverse, vertex_normal );

        float3 albedo = texture_2d(mesh.textures.x).SampleLevel(g_linear_repeat(), uv, 3).rgb;

        const float3 world_position = barycentrics.x * p0_world.xyz + barycentrics.y * p1_world.xyz + barycentrics.z * p2_world.xyz;

        // TODO: calculate lighting.
        Light light = lights[ 0 ];

        const float3 position_to_light = light.world_position - world_position;
        const float3 l = normalize( position_to_light );
        const float NoL = clamp(dot(vertex_normal, l), 0.0, 1.0);

        float attenuation = attenuation_square_falloff(position_to_light, 1.0f / light.radius);

        float3 light_intensity = float3(0.0f);
        if ( attenuation > 0.001f && NoL > 0.001f ) {
            light_intensity += ( light.intensity * attenuation * NoL ) * light.color;
        }

        float3 diffuse = albedo * light_intensity;

        // infinite bounces
        if ( use_infinite_bounces() ) {
            diffuse += albedo * sample_irradiance( world_position, vertex_normal, frame.camera_position.xyz ) * ddgi.infinite_bounces_multiplier;
        }

        radiance = diffuse;
        distance = RayTMin() + RayTCurrent();
    }

    payload.radiance = radiance;
    payload.distance = distance;
}

#endif // #if defined (CLOSEST_HIT_PROBE_RT_SLANG)


#if defined (MISS_PROBE_RT_SLANG)

[shader("miss")]
void main(inout RayPayload payload) {
    payload.radiance = float3( 0.529, 0.807, 0.921 );
    payload.distance = 1000.0f;
}

#endif // #if defined (MISS_PROBE_RT_SLANG)



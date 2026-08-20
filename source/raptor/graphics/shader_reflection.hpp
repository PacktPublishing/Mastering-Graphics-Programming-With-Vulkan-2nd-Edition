#pragma once

#include "foundation/static_array.hpp"
#include "foundation/static_string.hpp"

#include "graphics/gpu_resources.hpp"

namespace raptor {

//
struct SpecializationConstant {

    enum class Type : u16 {
        Type_i32 = 0,
        Type_u32,
        Type_f32,
        Type_count
    }; // enum Type

    union Value {

        i32             value_i;
        u32             value_u;
        f32             value_f;
    }; // union Value

    Value               value;
    Type                type;

    u16                 binding = 0;
    u32                 byte_stride = 0;
}; // struct SpecializationConstant

//
struct DescriptorBinding2 {
    VkDescriptorType    type;
    u32                 index = 0;
    u32                 count = 1;

    cstring             name;
}; // struct DescriptorBinding

//
struct DescriptorSetReflection {
    StaticArray<DescriptorBinding2, k_max_descriptors_per_set> bindings;
    u32                 set_index;
    u32                 padding;
}; // struct DescriptorSetReflection

//
struct ShaderReflection {
    StaticArray<DescriptorSetReflection, k_max_descriptor_set_layouts>  sets;
    StaticArray<SpecializationConstant, k_max_specialization_constants> specialization_constants;
    StaticArray<StaticString64, k_max_specialization_constants>       specialization_names;

    u32                 push_constants_stride   = 0;
    u32                 add_global_set          = 0;

    cstring             name                    = nullptr;
}; // struct ShaderReflection

} // namespace raptor

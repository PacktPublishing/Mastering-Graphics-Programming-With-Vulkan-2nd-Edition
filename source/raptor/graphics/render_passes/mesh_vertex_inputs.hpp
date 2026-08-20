#pragma once

#include "graphics/gpu_resources.hpp"

namespace raptor {

inline const VertexInputCreation vi_depth_pre = {
    .bindings = {
        { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT }, // position
    }
};

inline const VertexInputCreation vi_depth_pre_skinning = {
    .bindings = {
        { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX },
        { 2, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 3,  8, VK_VERTEX_INPUT_RATE_VERTEX },
        { 4,  8, VK_VERTEX_INPUT_RATE_VERTEX },
        { 5, 16, VK_VERTEX_INPUT_RATE_VERTEX },
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT },      // position
        { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT },   // tangent
        { 2, 2, VK_FORMAT_R32G32B32_SFLOAT },      // normal
        { 3, 3, VK_FORMAT_R32G32_SFLOAT },         // texcoord
        { 4, 4, VK_FORMAT_R16G16B16A16_SINT },     // joints (Short4)
        { 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT },   // weights
    }
};

inline const VertexInputCreation vi_gbuffer = {
    .bindings = {
        { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX },
        { 2, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 3,  8, VK_VERTEX_INPUT_RATE_VERTEX },
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT },      // position
        { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT },   // tangent
        { 2, 2, VK_FORMAT_R32G32B32_SFLOAT },      // normal
        { 3, 3, VK_FORMAT_R32G32_SFLOAT },         // texcoord
    }
};

inline const VertexInputCreation vi_gbuffer_skinning = {
    .bindings = {
        { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX },
        { 2, 12, VK_VERTEX_INPUT_RATE_VERTEX },
        { 3,  8, VK_VERTEX_INPUT_RATE_VERTEX },
        { 4,  8, VK_VERTEX_INPUT_RATE_VERTEX },
        { 5, 16, VK_VERTEX_INPUT_RATE_VERTEX },
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT },      // position
        { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT },   // tangent
        { 2, 2, VK_FORMAT_R32G32B32_SFLOAT },      // normal
        { 3, 3, VK_FORMAT_R32G32_SFLOAT },         // texcoord
        { 4, 4, VK_FORMAT_R16G16B16A16_UINT },     // joints (Short4)
        { 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT },   // weights
    }
};

} // namespace raptor

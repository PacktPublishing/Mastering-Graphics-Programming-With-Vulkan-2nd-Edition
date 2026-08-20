
#pragma once

#include "foundation/platform.hpp"

namespace raptor {

    // Index and generation handle, based on talk by Seb Aaltonen
    // https://enginearchitecture.realtimerendering.com/downloads/reac2023_modern_mobile_rendering_at_hypehype.pdf
    // 
    // Template argument is only to be strongly typed.
    template<typename T>
    struct Handle {

        static constexpr u32               k_invalid_generation    = 0;
        static constexpr u16               k_invalid_index         = 0xFFFFu;
        static constexpr u32               k_invalid_handle_id     = 0xFFFFFFFFu;

        Handle() : id( k_invalid_handle_id ) {}

        Handle( u16 index, u16 generation ) {
            RASSERT( index != k_invalid_index );
            RASSERT( generation != k_invalid_generation );
            id = ( ( (u32)generation ) << 16 ) | index;
        }

        u16                         index() const { return (u16)( id & 0xFFFFu ); }
        u16                         generation() const { return (u16)( ( id >> 16 ) & 0xFFFFu ); }

        bool                        is_valid() const    { return id != k_invalid_handle_id; }
        bool                        is_invalid() const  { return id == k_invalid_handle_id; }

        bool                        operator== ( const Handle<T>& other ) const { return id == other.id; }
        bool                        operator!= ( const Handle<T>& other ) const { return id != other.id; }

        u32                         id;
    }; // struct Handle


} // namespace idra

#pragma once

#include "foundation/array.hpp"
#include "foundation/memory.hpp"
#include "foundation/pool_handle.hpp"
#include "foundation/bit.hpp"

namespace raptor {

    //
    // Pool struct - fixed size pool with generational handles, free indices stack and active elements bitset.
    template<typename T, typename HandleType>
    struct Pool {
        void                init( Allocator* allocator, u32 initial_size );
        void                shutdown();

        HandleType          create( const T& value );
        HandleType          obtain();               // crea vuoto
        void                destroy( HandleType h );

        T*                  get( HandleType h );
        const T*            get( HandleType h ) const;

        Allocator*          allocator = nullptr;

        Array<T>            elements;
        Array<u16>          generations;
        Array<u16>          free_indices;
        BitSet              active_elements;

        u32                 size = 0;
        u32                 free_head = 0;
    }; // struct Pool

    // Implementation /////////////////////////////////////////////////////
    template<typename T, typename HandleType>
    void Pool<T, HandleType>::init( Allocator* allocator_, u32 initial_size ) {
        allocator = allocator_;
        size = initial_size;

        elements.init( allocator, initial_size, size );
        generations.init( allocator, initial_size, size );
        free_indices.init( allocator, initial_size, size );
        active_elements.init( allocator, size );

        free_head = 0;
        for ( u32 i = 0; i < size; i++ ) {
            free_indices[ i ] = i;
            generations[ i ] = 1;   // start from generation 1 (0 = invalid handle)
        }
    }

    template<typename T, typename HandleType>
    void Pool<T, HandleType>::shutdown() {
        if ( free_head != size ) {
            for ( u32 i = 0; i < size; i++ ) {
                
                if ( active_elements.get_bit( i ) ) {
                    rprint( "Pool element leaked: index, %u, generation %u\n", i, generations[ i ] );
                }
            }
        }

        elements.shutdown();
        generations.shutdown();
        free_indices.shutdown();
        active_elements.shutdown();
    }

    template<typename T, typename HandleType>
    HandleType Pool<T, HandleType>::create( const T& value ) {
        if ( free_head < size ) {
            u16 idx = free_indices[ free_head++ ];
            elements[ idx ] = value;
            active_elements.set_bit( idx );

            return { idx, generations[ idx ] };
        }
        RASSERT( false && "Pool exhausted!" );
        return HandleType();
    }

    template<typename T, typename HandleType>
    HandleType Pool<T, HandleType>::obtain() {
        if ( free_head < size ) {
            u16 idx = free_indices[ free_head++ ];
            active_elements.set_bit( idx );

            return { idx, generations[ idx ] };
        }
        RASSERT( false && "Pool exhausted!" );
        return HandleType();
    }

    template<typename T, typename HandleType>
    void Pool<T, HandleType>::destroy( HandleType h ) {
        u16 idx = h.index();
        if ( h.generation() != generations[ idx ] ) {
            return; // invalid handle
        }

        active_elements.clear_bit( idx );
        generations[ idx ]++; // bump gen, invalidating old handles
        free_indices[ --free_head ] = idx;
    }

    template<typename T, typename HandleType>
    T* Pool<T, HandleType>::get( HandleType h ) {
        if ( h.is_invalid() ) {
            return nullptr;
        }

        if ( h.generation() != generations[ h.index() ] ) {
            return nullptr;
        }
        return &elements[ h.index() ];
    }

    template<typename T, typename HandleType>
    const T* Pool<T, HandleType>::get( HandleType h ) const {
        if ( h.is_invalid() ) {
            return nullptr;
        }

        if ( h.generation() != generations[ h.index() ] ) {
            return nullptr;
        }
        return &elements[ h.index() ];
    }


} // namespace idra
#pragma once

#include "foundation/platform.hpp"
#include "foundation/service.hpp"

#define RAPTOR_IMGUI

namespace raptor {

    #define rkilo(size)             (size * 1024)
    #define rmega(size)             (size * 1024 * 1024)
    #define rgiga(size)             (size * 1024 * 1024 * 1024)

    static const sizet              k_thread_allocator_default_size = rkilo( 128 );

    // Memory Methods /////////////////////////////////////////////////////
    void            memory_copy( void* destination, void* source, sizet size );

    //
    //  Calculate aligned memory size.
    sizet           memory_align( sizet size, sizet alignment );

    // Memory Structs /////////////////////////////////////////////////////
    //
    //
    struct MemoryStatistics {
        sizet                       allocated_bytes;
        sizet                       total_bytes;

        u32                         allocation_count;

        void add( sizet a ) {
            if ( a ) {
                allocated_bytes += a;
                ++allocation_count;
            }
        }
    }; // struct MemoryStatistics

    //
    //
    struct Allocator {
        virtual ~Allocator() { }
        virtual void*               allocate( sizet size, sizet alignment ) = 0;
        virtual void*               allocate( sizet size, sizet alignment, cstring file, i32 line ) = 0;

        virtual void                deallocate( void* pointer ) = 0;
    }; // struct Allocator


    //
    //
    struct HeapAllocator : public Allocator {

        ~HeapAllocator() override;

        void                        init( sizet size );
        void                        shutdown();

#if defined RAPTOR_IMGUI
        void                        debug_ui();
#endif // RAPTOR_IMGUI

        void*                       allocate( sizet size, sizet alignment ) override;
        void*                       allocate( sizet size, sizet alignment, cstring file, i32 line ) override;

        void                        deallocate( void* pointer ) override;

        void*                       tlsf_handle;
        void*                       memory;
        sizet                       allocated_size = 0;
        sizet                       max_size = 0;
        
    }; // struct HeapAllocator

    //
    //
    struct ArenaAllocator : public Allocator {

        void                        init( sizet size );
        void                        shutdown();

        void*                       allocate( sizet size, sizet alignment ) override;
        void*                       allocate( sizet size, sizet alignment, cstring file, i32 line ) override;

        void                        deallocate( void* pointer ) override;

        sizet                       get_marker();
        void                        free_marker( sizet marker );

        void                        clear();

        u8*                         memory          = nullptr;
        sizet                       total_size      = 0;
        sizet                       allocated_size  = 0;
        sizet                       peak_used       = 0;

    }; // struct ArenaAllocator

    //
    struct ArenaScope {
        
        ArenaScope( ArenaAllocator* alloc ) : allocator( alloc ), marker( alloc->get_marker() ) { }
        ~ArenaScope()               { allocator->free_marker( marker ); }

        ArenaAllocator*             allocator;
        sizet                       marker;

    }; // struct ArenaScope

    //
    // DANGER: this should be used for NON runtime processes, like compilation of resources.
    struct MallocAllocator : public Allocator {
        void*                       allocate( sizet size, sizet alignment ) override;
        void*                       allocate( sizet size, sizet alignment, cstring file, i32 line ) override;

        void                        deallocate( void* pointer ) override;
    };

    // Memory Service /////////////////////////////////////////////////////
    // 
    // 
    struct MemoryServiceConfiguration {

        sizet                       maximum_dynamic_size = rmega( 32 );    // Defaults to max 32MB of dynamic memory.
        sizet                       thread_temp_allocator_size = k_thread_allocator_default_size;
    }; // struct MemoryServiceConfiguration
    //
    //
    struct MemoryService : public Service {

        RAPTOR_DECLARE_SERVICE( MemoryService );

        void                        init( void* configuration );
        void                        shutdown();

#if defined RAPTOR_IMGUI
        void                        imgui_draw();
#endif // RAPTOR_IMGUI

        void                        set_thread_temp_allocator( ArenaAllocator* allocator );
        ArenaAllocator*             get_thread_allocator();

        HeapAllocator               system_allocator;
        ArenaAllocator*             temp_thread_allocators;

        //
        // Test allocators.
        void                        test();

        sizet                       thread_temp_allocator_size = k_thread_allocator_default_size;

        static constexpr cstring    k_name = "raptor_memory_service";

    }; // struct MemoryService
    
    // Macro helpers //////////////////////////////////////////////////////

    // Allocation
    #define ralloca(size, allocator) \
        ((allocator)->allocate( (size), 1, __FILE__, __LINE__ ))

    // Allocation to memory type
    #define rallocam(size, allocator) \
        ((u8*)((allocator)->allocate( (size), 1, __FILE__, __LINE__ )))

    // Allocation casted to type
    #define rallocat(type, allocator) \
        ((type*)((allocator)->allocate( sizeof(type), 1, __FILE__, __LINE__ )))

    // Placement new with allocator
    #define rnew(type, allocator) \
        (new ((allocator)->allocate( sizeof(type), 1, __FILE__, __LINE__ )) type)

    // Placement new with allocator and alignment
    #define rnewa(type, allocator, alignment) \
        (new ((allocator)->allocate( sizeof(type), (alignment), __FILE__, __LINE__ )) type)

    // Allocation with alignment
    #define rallocaa(size, allocator, alignment) \
        ((allocator)->allocate( (size), (alignment), __FILE__, __LINE__ ))

    #define rdelete(pointer, type, allocator) \
        if ( pointer ) { \
            (pointer)->~type(); \
            ((allocator)->deallocate( pointer )); \
            pointer = nullptr; } \

    #define rfree(pointer, allocator) (allocator)->deallocate(pointer)

} // namespace raptor

#include "memory.hpp"
#include "memory_utils.hpp"
#include "assert.hpp"
#include "numerics.hpp"

#include "external/tlsf.h"

#include <stdlib.h>
#include <memory.h>

#if defined RAPTOR_IMGUI
#include "external/imgui/imgui.h"
#endif // RAPTOR_IMGUI

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

// Define this and add StackWalker to heavy memory profile
//#define RAPTOR_MEMORY_STACK

//
#define HEAP_ALLOCATOR_STATS

#if defined (RAPTOR_MEMORY_STACK)
#include "external/StackWalker.h"
#endif // RAPTOR_MEMORY_STACK

namespace raptor {

//#define RAPTOR_MEMORY_DEBUG
#if defined (RAPTOR_MEMORY_DEBUG)
    #define r_mem_assert(cond) RASSERT(cond)
#else
    #define r_mem_assert(cond)
#endif // RAPTOR_MEMORY_DEBUG

// Memory Service /////////////////////////////////////////////////////////
static MemoryService    s_memory_service;

// Locals
static size_t s_size = rmega(32) + tlsf_size() + 8;


struct TLSContext {
#if defined(_WIN32)
    DWORD       tls_index   = TLS_OUT_OF_INDEXES;
#else
    pthread_key_t tls_key;
    bool        tls_created = false;
#endif
}; // struct TLSContext

static TLSContext       s_tls_context;

//
// Walker methods
static void exit_walker( void* ptr, size_t size, int used, void* user );
static void imgui_walker( void* ptr, size_t size, int used, void* user );

MemoryService* MemoryService::instance() {
    return &s_memory_service;
}

//
//
void MemoryService::init( void* configuration ) {

    rprint( "Memory Service Init\n" );
    MemoryServiceConfiguration* memory_configuration = static_cast< MemoryServiceConfiguration* >( configuration );
    system_allocator.init( memory_configuration ? memory_configuration->maximum_dynamic_size : s_size );

#if defined(_WIN32)
    s_tls_context.tls_index = TlsAlloc();
#else
    s_tls_context.pthread_key_create( &s_tls_context.tls_key, nullptr );
    s_tls_context.tls_created = true;
#endif // WIN32
}

void MemoryService::shutdown() {

    system_allocator.shutdown();

#if defined(_WIN32)
    if ( s_tls_context.tls_index != TLS_OUT_OF_INDEXES )
        TlsFree( s_tls_context.tls_index );
    s_tls_context.tls_index = TLS_OUT_OF_INDEXES;
#else
    if ( s_tls_context.tls_created ) {
        pthread_key_delete( s_tls_context.tls_key );
        s_tls_context.tls_created = false;
    }
#endif // WIN32

    rprint( "Memory Service Shutdown\n" );
}

void MemoryService::set_thread_temp_allocator( ArenaAllocator* allocator ) {
#if defined(_WIN32)
    TlsSetValue( s_tls_context.tls_index, allocator );
#else
    pthread_setspecific( s_tls_context.tls_key, allocator );
#endif
}

// Get per thread allocator
ArenaAllocator* MemoryService::get_thread_allocator() {
    
#if defined(_WIN32)
    ArenaAllocator* allocator = (ArenaAllocator*)TlsGetValue( s_tls_context.tls_index );
#else
    ArenaAllocator* allocator = (ArenaAllocator*)pthread_getspecific( s_tls_context.tls_key );
#endif

    if ( allocator ) {
        return allocator;
    }

    // Lazy init of per-thread allocator
    static thread_local ArenaAllocator s_tls_temp;
    s_tls_temp.init( thread_temp_allocator_size );
    set_thread_temp_allocator( &s_tls_temp );

    return &s_tls_temp;
}

void exit_walker( void* ptr, size_t size, int used, void* user ) {
    MemoryStatistics* stats = ( MemoryStatistics* )user;
    stats->add( used ? size : 0 );

    if ( used )
        rprint( "Found active allocation %p, %llu\n", ptr, size );
}

#if defined RAPTOR_IMGUI
void imgui_walker( void* ptr, size_t size, int used, void* user ) {

    u32 memory_size = ( u32 )size;
    cstring memory_unit = "b";
    if ( memory_size > 1024 * 1024 ) {
        memory_size /= 1024 * 1024;
        memory_unit = "Mb";
    }
    else if ( memory_size > 1024 ) {
        memory_size /= 1024;
        memory_unit = "kb";
    }
    ImGui::Text( "\t%p %s size: %4llu %s\n", ptr, used ? "used" : "free", memory_size, memory_unit );

    MemoryStatistics* stats = ( MemoryStatistics* )user;
    stats->add( used ? size : 0 );
}


void MemoryService::imgui_draw() {

    if ( ImGui::Begin( "Memory Service" ) ) {

        system_allocator.debug_ui();
    }
    ImGui::End();
}

#endif // RAPTOR_IMGUI

void MemoryService::test() {

    //static u8 mem[ 1024 ];

    //// Allocate 3 times
    //void* a1 = ralloca( 16, &la );
    //void* a2 = ralloca( 20, &la );
    //void* a4 = ralloca( 10, &la );
    //// Free based on size
    //la.free( 10 );
    //void* a3 = ralloca( 10, &la );
    //RASSERT( a3 == a4 );

    //// Free based on pointer
    //rfree( a2, &la );
    //void* a32 = ralloca( 10, &la );
    //RASSERT( a32 == a2 );
    //// Test out of bounds
    //u8* out_bounds = ( u8* )a1 + 10000;
    //rfree( out_bounds, &la );
}

// Memory Structs /////////////////////////////////////////////////////////

// HeapAllocator //////////////////////////////////////////////////////////
HeapAllocator::~HeapAllocator() {
}

void HeapAllocator::init( sizet size ) {
    // Allocate
    memory = malloc( size );
    max_size = size;
    allocated_size = 0;

    tlsf_handle = tlsf_create_with_pool( memory, size );

    rprint( "HeapAllocator of size %llu created\n", size );
}

void HeapAllocator::shutdown() {

    // Check memory at the application exit.
    MemoryStatistics stats{ 0, max_size };
    pool_t pool = tlsf_get_pool( tlsf_handle );
    tlsf_walk_pool( pool, exit_walker, ( void* )&stats );

    if ( stats.allocated_bytes ) {
        rprint( "HeapAllocator Shutdown.\n===============\nFAILURE! Allocated memory detected. allocated %llu, total %llu\n===============\n\n", stats.allocated_bytes, stats.total_bytes );
    } else {
        rprint( "HeapAllocator Shutdown - all memory free!\n" );
    }

    RASSERTM( stats.allocated_bytes == 0, "Allocations still present. Check your code!" );

    tlsf_destroy( tlsf_handle );

    free( memory );
}

#if defined RAPTOR_IMGUI
void HeapAllocator::debug_ui() {

    ImGui::Separator();
    ImGui::Text( "Heap Allocator" );
    ImGui::Separator();
    MemoryStatistics stats{ 0, max_size };
    pool_t pool = tlsf_get_pool( tlsf_handle );
    tlsf_walk_pool( pool, imgui_walker, ( void* )&stats );

    ImGui::Separator();
    ImGui::Text( "\tAllocation count %d", stats.allocation_count );
    ImGui::Text( "\tAllocated %llu K, free %llu Mb, total %llu Mb", stats.allocated_bytes / (1024 * 1024), ( max_size - stats.allocated_bytes ) / ( 1024 * 1024 ), max_size / ( 1024 * 1024 ) );
}
#endif // RAPTOR_IMGUI


#if defined (RAPTOR_MEMORY_STACK)
struct RaptorStackWalker : public StackWalker {

    RaptorStackWalker() : StackWalker() {}

    bool    start_logging = false;

    void    OnCallstackEntry( CallstackEntryType eType, CallstackEntry& entry ) override {
        StackWalker::OnCallstackEntry( eType, entry );

        start_logging = true;
    }

    void    OnOutput( LPCSTR szText ) override {

        if ( start_logging ) {
            rprint( "%s", szText );
        }
    }
}; // class RaptorStackWalker

static RaptorStackWalker sw;
constexpr sizet k_hunt_size = 96;

void* HeapAllocator::allocate( sizet size, sizet alignment ) {

    void* allocated_memory = alignment == 1 ? tlsf_malloc( tlsf_handle, size ) : tlsf_memalign( tlsf_handle, alignment, size );
    sizet actual_size = tlsf_block_size( allocated_memory );
    allocated_size += actual_size;

    if ( actual_size == k_hunt_size ) {
        rprint( "Allocate: %p, size %llu \n", allocated_memory, actual_size );
        sw.ShowCallstack();
    }

    return allocated_memory;
}

void HeapAllocator::deallocate( void* pointer ) {
#if defined (HEAP_ALLOCATOR_STATS)
    sizet actual_size = tlsf_block_size( pointer );
    allocated_size -= actual_size;

    if ( actual_size == k_hunt_size ) {
        rprint( "Deallocate: %p, size %llu \n", pointer, actual_size );
        sw.ShowCallstack();
    }

    tlsf_free( tlsf_handle, pointer );
#else
    tlsf_free( tlsf_handle, pointer );
#endif
}
#else

void* HeapAllocator::allocate( sizet size, sizet alignment ) {
#if defined (HEAP_ALLOCATOR_STATS)
    void* allocated_memory = alignment == 1 ? tlsf_malloc( tlsf_handle, size ) : tlsf_memalign( tlsf_handle, alignment, size );
    sizet actual_size = tlsf_block_size( allocated_memory );
    allocated_size += actual_size;

    /*static const sizet k_debug_size = 176;
    if ( size == k_debug_size || actual_size == k_debug_size ) {
        return allocated_memory;
    }*/
    return allocated_memory;
#else
    return tlsf_malloc( tlsf_handle, size );
#endif // HEAP_ALLOCATOR_STATS
}

void HeapAllocator::deallocate( void* pointer ) {
#if defined (HEAP_ALLOCATOR_STATS)
    sizet actual_size = tlsf_block_size( pointer );
    allocated_size -= actual_size;

    tlsf_free( tlsf_handle, pointer );
#else
    tlsf_free( tlsf_handle, pointer );
#endif
}
#endif // RAPTOR_MEMORY_STACK

void* HeapAllocator::allocate( sizet size, sizet alignment, cstring file, i32 line ) {
    return allocate( size, alignment );
}

// Memory Methods /////////////////////////////////////////////////////////
void memory_copy( void* destination, void* source, sizet size ) {
    memcpy( destination, source, size );
}

sizet memory_align( sizet size, sizet alignment ) {
    const sizet alignment_mask = alignment - 1;
    return ( size + alignment_mask ) & ~alignment_mask;
}

// MallocAllocator ///////////////////////////////////////////////////////
void* MallocAllocator::allocate( sizet size, sizet alignment ) {
    return malloc( size );
}

void* MallocAllocator::allocate( sizet size, sizet alignment, cstring file, i32 line ) {
    return malloc( size );
}

void MallocAllocator::deallocate( void* pointer ) {
    free( pointer );
}

// ArenaAllocator ////////////////////////////////////////////////////////
void ArenaAllocator::init( sizet size ) {
    memory = (u8*)malloc( size );
    allocated_size = 0;
    total_size = size;
}

void ArenaAllocator::shutdown() {
    free( memory );
}

void* ArenaAllocator::allocate( sizet size, sizet alignment ) {
    RASSERT( size > 0 );

    const sizet new_start = memory_align( allocated_size, alignment );
    RASSERT( new_start < total_size );
    const sizet new_allocated_size = new_start + size;
    if ( new_allocated_size > total_size ) {
        RASSERT( false && "Overflow" );
        return nullptr;
    }

    allocated_size = new_allocated_size;
    peak_used = raptor::max( peak_used, new_allocated_size );
    return memory + new_start;
}

void* ArenaAllocator::allocate( sizet size, sizet alignment, cstring file, i32 line ) {
    return allocate( size, alignment );
}

void ArenaAllocator::deallocate( void* pointer ) {
    // No-op
}

sizet ArenaAllocator::get_marker() {
    return allocated_size;
}

void ArenaAllocator::free_marker( sizet marker ) {
    const sizet difference = marker - allocated_size;
    if ( difference > 0 ) {
        allocated_size = marker;
    }
}

void ArenaAllocator::clear() {
    allocated_size = 0;
}

} // namespace raptor

//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Minimal ELF shared object loader for PS Vita. Loads engine modules
//          built as PIC shared objects and implements the dlfcn.h API on top
//          of it (see common/vita/dlfcn.h).
//
//=============================================================================//
#ifndef SO_LOADER_H
#define SO_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// An entry of the table of symbols the launcher provides to modules (libc,
// libstdc++, SDL2, ...). Generated at build time by gen_exports.py from the
// imports of all modules, sorted by name.
typedef struct
{
	const char *name;
	void *addr;
} VitaExport;

extern const VitaExport g_VitaExports[];
extern const int g_VitaExportCount;

// Directory searched for a module by file name when it can't be opened from
// the path given to dlopen, e.g. "app0:bin"
void so_set_library_dir( const char *dir );

// Installs CPU exception handlers that write the faulting address, resolved
// to module + offset, to the given log file, then let the system handle the
// crash as usual (core dump in ux0:data)
void so_install_crash_handler( const char *log_path );

// Finishes GPU work, releases the shader compiler and the crash handlers, and
// exits the process (the memory reservations are left to the kernel)
void so_exit_process( int code );
void so_set_gl_ready( void );	// vitaGL is up (glFinish is safe)
void so_set_menus_visible( int bVisible );	// engine, each frame
int so_menus_visible( void );	// shaderapidx9: per-frame dynamic buffer discard
float so_hud_scale( void );	// HUD size: vgui2 Scheme.cpp, client vita_hud_size
void so_set_hud_scale( float flScale );
void so_reserve_register( const void *pStack, void *pCaller );	// tier1/memstack.cpp
void so_reserve_unregister( const void *pStack );
void so_reserve_commit_add( const void *pStack, int nKB );	// CMemoryStack commits
int so_reserve_committed_kb( void );
void so_reserve_top( char *pszOut, int nOutSize );	// the three stacks holding the most
void so_request_screenshot( void );	// main.cpp: save the next presented frame

// Separate arena for large blocks, carved out of the newlib heap (0 = off);
// call before any other thread starts. Stats: arena KB in use, peak and size,
// and the requests that fell back to the other heap
void so_large_heap_init( unsigned nMB );
// Walks newlib's heap and logs the first corrupt chunk (-vitadebug)
void so_heap_check( const char *where );
void so_large_heap_stats( unsigned *pInUseKB, unsigned *pPeakKB, unsigned *pSizeKB, unsigned *pLargeToNewlib, unsigned *pSmallToLarge );

// Appends a line to the launcher's log (ux0:data/hl2_vita_boot.txt) through
// sceIo directly: redirecting stdout to a file doesn't work with Vita's newlib,
// and reopening per line means nothing is lost if the process dies
void so_trace( const char *fmt, ... );

// Appends text as is (no newline added) to the same log; tier0 sends all
// engine spew here
void so_log_write( const char *text );

// Finds the module containing an address; returns its name and fills in the
// offset of the address from the module's load base
const char *so_module_for_address( uintptr_t addr, uintptr_t *offset );

// Logs the return addresses on the calling thread's stack, as a backtrace
void so_log_backtrace( const char *tag );

// Load-time / performance counters, see common/vita/vita_perf.h
uint64_t so_perf_now( void );
void so_perf_add( int counter, uint64_t start_us, uint64_t amount );
void so_perf_log( void );
// Adds the calling thread to the threads: line (CPU time, core, preemptions)
void so_thread_register( const char *name );

// Named sections (engine and client; any thread), logged with so_perf_log as
// ms per second, largest first. The name is "prefix name" (either may be
// NULL); both are copied, so they needn't outlive the call
void so_profile_add( const char *prefix, const char *name, uint64_t us );
void so_profile_enable( int enable );
int so_profile_enabled( void );

#ifdef __cplusplus
}
#endif

#endif // SO_LOADER_H

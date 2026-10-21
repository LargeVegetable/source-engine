//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Vita load-time and performance counters. Totals live in the
//          launcher (launcher_vita/so_loader.cpp), which logs them every 10
//          seconds; modules add timed scopes with VITA_PERF_SCOPE.
//
//=============================================================================//
#ifndef VITA_PERF_H
#define VITA_PERF_H

#ifdef PLATFORM_VITA
#include <stdint.h>

enum
{
	VITA_PERF_SHADER_COMPILE,	// glCompileShader (vitaGL: shader cache or on-device compile)
	VITA_PERF_TEX_UPLOAD,		// ToGL CGLMTex::WriteTexels (conversion + GL upload)
	VITA_PERF_FILE_OPEN,
	VITA_PERF_FILE_READ,		// amount = bytes
	VITA_PERF_FILE_SEEK,
	VITA_PERF_LZMA,				// amount = bytes out
	VITA_PERF_DRAW,				// ToGL DrawRangeElements (counted, not timed)
	VITA_PERF_SWAP,				// vglSwapBuffers (GPU wait, vsync, garbage collection)
	// engine frame phases (engine/host.cpp, engine/view.cpp, vphysics)
	VITA_PERF_HOST_INPUT,
	VITA_PERF_HOST_SERVER,		// server ticks, including physics
	VITA_PERF_PHYSICS,			// server vphysics simulation (game/server/physics.cpp PhysFrame)
	VITA_PERF_HOST_CLIENT,		// client ticks: packets, entity updates, prediction
	VITA_PERF_HOST_RENDER,		// Host_UpdateScreen: 3D view, VGUI, present
	VITA_PERF_VIEW_RENDER,		// client View_Render (the 3D view)
	VITA_PERF_HOST_SOUND,
	VITA_PERF_SV_THINK,			// server entity think functions (AI, scripts, NPCs)
	VITA_PERF_SV_SEND,			// server snapshots: entity packing + sending to the client
	VITA_PERF_SV_GAMEFRAME,		// server game DLL GameFrame (think, events, game systems)
	VITA_PERF_TRACE,			// engine traces (CEngineTrace::TraceRay; counted, not timed)
	VITA_PERF_MATQUEUE_EXEC,	// queued material system: render thread executing a frame's queue
	VITA_PERF_MATQUEUE_WAIT,	// queued material system: main thread waiting for the render thread
	VITA_PERF_HOST_SERVER_JOIN,	// main thread joining the threaded server job (waits, or runs it itself if no worker had started it)
	VITA_PERF_HOST_FRAME,		// the whole Host_RunFrame
	VITA_PERF_WORLD_DRAW,		// world brush batches drawn (engine/gl_rsurf.cpp; counted)
	VITA_PERF_MODEL_DRAW,		// studio models drawn: NPCs, physics props, viewmodel (engine/l_studio.cpp; counted)
	VITA_PERF_STATIC_PROP,		// static props drawn (engine/l_studio.cpp; amount = props)
	VITA_PERF_PARTICLE_SIM,		// client particle simulation (game/client/cdll_client_int.cpp)
	VITA_PERF_PARTICLES,		// particle systems drawn (game/client/particles_new.cpp; counted, amount = particles)
	VITA_PERF_FIRE_PARTICLES,	// the fire ones among them (env_fire*, burning*)
	VITA_PERF_VGUI,				// engine VGUI paint, HUD and menus (engine/vgui_baseui_interface.cpp)
	VITA_PERF_VGUI_CHARS,		// characters drawn by the VGUI surface (vguimatsurface; counted, amount = chars)
	VITA_PERF_COUNT
};

extern "C" uint64_t so_perf_now( void );
extern "C" void so_perf_add( int counter, uint64_t start_us, uint64_t amount );
extern "C" void so_perf_count( int counter, uint64_t amount );	// counted, no clock read
// Logs the calling thread's CPU time, core and preemptions every 10 s (once
// per thread; name must be a literal)
extern "C" void so_thread_register( const char *name );

struct CVitaPerfScope
{
	int m_nCounter;
	uint64_t m_nStart;
	uint64_t m_nAmount;
	CVitaPerfScope( int nCounter ) : m_nCounter( nCounter ), m_nStart( so_perf_now() ), m_nAmount( 0 ) {}
	~CVitaPerfScope() { so_perf_add( m_nCounter, m_nStart, m_nAmount ); }
};

#define VITA_PERF_SCOPE( counter ) CVitaPerfScope _vitaPerfScope( counter )

// Named engine/client sections, any thread (launcher_vita/so_loader.cpp):
// logged every 10 s as "[vita] engine/client ms/s:". prefix and name are
// copied; pass stable pointers (literals, pooled classnames) for speed
extern "C" void so_profile_add( const char *prefix, const char *name, uint64_t us );
// vita_profile cvar: all named-section timers (engine/client and server) are
// skipped when it's 0; the frame: counters above always run
extern "C" int so_profile_enabled( void );
struct CVitaSectionScope
{
	const char *m_pszPrefix;
	const char *m_pszName;
	uint64_t m_nStart;
	CVitaSectionScope( const char *pszPrefix, const char *pszName ) : m_pszPrefix( pszPrefix ), m_pszName( pszName ), m_nStart( so_profile_enabled() ? so_perf_now() : 0 ) {}
	~CVitaSectionScope() { if ( m_nStart ) so_profile_add( m_pszPrefix, m_pszName, so_perf_now() - m_nStart ); }
};
#define VITA_SECTION_SCOPE( name ) CVitaSectionScope _vitaSectionScope( NULL, name )

// Named server sections (game/server/physics_main.cpp), keyed by the string
// pointers (prefix, name) and logged as "prefix name": pass stable strings
// (literals, pooled classnames), never a reused buffer
void VitaProfileAdd( const char *pszPrefix, const char *pszName, uint64_t nTime );
inline void VitaProfileAdd( const char *pszName, uint64_t nTime ) { VitaProfileAdd( NULL, pszName, nTime ); }
struct CVitaProfileScope
{
	const char *m_pszPrefix;
	const char *m_pszName;
	uint64_t m_nStart;
	CVitaProfileScope( const char *pszName ) : m_pszPrefix( NULL ), m_pszName( pszName ), m_nStart( so_profile_enabled() ? so_perf_now() : 0 ) {}
	CVitaProfileScope( const char *pszPrefix, const char *pszName ) : m_pszPrefix( pszPrefix ), m_pszName( pszName ), m_nStart( so_profile_enabled() ? so_perf_now() : 0 ) {}
	~CVitaProfileScope() { if ( m_nStart ) VitaProfileAdd( m_pszPrefix, m_pszName, so_perf_now() - m_nStart ); }
};
#define VITA_PERF_AMOUNT( n ) ( _vitaPerfScope.m_nAmount = (n) )
#define VITA_PROFILE_SCOPE( name ) CVitaProfileScope _vitaProfileScope( name )
#else
#define VITA_PROFILE_SCOPE( name )
#define VITA_SECTION_SCOPE( name )
#define VITA_PERF_SCOPE( counter )
#define VITA_PERF_AMOUNT( n )
#endif

#endif // VITA_PERF_H

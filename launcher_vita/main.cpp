//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: PS Vita launcher. Replaces launcher_main: sets up the process,
//          loads liblauncher.so through the module loader and runs it.
//
//          Game data is expected under VITA_BASE_DIR (hl2/, platform/, ...
//          copied from a PC install); engine modules ship in app0:bin.
//
//=============================================================================//
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/vshbridge.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <kubridge.h>

#include "so_loader.h"
#include "version.h"

// -vitadebug in commandline.txt: the detailed logs (10 s performance and
// memory lines, saved frames, heap checks at map changes, slow server
// ticks, shader links). Off by default: the boot log keeps startup, a
// memory and performance summary a minute, errors and crashes
static bool s_bVitaDebug = false;

static bool s_bGLUp = false;	// vitaGL is initialized (the engine's or the error dialog's)


#define VITA_BASE_DIR "ux0:data/halflife2"
#define VITA_MAX_ARGS 64

// The engine and game code allocate from the newlib heap; what's left over
// stays with the kernel for module images, threads, vitaGL and the engine
// hunk (committed on demand). In d1_trainstation the heap peaked at ~150 MB
// while kernel memory fell to 5 MB with 256 MB here, so 32 MB moved back.
// Later, kernel free bottomed at 32-33 MB over eight maps while load peaks
// filled the heap to 191 of 224 MB, so 8 MB moved in (for the large-block
// arena; the GL threshold below drops with it)
int _newlib_heap_size_user = 232 * 1024 * 1024;

// The engine runs on its own thread (see below); the main thread only needs
// enough stack to start it
unsigned int sceUserMainThreadStackSize = 512 * 1024;

// Linux gives threads 8 MB by default and Source has deep call chains with
// large stack buffers
#define ENGINE_THREAD_STACK_SIZE ( 8 * 1024 * 1024 )

// Free system memory vitaGL leaves for everything else when SDL initializes
// it. SDL asks for 12 MB, but about 27 MB of engine modules load after the GL
// context exists (server, client, GameUI, vphysics, ...), and thread stacks
// and other kernel allocations come from the same budget, as does the engine
// hunk (committed while maps load). 64 MB left ~5 MB in d1_trainstation, and
// vitaGL takes whatever is above the threshold, so memory moved out of the
// newlib heap only reaches the engine if the threshold rises with it (and
// memory moved into the heap must come off it, or vitaGL's pool shrinks).
#define VITA_GL_RAM_THRESHOLD ( 88 * 1024 * 1024 )

// vitaGL's per-frame vertex/uniform scratch pools, one per display buffer.
// The 32 MB default (one pool even spilled into VRAM) is far more than the
// engine streams per frame, and took GPU memory textures then had to find in
// the newlib heap; overruns fall back to normal allocations and are logged
#define VITA_GL_CIRCULAR_POOL_SIZE ( 9 * 1024 * 1024 )
extern "C" void vglSetCircularPoolSize( unsigned int size );

typedef int ( *LauncherMain_t )( int argc, char **argv );

// Render and display size. The engine renders at -w x -h (640x368 unless
// commandline.txt sets them) and vitaGL's display buffers use the same size;
// the Vita's display controller scales 480x272, 640x368 or 720x408 to the
// screen in hardware. 960x544 (native) was tried: the GPU became the
// limit (the render thread waited 6-15 ms a frame at the swap), and
// 640x368 looked better in play
static int s_nDisplayW = 640, s_nDisplayH = 368;

static void TraceFreeMemory( const char *when )
{
	SceKernelFreeMemorySizeInfo info;
	info.size = sizeof( info );
	sceKernelGetFreeMemorySize( &info );
	char szTop[192];
	so_reserve_top( szTop, sizeof( szTop ) );
	so_trace( "free memory %s: main %d KB (memory stacks hold %d KB, most: %s), cdram %d KB, phycont %d KB", when,
		info.size_user / 1024, so_reserve_committed_kb(), szTop, info.size_cdram / 1024, info.size_phycont / 1024 );
}

// vglSemanticMode::VGL_MODE_GLOBAL and vglSemanticType values in vitaGL.h
#define VITA_GL_MODE_GLOBAL 1
#define VITA_GL_TYPE_TEXCOORD 1
#define VITA_GL_TYPE_COLOR 3
extern "C" void vglSetSemanticBindingMode( unsigned int mode );
// declared with a GLchar ** parameter in vitaGL.h but takes the name itself
extern "C" void vglAddSemanticBinding( const char *varying, int index, unsigned int type );

// Fixed hardware slots for the varyings ToGLES' shaders use, which follow
// D3D9's registers: oT0-7 texture coordinates, two colors and fog (renamed
// from ToGLES' _gl_* names by togles/linuxwin/vita_glsl.cpp), plus ToGLES'
// own preload test shaders. In global mode vitaGL otherwise hands
// out slots per name in first-come order from 10 TEXCOORDs and 2 COLORs
// shared by all shaders; once those ran out, two varyings shared a slot and
// the compile failed.
static void AddVaryingBindings()
{
	static const char *s_TexCoords[] = { "oT0", "oT1", "oT2", "oT3", "oT4", "oT5", "oT6", "oT7" };
	for ( int i = 0; i < 8; i++ )
		vglAddSemanticBinding( s_TexCoords[i], i, VITA_GL_TYPE_TEXCOORD );
	vglAddSemanticBinding( "vFogCoord", 8, VITA_GL_TYPE_TEXCOORD );
	vglAddSemanticBinding( "otex", 9, VITA_GL_TYPE_TEXCOORD );
	vglAddSemanticBinding( "vColor0", 0, VITA_GL_TYPE_COLOR );
	vglAddSemanticBinding( "vColor1", 1, VITA_GL_TYPE_COLOR );
}

// vitaGL is built with LOG_ERRORS (see build.sh), which prints through
// sceClibPrintf; the launcher is linked with --wrap=sceClibPrintf so those
// messages, shader compiler output included, land in the boot log
//
// Filtered, as the volume slowed startup down: shader compiler warnings are
// dropped (errors are kept), and each message is logged only the first few
// times. Messages are told apart by their text up to the first " (" (vitaGL
// errors read "file:line: function set ERROR (param: value)"); most of them
// share one format string, so counting by format hid all but the first few
// errors of any kind.
#define VITAGL_LOG_REPEATS 5
#define VITAGL_LOG_SITES 512

static unsigned int MessageSiteHash( const char *msg )
{
	unsigned int h = 2166136261u;
	for ( const char *p = msg; *p && *p != '\n' && !( p[0] == ' ' && p[1] == '(' ); p++ )
		h = ( h ^ (unsigned char)*p ) * 16777619u;
	return h;
}

extern "C" int __wrap_sceClibPrintf( const char *fmt, ... )
{
	static unsigned int s_Sites[VITAGL_LOG_SITES];
	static int s_Counts[VITAGL_LOG_SITES];
	static int s_NumSites;

	char buf[1024];
	va_list args;
	va_start( args, fmt );
	int len = vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );

	if ( strstr( buf, "Shader Compiler: W]" ) || strstr( buf, "Shader Compiler: I]" ) )
		return len;

	unsigned int site = MessageSiteHash( buf );
	int slot = -1;
	for ( int i = 0; i < s_NumSites; i++ )
	{
		if ( s_Sites[i] == site )
		{
			slot = i;
			break;
		}
	}
	if ( slot < 0 && s_NumSites < VITAGL_LOG_SITES )
	{
		slot = s_NumSites++;
		s_Sites[slot] = site;
	}
	if ( slot >= 0 && ++s_Counts[slot] > VITAGL_LOG_REPEATS )
		return len;

	so_log_write( "vitaGL: " );
	so_log_write( buf );
	if ( slot >= 0 && s_Counts[slot] == VITAGL_LOG_REPEATS )
		so_log_write( "vitaGL: (further repeats of the message above are not logged)\n" );
	return len;
}

// SDL calls vglInitExtended itself with a fixed 12 MB RAM threshold; the
// launcher is linked with --wrap=vglInitExtended so that call lands here
extern "C" int __real_vglInitExtended( int legacy_pool_size, int width, int height, int ram_threshold, int msaa );

extern "C" int __wrap_vglInitExtended( int legacy_pool_size, int width, int height, int ram_threshold, int msaa )
{
	TraceFreeMemory( "before vitaGL init" );
	so_trace( "vglInitExtended: RAM threshold %d MB instead of SDL's %d MB",
		VITA_GL_RAM_THRESHOLD / ( 1024 * 1024 ), ram_threshold / ( 1024 * 1024 ) );
	vglSetCircularPoolSize( VITA_GL_CIRCULAR_POOL_SIZE );
	so_trace( "vglInitExtended: display %dx%d (SDL asked for %dx%d)", s_nDisplayW, s_nDisplayH, width, height );
	int result = __real_vglInitExtended( legacy_pool_size, s_nDisplayW, s_nDisplayH, VITA_GL_RAM_THRESHOLD, msaa );
	s_bGLUp = true;
	so_set_gl_ready();
	TraceFreeMemory( "after vitaGL init" );

	// vitaGL's default postpones shader compiles into glLinkProgram, where a
	// failed compile crashed in sceGxmProgramGetParameterCount instead of
	// being reported. Compiling in glCompileShader lets ToGLES' compile
	// status check log the error and the shader source.
	vglSetSemanticBindingMode( VITA_GL_MODE_GLOBAL );
	AddVaryingBindings();
	so_trace( "vitaGL shader compiles moved to glCompileShader, varying slots fixed" );
	return result;
}

// Swap-time log (fps, memory) and the periodic screenshots. The launcher is
// linked with --wrap=vglSwapBuffers
#define GL_RGBA_ 0x1908
#define GL_UNSIGNED_BYTE_ 0x1401
#define GL_READ_FRAMEBUFFER_ 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING_ 0x8CAA
extern "C" void glReadPixels( int x, int y, int width, int height, unsigned int format, unsigned int type, void *data );
extern "C" void glGetIntegerv( unsigned int pname, int *data );
extern "C" void glBindFramebuffer( unsigned int target, unsigned int framebuffer );
#define GL_COLOR_BUFFER_BIT_ 0x4000
extern "C" void glClearColor( float red, float green, float blue, float alpha );
extern "C" void glClear( unsigned int mask );
extern "C" void __real_vglSwapBuffers( unsigned char has_commondialog );
extern "C" size_t vglMemFree( int type ); // vglMemType: 0 VRAM, 1 RAM, 2 PHYCONT
extern "C" size_t vglMemExternalInUse( void );
extern "C" void vglGetProgramShaderCounts( int *programs, int *shaders );	// vitaGL fork
extern "C" void vglGetBufferChurn( uint32_t *orphans, uint32_t *orphan_kb, uint32_t *reuses, uint32_t *spare_kb, uint32_t *copies, uint32_t *copy_kb );	// vitaGL fork
extern "C" void vglGetShaderPatcherUsage( uint32_t *buffer_used, uint32_t *vertex_usse_used, uint32_t *fragment_usse_used, uint32_t *pool_size );	// vitaGL fork

#define VITA_FRAME_W s_nDisplayW
#define VITA_FRAME_H s_nDisplayH

// Saves the back buffer as an uncompressed TGA, so frames can be looked at
// without photographing the screen (rows as glReadPixels returns them).
// Only the read happens here, on the thread presenting (it needs GL); the
// byte swap and the ~1 MB card write go to a short-lived thread, so a
// screenshot no longer stalls the frame being presented for the write
struct SaveFrameJob
{
	unsigned char *pixels;
	int w, h;
	char path[64];
};

static void *SaveFrameThread( void *param )
{
	SaveFrameJob *job = (SaveFrameJob *)param;
	// Created by the presenting thread, it would share that thread's core at
	// its priority (equal priorities aren't time-sliced, so the byte swap
	// could hold the render thread off): any core, lowest user priority, so
	// it only runs on idle time
	sceKernelChangeThreadCpuAffinityMask( sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_ALL );
	sceKernelChangeThreadPriority( sceKernelGetThreadId(), 191 );
	const int nPixels = job->w * job->h;
	for ( int i = 0; i < nPixels; i++ )
	{
		unsigned char r = job->pixels[i * 4];
		job->pixels[i * 4] = job->pixels[i * 4 + 2];	// TGA stores BGRA
		job->pixels[i * 4 + 2] = r;
		// opaque: Source writes depth to the back buffer's alpha, which
		// viewers showed as near-white (a "white airboat")
		job->pixels[i * 4 + 3] = 255;
	}
	unsigned char header[18] = {};
	header[2] = 2;	// uncompressed true color
	header[12] = job->w & 0xFF; header[13] = job->w >> 8;
	header[14] = job->h & 0xFF; header[15] = job->h >> 8;
	header[16] = 32;
	header[17] = 8;	// 8 alpha bits, bottom-up rows
	SceUID fd = sceIoOpen( job->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if ( fd >= 0 )
	{
		sceIoWrite( fd, header, sizeof( header ) );
		sceIoWrite( fd, job->pixels, nPixels * 4 );
		sceIoClose( fd );
		so_trace( "saved %s", job->path );
	}
	free( job->pixels );
	free( job );
	return NULL;
}

static void SaveFrame( const char *prefix, int index )
{
	SaveFrameJob *job = (SaveFrameJob *)malloc( sizeof( SaveFrameJob ) );
	if ( !job )
		return;
	job->w = VITA_FRAME_W;
	job->h = VITA_FRAME_H;
	job->pixels = (unsigned char *)malloc( job->w * job->h * 4 );
	if ( !job->pixels )
	{
		free( job );
		return;
	}
	glReadPixels( 0, 0, job->w, job->h, GL_RGBA_, GL_UNSIGNED_BYTE_, job->pixels );
	snprintf( job->path, sizeof( job->path ), "ux0:data/%s_%02d.tga", prefix, index );

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, 64 * 1024 );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	pthread_t thread;
	if ( pthread_create( &thread, &attr, SaveFrameThread, job ) != 0 )
		SaveFrameThread( job );	// no thread: write it here, as before
	pthread_attr_destroy( &attr );
}

static volatile int s_bScreenshotRequested = 0;


// vita_screenshot (engine): the next presented frame is saved as
// ux0:data/hl2_shot_NN.tga
extern "C" void so_request_screenshot( void )
{
	s_bScreenshotRequested = 1;
}

extern "C" void __wrap_vglSwapBuffers( unsigned char has_commondialog )
{
	// logged every 10 seconds (and the first swaps)
	static int s_nSwaps = 0, s_nSwapsAtLog = 0;
	static SceUInt64 s_nLastLog = 0;
	const SceUInt64 now = sceKernelGetProcessTimeWide();
	if ( s_nSwaps < 3 || now - s_nLastLog >= ( s_bVitaDebug ? 10000000ull : 60000000ull ) )
	{
		// a few full frames, at least 30 s apart (the first ones land on
		// loading screens, which swap only every few minutes); each is a
		// GPU sync for the read (the write is on its own thread), so only 8
		// per session
		static int s_nFramesSaved = 0;
		static SceUInt64 s_nLastFrameSaved = 0;
		if ( s_bVitaDebug && s_nSwaps >= 3 && s_nFramesSaved < 8 && ( !s_nFramesSaved || now - s_nLastFrameSaved >= 30000000ull ) )
		{
			int prevRead = 0;
			glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING_, &prevRead );
			glBindFramebuffer( GL_READ_FRAMEBUFFER_, 0 );
			SaveFrame( "hl2_frame", ++s_nFramesSaved );
			s_nLastFrameSaved = now;
			glBindFramebuffer( GL_READ_FRAMEBUFFER_, prevRead );
		}
		struct mallinfo mi = mallinfo();
		const int fps10 = s_nLastLog ? (int)( ( s_nSwaps - s_nSwapsAtLog ) * 10000000ull / ( now - s_nLastLog ) ) : 0;
		// GL programs and shaders in use (vitaGL caps them at 1024 and 2048;
		// ToGL links one program per shader pair) and ToGL's texture storage
		int nPrograms = 0, nShaders = 0;
		vglGetProgramShaderCounts( &nPrograms, &nShaders );
		// GXM shader patcher pools (1 MB each; patched variants are never
		// freed while their shader lives)
		uint32_t nPatchBuf = 0, nPatchVert = 0, nPatchFrag = 0, nPatchPool = 1;
		vglGetShaderPatcherUsage( &nPatchBuf, &nPatchVert, &nPatchFrag, &nPatchPool );
		static long long *s_pTexBytes = NULL;
		if ( !s_pTexBytes )
			s_pTexBytes = (long long *)dlsym( RTLD_DEFAULT, "g_nVitaTexBytes" );
		unsigned nLargeKB, nLargePeakKB, nLargeSizeKB, nLargeToNewlib, nSmallToLarge;
		so_large_heap_stats( &nLargeKB, &nLargePeakKB, &nLargeSizeKB, &nLargeToNewlib, &nSmallToLarge );
		// graphics memory vitaGL keeps for reuse by replaced dynamic buffers
		uint32_t nOrphans, nOrphanKB, nReuses, nSpareKB, nCopies, nCopyKB;
		vglGetBufferChurn( &nOrphans, &nOrphanKB, &nReuses, &nSpareKB, &nCopies, &nCopyKB );
		so_trace( "swap %d (%d.%d fps): heap %u KB in use (vitaGL %u KB, large blocks %u KB of %u, peak %u; fallbacks %u/%u), vitaGL free KB: vram %u ram %u phycont %u; "
			"textures %u KB, buffer spares %u KB, GL programs %d/1024, shaders %d/4096, patcher KB buf %u vert %u frag %u of %u each",
			s_nSwaps, fps10 / 10, fps10 % 10, (unsigned)mi.uordblks / 1024, (unsigned)( vglMemExternalInUse() / 1024 ),
			nLargeKB, nLargeSizeKB, nLargePeakKB, nLargeToNewlib, nSmallToLarge,
			(unsigned)( vglMemFree( 0 ) / 1024 ), (unsigned)( vglMemFree( 1 ) / 1024 ), (unsigned)( vglMemFree( 2 ) / 1024 ),
			s_pTexBytes ? (unsigned)( *s_pTexBytes / 1024 ) : 0, (unsigned)nSpareKB, nPrograms, nShaders,
			nPatchBuf / 1024, nPatchVert / 1024, nPatchFrag / 1024, nPatchPool / 1024 );
		// GL buffers replaced while the GPU may still read them (orphaned by
		// glBufferData, copied by glBufferSubData) in the last 10 s: each stays
		// allocated for a few frames (vram once ran out in a canal fight
		// with textures flat)
		// warn near vitaGL's fixed caps (a full table fails creation)
		if ( nShaders > 4096 * 9 / 10 || nPrograms > 1024 * 9 / 10 || nPatchBuf > nPatchPool * 9 / 10 ||
			 nPatchVert > nPatchPool * 9 / 10 || nPatchFrag > nPatchPool * 9 / 10 )
			so_trace( "[vita] WARNING: a vitaGL shader/program/patcher limit is over 90%% used" );
		s_nLastLog = now;
		s_nSwapsAtLog = s_nSwaps;
	}
	// on request (vita_screenshot, e.g. bound to a button when a glitch shows)
	if ( s_bScreenshotRequested )
	{
		s_bScreenshotRequested = 0;
		static int s_nShots = 0;
		int prevRead = 0;
		glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING_, &prevRead );
		glBindFramebuffer( GL_READ_FRAMEBUFFER_, 0 );
		SaveFrame( "hl2_shot", ++s_nShots );
		glBindFramebuffer( GL_READ_FRAMEBUFFER_, prevRead );
	}
	s_nSwaps++;
	const uint64_t nSwapStart = so_perf_now();
	__real_vglSwapBuffers( has_commondialog );
	so_perf_add( 7, nSwapStart, 0 );	// VITA_PERF_SWAP
}

struct EngineArgs
{
	int argc;
	char *argv[VITA_MAX_ARGS];
	int result;
};

// Setup problems, shown as a system message dialog with the fix (the user
// otherwise only saw the game close), then the game exits. The dialog needs
// vitaGL running to draw frames, which it can without the shader compiler
static void ShowErrorDialog( const char *msg )
{
	if ( !s_bGLUp )
	{
		__real_vglInitExtended( 0, 960, 544, 4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE );
		s_bGLUp = true;
	}
	SceMsgDialogUserMessageParam msgParam;
	memset( &msgParam, 0, sizeof( msgParam ) );
	msgParam.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
	msgParam.msg = (const SceChar8 *)msg;
	SceMsgDialogParam param;
	sceMsgDialogParamInit( &param );
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msgParam;
	if ( sceMsgDialogInit( &param ) < 0 )
		return;
	while ( sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING )
	{
		glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT_ );
		__real_vglSwapBuffers( 1 );	// with the common dialog
	}
	sceMsgDialogTerm();
}

static void FatalError( const char *msg )
{
	so_trace( "FATAL: %s", msg );
	ShowErrorDialog( msg );
	so_exit_process( 1 );
}

// The game files are the 20th anniversary update's when this shader has
// more dynamic combos than the engine's tables (see vertexshaderdx8.cpp).
// Only checked when the shaders are loose files
static bool IsAnniversaryGameData( void )
{
	FILE *f = fopen( VITA_BASE_DIR "/hl2/shaders/fxc/vertexlit_and_unlit_generic_ps20b.vcs", "rb" );
	if ( !f )
		return false;
	int header[3] = { 0, 0, 0 };	// version, total combos, dynamic combos
	const bool bRead = fread( header, sizeof( header ), 1, f ) == 1;
	fclose( f );
	if ( bRead && header[2] != 6 )
	{
		so_trace( "vertexlit_and_unlit_generic_ps20b.vcs has %d dynamic combos (pre-20th-anniversary: 6)", header[2] );
		return true;
	}
	return false;
}

// Extra arguments, one per whitespace-separated token, from an optional
// commandline.txt in the base dir, for testing without rebuilding
static void ReadExtraArgs( EngineArgs *args )
{
	static char s_Buf[4096];
	FILE *f = fopen( VITA_BASE_DIR "/commandline.txt", "r" );
	if ( !f )
		return;

	size_t len = fread( s_Buf, 1, sizeof( s_Buf ) - 1, f );
	fclose( f );
	s_Buf[len] = '\0';

	const int nFirst = args->argc;
	for ( char *tok = strtok( s_Buf, " \t\r\n" ); tok && args->argc < VITA_MAX_ARGS - 1; tok = strtok( NULL, " \t\r\n" ) )
		args->argv[args->argc++] = tok;
	char szLine[256];
	int nLen = 0;
	szLine[0] = 0;
	for ( int i = nFirst; i < args->argc && nLen < (int)sizeof( szLine ); i++ )
		nLen += snprintf( szLine + nLen, sizeof( szLine ) - nLen, " %s", args->argv[i] );
	so_trace( "commandline.txt:%s", szLine );
}

// Writes an editable override file to the memory card on first run. The
// engine execs it after the built-in Vita settings (engine/host.cpp).
static void WriteDefaultUserConfig( void )
{
	const char *path = VITA_BASE_DIR "/hl2/cfg/vita_user.cfg";
	// the current template is always written as vita_user_example.cfg, so
	// options added later show up without touching the user's own file
	const char *examplePath = VITA_BASE_DIR "/hl2/cfg/vita_user_example.cfg";
	static const char s_szConfig[] =
		"// Overrides for the HL2 Vita port, run after its built-in settings.\n"
		"// Remove the \"//\" in front of a line to turn it on.\n"
		"\n"
		"// Client and server frames in parallel (on by default)\n"
		"// host_thread_mode 0\n"
		"\n"
		"// Threaded work that is on by default; set to 0 to rule one out\n"
		"// cl_threaded_bone_setup 0\n"
		"// cl_threaded_client_leaf_system 0\n"
		"// r_threaded_renderables 0\n"
		"// r_threaded_particles 0\n"
		"\n"
		"// Server tick rate: set -tickrate 66 in ux0:data/halflife2/commandline.txt\n"
		"// for the original rate (the port runs 33). At 66, entities think every\n"
		"// other tick (sv_alternateticks 1).\n"
		"\n"
		"// Xbox 360 AI tuning (on by default)\n"
		"// ai_default_efficient 0\n"
		"// ai_strong_optimizations 0\n"
		"\n"
		"// NPC think budget per server tick in ms (default 10, as on PC).\n"
		"// Lower: cheaper server frames, less responsive crowds\n"
		"// ai_think_limit_ms 6\n"
		"\n"
		"// AI frame-rate step-down: every non-scripted NPC decides half as\n"
		"// often (default 50 = off, as on PC; 0.01 = always on)\n"
		"// ai_frametime_limit 0.01\n"
		"\n"
		"// Think interval for standing, idle NPCs (port default 0.2; 0.1 = PC)\n"
		"// ai_vita_idle_think_interval 0.1\n"
		"\n"
		"// Texture detail: 3 = 1/8 size per axis (default), 4 = 1/16 (less\n"
		"// memory, faster loads), 2 = 1/4 (sharpest; watch video memory)\n"
		"// mat_picmip 4\n"
		"\n"
		"// Smallest size picmip shrinks a texture to (default 64; 4 = stock,\n"
		"// which turned small textures like window glass into blocks)\n"
		"// mat_picmip_min_size 32\n"
		"\n"
		"// The same floor for cut-out textures (chain-link fences, grates,\n"
		"// foliage; default 256, below that their holes blur shut) and for\n"
		"// first-person weapons and hands (default 256)\n"
		"// mat_picmip_alphatest_min_size 128\n"
		"// mat_picmip_viewmodel_min_size 128\n"
		"\n"
		"// Static props' baked vertex lighting (default 1; 0 lights them like\n"
		"// physics props, which drew many black). Applies on map load\n"
		"// r_vita_staticpropvertexlighting 0\n"
		"\n"
		"// Lighting: 1 turns lightmaps off (flat, fully lit world)\n"
		"// mat_fullbright 1\n"
		"\n"
		"// Model lighting: world lights per model on top of the ambient cube\n"
		"// (default 0; 1-4 = less flat models, more CPU)\n"
		"// r_worldlights 1\n"
		"\n"
		"// Model detail: -1 picks by distance (default), 2 = low detail\n"
		"// everywhere (cheaper with crowds)\n"
		"// r_lod 2\n"
		"\n"
		"// Render on a second core (queued material system, on by default):\n"
		"// the shader and GL work moves off the main thread, with up to one\n"
		"// frame more input lag. 0 = everything on the main thread\n"
		"// vita_queued_render 0\n"
		"\n"
		"// Static props smaller than this radius on screen (pixels) aren't\n"
		"// drawn (default 3; 0 = all, higher = faster, more pop-in)\n"
		"// vita_staticprop_minpixels 0\n"
		"\n"
		"// Monitors (Breencast screens) redraw every N frames (default 2)\n"
		"// vita_monitor_interval 1\n"
		"\n"
		"// Physics: small, light props (20 kg or less: junk, planks, gibs) pass\n"
		"// through each other instead of stacking, which keeps fights with lots\n"
		"// of debris cheaper (they still hit the world, you, NPCs and bigger\n"
		"// props). 0 = stock collisions; vita_physics_small_mass sets the limit\n"
		"// vita_physics_small_props 0\n"
		"\n"
		"// Texture filtering: 0 = bilinear (visible seams between mip levels)\n"
		"// mat_trilinear 0\n"
		"\n"
		"// Render resolution: add -w 480 -h 272 (faster), -w 720 -h 408 or\n"
		"// -w 960 -h 544 (native; GPU-bound in busy scenes) to\n"
		"// ux0:data/halflife2/commandline.txt; the display only takes these\n"
		"// sizes and 640x368 (the default)\n"
		"\n"
		"// Memory/shader check of late maps: loads each map in vita_maptour_maps\n"
		"// (ten maps from Eli's lab to Breen's office) and stays\n"
		"// vita_maptour_seconds (30) in each. Uncomment this for one run (it\n"
		"// starts at every launch)\n"
		"// vita_maptour\n"
		"\n"
		"// Physics step while a wheeled vehicle (the jeep, an unlocked APC) is\n"
		"// awake: 0.015 s (default) steps physics twice per 30 ms server tick\n"
		"// for smoother vehicle physics (it didn't cure the APCs' jitter: they're\n"
		"// locked, below); 0 keeps one step per tick (less physics time)\n"
		"// vita_phys_vehicle_step 0.015\n"
		"\n"
		"// Combine APCs are locked in place once they're on the ground (they\n"
		"// jittered and bounced here); they still aim and fire. 0 lets them drive\n"
		"// vita_apc_lock 1\n"
		"\n"
		"// Detailed section timers in the log (default 0: they cost CPU).\n"
		"// Turn on for a measurement run only\n"
		"// vita_profile 1\n"
		"\n"
		"// Controls (default): R fire, L alt fire, cross jump, square use,\n"
		"// triangle flashlight, circle screenshot (vita_screenshot), rear\n"
		"// touchpad left half crouch (STICK1; the right half is STICK2, unbound).\n"
		"// Rebind any button\n"
		"// here, e.g. (names: A_BUTTON B_BUTTON X_BUTTON Y_BUTTON L_SHOULDER\n"
		"// R_SHOULDER BACK START UP DOWN LEFT RIGHT STICK1 STICK2)\n"
		"// bind STICK2 +reload\n"
		"\n"
		"// Screenshot of the next frame to ux0:data/hl2_shot_NN.tga (for\n"
		"// reporting visual glitches): on circle by default\n"
		"// bind B_BUTTON vita_screenshot\n"
		"\n"
		"// Frame rate counter\n"
		"// cl_showfps 1\n";
	SceUID fd = sceIoOpen( examplePath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if ( fd >= 0 )
	{
		sceIoWrite( fd, s_szConfig, sizeof( s_szConfig ) - 1 );
		sceIoClose( fd );
	}
	SceIoStat st;
	if ( sceIoGetstat( path, &st ) >= 0 )
		return;
	fd = sceIoOpen( path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if ( fd < 0 )
		return;
	sceIoWrite( fd, s_szConfig, sizeof( s_szConfig ) - 1 );
	sceIoClose( fd );
	so_trace( "wrote %s", path );
}

static volatile bool s_bEngineDone = false;

static void *EngineThread( void *param )
{
	EngineArgs *args = (EngineArgs *)param;

	// Left on its default affinity here, so threads it creates don't inherit a
	// pin; engine/host.cpp pins it each frame (vita_engine_core). The job
	// pool's workers pin themselves to cores 1 and 2, the render thread to 2.
	so_thread_register( "engine" );
	void *launcher = dlopen( "liblauncher.so", RTLD_NOW );
	if ( !launcher )
		FatalError( dlerror() );

	LauncherMain_t launcherMain = (LauncherMain_t)dlsym( launcher, "LauncherMain" );
	if ( !launcherMain )
		FatalError( "liblauncher.so has no LauncherMain" );

	so_trace( "calling LauncherMain" );
	args->result = launcherMain( args->argc, args->argv );
	so_trace( "LauncherMain returned %d", args->result );
	s_bEngineDone = true;
	return NULL;
}

int main( int argc, char **argv )
{
	// The last runs' logs are kept: hl2_vita_boot.1.txt is the previous run's,
	// up to .4, so a crash's log survives relaunching the game
	sceIoRemove( "ux0:data/hl2_vita_boot.4.txt" );
	for ( int i = 3; i >= 1; i-- )
	{
		char from[64], to[64];
		snprintf( from, sizeof( from ), "ux0:data/hl2_vita_boot.%d.txt", i );
		snprintf( to, sizeof( to ), "ux0:data/hl2_vita_boot.%d.txt", i + 1 );
		sceIoRename( from, to );
	}
	sceIoRename( "ux0:data/hl2_vita_boot.txt", "ux0:data/hl2_vita_boot.1.txt" );
	for ( int i = 1; i <= 8; i++ )
	{
		char path[64];
		snprintf( path, sizeof( path ), "ux0:data/hl2_frame_%02d.tga", i );
		sceIoRemove( path );
	}
	// manual screenshots (vita_screenshot) from the last session
	for ( int i = 1; i <= 99; i++ )
	{
		char path[64];
		snprintf( path, sizeof( path ), "ux0:data/hl2_shot_%02d.tga", i );
		if ( sceIoRemove( path ) < 0 )
			break;
	}
	so_trace( "Half-Life 2 for PS Vita " VITA_PORT_VERSION ", built " __DATE__ " " __TIME__ );

	// Source converts between UTF-8 and wide strings with mbstowcs/wcstombs
	// on POSIX (tier1 V_strtowcs/V_wcstostr), which only handle non-ASCII
	// text in a UTF-8 locale; newlib starts in "C". The Linux launcher forces
	// en_US.UTF-8 for this; LC_ALL in the environment also covers vgui's
	// later setlocale( LC_CTYPE, "" ) calls.
	setenv( "LC_ALL", "C.UTF-8", 1 );
	const char *pLocale = setlocale( LC_ALL, "C.UTF-8" );
	so_trace( "locale: %s", pLocale ? pLocale : "(setlocale failed)" );

	// full speed: 444 MHz CPU, 222 MHz bus and GPU
	scePowerSetArmClockFrequency( 444 );
	scePowerSetBusClockFrequency( 222 );
	scePowerSetGpuClockFrequency( 222 );
	scePowerSetGpuXbarClockFrequency( 166 );

	// Launcher messages go to the trace above; the engine writes its own
	// engine.log in the base dir once it's running (see launcher.cpp).
	// stdout can't be redirected to a file on Vita, so plain printf output
	// is lost.

	// kubridge is a kernel plugin the user installs; without it modules
	// can't be mapped executable. Its imports can't be checked for NULL:
	// unresolved Vita import stubs are real functions that jump to 0.
	int searchBuf[2];
	if ( _vshKernelSearchModuleByName( "kubridge", searchBuf ) < 0 )
		FatalError( "kubridge is not loaded.\n\nCopy kubridge.skprx (v0.3.1_hotfix or later) to ur0:tai/, add ur0:tai/kubridge.skprx under *KERNEL in your taiHEN config.txt, and reboot.\n\nSee INSTALL_VITA.md, step 1." );
	so_trace( "kubridge found" );

	// vitaGL compiles shaders at runtime with Sony's compiler module, which
	// users extract from their own firmware (ShaRKBR33D); without it the
	// first shader compile dereferences NULL deep inside vitaGL
	SceIoStat st;
	if ( sceIoGetstat( "ur0:data/libshacccg.suprx", &st ) < 0 &&
		 sceIoGetstat( "ur0:data/external/libshacccg.suprx", &st ) < 0 )
		FatalError( "The shader compiler (libshacccg.suprx) is missing.\n\nExtract it from your Vita with ShaRKBR33D: it goes to ur0:data/.\n\nSee INSTALL_VITA.md, step 2." );
	so_trace( "libshacccg.suprx found" );
	TraceFreeMemory( "at startup" );

	so_set_library_dir( "app0:bin" );

	SceIoStat stGame, stPlatform;
	if ( chdir( VITA_BASE_DIR ) != 0 || sceIoGetstat( VITA_BASE_DIR "/hl2/gameinfo.txt", &stGame ) < 0 )
		FatalError( "Half-Life 2's game files weren't found.\n\nCopy the hl2 and platform folders from your Steam copy of Half-Life 2 to ux0:data/halflife2/ (you need ux0:data/halflife2/hl2/gameinfo.txt).\n\nSee INSTALL_VITA.md, step 4." );
	if ( sceIoGetstat( VITA_BASE_DIR "/platform", &stPlatform ) < 0 )
		FatalError( "The platform folder is missing.\n\nCopy the platform folder from your Steam copy of Half-Life 2 to ux0:data/halflife2/, next to hl2.\n\nSee INSTALL_VITA.md, step 4." );
	if ( IsAnniversaryGameData() )
		FatalError( "These game files are from Half-Life 2's 20th anniversary update, which this port can't run.\n\nIn Steam, switch Half-Life 2 to the beta \"steam_legacy - Pre-20th Anniversary Build\", then copy the hl2 folder again.\n\nSee INSTALL_VITA.md, step 3." );
	WriteDefaultUserConfig();

	static EngineArgs s_Args;
	s_Args.argv[s_Args.argc++] = (char *)"hl2_vita";
	s_Args.argv[s_Args.argc++] = (char *)"-game";
	s_Args.argv[s_Args.argc++] = (char *)"hl2";
	s_Args.argv[s_Args.argc++] = (char *)"-basedir";
	s_Args.argv[s_Args.argc++] = (char *)VITA_BASE_DIR;
	ReadExtraArgs( &s_Args );
	for ( int i = 1; i < s_Args.argc; i++ )
		if ( !strcmp( s_Args.argv[i], "-vitadebug" ) )
			s_bVitaDebug = true;
#ifdef VITA_DEV_BUILD
	// testing builds (build.sh HL2_VITA_DEV=1) always log in detail
	if ( !s_bVitaDebug && s_Args.argc < VITA_MAX_ARGS - 2 )
	{
		s_Args.argv[s_Args.argc++] = (char *)"-vitadebug";
		s_bVitaDebug = true;
	}
#endif
	so_trace( "detailed logs %s", s_bVitaDebug ? "on (-vitadebug)" : "off (add -vitadebug to commandline.txt for them)" );
	// -vita_largeheap <MB>: size of the arena for 256 KB+ blocks, carved out
	// of the heap above (see so_loader.cpp); 0 keeps one heap
	unsigned nLargeHeapMB = 72;
	for ( int i = 1; i < s_Args.argc; i++ )
	{
		if ( i + 1 < s_Args.argc && !strcmp( s_Args.argv[i], "-vita_largeheap" ) )
			nLargeHeapMB = (unsigned)atoi( s_Args.argv[i + 1] );
	}
	so_large_heap_init( nLargeHeapMB );
	so_install_crash_handler( VITA_BASE_DIR "/crash.log" );
	so_trace( "crash handler installed" );
	// Render size: -w/-h from commandline.txt, else 640x368. Windowed, since
	// the fullscreen path only accepts the display modes SDL lists (960x544)
	bool bHaveW = false, bHaveH = false, bHaveWindowed = false;
	for ( int i = 1; i < s_Args.argc; i++ )
	{
		if ( i + 1 < s_Args.argc && ( !strcmp( s_Args.argv[i], "-w" ) || !strcmp( s_Args.argv[i], "-width" ) ) )
		{
			s_nDisplayW = atoi( s_Args.argv[i + 1] );
			bHaveW = true;
		}
		if ( i + 1 < s_Args.argc && ( !strcmp( s_Args.argv[i], "-h" ) || !strcmp( s_Args.argv[i], "-height" ) ) )
		{
			s_nDisplayH = atoi( s_Args.argv[i + 1] );
			bHaveH = true;
		}
		if ( !strcmp( s_Args.argv[i], "-windowed" ) || !strcmp( s_Args.argv[i], "-sw" ) )
			bHaveWindowed = true;
	}
	static char s_szW[16], s_szH[16];
	snprintf( s_szW, sizeof( s_szW ), "%d", s_nDisplayW );
	snprintf( s_szH, sizeof( s_szH ), "%d", s_nDisplayH );
	if ( !bHaveW && s_Args.argc < VITA_MAX_ARGS - 3 )
	{
		s_Args.argv[s_Args.argc++] = (char *)"-w";
		s_Args.argv[s_Args.argc++] = s_szW;
	}
	if ( !bHaveH && s_Args.argc < VITA_MAX_ARGS - 3 )
	{
		s_Args.argv[s_Args.argc++] = (char *)"-h";
		s_Args.argv[s_Args.argc++] = s_szH;
	}
	if ( !bHaveWindowed && s_Args.argc < VITA_MAX_ARGS - 2 )
		s_Args.argv[s_Args.argc++] = (char *)"-windowed";
	// Server tick rate 33 unless commandline.txt sets one: player commands,
	// physics steps and per-tick engine work run half as often as at 66 (the
	// server frame was the frame-rate limit); engine/host.cpp turns
	// sv_alternateticks off at this rate so entities still think 33x a second
	bool bHaveTickrate = false, bMenuMap = false;
	for ( int i = 1; i < s_Args.argc; i++ )
	{
		if ( !strcmp( s_Args.argv[i], "-tickrate" ) )
			bHaveTickrate = true;
		if ( !strcmp( s_Args.argv[i], "-menumap" ) )
			bMenuMap = true;
	}
	// Static main menu: GameUI draws console/background01 instead of
	// loading a background map at startup (the map load took most of the
	// time to the menu). "-menumap" in commandline.txt brings it back.
	if ( !bMenuMap && s_Args.argc < VITA_MAX_ARGS - 2 )
		s_Args.argv[s_Args.argc++] = (char *)"-nobackgroundlevel";
	if ( !bHaveTickrate && s_Args.argc < VITA_MAX_ARGS - 3 )
	{
		s_Args.argv[s_Args.argc++] = (char *)"-tickrate";
		s_Args.argv[s_Args.argc++] = (char *)"33";
	}
	so_trace( "render size %dx%d", s_nDisplayW, s_nDisplayH );
	s_Args.argv[s_Args.argc] = NULL;

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, ENGINE_THREAD_STACK_SIZE );

	pthread_t thread;
	if ( pthread_create( &thread, &attr, EngineThread, &s_Args ) != 0 )
		FatalError( "The game couldn't start (no engine thread).\n\nRestart the Vita and try again. If it keeps happening, report it with ux0:data/hl2_vita_boot.txt." );

	// Keep the screen on and the system awake while the engine runs: without
	// input the Vita dims and turns off the OLED after a few minutes, and
	// level loads take far longer than that with no events pumped (SDL only
	// ticks from its event pump)
	for ( int nTick = 1; !s_bEngineDone; nTick++ )
	{
		sceKernelPowerTick( SCE_KERNEL_POWER_TICK_DEFAULT );
		if ( nTick % ( s_bVitaDebug ? 10 : 60 ) == 0 )
		{
			so_perf_log();
			// kernel memory left for module images, thread stacks and the
			// engine hunk (committed on demand); logged when it moves by 1 MB+
			static int s_nLastFreeKB = -1;
			SceKernelFreeMemorySizeInfo info;
			info.size = sizeof( info );
			sceKernelGetFreeMemorySize( &info );
			int nFreeKB = info.size_user / 1024;
			if ( s_nLastFreeKB < 0 || abs( nFreeKB - s_nLastFreeKB ) >= 1024 )
			{
				TraceFreeMemory( "now" );
				s_nLastFreeKB = nFreeKB;
			}
		}
		sceKernelDelayThread( 1000 * 1000 );
	}
	pthread_join( thread, NULL );

	so_exit_process( 0 );
	return 0;
}

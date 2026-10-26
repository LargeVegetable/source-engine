//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Minimal ELF shared object loader for PS Vita.
//
// Engine modules are linked as PIC shared objects without libc (see
// scripts/waifulib/xcompile.py). Loading one:
//   1. find a free virtual address range for the whole image
//   2. build the image in a heap staging buffer and apply its relocations,
//      resolving imports against the launcher's export table first and then
//      the module's own dependency tree, like dlopen(RTLD_LOCAL) on Linux
//   3. allocate a memory block per PT_LOAD segment at the matching address
//      through kubridge (user code can't map executable memory on its own),
//      copy the segments in and flush caches
//   4. run the module's constructors
//
// Modules are never unloaded; dlclose only drops a reference.
//
//=============================================================================//
#include "so_loader.h"
#include "game.h"

#include <elf.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <reent.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/motion.h>
#include <math.h>
#include <kubridge.h>

#include <vector>

// not exposed by the SDK headers; RW (0x0C20D060) without write, with execute
#define SO_MEMBLOCK_TYPE_USER_RX 0x0C20D050
// SceKernelAllocMemBlockKernelOpt::attr bit selecting a fixed address (field_C)
#define SO_MEMBLOCK_ATTR_HAS_VBASE 0x1

#define SO_PAGE_SIZE 0x1000
#define SO_PAGE_DOWN( x ) ( (x) & ~(uintptr_t)( SO_PAGE_SIZE - 1 ) )
#define SO_PAGE_UP( x ) SO_PAGE_DOWN( (x) + SO_PAGE_SIZE - 1 )

// address space probes are rounded to this, to keep blocks apart
#define SO_RESERVE_ALIGN 0x100000

struct SoSegment
{
	uintptr_t addr; // final address, page aligned
	size_t size;
	bool exec;
	SceUID block;
};

struct SoModule
{
	char path[512];
	const char *name; // file name part of path
	uintptr_t base;
	size_t span;
	int refcount;

	std::vector<SoSegment> segments;

	// pointers into the loaded image
	const Elf32_Sym *dynsym;
	const char *dynstr;
	const uint32_t *hash; // DT_HASH: nbucket, nchain, buckets[], chains[]
	uintptr_t exidx;
	size_t exidx_count;

	std::vector<SoModule *> deps; // DT_NEEDED, in order
};

extern "C" const uint32_t __exidx_start[];
extern "C" const uint32_t __exidx_end[];

static std::vector<SoModule *> s_Modules;
static pthread_mutex_t s_Mutex;
static pthread_once_t s_MutexOnce = PTHREAD_ONCE_INIT;
static char s_LibraryDir[256] = "app0:bin";
static char s_Error[512];

// handle returned by dlopen(NULL), standing for the global scope
static char s_MainProgramHandle;

static void InitMutex()
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init( &attr );
	pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
	pthread_mutex_init( &s_Mutex, &attr );
	pthread_mutexattr_destroy( &attr );
}

static void Lock()
{
	pthread_once( &s_MutexOnce, InitMutex );
	pthread_mutex_lock( &s_Mutex );
}

static void Unlock()
{
	pthread_mutex_unlock( &s_Mutex );
}

extern "C" void so_trace( const char *fmt, ... )
{
	char line[512];
	va_list args;
	va_start( args, fmt );
	int len = vsnprintf( line, sizeof( line ) - 1, fmt, args );
	va_end( args );
	if ( len < 0 )
		return;
	if ( len > (int)sizeof( line ) - 2 )
		len = sizeof( line ) - 2;
	line[len++] = '\n';
	line[len] = '\0';

	so_log_write( line );
}

// The log stays open: sceIoWrite has no user-side buffering, so lines reach
// the file even if the process dies right after. Reopening per line made
// logging a large share of startup time.
static SceUID s_LogFd = -1;
static pthread_once_t s_LogOnce = PTHREAD_ONCE_INIT;

static void OpenLog()
{
	s_LogFd = sceIoOpen( "ux0:data/" VITA_FILE_PREFIX "_vita_boot.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777 );
}

extern "C" void so_log_write( const char *text )
{
	pthread_once( &s_LogOnce, OpenLog );
	if ( s_LogFd >= 0 )
		sceIoWrite( s_LogFd, text, strlen( text ) );
}

static void SetError( const char *fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	vsnprintf( s_Error, sizeof( s_Error ), fmt, args );
	va_end( args );
	so_trace( "error: %s", s_Error );
}

static const char *FileName( const char *path )
{
	const char *slash = strrchr( path, '/' );
	const char *colon = strrchr( path, ':' );
	const char *sep = slash > colon ? slash : colon;
	return sep ? sep + 1 : path;
}

//-----------------------------------------------------------------------------
// Symbol lookup
//-----------------------------------------------------------------------------
static uint32_t ElfHash( const char *name )
{
	uint32_t h = 0;
	while ( *name )
	{
		h = ( h << 4 ) + (uint8_t)*name++;
		uint32_t g = h & 0xf0000000;
		if ( g )
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

// Returns a symbol the module defines (not imports), or NULL
static const Elf32_Sym *FindDefinedSymbol( const SoModule *mod, const char *name, uint32_t hash )
{
	if ( !mod->hash )
		return NULL;

	uint32_t nbucket = mod->hash[0];
	const uint32_t *buckets = &mod->hash[2];
	const uint32_t *chains = &mod->hash[2 + nbucket];

	for ( uint32_t i = buckets[hash % nbucket]; i != STN_UNDEF; i = chains[i] )
	{
		const Elf32_Sym *sym = &mod->dynsym[i];
		if ( sym->st_shndx == SHN_UNDEF )
			continue;
		if ( ELF32_ST_BIND( sym->st_info ) == STB_LOCAL )
			continue;
		if ( strcmp( mod->dynstr + sym->st_name, name ) == 0 )
			return sym;
	}
	return NULL;
}

static void *FindExport( const char *name )
{
	int lo = 0, hi = g_VitaExportCount - 1;
	while ( lo <= hi )
	{
		int mid = ( lo + hi ) / 2;
		int cmp = strcmp( name, g_VitaExports[mid].name );
		if ( cmp == 0 )
			return g_VitaExports[mid].addr;
		if ( cmp < 0 )
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

// Breadth-first local scope of a module: itself, then its dependencies
static void BuildScope( SoModule *mod, std::vector<SoModule *> &scope )
{
	scope.clear();
	scope.push_back( mod );
	for ( size_t i = 0; i < scope.size(); i++ )
	{
		for ( SoModule *dep : scope[i]->deps )
		{
			bool seen = false;
			for ( SoModule *m : scope )
				seen |= ( m == dep );
			if ( !seen )
				scope.push_back( dep );
		}
	}
}

static bool ResolveSymbol( const std::vector<SoModule *> &scope, const char *name, uintptr_t *addr )
{
	void *exported = FindExport( name );
	if ( exported )
	{
		*addr = (uintptr_t)exported;
		return true;
	}

	uint32_t hash = ElfHash( name );
	for ( SoModule *mod : scope )
	{
		const Elf32_Sym *sym = FindDefinedSymbol( mod, name, hash );
		if ( sym )
		{
			*addr = mod->base + sym->st_value;
			return true;
		}
	}
	return false;
}

//-----------------------------------------------------------------------------
// Loading
//-----------------------------------------------------------------------------
static void *ReadFile( const char *path, size_t *size )
{
	FILE *f = fopen( path, "rb" );
	if ( !f )
		return NULL;

	fseek( f, 0, SEEK_END );
	long len = ftell( f );
	fseek( f, 0, SEEK_SET );

	void *data = len > 0 ? malloc( len ) : NULL;
	if ( data && fread( data, 1, len, f ) != (size_t)len )
	{
		free( data );
		data = NULL;
	}
	fclose( f );

	*size = len;
	return data;
}

static uintptr_t ReserveAddressRange( size_t size )
{
	// Let the kernel pick a free range by allocating a block of the full size,
	// then release it so the segments can be allocated at fixed addresses
	// inside it. Nothing else allocates while the loader lock is held.
	size = ( size + SO_RESERVE_ALIGN - 1 ) & ~(size_t)( SO_RESERVE_ALIGN - 1 );
	SceUID probe = sceKernelAllocMemBlock( "so_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL );
	if ( probe < 0 )
		return 0;

	void *base = NULL;
	sceKernelGetMemBlockBase( probe, &base );
	sceKernelFreeMemBlock( probe );
	return (uintptr_t)base;
}

static void FreeSegments( SoModule *mod )
{
	for ( SoSegment &seg : mod->segments )
	{
		if ( seg.block >= 0 )
			sceKernelFreeMemBlock( seg.block );
	}
	mod->segments.clear();
}

static bool Relocate( SoModule *mod, uint8_t *image, const Elf32_Rel *rels, size_t count, const std::vector<SoModule *> &scope )
{
	bool ok = true;

	for ( size_t i = 0; i < count; i++ )
	{
		const Elf32_Rel *rel = &rels[i];
		uint32_t *where = (uint32_t *)( image + rel->r_offset );
		uint32_t type = ELF32_R_TYPE( rel->r_info );
		const Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM( rel->r_info )];

		if ( type == R_ARM_NONE )
			continue;

		if ( type == R_ARM_RELATIVE )
		{
			*where += mod->base;
			continue;
		}

		if ( type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT )
		{
			SetError( "%s: unsupported relocation type %u", mod->name, type );
			return false;
		}

		const char *name = mod->dynstr + sym->st_name;
		uintptr_t value = 0;
		if ( ELF32_R_SYM( rel->r_info ) == STN_UNDEF )
		{
			// no symbol: S is 0
		}
		else if ( !ResolveSymbol( scope, name, &value ) )
		{
			if ( ELF32_ST_BIND( sym->st_info ) != STB_WEAK )
			{
				// keep going so every missing import gets reported at once
				SetError( "%s: unresolved symbol %s", mod->name, name );
				ok = false;
			}
		}

		if ( type == R_ARM_ABS32 )
			*where += value;
		else
			*where = value;
	}

	return ok;
}

static SoModule *LoadModule( const char *path );

static SoModule *FindLoaded( const char *name )
{
	for ( SoModule *mod : s_Modules )
	{
		if ( strcmp( mod->name, name ) == 0 )
			return mod;
	}
	return NULL;
}

static SoModule *LoadModuleFromFile( const char *path, uint8_t *file, size_t file_size )
{
	const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)file;
	if ( file_size < sizeof( Elf32_Ehdr ) || memcmp( ehdr->e_ident, ELFMAG, SELFMAG ) != 0 ||
		 ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_machine != EM_ARM || ehdr->e_type != ET_DYN )
	{
		SetError( "%s: not an ARM shared object", path );
		return NULL;
	}

	const Elf32_Phdr *phdrs = (const Elf32_Phdr *)( file + ehdr->e_phoff );

	SoModule *mod = new SoModule();
	snprintf( mod->path, sizeof( mod->path ), "%s", path );
	mod->name = FileName( mod->path );
	mod->refcount = 1;

	uintptr_t dynamic_vaddr = 0;
	for ( int i = 0; i < ehdr->e_phnum; i++ )
	{
		const Elf32_Phdr *ph = &phdrs[i];
		if ( ph->p_type == PT_LOAD )
		{
			uintptr_t end = SO_PAGE_UP( ph->p_vaddr + ph->p_memsz );
			if ( end > mod->span )
				mod->span = end;
		}
		else if ( ph->p_type == PT_DYNAMIC )
		{
			dynamic_vaddr = ph->p_vaddr;
		}
		else if ( ph->p_type == PT_ARM_EXIDX )
		{
			mod->exidx = ph->p_vaddr;
			mod->exidx_count = ph->p_memsz / 8;
		}
	}

	if ( !dynamic_vaddr )
	{
		SetError( "%s: no dynamic section", path );
		delete mod;
		return NULL;
	}

	// build the image in a staging buffer; it moves to the final blocks once
	// relocated, since text can only be written there through kubridge
	uint8_t *image = (uint8_t *)memalign( SO_PAGE_SIZE, mod->span );
	if ( !image )
	{
		SetError( "%s: out of memory staging %u bytes", path, (unsigned)mod->span );
		delete mod;
		return NULL;
	}
	memset( image, 0, mod->span );

	for ( int i = 0; i < ehdr->e_phnum; i++ )
	{
		const Elf32_Phdr *ph = &phdrs[i];
		if ( ph->p_type == PT_LOAD )
			memcpy( image + ph->p_vaddr, file + ph->p_offset, ph->p_filesz );
	}

	// symbol tables are read from the staging image while relocating, and
	// from the final image afterwards
	const Elf32_Dyn *dyn = (const Elf32_Dyn *)( image + dynamic_vaddr );
	uintptr_t symtab = 0, strtab = 0, hash = 0, rel = 0, jmprel = 0;
	size_t relsz = 0, pltrelsz = 0, init_arraysz = 0;
	uintptr_t init = 0, init_array = 0;

	for ( ; dyn->d_tag != DT_NULL; dyn++ )
	{
		switch ( dyn->d_tag )
		{
		case DT_SYMTAB: symtab = dyn->d_un.d_ptr; break;
		case DT_STRTAB: strtab = dyn->d_un.d_ptr; break;
		case DT_HASH: hash = dyn->d_un.d_ptr; break;
		case DT_REL: rel = dyn->d_un.d_ptr; break;
		case DT_RELSZ: relsz = dyn->d_un.d_val; break;
		case DT_JMPREL: jmprel = dyn->d_un.d_ptr; break;
		case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
		case DT_INIT: init = dyn->d_un.d_ptr; break;
		case DT_INIT_ARRAY: init_array = dyn->d_un.d_ptr; break;
		case DT_INIT_ARRAYSZ: init_arraysz = dyn->d_un.d_val; break;
		case DT_RELA:
			SetError( "%s: RELA relocations are not supported", path );
			free( image );
			delete mod;
			return NULL;
		}
	}

	mod->dynsym = (const Elf32_Sym *)( image + symtab );
	mod->dynstr = (const char *)( image + strtab );
	mod->hash = hash ? (const uint32_t *)( image + hash ) : NULL;

	// dependencies first, so their constructors have run before ours
	dyn = (const Elf32_Dyn *)( image + dynamic_vaddr );
	for ( ; dyn->d_tag != DT_NULL; dyn++ )
	{
		if ( dyn->d_tag != DT_NEEDED )
			continue;

		const char *needed = mod->dynstr + dyn->d_un.d_val;
		SoModule *dep = LoadModule( needed );
		if ( !dep )
		{
			SetError( "%s: can't load dependency %s", path, needed );
			free( image );
			delete mod;
			return NULL;
		}
		mod->deps.push_back( dep );
	}

	// Only now pick our address: the probe block is freed right away, so a
	// range picked before loading the dependencies would be handed to them
	// too. From here until our segments are mapped, nothing else allocates
	// memory blocks.
	mod->base = ReserveAddressRange( mod->span );
	so_trace( "%s: span 0x%x at 0x%08x", mod->name, (unsigned)mod->span, (unsigned)mod->base );
	if ( !mod->base )
	{
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof( info );
		sceKernelGetFreeMemorySize( &info );
		SetError( "%s: no memory for %u bytes (%d KB free)", path, (unsigned)mod->span, info.size_user / 1024 );
		free( image );
		delete mod;
		return NULL;
	}

	std::vector<SoModule *> scope;
	BuildScope( mod, scope );

	bool ok = Relocate( mod, image, (const Elf32_Rel *)( image + rel ), relsz / sizeof( Elf32_Rel ), scope );
	ok &= Relocate( mod, image, (const Elf32_Rel *)( image + jmprel ), pltrelsz / sizeof( Elf32_Rel ), scope );
	if ( !ok )
	{
		free( image );
		delete mod;
		return NULL;
	}

	for ( int i = 0; i < ehdr->e_phnum; i++ )
	{
		const Elf32_Phdr *ph = &phdrs[i];
		if ( ph->p_type != PT_LOAD )
			continue;

		SoSegment seg;
		seg.addr = mod->base + SO_PAGE_DOWN( ph->p_vaddr );
		seg.size = SO_PAGE_UP( mod->base + ph->p_vaddr + ph->p_memsz ) - seg.addr;
		seg.exec = ( ph->p_flags & PF_X ) != 0;

		SceKernelAllocMemBlockKernelOpt opt;
		memset( &opt, 0, sizeof( opt ) );
		opt.size = sizeof( opt );
		opt.attr = SO_MEMBLOCK_ATTR_HAS_VBASE;
		opt.field_C = seg.addr;

		seg.block = kuKernelAllocMemBlock( seg.exec ? "so_text" : "so_data",
			seg.exec ? SO_MEMBLOCK_TYPE_USER_RX : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, seg.size, &opt );
		mod->segments.push_back( seg );

		if ( seg.block < 0 )
		{
			SetError( "%s: can't map %u bytes at 0x%08x (0x%08x)", path, (unsigned)seg.size, (unsigned)seg.addr, seg.block );
			FreeSegments( mod );
			free( image );
			delete mod;
			return NULL;
		}

		so_trace( "%s: mapped %s 0x%08x size 0x%x", mod->name, seg.exec ? "text" : "data", (unsigned)seg.addr, (unsigned)seg.size );
		const uint8_t *src = image + ( seg.addr - mod->base );
		if ( seg.exec )
		{
			kuKernelCpuUnrestrictedMemcpy( (void *)seg.addr, src, seg.size );
			kuKernelFlushCaches( (void *)seg.addr, seg.size );
		}
		else
		{
			memcpy( (void *)seg.addr, src, seg.size );
		}
	}

	free( image );

	mod->dynsym = (const Elf32_Sym *)( mod->base + symtab );
	mod->dynstr = (const char *)( mod->base + strtab );
	mod->hash = hash ? (const uint32_t *)( mod->base + hash ) : NULL;

	s_Modules.push_back( mod );

	// constructors last, once the module can be found by dladdr/dlsym
	so_trace( "%s: running constructors", mod->name );
	typedef void ( *InitFn )( void );
	if ( init )
		( (InitFn)( mod->base + init ) )();

	InitFn *ctors = (InitFn *)( mod->base + init_array );
	for ( size_t i = 0; i < init_arraysz / sizeof( InitFn ); i++ )
	{
		if ( ctors[i] && ctors[i] != (InitFn)-1 )
			ctors[i]();
	}

	so_trace( "%s: loaded at 0x%08x (%u KB)", mod->name, (unsigned)mod->base, (unsigned)( mod->span / 1024 ) );
	return mod;
}

static SoModule *LoadModule( const char *path )
{
	SoModule *loaded = FindLoaded( FileName( path ) );
	if ( loaded )
	{
		loaded->refcount++;
		return loaded;
	}

	char fallback[512];
	size_t size = 0;
	uint8_t *file = (uint8_t *)ReadFile( path, &size );
	if ( !file )
	{
		// engine paths point at bin/ dirs next to the game data; modules
		// ship in the app's own bin dir instead
		snprintf( fallback, sizeof( fallback ), "%s/%s", s_LibraryDir, FileName( path ) );
		file = (uint8_t *)ReadFile( fallback, &size );
		path = fallback;
	}

	if ( !file )
	{
		SetError( "%s: no such file", path );
		return NULL;
	}

	so_trace( "loading %s (%u bytes)", path, (unsigned)size );
	SoModule *mod = LoadModuleFromFile( path, file, size );
	free( file );
	return mod;
}

//-----------------------------------------------------------------------------
// dlfcn.h
//-----------------------------------------------------------------------------
extern "C" void *dlopen( const char *filename, int flags )
{
	if ( !filename )
		return &s_MainProgramHandle;

	Lock();
	SoModule *mod = NULL;
	if ( flags & RTLD_NOLOAD )
	{
		mod = FindLoaded( FileName( filename ) );
		if ( mod )
			mod->refcount++;
	}
	else
	{
		mod = LoadModule( filename );
	}
	Unlock();
	return mod;
}

extern "C" void *dlsym( void *handle, const char *symbol )
{
	Lock();

	uintptr_t addr = 0;
	bool found = false;
	if ( handle == RTLD_DEFAULT || handle == &s_MainProgramHandle )
	{
		void *exported = FindExport( symbol );
		if ( exported )
		{
			addr = (uintptr_t)exported;
			found = true;
		}

		uint32_t hash = ElfHash( symbol );
		for ( size_t i = 0; !found && i < s_Modules.size(); i++ )
		{
			const Elf32_Sym *sym = FindDefinedSymbol( s_Modules[i], symbol, hash );
			if ( sym )
			{
				addr = s_Modules[i]->base + sym->st_value;
				found = true;
			}
		}
	}
	else
	{
		// like glibc: the module itself, then its dependencies
		SoModule *mod = (SoModule *)handle;
		std::vector<SoModule *> scope;
		BuildScope( mod, scope );

		uint32_t hash = ElfHash( symbol );
		for ( size_t i = 0; !found && i < scope.size(); i++ )
		{
			const Elf32_Sym *sym = FindDefinedSymbol( scope[i], symbol, hash );
			if ( sym )
			{
				addr = scope[i]->base + sym->st_value;
				found = true;
			}
		}
	}

	if ( !found )
		snprintf( s_Error, sizeof( s_Error ), "undefined symbol: %s", symbol );

	Unlock();
	return (void *)addr;
}

extern "C" int dlclose( void *handle )
{
	if ( handle && handle != RTLD_DEFAULT && handle != &s_MainProgramHandle )
	{
		Lock();
		( (SoModule *)handle )->refcount--;
		Unlock();
	}
	return 0;
}

extern "C" char *dlerror( void )
{
	static char s_Returned[sizeof( s_Error )];
	if ( !s_Error[0] )
		return NULL;

	memcpy( s_Returned, s_Error, sizeof( s_Error ) );
	s_Error[0] = '\0';
	return s_Returned;
}

static SoModule *ModuleForAddress( uintptr_t addr )
{
	for ( SoModule *mod : s_Modules )
	{
		if ( addr >= mod->base && addr < mod->base + mod->span )
			return mod;
	}
	return NULL;
}

extern "C" int dladdr( const void *addr, Dl_info *info )
{
	Lock();
	SoModule *mod = ModuleForAddress( (uintptr_t)addr );
	if ( !mod )
	{
		Unlock();
		return 0;
	}

	info->dli_fname = mod->path;
	info->dli_fbase = (void *)mod->base;
	info->dli_sname = NULL;
	info->dli_saddr = NULL;

	// nearest preceding symbol; the hash chain array has one entry per symbol
	uintptr_t offset = (uintptr_t)addr - mod->base;
	uint32_t nsyms = mod->hash ? mod->hash[1] : 0;
	uintptr_t best = 0;
	for ( uint32_t i = 1; i < nsyms; i++ )
	{
		const Elf32_Sym *sym = &mod->dynsym[i];
		uintptr_t value = sym->st_value & ~(uintptr_t)1; // thumb bit
		if ( sym->st_shndx == SHN_UNDEF || value > offset || value < best )
			continue;

		best = value;
		info->dli_sname = mod->dynstr + sym->st_name;
		info->dli_saddr = (void *)( mod->base + sym->st_value );
	}

	Unlock();
	return 1;
}

//-----------------------------------------------------------------------------
// C++ exception support: lets libgcc's unwinder find the exception index
// tables of loaded modules. Weak in libgcc, which otherwise only knows the
// executable's own table.
//-----------------------------------------------------------------------------
extern "C" uintptr_t __gnu_Unwind_Find_exidx( uintptr_t pc, int *count )
{
	// called during unwinding, so no locking; modules are never removed and
	// a module is only added to the list once it's fully loaded
	for ( SoModule *mod : s_Modules )
	{
		if ( pc >= mod->base && pc < mod->base + mod->span )
		{
			*count = mod->exidx_count;
			return mod->exidx ? mod->base + mod->exidx : 0;
		}
	}

	*count = __exidx_end - __exidx_start;
	*count /= 2; // entries are two words
	return (uintptr_t)__exidx_start;
}

//-----------------------------------------------------------------------------
// Misc
//-----------------------------------------------------------------------------
extern "C" void so_set_library_dir( const char *dir )
{
	snprintf( s_LibraryDir, sizeof( s_LibraryDir ), "%s", dir );
}

extern "C" const char *so_module_for_address( uintptr_t addr, uintptr_t *offset )
{
	SoModule *mod = ModuleForAddress( addr );
	if ( !mod )
		return NULL;
	*offset = addr - mod->base;
	return mod->name;
}

// Logs the return addresses found on the calling thread's stack, from here
// up to the top of the stack: module+offset for loaded modules, and odd
// (Thumb) addresses near this function for the executable. Map those with
// ELF address = addr - ref + nm address of so_log_backtrace
extern "C" void so_log_backtrace( const char *tag )
{
	SceKernelThreadInfo info;
	info.size = sizeof( info );
	if ( sceKernelGetThreadInfo( sceKernelGetThreadId(), &info ) < 0 )
		return;

	volatile uint32_t marker = 0;
	const uint32_t *word = (const uint32_t *)( (uintptr_t)&marker & ~3u );
	const uint32_t *top = (const uint32_t *)( (uintptr_t)info.stack + info.stackSize );
	const uint32_t ref = (uint32_t)(uintptr_t)&so_log_backtrace;

	so_trace( "backtrace (%s): thread %s, ref 0x%08x", tag, info.name, ref );
	int nLogged = 0;
	for ( ; word < top && nLogged < 48; word++ )
	{
		uintptr_t offset;
		const char *name = so_module_for_address( *word, &offset );
		if ( name )
		{
			so_trace( "  %s+0x%x", name, (unsigned)offset );
			nLogged++;
		}
		else if ( ( *word & 1 ) && *word + 0x100000 > ref && *word < ref + 0x400000 )
		{
			so_trace( "  exe 0x%08x", (unsigned)*word );
			nLogged++;
		}
	}
}

// Load-time / performance counters (common/vita/vita_perf.h): calls, time
// and an amount (bytes) per counter, added from any thread
#define SO_PERF_COUNTERS 32
static uint64_t s_PerfCalls[SO_PERF_COUNTERS];
static uint64_t s_PerfTime[SO_PERF_COUNTERS];
static uint64_t s_PerfAmount[SO_PERF_COUNTERS];

extern "C" uint64_t so_perf_now( void )
{
	return sceKernelGetProcessTimeWide();
}

// Count only, no clock read: for per-draw/per-call events (draws, uniform
// uploads, traces), where two kernel time reads per call cost more than the
// timing told us
extern "C" void so_perf_count( int counter, uint64_t amount )
{
	if ( counter < 0 || counter >= SO_PERF_COUNTERS )
		return;
	__atomic_add_fetch( &s_PerfCalls[counter], 1, __ATOMIC_RELAXED );
	__atomic_add_fetch( &s_PerfAmount[counter], amount, __ATOMIC_RELAXED );
}

// Shader compiles so far (VITA_PERF_SHADER_COMPILE: vitaGL's glCompileShader,
// a cache read or, on a cold cache, a compile on the Vita) and their time
extern "C" void so_shader_compiles( uint64_t *pCalls, uint64_t *pMicroseconds )
{
	so_perf_read( 0, pCalls, pMicroseconds );
}

// Files opened since the last swap, by type, for the in-game long-frame
// line (main.cpp): what a level's first frame reads. Any thread
static pthread_mutex_t s_FileNoteLock = PTHREAD_MUTEX_INITIALIZER;
struct SoFileType
{
	char m_szExt[8];
	unsigned m_nOpens;
};
static SoFileType s_FileTypes[12];
static int s_nFileTypes;
static char s_szFirstFiles[3][64];
static unsigned s_nFileNotes;

extern "C" void so_file_open_note( const char *path )
{
	const char *pDot = strrchr( path, '.' );
	const char *pExt = ( pDot && !strchr( pDot, '/' ) ) ? pDot + 1 : "-";
	pthread_mutex_lock( &s_FileNoteLock );
	if ( s_nFileNotes < 3 )
		snprintf( s_szFirstFiles[s_nFileNotes], sizeof( s_szFirstFiles[0] ), "%s", path );
	s_nFileNotes++;
	int i;
	for ( i = 0; i < s_nFileTypes; i++ )
	{
		if ( !strncmp( s_FileTypes[i].m_szExt, pExt, sizeof( s_FileTypes[i].m_szExt ) - 1 ) )
			break;
	}
	if ( i == s_nFileTypes && s_nFileTypes < 12 )
	{
		snprintf( s_FileTypes[i].m_szExt, sizeof( s_FileTypes[i].m_szExt ), "%s", pExt );
		s_FileTypes[i].m_nOpens = 0;
		s_nFileTypes++;
	}
	if ( i < s_nFileTypes )
		s_FileTypes[i].m_nOpens++;
	pthread_mutex_unlock( &s_FileNoteLock );
}

// "wav 120, vtf 30 (first: a, b, c)" for the opens since the last call
// (empty if none), and starts over
extern "C" void so_file_notes_take( char *pszOut, int nOutSize )
{
	pszOut[0] = 0;
	if ( !s_nFileNotes )
		return;
	pthread_mutex_lock( &s_FileNoteLock );
	int nLen = 0;
	for ( int i = 0; i < s_nFileTypes && nLen < nOutSize; i++ )
		nLen += snprintf( pszOut + nLen, nOutSize - nLen, "%s%s %u", i ? ", " : "", s_FileTypes[i].m_szExt, s_FileTypes[i].m_nOpens );
	for ( unsigned i = 0; i < s_nFileNotes && i < 3 && nLen < nOutSize; i++ )
		nLen += snprintf( pszOut + nLen, nOutSize - nLen, "%s%s", i ? ", " : " (first: ", s_szFirstFiles[i] );
	if ( nLen < nOutSize )
		snprintf( pszOut + nLen, nOutSize - nLen, ")" );
	s_nFileTypes = 0;
	s_nFileNotes = 0;
	pthread_mutex_unlock( &s_FileNoteLock );
}

// A counter's totals so far (VITA_PERF_* in common/vita/vita_perf.h)
extern "C" void so_perf_read( int counter, uint64_t *pCalls, uint64_t *pMicroseconds )
{
	if ( counter < 0 || counter >= SO_PERF_COUNTERS )
	{
		*pCalls = *pMicroseconds = 0;
		return;
	}
	*pCalls = __atomic_load_n( &s_PerfCalls[counter], __ATOMIC_RELAXED );
	*pMicroseconds = __atomic_load_n( &s_PerfTime[counter], __ATOMIC_RELAXED );
}

// Load checkpoints: the main menu, each map load's start, its material
// precache (where Source creates the shaders) and the player getting
// control. Each line gives the shader compiles since the previous one, so
// a cold shader cache's cost shows up per step. Main thread only
static void ShaderRecordMap( const char *pszMap );

extern "C" void so_load_mark( const char *what )
{
	if ( !strncmp( what, "load ", 5 ) )
		ShaderRecordMap( what + 5 );
	static uint64_t s_nLastCalls, s_nLastTime, s_nLastAt;
	static char s_szLast[64] = "launch";
	uint64_t calls, time;
	so_shader_compiles( &calls, &time );
	const uint64_t now = sceKernelGetProcessTimeWide();
	const uint64_t dt = time - s_nLastTime, span = now - s_nLastAt;
	so_trace( "mark %s @%u.%01u s (%u.%01u s after %s): shader compiles +%u (%u.%01u s); %u in all (%u.%01u s)",
		what, (unsigned)( now / 1000000 ), (unsigned)( now / 100000 % 10 ),
		(unsigned)( span / 1000000 ), (unsigned)( span / 100000 % 10 ), s_szLast,
		(unsigned)( calls - s_nLastCalls ), (unsigned)( dt / 1000000 ), (unsigned)( dt / 100000 % 10 ),
		(unsigned)calls, (unsigned)( time / 1000000 ), (unsigned)( time / 100000 % 10 ) );
	s_nLastCalls = calls;
	s_nLastTime = time;
	s_nLastAt = now;
	snprintf( s_szLast, sizeof( s_szLast ), "%s", what );
}

extern "C" void so_perf_add( int counter, uint64_t start_us, uint64_t amount )
{
	if ( counter < 0 || counter >= SO_PERF_COUNTERS )
		return;
	uint64_t elapsed = sceKernelGetProcessTimeWide() - start_us;
	__atomic_add_fetch( &s_PerfCalls[counter], 1, __ATOMIC_RELAXED );
	__atomic_add_fetch( &s_PerfTime[counter], elapsed, __ATOMIC_RELAXED );
	__atomic_add_fetch( &s_PerfAmount[counter], amount, __ATOMIC_RELAXED );
}

// Threads logged after the frame: line (so_thread_register): the kernel's
// run time, core and core moves, read every 10 s. Costs nothing per frame;
// says whether a thread was running or blocked. (Preemption counts were
// always 0: equal-priority threads aren't time-sliced)
#define SO_THREADS 8
struct SoThread
{
	SceUID uid;			// 0 until the entry is filled in
	const char *name;	// a literal
	uint64_t lastRun;
	int32_t lastMoves;
};
static SoThread s_Threads[SO_THREADS];
static int s_nThreads;

// vitasdk's pthread-embedded sem_destroy (verified by disassembly) always
// sleeps 1 ms (pte_osThreadSleep) before destroying the semaphore's internal
// mutex, "to let other threads leave sem ops". Every pthread condition
// variable owns two semaphores, and every CThreadEvent / CJob owns one, so
// each job freed per frame slept twice, 1-7 ms each with other threads taking
// the core: the server join (the job is freed there), the render queue's job
// in EndFrame, sound loads. Same steps, but destroy first and only sleep when
// the mutex is really busy. Layout: struct sem_t_ { int value;
// pthread_mutex_t lock; SceUID sem; }; SEM_VALUE_MAX is 0x7fff
struct PteSem
{
	int value;
	pthread_mutex_t lock;
	int sem;
};
extern "C" int pte_osSemaphoreDelete( int handle );
extern "C" void pte_osThreadSleep( unsigned int msecs );
extern "C" int __wrap_sem_destroy( PteSem **sem )
{
	PteSem *s = sem ? *sem : NULL;
	if ( !s )
	{
		errno = EINVAL;
		return -1;
	}
	int result = pthread_mutex_lock( &s->lock );
	if ( result )
	{
		errno = result;
		return -1;
	}
	if ( s->value < 0 )
	{
		pthread_mutex_unlock( &s->lock );
		errno = EBUSY;
		return -1;
	}
	if ( pte_osSemaphoreDelete( s->sem ) != 0 )
	{
		pthread_mutex_unlock( &s->lock );
		errno = EINVAL;
		return -1;
	}
	*sem = NULL;
	s->value = 0x7fff;
	pthread_mutex_unlock( &s->lock );
	while ( pthread_mutex_destroy( &s->lock ) == EBUSY )
		pte_osThreadSleep( 1 );
	free( s );
	return 0;
}

// Large blocks (256 KB and up) get their own arena, carved out of the newlib
// heap at startup, so vitaGL's GPU mapping of the heap and its address checks
// still cover them. In one heap, small long-lived allocations land between
// the big transient ones (map loads, sounds, textures being read): after
// eight maps the heap had 92 MB free but no 2.2 MB hole for a sound (in
// d3_citadel_03). Either heap falls back to the other when it can't serve a
// request. sceClibMspace isn't thread-safe, hence the lock. Linked with
// --wrap (build.sh), which covers every module through the export table;
// newlib's internal _malloc_r/_free_r pairs stay on the newlib heap
#define VITA_LARGE_ALLOC ( 256 * 1024 )
static SceClibMspace s_LargeHeap;
static unsigned char *s_pLargeBase, *s_pLargeEnd;
static SceKernelLwMutexWork s_LargeLock __attribute__(( aligned( 8 ) ));
static unsigned s_nLargeToNewlib, s_nSmallToLarge;	// fallbacks, either way

extern "C" void *__real_malloc( size_t size );
extern "C" void *__real_calloc( size_t count, size_t size );
extern "C" void *__real_realloc( void *ptr, size_t size );
extern "C" void *__real_memalign( size_t alignment, size_t size );
extern "C" void __real_free( void *ptr );
extern "C" size_t __real_malloc_usable_size( void *ptr );
extern "C" struct mallinfo __real_mallinfo( void );

static inline bool InLargeHeap( const void *p )
{
	return (const unsigned char *)p >= s_pLargeBase && (const unsigned char *)p < s_pLargeEnd;
}

static void *LargeAlloc( size_t alignment, size_t size )
{
	if ( !s_LargeHeap )
		return NULL;
	sceKernelLockLwMutex( &s_LargeLock, 1, NULL );
	void *p = alignment > 8 ? sceClibMspaceMemalign( s_LargeHeap, alignment, size ) : sceClibMspaceMalloc( s_LargeHeap, size );
	sceKernelUnlockLwMutex( &s_LargeLock, 1 );
	return p;
}

static void LargeFree( void *p )
{
	sceKernelLockLwMutex( &s_LargeLock, 1, NULL );
	sceClibMspaceFree( s_LargeHeap, p );
	sceKernelUnlockLwMutex( &s_LargeLock, 1 );
}

void so_large_heap_init( unsigned nMB )
{
	if ( !nMB )
	{
		so_trace( "large-block heap off" );
		return;
	}
	const size_t size = (size_t)nMB << 20;
	unsigned char *pBase = (unsigned char *)__real_memalign( 4096, size );
	SceClibMspace heap = pBase ? sceClibMspaceCreate( pBase, size ) : NULL;
	if ( !heap || sceKernelCreateLwMutex( &s_LargeLock, "large_heap", 0, 0, NULL ) < 0 )
	{
		so_trace( "large-block heap: no %u MB arena, everything stays in one heap", nMB );
		__real_free( pBase );
		return;
	}
	s_pLargeBase = pBase;
	s_pLargeEnd = pBase + size;
	s_LargeHeap = heap;
	so_trace( "large-block heap: %u MB for blocks of %u KB and up", nMB, VITA_LARGE_ALLOC / 1024 );
}

void so_large_heap_stats( unsigned *pInUseKB, unsigned *pPeakKB, unsigned *pSizeKB, unsigned *pLargeToNewlib, unsigned *pSmallToLarge )
{
	SceClibMspaceStats stats = {};
	if ( s_LargeHeap )
	{
		sceKernelLockLwMutex( &s_LargeLock, 1, NULL );
		sceClibMspaceMallocStatsFast( s_LargeHeap, &stats );
		sceKernelUnlockLwMutex( &s_LargeLock, 1 );
	}
	*pInUseKB = stats.current_in_use / 1024;
	*pPeakKB = stats.peak_in_use / 1024;
	*pSizeKB = ( s_pLargeEnd - s_pLargeBase ) / 1024;
	*pLargeToNewlib = s_nLargeToNewlib;
	*pSmallToLarge = s_nSmallToLarge;
}

// Allocation failures (both heaps), logged with the request, the heaps' state
// and a backtrace (the first 8). Engine allocations through tier0 log their
// own OOM, but plain new/malloc (libstdc++, vtf loading) aborted with nothing
// in the log (a bad_alloc while loading d2_coast_10's textures)
static int s_nAllocFailures;
static int s_bInAllocReport;

static void AllocFailed( const char *what, size_t size )
{
	if ( s_nAllocFailures >= 8 || __atomic_exchange_n( &s_bInAllocReport, 1, __ATOMIC_ACQUIRE ) )
		return;
	s_nAllocFailures++;
	struct mallinfo mi = __real_mallinfo();
	unsigned nInUse, nPeak, nSize, nLargeToNewlib, nSmallToLarge;
	so_large_heap_stats( &nInUse, &nPeak, &nSize, &nLargeToNewlib, &nSmallToLarge );
	so_trace( "[vita] %s( %u ) failed: newlib heap %u KB in use (the arena counts as used), %u KB free (%u KB at the top); "
		"large-block arena %u of %u KB in use (peak %u)", what, (unsigned)size,
		(unsigned)( mi.uordblks / 1024 ), (unsigned)( mi.fordblks / 1024 ), (unsigned)( mi.keepcost / 1024 ), nInUse, nSize, nPeak );
	so_log_backtrace( what );
	__atomic_store_n( &s_bInAllocReport, 0, __ATOMIC_RELEASE );
}

extern "C" void *__wrap_malloc( size_t size )
{
	void *p;
	if ( size >= VITA_LARGE_ALLOC && ( p = LargeAlloc( 0, size ) ) )
		return p;
	p = __real_malloc( size );
	if ( !p && size )
	{
		if ( size < VITA_LARGE_ALLOC && ( p = LargeAlloc( 0, size ) ) )
			s_nSmallToLarge++;
		else
			AllocFailed( "malloc", size );
	}
	else if ( size >= VITA_LARGE_ALLOC && s_LargeHeap )
	{
		s_nLargeToNewlib++;
	}
	return p;
}

extern "C" void *__wrap_calloc( size_t count, size_t size )
{
	const size_t total = count * size;
	if ( size && total / size != count )
		return NULL;
	void *p;
	if ( total >= VITA_LARGE_ALLOC && ( p = LargeAlloc( 0, total ) ) )
	{
		memset( p, 0, total );
		return p;
	}
	p = __real_calloc( count, size );
	if ( !p && total )
	{
		if ( total < VITA_LARGE_ALLOC && ( p = LargeAlloc( 0, total ) ) )
		{
			memset( p, 0, total );
			s_nSmallToLarge++;
		}
		else
		{
			AllocFailed( "calloc", total );
		}
	}
	else if ( total >= VITA_LARGE_ALLOC && s_LargeHeap )
	{
		s_nLargeToNewlib++;
	}
	return p;
}

extern "C" void *__wrap_memalign( size_t alignment, size_t size )
{
	void *p;
	if ( size >= VITA_LARGE_ALLOC && ( p = LargeAlloc( alignment, size ) ) )
		return p;
	p = __real_memalign( alignment, size );
	if ( !p && size )
	{
		if ( size < VITA_LARGE_ALLOC && ( p = LargeAlloc( alignment, size ) ) )
			s_nSmallToLarge++;
		else
			AllocFailed( "memalign", size );
	}
	else if ( size >= VITA_LARGE_ALLOC && s_LargeHeap )
	{
		s_nLargeToNewlib++;
	}
	return p;
}

extern "C" void __wrap_free( void *ptr )
{
	if ( InLargeHeap( ptr ) )
		LargeFree( ptr );
	else
		__real_free( ptr );
}

extern "C" size_t __wrap_malloc_usable_size( void *ptr )
{
	return InLargeHeap( ptr ) ? sceClibMspaceMallocUsableSize( ptr ) : __real_malloc_usable_size( ptr );
}

// Moves a block to a new allocation (the other heap), freeing the old one
static void *MoveBlock( void *ptr, size_t oldSize, void *p, size_t size )
{
	memcpy( p, ptr, oldSize < size ? oldSize : size );
	__wrap_free( ptr );
	return p;
}

extern "C" void *__wrap_realloc( void *ptr, size_t size )
{
	if ( !ptr )
		return __wrap_malloc( size );

	void *p;
	if ( InLargeHeap( ptr ) )
	{
		const size_t oldSize = sceClibMspaceMallocUsableSize( ptr );
		// shrunk well below the threshold: back to the newlib heap, so it
		// doesn't pin a hole in the arena
		if ( size < VITA_LARGE_ALLOC / 2 && ( p = __real_malloc( size ? size : 1 ) ) )
			return MoveBlock( ptr, oldSize, p, size );
		sceKernelLockLwMutex( &s_LargeLock, 1, NULL );
		p = sceClibMspaceRealloc( s_LargeHeap, ptr, size ? size : 1 );
		sceKernelUnlockLwMutex( &s_LargeLock, 1 );
		if ( p )
			return p;
		// arena full
		if ( ( p = __real_malloc( size ) ) )
		{
			s_nLargeToNewlib++;
			return MoveBlock( ptr, oldSize, p, size );
		}
		AllocFailed( "realloc", size );
		return NULL;
	}

	if ( size >= VITA_LARGE_ALLOC && ( p = LargeAlloc( 0, size ) ) )
		return MoveBlock( ptr, __real_malloc_usable_size( ptr ), p, size );
	p = __real_realloc( ptr, size );
	if ( !p && size )
	{
		if ( ( p = LargeAlloc( 0, size ) ) )
		{
			s_nSmallToLarge++;
			return MoveBlock( ptr, __real_malloc_usable_size( ptr ), p, size );
		}
		AllocFailed( "realloc", size );
	}
	else if ( size >= VITA_LARGE_ALLOC && s_LargeHeap )
	{
		s_nLargeToNewlib++;
	}
	return p;
}

//-----------------------------------------------------------------------------
// Heap check, with -vitadebug (a rare crash in newlib's malloc came at a map
// change). Walks newlib's chunks (dlmalloc 2.6.4 layout:
// prev_size, size with PREV_INUSE, then fd/bk while free) under its lock and
// checks sizes, free chunks' boundary tags and links. The first bad chunk is
// logged with the chunk before it and the words around it, so what wrote over
// it can be told from the data; later checks are skipped. Temporary
//-----------------------------------------------------------------------------
extern "C" char *__malloc_sbrk_base;
extern "C" void *__malloc_av_[];
extern "C" void __malloc_lock( struct _reent * );
extern "C" void __malloc_unlock( struct _reent * );

struct HeapChunk
{
	uint32_t prevSize;
	uint32_t size;
	HeapChunk *fd;
	HeapChunk *bk;
};

static volatile int s_bHeapCorrupt;

extern "C" void so_heap_check( const char *where )
{
	if ( s_bHeapCorrupt || !__malloc_sbrk_base || __malloc_sbrk_base == (char *)-1 )
		return;
	static char s_szReport[1400];
	int len = 0;
	const uint64_t t0 = sceKernelGetProcessTimeWide();
	unsigned nChunks = 0, nFree = 0;
	const char *bad = NULL;

	__malloc_lock( _REENT );
	HeapChunk *top = (HeapChunk *)__malloc_av_[2];
	const uintptr_t lo = (uintptr_t)__malloc_sbrk_base;
	const uintptr_t hi = (uintptr_t)top + ( top->size & ~3u );
	const uintptr_t binLo = (uintptr_t)&__malloc_av_[0] - 8, binHi = (uintptr_t)&__malloc_av_[258];
	// the first chunk: memory 8-aligned, as malloc_extend_top places it
	uintptr_t first = lo;
	if ( ( first + 8 ) & 7 )
		first = ( ( first + 8 + 7 ) & ~(uintptr_t)7 ) - 8;
	HeapChunk *p = (HeapChunk *)first, *prev = NULL;
	while ( p != top )
	{
		if ( (uintptr_t)p < lo || (uintptr_t)p >= (uintptr_t)top )
		{
			bad = "chunk outside the heap";
			break;
		}
		const uint32_t size = p->size & ~3u;
		if ( size < 16 || ( size & 7 ) || (uintptr_t)p + size > (uintptr_t)top )
		{
			bad = "bad chunk size";
			break;
		}
		HeapChunk *next = (HeapChunk *)( (char *)p + size );
		if ( !( next->size & 1 ) )
		{
			// p is free: its size is repeated at the next chunk, and it sits in
			// a doubly linked bin list
			nFree++;
			const uintptr_t fd = (uintptr_t)p->fd, bk = (uintptr_t)p->bk;
			if ( next->prevSize != size )
				bad = "free chunk whose size isn't repeated by the next chunk";
			else if ( !( ( fd >= lo && fd < hi ) || ( fd >= binLo && fd < binHi ) ) || !( ( bk >= lo && bk < hi ) || ( bk >= binLo && bk < binHi ) ) )
				bad = "free chunk linked outside the heap";
			else if ( p->fd->bk != p || p->bk->fd != p )
				bad = "free chunk whose neighbours don't link back";
			if ( bad )
				break;
		}
		nChunks++;
		prev = p;
		p = next;
	}
	if ( bad )
	{
		s_bHeapCorrupt = 1;
		len += snprintf( s_szReport + len, sizeof( s_szReport ) - len,
			"[vita] HEAP CORRUPT (%s): %s at chunk 0x%08x (prev_size 0x%x, size 0x%x, fd 0x%08x, bk 0x%08x); chunk before it 0x%08x, size 0x%x; %u chunks walked\n",
			where, bad, (unsigned)(uintptr_t)p, p->prevSize, p->size, (unsigned)(uintptr_t)p->fd, (unsigned)(uintptr_t)p->bk,
			(unsigned)(uintptr_t)prev, prev ? prev->size : 0, nChunks );
		// the words around the bad chunk: 16 before, 16 from it
		const uint32_t *w = (const uint32_t *)p - 16;
		for ( int row = 0; row < 4 && len < (int)sizeof( s_szReport ) - 100; row++ )
		{
			len += snprintf( s_szReport + len, sizeof( s_szReport ) - len, "  %08x:", (unsigned)(uintptr_t)( w + row * 8 ) );
			for ( int i = 0; i < 8; i++ )
				len += snprintf( s_szReport + len, sizeof( s_szReport ) - len, " %08x", w[row * 8 + i] );
			len += snprintf( s_szReport + len, sizeof( s_szReport ) - len, "\n" );
		}
	}
	__malloc_unlock( _REENT );

	const unsigned ms = (unsigned)( ( sceKernelGetProcessTimeWide() - t0 ) / 1000 );
	if ( bad )
	{
		so_log_write( s_szReport );
		// what the bad bytes might point into
		uintptr_t offset = 0;
		const uint32_t *w = (const uint32_t *)p;
		for ( int i = 0; i < 4; i++ )
		{
			const char *module = so_module_for_address( w[i], &offset );
			if ( module )
				so_trace( "  word %d 0x%08x is in %s+0x%x", i, w[i], module, (unsigned)offset );
		}
	}
	else
	{
		so_trace( "heap check (%s): ok, %u chunks (%u free), %u ms", where, nChunks, nFree, ms );
	}
}

// newlib's setvbuf takes its buffer from plain malloc (wrapped) but fclose
// gives it back with _free_r (not wrapped), so the buffer has to come from
// the newlib heap even when that heap is full and the arena would take it
// (the filesystem asks for 32-64 KB). Only newlib object that mixes the two
// that way (tzset's _malloc_r + free is safe: free checks the address)
extern "C" int __real_setvbuf( FILE *fp, char *buf, int mode, size_t size );
extern "C" int __wrap_setvbuf( FILE *fp, char *buf, int mode, size_t size )
{
	if ( buf || mode == _IONBF )
		return __real_setvbuf( fp, buf, mode, size );
	buf = (char *)__real_malloc( size ? size : BUFSIZ );
	if ( !buf )
		return __real_setvbuf( fp, NULL, _IONBF, 0 );
	const int ret = __real_setvbuf( fp, buf, mode, size ? size : BUFSIZ );
	if ( ret == 0 )
		fp->_flags |= __SMBF;	// fclose frees it
	else
		__real_free( buf );
	return ret;
}

// Both heaps as one, for the engine's memory checks (the map-change flush,
// OOM messages, the tour report): the arena's block counts as used in the
// newlib heap, so its free space is moved back
extern "C" struct mallinfo __wrap_mallinfo( void )
{
	struct mallinfo mi = __real_mallinfo();
	if ( s_LargeHeap )
	{
		SceClibMspaceStats stats;
		sceKernelLockLwMutex( &s_LargeLock, 1, NULL );
		sceClibMspaceMallocStatsFast( s_LargeHeap, &stats );
		sceKernelUnlockLwMutex( &s_LargeLock, 1 );
		const size_t nArenaFree = ( s_pLargeEnd - s_pLargeBase ) - stats.current_in_use;
		mi.uordblks -= nArenaFree;
		mi.fordblks += nArenaFree;
	}
	return mi;
}

extern "C" void so_thread_register( const char *name )
{
	int i = __atomic_fetch_add( &s_nThreads, 1, __ATOMIC_RELAXED );
	if ( i >= SO_THREADS )
		return;
	SceUID uid = sceKernelGetThreadId();
	s_Threads[i].name = name;
	__atomic_store_n( &s_Threads[i].uid, uid, __ATOMIC_RELEASE );

	SceKernelThreadInfo info;
	info.size = sizeof( info );
	if ( sceKernelGetThreadInfo( uid, &info ) >= 0 )
		so_trace( "thread %s: priority 0x%x, cpu affinity 0x%08x, core %d", name,
			(unsigned)info.currentPriority, (unsigned)info.currentCpuAffinityMask, (int)info.currentCpuId );
}

static void ThreadsLog( unsigned nFrames )
{
	static uint64_t s_LastWall;
	uint64_t nWall = sceKernelGetProcessTimeWide();
	uint64_t nInterval = s_LastWall ? nWall - s_LastWall : 0;
	s_LastWall = nWall;

	char line[512];
	int len = snprintf( line, sizeof( line ), "threads: cpu ms/frame (busy %%, core, core moves):" );
	bool bAny = false;
	int nThreads = __atomic_load_n( &s_nThreads, __ATOMIC_RELAXED );
	for ( int i = 0; i < nThreads && i < SO_THREADS; i++ )
	{
		SoThread &t = s_Threads[i];
		SceUID uid = __atomic_load_n( &t.uid, __ATOMIC_ACQUIRE );
		SceKernelThreadInfo info;
		info.size = sizeof( info );
		if ( !uid || sceKernelGetThreadInfo( uid, &info ) < 0 )
			continue;
		uint64_t nRun = info.runClocks - t.lastRun;	// microseconds
		int32_t nMoves = info.changeCpuCount - t.lastMoves;
		bool bFirst = !t.lastRun;
		t.lastRun = info.runClocks;
		t.lastMoves = info.changeCpuCount;
		if ( bFirst || !nInterval || len >= (int)sizeof( line ) )
			continue;
		unsigned nMs = (unsigned)( nRun / nFrames / 100 );	// tenths
		// currentCpuId is only valid while the thread runs
		int nCore = ( info.currentCpuId >= 0 && info.currentCpuId <= 3 ) ? info.currentCpuId : info.lastExecutedCpuId;
		len += snprintf( line + len, sizeof( line ) - len, " %s %u.%u (%u%%, %d, %d);", t.name,
			nMs / 10, nMs % 10, (unsigned)( nRun * 100 / nInterval ), nCore, (int)nMoves );
		bAny = true;
	}
	if ( bAny )
		so_trace( "%s", line );
}

//-----------------------------------------------------------------------------
// Shader record (testing builds; main.cpp starts it): one line per shader
// created at first draw, "v|p <name> <static> <dynamic> <ms>", after a
// "map <name>" line per map loaded, for the per-map lists a prepare-shaders
// step would compile. Buffered and written by the launcher's main loop
// each second, so the game's threads never wait on the memory card
//-----------------------------------------------------------------------------
static SceUID s_nShaderRecordFd = -1;
static char s_szShaderRecord[32 * 1024];
static int s_nShaderRecordLen;
static pthread_mutex_t s_ShaderRecordLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_nShaderRecordDropped;

extern "C" void so_shader_record_start( const char *path, const char *header )
{
	s_nShaderRecordFd = sceIoOpen( path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777 );
	if ( s_nShaderRecordFd < 0 )
	{
		so_trace( "shader record: can't open %s (0x%08x)", path, (unsigned)s_nShaderRecordFd );
		return;
	}
	so_trace( "shader record: %s", path );
	sceIoWrite( s_nShaderRecordFd, header, strlen( header ) );
}

static void ShaderRecordAppend( const char *line, int len )
{
	pthread_mutex_lock( &s_ShaderRecordLock );
	if ( s_nShaderRecordLen + len <= (int)sizeof( s_szShaderRecord ) )
	{
		memcpy( s_szShaderRecord + s_nShaderRecordLen, line, len );
		s_nShaderRecordLen += len;
	}
	else
		s_nShaderRecordDropped++;
	pthread_mutex_unlock( &s_ShaderRecordLock );
}

static void ShaderRecordMap( const char *pszMap )
{
	if ( s_nShaderRecordFd < 0 )
		return;
	char line[160];
	int len = snprintf( line, sizeof( line ), "map %s\n", pszMap );
	if ( len > 0 && len < (int)sizeof( line ) )
		ShaderRecordAppend( line, len );
}

extern "C" void so_shader_record( int bVertex, const char *pName, int nStatic, int nDynamic, unsigned nMicroseconds, int bFailed )
{
	if ( s_nShaderRecordFd < 0 )
		return;
	char line[200];
	int len = snprintf( line, sizeof( line ), "%c %s %d %d %u%s\n", bVertex ? 'v' : 'p', pName ? pName : "?",
		nStatic, nDynamic, ( nMicroseconds + 500 ) / 1000, bFailed ? " failed" : "" );
	if ( len > 0 && len < (int)sizeof( line ) )
		ShaderRecordAppend( line, len );
}

// main loop, each second, and at exit
extern "C" void so_shader_record_flush( void )
{
	if ( s_nShaderRecordFd < 0 )
		return;
	static char s_szOut[sizeof( s_szShaderRecord )];
	pthread_mutex_lock( &s_ShaderRecordLock );
	const int len = s_nShaderRecordLen;
	memcpy( s_szOut, s_szShaderRecord, len );
	s_nShaderRecordLen = 0;
	const unsigned nDropped = s_nShaderRecordDropped;
	s_nShaderRecordDropped = 0;
	pthread_mutex_unlock( &s_ShaderRecordLock );
	if ( len )
		sceIoWrite( s_nShaderRecordFd, s_szOut, len );
	if ( nDropped )
		so_trace( "shader record: %u lines dropped (buffer full)", nDropped );
}

//-----------------------------------------------------------------------------
// Hang reports. The modules' blocking waits go through the wrappers below
// (build.sh --wrap): a contended pthread_mutex_lock (CThreadMutex), every
// pthread_cond_timedwait (CThreadEvent, job waits) and usleep (ThreadSleep in
// polling loops). While one blocks, a slot holds the thread, the object, the
// caller and the stack position. Once no perf counter has moved for
// SO_HANG_SECONDS (no frame, file read, shader compile or texture upload),
// so_hang_check logs each waiting thread with the return addresses on its
// stack, innermost first, once per stall. module+offset is the module's own
// address and exe+offset is from 0x81000000 in hl2_vita.elf, for
// arm-vita-eabi-addr2line. An uncontended lock costs one trylock, as before
//-----------------------------------------------------------------------------
#define SO_HANG_SECONDS 30
#define SO_WAITS 48
#define SO_THREAD_NAMES 48

struct SoWait
{
	SceUID uid;		// 0: free
	int ready;		// the fields below are filled in
	const char *what;
	const void *object;
	uintptr_t caller;
	uintptr_t sp;
	uint64_t since;
};
static SoWait s_Waits[SO_WAITS];

struct SoThreadName
{
	SceUID uid;
	char name[32];
};
static SoThreadName s_ThreadNames[SO_THREAD_NAMES];
static int s_nThreadNames;

static SoWait *WaitBegin( const char *what, const void *object, void *caller, const volatile void *sp )
{
	const SceUID uid = sceKernelGetThreadId();
	for ( int i = 0; i < SO_WAITS; i++ )
	{
		SceUID expected = 0;
		if ( __atomic_load_n( &s_Waits[i].uid, __ATOMIC_RELAXED ) == 0 &&
			__atomic_compare_exchange_n( &s_Waits[i].uid, &expected, uid, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ) )
		{
			SoWait *w = &s_Waits[i];
			w->what = what;
			w->object = object;
			w->caller = (uintptr_t)caller;
			w->sp = (uintptr_t)sp;
			w->since = sceKernelGetProcessTimeWide();
			__atomic_store_n( &w->ready, 1, __ATOMIC_RELEASE );
			return w;
		}
	}
	return NULL;
}

static void WaitEnd( SoWait *w )
{
	if ( !w )
		return;
	__atomic_store_n( &w->ready, 0, __ATOMIC_RELAXED );
	__atomic_store_n( &w->uid, 0, __ATOMIC_RELEASE );
}

extern "C" int __real_pthread_mutex_lock( pthread_mutex_t *mutex );
extern "C" int __wrap_pthread_mutex_lock( pthread_mutex_t *mutex )
{
	int result = pthread_mutex_trylock( mutex );
	if ( result != EBUSY )
		return result;
	volatile int marker = 0;
	SoWait *w = WaitBegin( "mutex", mutex, __builtin_return_address( 0 ), &marker );
	result = __real_pthread_mutex_lock( mutex );
	WaitEnd( w );
	return result;
}

extern "C" int __real_pthread_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime );
extern "C" int __wrap_pthread_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime )
{
	volatile int marker = 0;
	SoWait *w = WaitBegin( "event", cond, __builtin_return_address( 0 ), &marker );
	const int result = __real_pthread_cond_timedwait( cond, mutex, abstime );
	WaitEnd( w );
	return result;
}

extern "C" int __real_usleep( useconds_t usec );
extern "C" int __wrap_usleep( useconds_t usec )
{
	volatile int marker = 0;
	SoWait *w = WaitBegin( "sleep", NULL, __builtin_return_address( 0 ), &marker );
	const int result = __real_usleep( usec );
	WaitEnd( w );
	return result;
}

// tier0's CThread::ThreadProc: names the calling thread in hang reports
// (thread pools name theirs FsAsyncIO0, SaveJob0 and so on). A ring: the
// oldest names go first
extern "C" void so_thread_name( const char *name )
{
	SoThreadName &t = s_ThreadNames[(unsigned)__atomic_fetch_add( &s_nThreadNames, 1, __ATOMIC_RELAXED ) % SO_THREAD_NAMES];
	__atomic_store_n( &t.uid, 0, __ATOMIC_RELAXED );
	snprintf( t.name, sizeof( t.name ), "%s", name ? name : "" );
	__atomic_store_n( &t.uid, sceKernelGetThreadId(), __ATOMIC_RELEASE );
}

static const char *ThreadName( SceUID uid, const SceKernelThreadInfo &info )
{
	const int nThreads = __atomic_load_n( &s_nThreads, __ATOMIC_RELAXED );
	for ( int i = 0; i < nThreads && i < SO_THREADS; i++ )
	{
		if ( __atomic_load_n( &s_Threads[i].uid, __ATOMIC_ACQUIRE ) == uid )
			return s_Threads[i].name;
	}
	for ( int i = 0; i < SO_THREAD_NAMES; i++ )
	{
		if ( __atomic_load_n( &s_ThreadNames[i].uid, __ATOMIC_ACQUIRE ) == uid )
			return s_ThreadNames[i].name;
	}
	return info.name;	// the kernel's
}

// " module+0xoffset" for a return address (odd: everything is Thumb) in a
// module's code or the launcher's
static bool DescribeCode( uintptr_t addr, uintptr_t exeText, size_t exeSize, char *out, size_t size )
{
	if ( !( addr & 1 ) )
		return false;
	addr &= ~(uintptr_t)1;
	if ( addr >= exeText && addr < exeText + exeSize )
	{
		snprintf( out, size, " exe+0x%x", (unsigned)( addr - exeText ) );
		return true;
	}
	for ( SoModule *mod : s_Modules )
	{
		for ( const SoSegment &seg : mod->segments )
		{
			if ( seg.exec && addr >= seg.addr && addr < seg.addr + seg.size )
			{
				snprintf( out, size, " %s+0x%x", mod->name, (unsigned)( addr - mod->base ) );
				return true;
			}
		}
	}
	return false;
}

static void HangReport( void )
{
	so_trace( "hang? nothing has moved for %d s (no frame, file read, shader compile or texture upload)", SO_HANG_SECONDS );

	uintptr_t exeText = 0;
	size_t exeSize = 0;
	SceKernelModuleInfo mod;
	mod.size = sizeof( mod );
	if ( sceKernelGetModuleInfo( sceKernelGetModuleIdByAddr( (void *)&HangReport ), &mod ) >= 0 )
	{
		exeText = (uintptr_t)mod.segments[0].vaddr;
		exeSize = mod.segments[0].memsz;
	}

	// the named threads, waiting in a noted wait or not
	const int nThreads = __atomic_load_n( &s_nThreads, __ATOMIC_RELAXED );
	for ( int i = 0; i < nThreads && i < SO_THREADS; i++ )
	{
		SceKernelThreadInfo info;
		info.size = sizeof( info );
		const SceUID uid = __atomic_load_n( &s_Threads[i].uid, __ATOMIC_ACQUIRE );
		if ( uid && sceKernelGetThreadInfo( uid, &info ) >= 0 )
			so_trace( "  thread %s: status 0x%x, kernel wait type 0x%x on 0x%x", s_Threads[i].name,
				(unsigned)info.status, (unsigned)info.waitType, (unsigned)info.waitId );
	}

	// every thread blocked in a noted wait, with its stack
	const uint64_t now = sceKernelGetProcessTimeWide();
	for ( int i = 0; i < SO_WAITS; i++ )
	{
		SoWait &w = s_Waits[i];
		if ( !__atomic_load_n( &w.ready, __ATOMIC_ACQUIRE ) )
			continue;
		const SceUID uid = __atomic_load_n( &w.uid, __ATOMIC_RELAXED );
		const uintptr_t sp = w.sp;
		SceKernelThreadInfo info;
		info.size = sizeof( info );
		if ( !uid || sceKernelGetThreadInfo( uid, &info ) < 0 )
			continue;
		char caller[64];
		if ( !DescribeCode( w.caller, exeText, exeSize, caller, sizeof( caller ) ) )
			snprintf( caller, sizeof( caller ), " 0x%08x", (unsigned)w.caller );
		so_trace( "  %s (0x%x, status 0x%x): %s 0x%08x for %u ms, called from%s", ThreadName( uid, info ), (unsigned)uid,
			(unsigned)info.status, w.what, (unsigned)(uintptr_t)w.object, (unsigned)( ( now - w.since ) / 1000 ), caller );

		const uintptr_t top = (uintptr_t)info.stack + info.stackSize;
		if ( sp < (uintptr_t)info.stack || sp >= top )
			continue;
		char line[480];
		int len = snprintf( line, sizeof( line ), "    stack:" );
		int nFound = 0;
		for ( const uint32_t *p = (const uint32_t *)( sp & ~(uintptr_t)3 ); (uintptr_t)p < top && nFound < 48; p++ )
		{
			char where[64];
			if ( !DescribeCode( *p, exeText, exeSize, where, sizeof( where ) ) )
				continue;
			if ( len + (int)strlen( where ) >= (int)sizeof( line ) )
			{
				so_trace( "%s", line );
				len = snprintf( line, sizeof( line ), "    stack:" );
			}
			len += snprintf( line + len, sizeof( line ) - len, "%s", where );
			nFound++;
		}
		so_trace( "%s", line );
	}
	so_trace( "hang? end of report" );
}

// The launcher's main loop, every second
extern "C" void so_hang_check( void )
{
	static uint64_t s_nLastSum;
	static int s_nStill;
	uint64_t sum = 0;
	for ( int i = 0; i < SO_PERF_COUNTERS; i++ )
		sum += __atomic_load_n( &s_PerfCalls[i], __ATOMIC_RELAXED );
	if ( sum != s_nLastSum )
	{
		s_nLastSum = sum;
		s_nStill = 0;
		return;
	}
	if ( ++s_nStill == SO_HANG_SECONDS )
		HangReport();
}

// Named sections: a small table matched on the caller's string pointers
// (fast path) or the copied text; a spinlock covers it, since client jobs
// (bone setup, particles) add from worker threads
struct SoProfileEntry
{
	const char *prefixKey;
	const char *nameKey;
	char text[48];
	uint64_t time;
	uint32_t count;
};
#define SO_PROFILE_ENTRIES 128
static SoProfileEntry s_Profile[SO_PROFILE_ENTRIES];
static volatile int s_ProfileLock;
static uint64_t s_ProfileStart;

static void ProfileLock( void )
{
	while ( __atomic_exchange_n( &s_ProfileLock, 1, __ATOMIC_ACQUIRE ) )
		;
}

static void ProfileUnlock( void )
{
	__atomic_store_n( &s_ProfileLock, 0, __ATOMIC_RELEASE );
}

static void ProfileFormat( char *out, size_t size, const char *prefix, const char *name )
{
	snprintf( out, size, "%s%s%s", prefix ? prefix : "", ( prefix && name ) ? " " : "", name ? name : "" );
}

// vita_profile (engine cvar): the named-section timers are skipped when off
static volatile int s_ProfileEnabled = 0;	// vita_profile (engine/host.cpp)

extern "C" void so_profile_enable( int enable )
{
	s_ProfileEnabled = enable;
}

extern "C" int so_profile_enabled( void )
{
	return s_ProfileEnabled;
}

extern "C" void so_profile_add( const char *prefix, const char *name, uint64_t us )
{
	if ( !s_ProfileEnabled )
		return;
	char text[48];
	text[0] = 0;
	ProfileLock();
	int iFree = -1;
	for ( int i = 0; i < SO_PROFILE_ENTRIES; i++ )
	{
		SoProfileEntry &e = s_Profile[i];
		if ( !e.text[0] )
		{
			iFree = i;
			break;
		}
		if ( e.prefixKey == prefix && e.nameKey == name )
		{
			e.time += us;
			e.count++;
			ProfileUnlock();
			return;
		}
	}
	// not matched by pointer: compare the text (names whose storage moved)
	ProfileFormat( text, sizeof( text ), prefix, name );
	for ( int i = 0; i < SO_PROFILE_ENTRIES && s_Profile[i].text[0]; i++ )
	{
		SoProfileEntry &e = s_Profile[i];
		if ( !strcmp( e.text, text ) )
		{
			e.prefixKey = prefix;
			e.nameKey = name;
			e.time += us;
			e.count++;
			ProfileUnlock();
			return;
		}
	}
	if ( iFree >= 0 )
	{
		SoProfileEntry &e = s_Profile[iFree];
		e.prefixKey = prefix;
		e.nameKey = name;
		memcpy( e.text, text, sizeof( e.text ) );
		e.time = us;
		e.count = 1;
	}
	ProfileUnlock();
}

static void ProfileLog( void )
{
	uint64_t now = sceKernelGetProcessTimeWide();
	if ( !s_ProfileStart )
		s_ProfileStart = now;
	double seconds = ( now - s_ProfileStart ) / 1000000.0;
	if ( seconds < 1.0 )
		return;

	static SoProfileEntry s_Copy[SO_PROFILE_ENTRIES];
	ProfileLock();
	memcpy( s_Copy, s_Profile, sizeof( s_Copy ) );
	memset( s_Profile, 0, sizeof( s_Profile ) );
	ProfileUnlock();
	s_ProfileStart = now;

	char line[1800];
	int len = snprintf( line, sizeof( line ), "[vita] engine/client ms/s:" );
	for ( int n = 0; n < 24 && len < (int)sizeof( line ) - 90; n++ )
	{
		int best = -1;
		for ( int i = 0; i < SO_PROFILE_ENTRIES; i++ )
		{
			if ( s_Copy[i].text[0] && s_Copy[i].count && ( best < 0 || s_Copy[i].time > s_Copy[best].time ) )
				best = i;
		}
		if ( best < 0 || s_Copy[best].time == 0 )
			break;
		len += snprintf( line + len, sizeof( line ) - len, " %s %.1f (%u/s)", s_Copy[best].text,
			s_Copy[best].time / 1000.0 / seconds, (unsigned)( s_Copy[best].count / seconds ) );
		s_Copy[best].count = 0;	// taken
	}
	if ( len > 26 )
	{
		line[len++] = '\n';
		line[len] = 0;
		so_log_write( line );	// so_trace truncates at 512
	}
}

// Logs the totals if anything changed since the last call
extern "C" void so_perf_log( void )
{
	ProfileLog();

	static uint64_t s_LastCalls[SO_PERF_COUNTERS];
	bool changed = false;
	uint64_t calls[SO_PERF_COUNTERS], time[SO_PERF_COUNTERS], amount[SO_PERF_COUNTERS];
	for ( int i = 0; i < SO_PERF_COUNTERS; i++ )
	{
		calls[i] = __atomic_load_n( &s_PerfCalls[i], __ATOMIC_RELAXED );
		time[i] = __atomic_load_n( &s_PerfTime[i], __ATOMIC_RELAXED );
		amount[i] = __atomic_load_n( &s_PerfAmount[i], __ATOMIC_RELAXED );
		changed |= calls[i] != s_LastCalls[i];
		s_LastCalls[i] = calls[i];
	}
	if ( !changed )
		return;
	// order matches VITA_PERF_* in vita_perf.h
	so_trace( "perf @%u s: shader compile %u (%u.%01u s), tex upload %u (%u.%01u s), "
		"file open %u (%u.%01u s), read %u (%u MB, %u.%01u s), seek %u (%u.%01u s), lzma %u (%u MB, %u.%01u s), "
		"draws %u, swap %u (%u.%01u s)",
		(unsigned)( sceKernelGetProcessTimeWide() / 1000000 ),
		(unsigned)calls[0], (unsigned)( time[0] / 1000000 ), (unsigned)( time[0] / 100000 % 10 ),
		(unsigned)calls[1], (unsigned)( time[1] / 1000000 ), (unsigned)( time[1] / 100000 % 10 ),
		(unsigned)calls[2], (unsigned)( time[2] / 1000000 ), (unsigned)( time[2] / 100000 % 10 ),
		(unsigned)calls[3], (unsigned)( amount[3] >> 20 ), (unsigned)( time[3] / 1000000 ), (unsigned)( time[3] / 100000 % 10 ),
		(unsigned)calls[4], (unsigned)( time[4] / 1000000 ), (unsigned)( time[4] / 100000 % 10 ),
		(unsigned)calls[5], (unsigned)( amount[5] >> 20 ), (unsigned)( time[5] / 1000000 ), (unsigned)( time[5] / 100000 % 10 ),
		(unsigned)calls[6],
		(unsigned)calls[7], (unsigned)( time[7] / 1000000 ), (unsigned)( time[7] / 100000 % 10 ) );

	// Frame phases (VITA_PERF_HOST_*), as average milliseconds per presented
	// frame over the last interval
	static uint64_t s_LastTime[SO_PERF_COUNTERS], s_LastSwaps;
	uint64_t nFrames = calls[7] - s_LastSwaps;
	if ( nFrames )
	{
		unsigned ms[SO_PERF_COUNTERS];
		for ( int i = 8; i < 23; i++ )
		{
			// tenths of a millisecond per frame
			ms[i] = (unsigned)( ( time[i] - s_LastTime[i] ) / nFrames / 100 );
			s_LastTime[i] = time[i];
		}
		s_LastSwaps = calls[7];
		// draws, uniform uploads and traces are counted, not timed
		// (so_perf_count): per frame
		static uint64_t s_LastTraces, s_LastDraws;
		unsigned nTraces = (unsigned)( ( calls[18] - s_LastTraces ) / nFrames );
		s_LastTraces = calls[18];
		unsigned nDraws = (unsigned)( ( calls[6] - s_LastDraws ) / nFrames );
		s_LastDraws = calls[6];
		static uint64_t s_LastWorldDraws;
		unsigned nWorldDraws = (unsigned)( ( calls[23] - s_LastWorldDraws ) / nFrames );	// VITA_PERF_WORLD_DRAW
		s_LastWorldDraws = calls[23];
		static uint64_t s_LastModels, s_LastStaticProps;
		unsigned nModels = (unsigned)( ( calls[24] - s_LastModels ) / nFrames );	// VITA_PERF_MODEL_DRAW
		s_LastModels = calls[24];
		unsigned nStaticProps = (unsigned)( ( amount[25] - s_LastStaticProps ) / nFrames );	// VITA_PERF_STATIC_PROP
		s_LastStaticProps = amount[25];
		// particles: simulation ms (VITA_PERF_PARTICLE_SIM), particles and
		// systems drawn, fire among them (VITA_PERF_PARTICLES, _FIRE_PARTICLES)
		// VGUI: engine paint ms (VITA_PERF_VGUI, both calls a frame) and
		// characters drawn (VITA_PERF_VGUI_CHARS)
		static uint64_t s_LastVgui, s_LastVguiChars;
		unsigned nVgui = (unsigned)( ( time[29] - s_LastVgui ) / nFrames / 100 );
		unsigned nVguiChars = (unsigned)( ( amount[30] - s_LastVguiChars ) / nFrames );
		s_LastVgui = time[29];
		s_LastVguiChars = amount[30];
		static uint64_t s_LastParticleSim, s_LastSystems, s_LastParticles, s_LastFireSystems, s_LastFireParticles;
		unsigned nParticleSim = (unsigned)( ( time[26] - s_LastParticleSim ) / nFrames / 100 );
		s_LastParticleSim = time[26];
		unsigned nSystems = (unsigned)( ( calls[27] - s_LastSystems ) / nFrames );
		unsigned nParticles = (unsigned)( ( amount[27] - s_LastParticles ) / nFrames );
		unsigned nFireSystems = (unsigned)( ( calls[28] - s_LastFireSystems ) / nFrames );
		unsigned nFireParticles = (unsigned)( ( amount[28] - s_LastFireParticles ) / nFrames );
		s_LastSystems = calls[27]; s_LastParticles = amount[27];
		s_LastFireSystems = calls[28]; s_LastFireParticles = amount[28];
		so_trace( "frame: %u frames, ms/frame: input %u.%u, server %u.%u (game dll %u.%u: think %u.%u, physics %u.%u; snapshots %u.%u), "
			"client %u.%u, render %u.%u (3d view %u.%u), sound %u.%u, server join %u.%u, host frame %u.%u; render thread %u.%u, main waits %u.%u; per frame: traces %u, draws %u (world %u), models %u, static props %u, "
			"particles %u in %u systems (fire %u in %u, sim %u.%u ms), vgui %u.%u ms (%u chars)",
			(unsigned)nFrames, ms[8] / 10, ms[8] % 10, ms[9] / 10, ms[9] % 10, ms[17] / 10, ms[17] % 10, ms[15] / 10, ms[15] % 10,
			ms[10] / 10, ms[10] % 10, ms[16] / 10, ms[16] % 10,
			ms[11] / 10, ms[11] % 10, ms[12] / 10, ms[12] % 10, ms[13] / 10, ms[13] % 10, ms[14] / 10, ms[14] % 10,
			ms[21] / 10, ms[21] % 10,
			ms[22] / 10, ms[22] % 10,
			ms[19] / 10, ms[19] % 10, ms[20] / 10, ms[20] % 10,
			nTraces, nDraws, nWorldDraws, nModels, nStaticProps,
			nParticles, nSystems, nFireParticles, nFireSystems, nParticleSim / 10, nParticleSim % 10,
			nVgui / 10, nVgui % 10, nVguiChars );
		ThreadsLog( (unsigned)nFrames );
	}
}

static char s_CrashLogPath[256];

// The crash log is written without any libc calls: formatting functions use
// per-thread state and locks that aren't safe in an exception handler (an
// snprintf here left crash.log empty on device)
struct CrashLine
{
	char buf[256];
	int len;
};

static void CrashPut( CrashLine *line, const char *str )
{
	while ( *str && line->len < (int)sizeof( line->buf ) - 1 )
		line->buf[line->len++] = *str++;
}

static void CrashPutHex( CrashLine *line, uint32_t value )
{
	static const char s_Digits[] = "0123456789abcdef";
	CrashPut( line, "0x" );
	for ( int shift = 28; shift >= 0; shift -= 4 )
	{
		if ( line->len < (int)sizeof( line->buf ) - 1 )
			line->buf[line->len++] = s_Digits[( value >> shift ) & 0xf];
	}
}

static void CrashLogLine( SceUID fd, const char *label, uint32_t addr )
{
	CrashLine line;
	line.len = 0;
	CrashPut( &line, label );
	CrashPut( &line, " " );
	CrashPutHex( &line, addr );

	uintptr_t offset = 0;
	const char *name = so_module_for_address( addr, &offset );
	if ( name )
	{
		CrashPut( &line, "  " );
		CrashPut( &line, name );
		CrashPut( &line, "+" );
		CrashPutHex( &line, offset );
	}
	CrashPut( &line, "\n" );
	sceIoWrite( fd, line.buf, line.len );
}

// Both kubridge context layouts start with r0-r12, sp, lr, pc
static void WriteCrashLog( const char *type, const uint32_t *regs, uint32_t far )
{
	SceUID fd = sceIoOpen( s_CrashLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if ( fd >= 0 )
	{
		CrashLine line;
		line.len = 0;
		CrashPut( &line, type );
		CrashPut( &line, "\n" );
		sceIoWrite( fd, line.buf, line.len );

		// which thread: the engine's, or one of vitaGL's, SDL's, the engine's
		// job threads...
		SceKernelThreadInfo info;
		info.size = sizeof( info );
		SceUID thid = sceKernelGetThreadId();
		line.len = 0;
		CrashPut( &line, "thread " );
		CrashPutHex( &line, (uint32_t)thid );
		CrashPut( &line, " " );
		if ( sceKernelGetThreadInfo( thid, &info ) == 0 )
		{
			info.name[sizeof( info.name ) - 1] = '\0';
			CrashPut( &line, info.name );
		}
		CrashPut( &line, "\n" );
		sceIoWrite( fd, line.buf, line.len );

		// the executable loads at a varying base; this anchor lets addresses
		// in it be matched up with the ELF: ELF address = addr - ref + the ELF
		// address of WriteCrashLog
		CrashLogLine( fd, "ref", (uint32_t)(uintptr_t)&WriteCrashLog );

		uint32_t sp = regs[13], lr = regs[14], pc = regs[15];
		CrashLogLine( fd, "pc ", pc );
		CrashLogLine( fd, "lr ", lr );
		CrashLogLine( fd, "far", far );

		static const char *s_RegNames[] = { "r0 ", "r1 ", "r2 ", "r3 ", "r4 ", "r5 ", "r6 ",
			"r7 ", "r8 ", "r9 ", "r10", "r11", "r12" };
		for ( int i = 0; i <= 12; i++ )
			CrashLogLine( fd, s_RegNames[i], regs[i] );
		CrashLogLine( fd, "sp ", sp );

		// raw stack words that point into modules or (near ref) into the
		// executable, which holds vitaGL, SDL and newlib, as a poor man's
		// backtrace
		const uint32_t ref = (uint32_t)(uintptr_t)&WriteCrashLog;
		const uint32_t *stack = (const uint32_t *)sp;
		for ( int i = 0; i < 128; i++ )
		{
			uintptr_t offset;
			uint32_t word = stack[i];
			if ( so_module_for_address( word, &offset ) )
				CrashLogLine( fd, "stk", word );
			else if ( ( word & 1 ) && word + 0x100000 > ref && word < ref + 0x400000 )
				CrashLogLine( fd, "exe", word ); // odd: a Thumb return address
		}

		sceIoClose( fd );
	}
}

static void ExceptionHandler( KuKernelExceptionContext *ctx )
{
	static const char *s_Types[] = { "data abort", "prefetch abort", "undefined instruction" };
	WriteCrashLog( ctx->exceptionType < 3 ? s_Types[ctx->exceptionType] : "unknown exception", &ctx->r0, ctx->FAR );

	// Hand the crash on to the system so it still writes its core dump (all
	// threads, modules, memory): with no handler registered, kubridge passes
	// exceptions to the system's own handling, and returning from here
	// retries the faulting instruction, which faults again the same way.
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT );
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT );
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION );
}

extern "C" void glFinish( void );
extern "C" void glReleaseShaderCompiler( void );
extern "C" void sceShaccCgExtDisableExtensions( void );

static volatile int s_bGLReady = 0;

// HUD size (the HUD is hard to read at 368 lines): vgui2's
// Scheme.cpp scales the schemes loaded from ClientScheme.res by this; the
// client's vita_hud_size sets it. 1.25 is vita_hud_size 1, the default
static float s_flHudScale = 1.25f;
extern "C" float so_hud_scale( void )
{
	return s_flHudScale;
}
extern "C" void so_set_hud_scale( float flScale )
{
	s_flHudScale = flScale;
}

// Memory committed in CMemoryStack reservations (tier1/memstack.cpp), for the
// free memory lines: the total, and the stacks holding the most by the code
// that made them (module+offset; addr2line names it). The rest of what's used
// outside the heap and vitaGL's pools is module images and thread stacks.
// Measured per stack: hunk ~11 MB, save memory 3 MB (empty; no longer
// committed up front), ten small stacks at their 1 MB minimum using < 1 MB
// together (KeyValues, DMX, material system queues)
struct SoStack
{
	const void *stack;	// NULL: free slot (the stack was destroyed)
	void *caller;
	int committedKB;
};
static SoStack s_Stacks[32];
static int s_nStacks = 0;	// slots ever used
static volatile int s_nReserveCommittedKB = 0;

extern "C" void so_reserve_register( const void *pStack, void *pCaller )
{
	// a destroyed stack's slot first (registration is rare: engine startup,
	// a few per map load)
	static volatile int s_nLock = 0;
	while ( __sync_lock_test_and_set( &s_nLock, 1 ) )
		;
	int n = -1;
	for ( int i = 0; i < s_nStacks && n < 0; i++ )
		if ( !s_Stacks[i].stack )
			n = i;
	if ( n < 0 && s_nStacks < (int)( sizeof( s_Stacks ) / sizeof( s_Stacks[0] ) ) )
		n = s_nStacks;
	if ( n >= 0 )
	{
		s_Stacks[n].caller = pCaller;
		s_Stacks[n].committedKB = 0;
		__sync_synchronize();
		s_Stacks[n].stack = pStack;
		if ( n == s_nStacks )
			s_nStacks++;
	}
	__sync_lock_release( &s_nLock );
}

extern "C" void so_reserve_unregister( const void *pStack )
{
	for ( int i = 0; i < s_nStacks; i++ )
		if ( s_Stacks[i].stack == pStack )
			s_Stacks[i].stack = NULL;
}

extern "C" void so_reserve_commit_add( const void *pStack, int nKB )
{
	__sync_fetch_and_add( &s_nReserveCommittedKB, nKB );
	for ( int i = 0; i < s_nStacks; i++ )
	{
		if ( s_Stacks[i].stack == pStack )
		{
			__sync_fetch_and_add( &s_Stacks[i].committedKB, nKB );
			break;
		}
	}
}

extern "C" int so_reserve_committed_kb( void )
{
	return s_nReserveCommittedKB;
}

// " module+0xoffset N KB" for the three stacks holding the most
extern "C" void so_reserve_top( char *pszOut, int nOutSize )
{
	int n = s_nStacks;
	int top[3] = { -1, -1, -1 };
	for ( int i = 0; i < n; i++ )
	{
		if ( !s_Stacks[i].stack )
			continue;
		for ( int t = 0; t < 3; t++ )
		{
			if ( top[t] < 0 || s_Stacks[i].committedKB > s_Stacks[top[t]].committedKB )
			{
				for ( int u = 2; u > t; u-- )
					top[u] = top[u - 1];
				top[t] = i;
				break;
			}
		}
	}
	int nLen = 0;
	pszOut[0] = '\0';
	for ( int t = 0; t < 3 && top[t] >= 0 && nLen < nOutSize; t++ )
	{
		uintptr_t offset = 0;
		const char *module = so_module_for_address( (uintptr_t)s_Stacks[top[t]].caller, &offset );
		nLen += snprintf( pszOut + nLen, nOutSize - nLen, "%s%s+0x%x %d KB", t ? ", " : "", module ? module : "?",
			(unsigned)offset, s_Stacks[top[t]].committedKB );
	}
}

// The port's version (launcher_vita/version.h, game.h), for GameUI's menus
extern "C" const char *so_port_version( void )
{
	return "v" VITA_PORT_VERSION;
}

// A menu is up (no map, loading, or the game UI over the game): set by the
// engine each frame, read by shaderapidx9 (per-frame dynamic buffer discard)
static volatile int s_bMenusVisible = 1;
extern "C" void so_set_menus_visible( int bVisible )
{
	s_bMenusVisible = bVisible;
}
extern "C" int so_menus_visible( void )
{
	return s_bMenusVisible;
}

// A long task with no input (GameUI's PREPARE SHADERS, hours): the main loop
// keeps the system awake but lets the screen dim and turn off, against burn
// in on the OLED and to save the battery
static volatile int s_bLongTask;
extern "C" void so_set_long_task( int bActive )
{
	s_bLongTask = bActive;
}

extern "C" int so_long_task( void )
{
	return s_bLongTask;
}

// Gyro aiming (client in_joystick.cpp, Options > Controls): the Vita's
// angular velocity about its vertical axis (turning: yaw, positive to the
// left) and its horizontal axis (tilting: pitch, positive with the top edge
// coming towards the player), in radians a second (a quick turn of the wrist
// reads ~3); 0 if the motion sensor
// isn't available. Sampling starts at the first call, with the system's
// gyro bias correction, and a small dead zone so a Vita held still doesn't
// drift the view
extern "C" int so_gyro_rates( float *pYaw, float *pPitch )
{
	static int s_nState = 0;	// 0 not started, 1 sampling, -1 unavailable
	if ( s_nState == 0 )
	{
		s_nState = sceMotionStartSampling() >= 0 ? 1 : -1;
		if ( s_nState > 0 )
		{
			sceMotionSetGyroBiasCorrection( 1 );
			sceMotionSetDeadband( 1 );
		}
		so_trace( "gyro: motion sampling %s", s_nState > 0 ? "started" : "unavailable" );
	}
	if ( s_nState < 0 )
		return 0;
	SceMotionState state;
	if ( sceMotionGetState( &state ) < 0 )
		return 0;
	const float kDeadZone = 0.02f;	// ~1 degree a second
	float flYaw = state.angularVelocity.y, flPitch = state.angularVelocity.x;
	flYaw = fabsf( flYaw ) <= kDeadZone ? 0.0f : flYaw - copysignf( kDeadZone, flYaw );
	flPitch = fabsf( flPitch ) <= kDeadZone ? 0.0f : flPitch - copysignf( kDeadZone, flPitch );

	*pYaw = flYaw;
	*pPitch = flPitch;
	return 1;
}

// The options menu waits for a button to assign (GameUI VitaMenu.cpp): the
// front touchscreen's halves act as buttons then too, not only in game
// (inputsystem joystick_sdl.cpp)
static volatile int s_bTouchCapture = 0;
extern "C" void so_set_touch_capture( int bCapture )
{
	s_bTouchCapture = bCapture;
}
extern "C" int so_touch_capture( void )
{
	return s_bTouchCapture;
}

// vitaGL is initialized (main.cpp's vglInitExtended wrapper): glFinish is safe
extern "C" void so_set_gl_ready( void )
{
	s_bGLReady = 1;
}

// Leaves the process. GPU work is finished and vitaGL's shader compiler
// released first. The kubridge exception handlers live in the kernel and point
// into this process (exiting with them registered crashed the whole
// Vita), so they're released last: until then a crash in any thread during
// the quit still reaches crash.log. The kubridge memory reservations
// (CMemoryStack) are left to the kernel: freeing them while other threads
// still ran hung the quit. (What locked the whole Vita on every exit
// was partly committed megabytes in them; see tier1/memstack.cpp.) Safe from
// any thread; for the GL work, call it on the rendering thread (the engine
// thread)
extern "C" void so_exit_process( int code )
{
	so_trace( "exiting (code %d): finishing GPU work", code );
	// engine.log is flushed every quarter second (tier0 dbg.cpp): write out
	// the rest, since the logger's destructor doesn't run
	void ( *pFlushEngineLog )( void ) = (void ( * )( void ))dlsym( RTLD_DEFAULT, "VitaEngineLogFlush" );
	if ( pFlushEngineLog )
		pFlushEngineLog();
	so_shader_record_flush();
	if ( s_bGLReady )
	{
		glFinish();
		// vitaGL's shader compiler: Sony's libshacccg, loaded by the process,
		// with taiHEN hooks into it (SceShaccCgExt). Unhook first, then
		// release the compiler and unload the module (vitashark unloads
		// before unhooking; unhooking twice is a no-op)
		sceShaccCgExtDisableExtensions();
		glReleaseShaderCompiler();
	}
	so_trace( "GPU work finished, shader compiler released; exiting" );
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT );
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT );
	kuKernelReleaseExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION );
	sceKernelExitProcess( code );
}

// Needs a kubridge with exception handler support (bythos14's releases);
// calling it on an older one jumps to 0, like any unresolved import
extern "C" void so_install_crash_handler( const char *log_path )
{
	snprintf( s_CrashLogPath, sizeof( s_CrashLogPath ), "%s", log_path );

	KuKernelExceptionHandlerOpt opt;
	opt.size = sizeof( opt );
	kuKernelRegisterExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, ExceptionHandler, NULL, &opt );
	kuKernelRegisterExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, ExceptionHandler, NULL, &opt );
	kuKernelRegisterExceptionHandler( KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, ExceptionHandler, NULL, &opt );
}

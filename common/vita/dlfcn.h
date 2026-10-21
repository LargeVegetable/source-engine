//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: dlfcn.h replacement for PS Vita. Vita homebrew has no dynamic
//          linker; these are implemented by the launcher's ELF loader, which
//          loads engine modules built as shared objects.
//
//=============================================================================//
#ifndef VITA_DLFCN_H
#define VITA_DLFCN_H

#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_GLOBAL  0x0100
#define RTLD_LOCAL   0x0000
#define RTLD_NOLOAD  0x0004

#define RTLD_DEFAULT ((void *)0)

typedef struct
{
	const char *dli_fname;
	void       *dli_fbase;
	const char *dli_sname;
	void       *dli_saddr;
} Dl_info;

#ifdef __cplusplus
extern "C" {
#endif

void *dlopen( const char *filename, int flags );
void *dlsym( void *handle, const char *symbol );
int dlclose( void *handle );
char *dlerror( void );
int dladdr( const void *addr, Dl_info *info );

#ifdef __cplusplus
}
#endif

#endif // VITA_DLFCN_H

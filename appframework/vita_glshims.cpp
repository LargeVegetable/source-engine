//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Stand-ins for GL entry points ToGLES requires that vitaGL doesn't
//          provide. The Vita GPU (GXM) has no blend constant, volume textures
//          or multiple render targets, and vitaGL has no sync objects; each
//          shim does the closest safe thing.
//
//=============================================================================//
#ifdef PLATFORM_VITA

#include <string.h>

// vitaGL's ABI, declared here rather than pulling vitaGL.h in next to ToGLES's
// own GL headers
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned int GLbitfield;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef unsigned long long GLuint64;
typedef struct __GLsync *GLsync;

extern "C" void glFinish( void );
extern "C" void glTexParameterf( GLenum target, GLenum pname, GLfloat param );
extern "C" void glSamplerParameterf( GLuint sampler, GLenum pname, GLfloat param );

#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_SYNC_STATUS 0x9114
#define GL_SIGNALED 0x9119
#define GL_ALREADY_SIGNALED 0x911A
#define GL_COLOR_ATTACHMENT0 0x8CE0

// Sync objects: vitaGL has none, so waiting means finishing all GPU work.
// Correct, if slower than real fences; the sync handle is never dereferenced.
static GLsync VitaFenceSync( GLenum condition, GLbitfield flags )
{
	static char s_Fence;
	return (GLsync)&s_Fence;
}

static GLenum VitaClientWaitSync( GLsync sync, GLbitfield flags, GLuint64 timeout )
{
	glFinish();
	return GL_ALREADY_SIGNALED;
}

static void VitaWaitSync( GLsync sync, GLbitfield flags, GLuint64 timeout )
{
	glFinish();
}

static void VitaDeleteSync( GLsync sync )
{
}

static void VitaGetSynciv( GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values )
{
	if ( bufSize < 1 )
		return;
	values[0] = pname == GL_SYNC_STATUS ? GL_SIGNALED : 0;
	if ( length )
		*length = 1;
}

// GXM has no constant blend color; only used by a few blend modes
static void VitaBlendColor( GLfloat r, GLfloat g, GLfloat b, GLfloat a )
{
}

// GXM has no volume textures; Source uses them for color correction lookup
// tables, which have to stay disabled (mat_colorcorrection 0)
static void VitaTexImage3D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels )
{
}

static void VitaCompressedTexImage3D( GLenum target, GLint level, GLenum internalformat, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data )
{
}

// one color target only: the single-buffer case ToGLES uses is the default
static void VitaDrawBuffers( GLsizei n, const GLenum *bufs )
{
}

// vitaGL frees shaders with the program; detaching isn't needed
static void VitaDetachShader( GLuint program, GLuint shader )
{
}

// required by ToGLES' entry point table but never called
static void VitaCopyBufferSubData( GLenum readTarget, GLenum writeTarget, GLintptr readOffset,
	GLintptr writeOffset, GLsizeiptr size )
{
}

// only the single-value parameters (LOD bias) carry over; there's no border
// color support to forward GL_TEXTURE_BORDER_COLOR to
static void VitaTexParameterfv( GLenum target, GLenum pname, const GLfloat *params )
{
	if ( pname == GL_TEXTURE_LOD_BIAS )
		glTexParameterf( target, pname, params[0] );
}

static void VitaSamplerParameterfv( GLuint sampler, GLenum pname, const GLfloat *params )
{
	if ( pname == GL_TEXTURE_LOD_BIAS )
		glSamplerParameterf( sampler, pname, params[0] );
}

// texture size queries; ToGLES tracks sizes itself, this only feeds debug paths
static void VitaGetTexLevelParameteriv( GLenum target, GLint level, GLenum pname, GLint *params )
{
	*params = 0;
}

struct VitaGLShim
{
	const char *name;
	void *func;
};

static const VitaGLShim s_Shims[] =
{
	{ "glBlendColor", (void *)VitaBlendColor },
	{ "glClientWaitSync", (void *)VitaClientWaitSync },
	{ "glCompressedTexImage3D", (void *)VitaCompressedTexImage3D },
	{ "glCopyBufferSubData", (void *)VitaCopyBufferSubData },
	{ "glDeleteSync", (void *)VitaDeleteSync },
	{ "glDetachShader", (void *)VitaDetachShader },
	{ "glDrawBuffers", (void *)VitaDrawBuffers },
	{ "glFenceSync", (void *)VitaFenceSync },
	{ "glGetSynciv", (void *)VitaGetSynciv },
	{ "glGetTexLevelParameteriv", (void *)VitaGetTexLevelParameteriv },
	{ "glSamplerParameterfv", (void *)VitaSamplerParameterfv },
	{ "glTexImage3D", (void *)VitaTexImage3D },
	{ "glTexParameterfv", (void *)VitaTexParameterfv },
	{ "glWaitSync", (void *)VitaWaitSync },
};

//-----------------------------------------------------------------------------
// Returns a stand-in for a GL function vitaGL lacks, or NULL to look it up
// in vitaGL as usual
//-----------------------------------------------------------------------------
void *VitaGLShimLookup( const char *name )
{
	for ( size_t i = 0; i < sizeof( s_Shims ) / sizeof( s_Shims[0] ); i++ )
	{
		if ( !strcmp( s_Shims[i].name, name ) )
			return s_Shims[i].func;
	}
	return NULL;
}

#endif // PLATFORM_VITA

//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Converts the GLSL ES 3.00 that ToGLES generates into GLSL ES 1.00
//          for vitaGL, whose GLSL-to-Cg translator only understands the
//          older dialect: attribute/varying instead of in/out, gl_FragColor
//          and gl_FragData instead of declared outputs, and texture lookups
//          named by sampler type (texture2D, textureCube, ...) instead of the
//          overloaded texture(). Runs on the finished shader text, so it
//          covers both translated D3D shaders and ToGLES' own GLSL.
//
//=============================================================================//
#ifdef PLATFORM_VITA

#include "vita_glsl.h"

#include <ctype.h>
#include <stdio.h>
#include <map>
#include <vector>
#include <string.h>
#include <string>

enum VitaSamplerType
{
	SAMPLER_2D,
	SAMPLER_CUBE,
	SAMPLER_3D,
	SAMPLER_2D_SHADOW,
};

static bool IsIdentChar( char c )
{
	return isalnum( (unsigned char)c ) || c == '_';
}

// Whether the identifier at pos is the whole word (not part of a longer one)
static bool IsWordAt( const std::string &s, size_t pos, const char *word )
{
	size_t len = strlen( word );
	if ( s.compare( pos, len, word ) != 0 )
		return false;
	if ( pos > 0 && IsIdentChar( s[pos - 1] ) )
		return false;
	return pos + len >= s.size() || !IsIdentChar( s[pos + len] );
}

static void ReplaceWord( std::string &s, const char *from, const char *to )
{
	size_t fromLen = strlen( from ), toLen = strlen( to );
	for ( size_t pos = s.find( from ); pos != std::string::npos; pos = s.find( from, pos ) )
	{
		if ( IsWordAt( s, pos, from ) )
		{
			s.replace( pos, fromLen, to );
			pos += toLen;
		}
		else
		{
			pos += fromLen;
		}
	}
}

static bool StartsWith( const std::string &s, const char *prefix )
{
	return s.compare( 0, strlen( prefix ), prefix ) == 0;
}

// Records the type of every sampler declared, whether as a uniform or as a
// function parameter (ToGLES' shadow helpers take sampler2D parameters)
static void CollectSamplers( const std::string &s, std::map<std::string, VitaSamplerType> &samplers )
{
	static const struct { const char *keyword; VitaSamplerType type; } s_Types[] =
	{
		{ "sampler2DShadow", SAMPLER_2D_SHADOW },
		{ "sampler2D", SAMPLER_2D },
		{ "samplerCube", SAMPLER_CUBE },
		{ "sampler3D", SAMPLER_3D },
	};

	for ( size_t t = 0; t < sizeof( s_Types ) / sizeof( s_Types[0] ); t++ )
	{
		const char *keyword = s_Types[t].keyword;
		for ( size_t pos = s.find( keyword ); pos != std::string::npos; pos = s.find( keyword, pos + 1 ) )
		{
			if ( !IsWordAt( s, pos, keyword ) )
				continue;

			size_t name = pos + strlen( keyword );
			while ( name < s.size() && isspace( (unsigned char)s[name] ) )
				name++;
			size_t end = name;
			while ( end < s.size() && IsIdentChar( s[end] ) )
				end++;
			if ( end > name )
				samplers[s.substr( name, end - name )] = s_Types[t].type;
		}
	}
}

// Index just past the ')' matching the '(' at open, or npos
static size_t MatchParen( const std::string &s, size_t open )
{
	int depth = 0;
	for ( size_t i = open; i < s.size(); i++ )
	{
		if ( s[i] == '(' )
			depth++;
		else if ( s[i] == ')' && --depth == 0 )
			return i + 1;
	}
	return std::string::npos;
}

// Splits the arguments of the call whose '(' is at open; returns the index
// just past the matching ')', or npos
static size_t SplitArgs( const std::string &s, size_t open, std::vector<std::string> &args )
{
	args.clear();
	int depth = 0;
	size_t argStart = open + 1;
	for ( size_t i = open; i < s.size(); i++ )
	{
		char c = s[i];
		if ( c == '(' )
		{
			depth++;
		}
		else if ( c == ')' && --depth == 0 )
		{
			args.push_back( s.substr( argStart, i - argStart ) );
			return i + 1;
		}
		else if ( c == ',' && depth == 1 )
		{
			args.push_back( s.substr( argStart, i - argStart ) );
			argStart = i + 1;
		}
	}
	return std::string::npos;
}

// vitaGL's translator has no shadow samplers: do what shadow2D does with
// the default GL_LEQUAL compare, 1 where the reference depth is <= the stored
// depth, as a plain lookup and step()
static std::string ShadowLookup( const std::string &sampler, const std::string &coord, bool bProjective )
{
	std::string c = "(" + coord + ")";
	if ( bProjective )
		return "vec4(step(" + c + ".z / " + c + ".w, texture2D(" + sampler + ", " + c + ".xy / " + c + ".w).r))";
	return "vec4(step(" + c + ".z, texture2D(" + sampler + ", " + c + ".xy).r))";
}

// Renames texture(), textureLod() and textureProj() calls after the type of
// the sampler passed as their first argument, and expands shadow lookups
static void ConvertTextureCalls( std::string &s, const std::map<std::string, VitaSamplerType> &samplers )
{
	static const char *s_Funcs[] = { "textureProj", "textureLod", "texture", "shadow2DProj", "shadow2D" };

	for ( size_t f = 0; f < sizeof( s_Funcs ) / sizeof( s_Funcs[0] ); f++ )
	{
		const char *func = s_Funcs[f];
		size_t funcLen = strlen( func );

		for ( size_t pos = s.find( func ); pos != std::string::npos; pos = s.find( func, pos + 1 ) )
		{
			if ( !IsWordAt( s, pos, func ) )
				continue;

			size_t open = pos + funcLen;
			while ( open < s.size() && isspace( (unsigned char)s[open] ) )
				open++;
			if ( open >= s.size() || s[open] != '(' )
				continue;

			size_t arg = open + 1;
			while ( arg < s.size() && isspace( (unsigned char)s[arg] ) )
				arg++;
			size_t argEnd = arg;
			while ( argEnd < s.size() && IsIdentChar( s[argEnd] ) )
				argEnd++;

			std::map<std::string, VitaSamplerType>::const_iterator it = samplers.find( s.substr( arg, argEnd - arg ) );
			VitaSamplerType type = it != samplers.end() ? it->second : SAMPLER_2D;

			if ( type == SAMPLER_2D_SHADOW )
			{
				std::vector<std::string> args;
				size_t close = SplitArgs( s, open, args );
				if ( close != std::string::npos && args.size() >= 2 )
				{
					bool bProjective = ( f == 0 || f == 3 ); // textureProj, shadow2DProj
					std::string lookup = ShadowLookup( args[0], args[1], bProjective );
					s.replace( pos, close - pos, lookup );
				}
				continue;
			}

			// shadow2D on a non-shadow sampler is ToGLES' own helper macro
			if ( f >= 3 )
				continue;

			if ( type == SAMPLER_3D )
			{
				// GXM has no volume textures; only color correction samples
				// them, and it's kept disabled. Replace the whole lookup.
				size_t close = MatchParen( s, open );
				if ( close != std::string::npos )
					s.replace( pos, close - pos, "vec4(0.0)" );
				continue;
			}

			const char *name;
			if ( f == 0 ) // textureProj
				name = type == SAMPLER_2D_SHADOW ? "shadow2DProj" : "texture2DProj";
			else if ( f == 1 ) // textureLod
				name = type == SAMPLER_CUBE ? "textureCubeLod" : "texture2DLod";
			else
				name = type == SAMPLER_CUBE ? "textureCube" : type == SAMPLER_2D_SHADOW ? "shadow2D" : "texture2D";

			s.replace( pos, funcLen, name );
		}
	}
}

// Rewrites in/out declarations at the start of a line into attribute/varying,
// and drops declared fragment outputs in favour of the built-ins
static void ConvertDeclarations( std::string &s, bool bFragment )
{
	std::string out;
	out.reserve( s.size() );

	size_t lineStart = 0;
	while ( lineStart < s.size() )
	{
		size_t lineEnd = s.find( '\n', lineStart );
		if ( lineEnd == std::string::npos )
			lineEnd = s.size();
		std::string line = s.substr( lineStart, lineEnd - lineStart );

		size_t first = 0;
		while ( first < line.size() && isspace( (unsigned char)line[first] ) )
			first++;
		std::string body = line.substr( first );

		bool keep = true;
		// compatibility defines for the other direction, and fragment output
		// declarations: GLSL ES 1.00 has these built in
		if ( StartsWith( body, "#define varying" ) || StartsWith( body, "#define attribute" ) ||
			 StartsWith( body, "#define gl_FragData _gl_FragData" ) ||
			 ( bFragment && StartsWith( body, "out " ) ) )
		{
			keep = false;
		}
		// precision for sampler3D isn't valid GLSL ES 1.00
		else if ( StartsWith( body, "precision " ) && body.find( "sampler" ) != std::string::npos )
		{
			keep = false;
		}
		else
		{
			if ( StartsWith( body, "centroid " ) )
				body = body.substr( 9 );

			if ( StartsWith( body, "in " ) )
				body = ( bFragment ? "varying " : "attribute " ) + body.substr( 3 );
			else if ( !bFragment && StartsWith( body, "out " ) )
				body = "varying " + body.substr( 4 );

			line = line.substr( 0, first ) + body;
		}

		if ( keep )
		{
			out += line;
			out += '\n';
		}
		lineStart = lineEnd + 1;
	}

	s.swap( out );
}

// GXM renders to a single color target. Writes to further gl_FragData
// entries (shaders that also output normals/depth for multiple render
// targets) go to a throwaway variable instead; only target 0 is drawn.
static void DropExtraRenderTargets( std::string &s )
{
	bool bFound = false;
	for ( int i = 1; i < 4; i++ )
	{
		char target[32];
		snprintf( target, sizeof( target ), "gl_FragData[%d]", i );
		for ( size_t pos = s.find( target ); pos != std::string::npos; pos = s.find( target, pos ) )
		{
			s.replace( pos, strlen( target ), "vita_UnusedFragData" );
			bFound = true;
		}
	}

	if ( !bFound )
		return;

	// declare it after the precision statement, ahead of any use
	size_t decl = s.find( "precision " );
	decl = decl != std::string::npos ? s.find( '\n', decl ) : std::string::npos;
	decl = decl != std::string::npos ? decl + 1 : 0;
	s.insert( decl, "vec4 vita_UnusedFragData;\n" );
}

void VitaConvertGLSL( const char *pSource, int nLength, bool bFragment, std::string &out )
{
	out.assign( pSource, nLength );

	size_t version = out.find( "#version 300 es" );
	if ( version != std::string::npos )
		out.replace( version, 15, "#version 100" );

	ConvertDeclarations( out, bFragment );

	std::map<std::string, VitaSamplerType> samplers;
	CollectSamplers( out, samplers );
	ConvertTextureCalls( out, samplers );

	// vitaGL spots built-ins by plain substring search: "_gl_FrontColor"
	// made it declare its own gl_FrontColor output on top of ours, both on
	// COLOR0. Use names without "gl_" (bound to fixed slots by the launcher).
	ReplaceWord( out, "_gl_FrontColor", "vColor0" );
	ReplaceWord( out, "_gl_FrontSecondaryColor", "vColor1" );
	ReplaceWord( out, "_gl_FogFragCoord", "vFogCoord" );

	// GLSL ES 1.00 has no sampler3D, and vitaGL no sampler2DShadow; their
	// lookups are gone by now
	ReplaceWord( out, "sampler3D", "sampler2D" );
	ReplaceWord( out, "sampler2DShadow", "sampler2D" );

	// vitaGL's translator chokes on double negation ("a - -b"), which the
	// D3D shader translator emits for subtracting a negated register
	for ( size_t pos = out.find( '-' ); pos != std::string::npos; pos = out.find( '-', pos + 1 ) )
	{
		size_t next = pos + 1;
		while ( next < out.size() && out[next] == ' ' )
			next++;
		if ( next > pos + 1 && next < out.size() && out[next] == '-' )
			out.replace( pos, next - pos + 1, "+ " );
	}

	if ( bFragment )
	{
		ReplaceWord( out, "_gl_FragColor", "gl_FragColor" );
		ReplaceWord( out, "_gl_FragData", "gl_FragData" );
		DropExtraRenderTargets( out );
	}
}

#endif // PLATFORM_VITA

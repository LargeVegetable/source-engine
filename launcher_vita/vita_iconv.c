//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: iconv for the engine modules. VitaSDK's newlib is built without
//          any built-in encodings (it would load conversion tables from
//          files), so every conversion the engine asks for failed, leaving
//          localized strings empty. The engine only converts between UTF-8,
//          UCS-2/UTF-16 and UCS-4/UTF-32, all little endian, which are done
//          here directly. Defining iconv_open/iconv/iconv_close here keeps
//          newlib's versions out of the link.
//
//=============================================================================//
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

typedef enum
{
	ENC_UTF8,
	ENC_UTF16LE, // also UCS-2: surrogate pairs are only produced when needed
	ENC_UTF32LE, // also UCS-4 and wchar_t, which is 4 bytes on Vita
} VitaEncoding;

typedef struct
{
	VitaEncoding from;
	VitaEncoding to;
} VitaIconv;

static int ParseEncoding( const char *name, VitaEncoding *enc )
{
	// ignore suffixes like //TRANSLIT or //IGNORE
	char buf[32];
	size_t len = strcspn( name, "/" );
	if ( len >= sizeof( buf ) )
		return 0;
	memcpy( buf, name, len );
	buf[len] = '\0';

	if ( !strcasecmp( buf, "UTF-8" ) || !strcasecmp( buf, "UTF8" ) )
		*enc = ENC_UTF8;
	else if ( !strcasecmp( buf, "UCS-2LE" ) || !strcasecmp( buf, "UTF-16LE" ) || !strcasecmp( buf, "UCS-2" ) || !strcasecmp( buf, "UTF-16" ) )
		*enc = ENC_UTF16LE;
	else if ( !strcasecmp( buf, "UCS-4LE" ) || !strcasecmp( buf, "UTF-32LE" ) || !strcasecmp( buf, "UCS-4" ) ||
			  !strcasecmp( buf, "UTF-32" ) || !strcasecmp( buf, "WCHAR_T" ) )
		*enc = ENC_UTF32LE;
	else
		return 0;
	return 1;
}

iconv_t iconv_open( const char *tocode, const char *fromcode )
{
	VitaEncoding from, to;
	if ( !ParseEncoding( fromcode, &from ) || !ParseEncoding( tocode, &to ) )
	{
		errno = EINVAL;
		return (iconv_t)-1;
	}

	VitaIconv *cd = (VitaIconv *)malloc( sizeof( VitaIconv ) );
	if ( !cd )
	{
		errno = ENOMEM;
		return (iconv_t)-1;
	}
	cd->from = from;
	cd->to = to;
	return (iconv_t)cd;
}

int iconv_close( iconv_t cd )
{
	free( cd );
	return 0;
}

// Decodes one code point; returns bytes consumed, 0 for incomplete input,
// -1 for an invalid sequence
static int Decode( VitaEncoding enc, const unsigned char *in, size_t len, uint32_t *cp )
{
	switch ( enc )
	{
	case ENC_UTF8:
	{
		unsigned char c = in[0];
		int n;
		uint32_t value;
		if ( c < 0x80 ) { *cp = c; return 1; }
		else if ( ( c & 0xe0 ) == 0xc0 ) { n = 2; value = c & 0x1f; }
		else if ( ( c & 0xf0 ) == 0xe0 ) { n = 3; value = c & 0x0f; }
		else if ( ( c & 0xf8 ) == 0xf0 ) { n = 4; value = c & 0x07; }
		else return -1;

		if ( len < (size_t)n )
			return 0;
		for ( int i = 1; i < n; i++ )
		{
			if ( ( in[i] & 0xc0 ) != 0x80 )
				return -1;
			value = ( value << 6 ) | ( in[i] & 0x3f );
		}
		*cp = value;
		return n;
	}

	case ENC_UTF16LE:
	{
		if ( len < 2 )
			return 0;
		uint32_t unit = in[0] | ( in[1] << 8 );
		if ( unit >= 0xd800 && unit < 0xdc00 )
		{
			if ( len < 4 )
				return 0;
			uint32_t low = in[2] | ( in[3] << 8 );
			if ( low < 0xdc00 || low >= 0xe000 )
				return -1;
			*cp = 0x10000 + ( ( unit - 0xd800 ) << 10 ) + ( low - 0xdc00 );
			return 4;
		}
		*cp = unit;
		return 2;
	}

	case ENC_UTF32LE:
		if ( len < 4 )
			return 0;
		*cp = in[0] | ( in[1] << 8 ) | ( in[2] << 16 ) | ( (uint32_t)in[3] << 24 );
		return 4;
	}
	return -1;
}

// Encodes one code point; returns bytes written, 0 if it doesn't fit
static int Encode( VitaEncoding enc, uint32_t cp, unsigned char *out, size_t len )
{
	switch ( enc )
	{
	case ENC_UTF8:
		if ( cp < 0x80 )
		{
			if ( len < 1 ) return 0;
			out[0] = (unsigned char)cp;
			return 1;
		}
		if ( cp < 0x800 )
		{
			if ( len < 2 ) return 0;
			out[0] = 0xc0 | ( cp >> 6 );
			out[1] = 0x80 | ( cp & 0x3f );
			return 2;
		}
		if ( cp < 0x10000 )
		{
			if ( len < 3 ) return 0;
			out[0] = 0xe0 | ( cp >> 12 );
			out[1] = 0x80 | ( ( cp >> 6 ) & 0x3f );
			out[2] = 0x80 | ( cp & 0x3f );
			return 3;
		}
		if ( len < 4 ) return 0;
		out[0] = 0xf0 | ( cp >> 18 );
		out[1] = 0x80 | ( ( cp >> 12 ) & 0x3f );
		out[2] = 0x80 | ( ( cp >> 6 ) & 0x3f );
		out[3] = 0x80 | ( cp & 0x3f );
		return 4;

	case ENC_UTF16LE:
		if ( cp >= 0x10000 )
		{
			if ( len < 4 ) return 0;
			uint32_t v = cp - 0x10000;
			uint32_t high = 0xd800 + ( v >> 10 ), low = 0xdc00 + ( v & 0x3ff );
			out[0] = high & 0xff; out[1] = high >> 8;
			out[2] = low & 0xff; out[3] = low >> 8;
			return 4;
		}
		if ( len < 2 ) return 0;
		out[0] = cp & 0xff;
		out[1] = cp >> 8;
		return 2;

	case ENC_UTF32LE:
		if ( len < 4 ) return 0;
		out[0] = cp & 0xff; out[1] = ( cp >> 8 ) & 0xff;
		out[2] = ( cp >> 16 ) & 0xff; out[3] = cp >> 24;
		return 4;
	}
	return 0;
}

size_t iconv( iconv_t handle, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft )
{
	// no shift state to reset in any supported encoding
	if ( !inbuf || !*inbuf )
		return 0;

	VitaIconv *cd = (VitaIconv *)handle;
	while ( *inbytesleft > 0 )
	{
		uint32_t cp;
		int consumed = Decode( cd->from, (const unsigned char *)*inbuf, *inbytesleft, &cp );
		if ( consumed < 0 )
		{
			errno = EILSEQ;
			return (size_t)-1;
		}
		if ( consumed == 0 )
		{
			errno = EINVAL;
			return (size_t)-1;
		}

		int written = Encode( cd->to, cp, (unsigned char *)*outbuf, *outbytesleft );
		if ( written == 0 )
		{
			errno = E2BIG;
			return (size_t)-1;
		}

		*inbuf += consumed;
		*inbytesleft -= consumed;
		*outbuf += written;
		*outbytesleft -= written;
	}
	return 0;
}

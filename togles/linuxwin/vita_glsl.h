//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: GLSL ES 3.00 -> 1.00 conversion for vitaGL, see vita_glsl.cpp
//
//=============================================================================//
#ifndef VITA_GLSL_H
#define VITA_GLSL_H

#include <string>

void VitaConvertGLSL( const char *pSource, int nLength, bool bFragment, std::string &out );

#endif // VITA_GLSL_H

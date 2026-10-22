//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: The game this launcher is built for: its data folder on the
//          memory card, its game directory, its names and its version.
//          build.sh reads the game from the waf build (--build-games) and
//          defines VITA_GAME_PORTAL for Portal; without it the launcher is
//          Half-Life 2's.
//
//=============================================================================//
#ifndef VITA_GAME_H
#define VITA_GAME_H

#include "version.h"

#if defined( VITA_GAME_PORTAL )
#define VITA_GAME_NAME		"Portal"
#define VITA_GAME_SHORT		"Portal"			// in the vita_user.cfg template
#define VITA_GAME_DIR		"portal"			// -game: the folder with gameinfo.txt
#define VITA_BASE_DIR		"ux0:data/portal"
#define VITA_FILE_PREFIX	"portal"			// ux0:data/portal_vita_boot.txt, portal_shot_NN.tga
#define VITA_PORT_VERSION	VITA_PORTAL_VERSION
#define VITA_SHARED_DIR		"ux0:data/halflife2"	// the HL2 port's hl2 and platform, when Portal has no copy of its own
#else
#define VITA_GAME_NAME		"Half-Life 2"
#define VITA_GAME_SHORT		"HL2"
#define VITA_GAME_DIR		"hl2"
#define VITA_BASE_DIR		"ux0:data/halflife2"
#define VITA_FILE_PREFIX	"hl2"
#define VITA_PORT_VERSION	VITA_HL2_VERSION
#endif

#endif // VITA_GAME_H

//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Each game's port version, shown in the boot log and the main
//          menu (game.h picks the one being built). launcher_vita/build.sh
//          reads VITA_<GAME>_APP_VER for param.sfo, package_release.sh
//          VITA_<GAME>_VERSION for the zip's name.
//
//=============================================================================//
#ifndef VITA_VERSION_H
#define VITA_VERSION_H

#define VITA_HL2_VERSION		"0.1.2-beta"
#define VITA_HL2_APP_VER		"00.12"		// param.sfo APP_VER: XX.YY

#define VITA_PORTAL_VERSION		"0.1.0-beta"
#define VITA_PORTAL_APP_VER		"00.10"

#endif // VITA_VERSION_H

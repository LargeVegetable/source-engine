//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: The Vita's menu screens, in the spirit of the Xbox 360/PS3 UI:
//          full-screen lists driven by the D-pad, cross and circle (touch taps
//          work too), in place of the PC's mouse dialogs. New game (chapters),
//          load and save (with the save's screenshot), options in tabs
//          (controls remapping, game, audio, video) and quit. Screens exist
//          only while they're open: nothing runs during play.
//
//=============================================================================//

#ifndef VITAMENU_H
#define VITAMENU_H
#ifdef _WIN32
#pragma once
#endif

#include <vgui/VGUI.h>

namespace vgui
{
	class Panel;
};

void VitaMenu_OpenNewGame( vgui::Panel *pParent );
void VitaMenu_OpenLoadGame( vgui::Panel *pParent );
void VitaMenu_OpenSaveGame( vgui::Panel *pParent );
void VitaMenu_OpenOptions( vgui::Panel *pParent );	// starts on the controls
void VitaMenu_OpenQuit( vgui::Panel *pParent );
void VitaMenu_OpenPrepareShaders( vgui::Panel *pParent );	// compiles every shader up front

// The main menu's item font (CGameMenuItem): larger than the PC scheme's
vgui::HFont VitaMenu_ItemFont();

#endif // VITAMENU_H

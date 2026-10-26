//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Vita port: the main menu's PREPARE SHADERS. GameUI (VitaMenu.cpp)
//          runs it a step per frame through VitaPrepareShaders, exported by
//          materialsystem (cmaterialsystem.cpp), which precaches every
//          material and has shaderapidx9 compile every shader they can use
//          into vitaGL's shader cache.
//
//=============================================================================//
#ifndef VITA_PREPARE_SHADERS_H
#define VITA_PREPARE_SHADERS_H

enum VitaPrepareCommand_t
{
	VITA_PREPARE_QUERY = 0,	// fill in the progress kept from earlier runs
	VITA_PREPARE_START,		// begin, or continue where the last run stopped
	VITA_PREPARE_RESTART,	// begin from the first material
	VITA_PREPARE_STEP,		// work for the budget given
	VITA_PREPARE_STOP,		// stop, keeping the progress
};

enum VitaPrepareState_t
{
	VITA_PREPARE_IDLE = 0,
	VITA_PREPARE_FINDING,	// listing the materials
	VITA_PREPARE_RUNNING,
	VITA_PREPARE_DONE,
	VITA_PREPARE_STOPPED,
};

struct VitaPrepareStatus_t
{
	int nState;				// VitaPrepareState_t
	int nMaterialsDone;		// of nMaterialsTotal (0 while finding them)
	int nMaterialsTotal;
	int nShadersCreated;	// this run
	int nShadersFailed;
	int nSavedDone;			// VITA_PREPARE_QUERY: kept from earlier runs, of nSavedTotal
	int nSavedTotal;
	int nSavedSkipped;		// materials skipped after runs that ended without stopping
	int bSavedComplete;		// a run got through every material
	char szCurrent[96];		// the material being prepared
};

// materialsystem's export, found with dlsym
typedef int ( *VitaPrepareShadersFn_t )( int nCommand, unsigned nBudgetUs, VitaPrepareStatus_t *pStatus );

#endif // VITA_PREPARE_SHADERS_H

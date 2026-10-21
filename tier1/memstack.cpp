//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//

#if defined( _WIN32 ) && !defined( _X360 )
#define WIN_32_LEAN_AND_MEAN
#include <windows.h>
#define VA_COMMIT_FLAGS MEM_COMMIT
#define VA_RESERVE_FLAGS MEM_RESERVE
#elif defined( _X360 )
#define VA_COMMIT_FLAGS (MEM_COMMIT|MEM_NOZERO|MEM_LARGE_PAGES)
#define VA_RESERVE_FLAGS (MEM_RESERVE|MEM_LARGE_PAGES)
#endif

#ifdef PLATFORM_VITA
// Stacks are sized for the worst case (the hunk alone is 48 MB) and rely on
// reserving address space and committing pages as they fill, like the
// Windows build does with VirtualAlloc. Allocating them outright, as other
// POSIX builds do, used up the whole Vita heap before the main menu.
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>

// kubridge (bythos14's, src/mprotect.c) reservations lock the whole Vita
// when the game exits (quit, or closing from LiveArea) if megabytes are left
// partly committed. Exit tests (launcher, since removed): four
// or five reservations with 64-256 KB committed in each lock, whatever the
// reservation sizes; one or four with whole megabytes committed (in 64 KB or
// 512 KB commits) don't. A megabyte is what one entry of the first-level page
// table maps. So these stacks commit and decommit whole megabytes at megabyte
// boundaries, each as two 512 KB commits (a tested pattern), which also means
// kubridge never splits a committed page (that broke its page list, 21:15
// build). Stacks under a megabyte are allocated outright instead
#define VITA_COMMIT_CHUNK ( 1024 * 1024 )
extern "C" void so_reserve_commit_add( const void *pStack, int nKB );	// launcher: for the memory log
extern "C" void so_reserve_register( const void *pStack, void *pCaller );
extern "C" void so_reserve_unregister( const void *pStack );
#define VITA_COMMIT_PIECE ( 512 * 1024 )

// Commits [pStart, pStart + nBytes), whole megabytes. On failure, decommits
// what this call committed, including the failed piece (kubridge marks it
// committed before it finds no memory), so the commit limit stays exact
static int VitaCommitChunks( unsigned char *pStart, unsigned nBytes )
{
	for ( unsigned nDone = 0; nDone < nBytes; nDone += VITA_COMMIT_PIECE )
	{
		int res = kuKernelMemCommit( pStart + nDone, VITA_COMMIT_PIECE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, NULL );
		if ( res < 0 )
		{
			kuKernelMemDecommit( pStart, nDone + VITA_COMMIT_PIECE );
			return res;
		}
	}
	return 0;
}
#endif

#include "tier0/dbg.h"
#include "memstack.h"
#include "utlmap.h"
#include "tier0/memdbgon.h"

#ifdef PLATFORM_VITA
// Reservations are whole megabytes from a megabyte boundary, so the stacks'
// committed megabytes are whole first-level page table entries. The kernel
// puts blocks of a megabyte or more on a boundary anyway; one it doesn't is
// made again at the next boundary inside a megabyte-larger probe
#define VITA_RESERVE_ALIGN ( 1024 * 1024 )
static SceUID VitaReserve( void **ppBase, unsigned nSize )
{
	nSize = AlignValue( nSize, VITA_RESERVE_ALIGN );
	void *pBase = NULL;
	SceUID block = kuKernelMemReserve( &pBase, nSize, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW );
	if ( block < 0 || !( (uintptr_t)pBase & ( VITA_RESERVE_ALIGN - 1 ) ) )
	{
		*ppBase = pBase;
		return block;
	}

	// Find room for a megabyte more, then reserve at the boundary inside it
	sceKernelFreeMemBlock( block );
	void *pProbe = NULL;
	block = kuKernelMemReserve( &pProbe, nSize + VITA_RESERVE_ALIGN, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW );
	if ( block < 0 )
		return block;
	sceKernelFreeMemBlock( block );
	pBase = AlignValue( (unsigned char *)pProbe, VITA_RESERVE_ALIGN );
	block = kuKernelMemReserve( &pBase, nSize, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW );
	if ( block < 0 )
	{
		// shouldn't happen: keep going with a block off the boundary
		Warning( "CMemoryStack: can't reserve %u KB at a megabyte boundary (0x%08x)\n", nSize / 1024, block );
		pBase = NULL;
		block = kuKernelMemReserve( &pBase, nSize + VITA_RESERVE_ALIGN, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW );
	}
	*ppBase = pBase;
	return block;
}
#endif

#ifdef _WIN32
#pragma warning(disable:4073)
#pragma init_seg(lib)
#endif

//-----------------------------------------------------------------------------

MEMALLOC_DEFINE_EXTERNAL_TRACKING(CMemoryStack);

//-----------------------------------------------------------------------------

CMemoryStack::CMemoryStack()
 : 	m_pBase( NULL ),
	m_pNextAlloc( NULL ),
	m_pAllocLimit( NULL ),
	m_pCommitLimit( NULL ),
	m_alignment( 16 ),
#if defined(_WIN32) || defined( PLATFORM_VITA )
 	m_commitSize( 0 ),
	m_minCommit( 0 ),
#endif
#ifdef PLATFORM_VITA
	m_reserveBlock( -1 ),
#endif
 	m_maxSize( 0 )
{
}
	
//-------------------------------------

CMemoryStack::~CMemoryStack()
{
	if ( m_pBase )
		Term();
}

//-------------------------------------

bool CMemoryStack::Init( unsigned maxSize, unsigned commitSize, unsigned initialCommit, unsigned alignment )
{
	Assert( !m_pBase );

#ifdef _X360
	m_bPhysical = false;
#endif

	m_maxSize = maxSize;
	m_alignment = AlignValue( alignment, 4 );

	Assert( m_alignment == alignment );
	Assert( m_maxSize > 0 );

#if defined(_WIN32)
	if ( commitSize != 0 )
	{
		m_commitSize = commitSize;
	}

	unsigned pageSize;

#ifndef _X360
	SYSTEM_INFO sysInfo;
	GetSystemInfo( &sysInfo );
	Assert( !( sysInfo.dwPageSize & (sysInfo.dwPageSize-1)) );
	pageSize = sysInfo.dwPageSize;
#else
	pageSize = 64*1024;
#endif

	if ( m_commitSize == 0 )
	{
		m_commitSize = pageSize;
	}
	else
	{
		m_commitSize = AlignValue( m_commitSize, pageSize );
	}

	m_maxSize = AlignValue( m_maxSize, m_commitSize );
	
	Assert( m_maxSize % pageSize == 0 && m_commitSize % pageSize == 0 && m_commitSize <= m_maxSize );

	m_pBase = (unsigned char *)VirtualAlloc( NULL, m_maxSize, VA_RESERVE_FLAGS, PAGE_NOACCESS );
	Assert( m_pBase );
	m_pCommitLimit = m_pNextAlloc = m_pBase;

	if ( initialCommit )
	{
		initialCommit = AlignValue( initialCommit, m_commitSize );
		Assert( initialCommit < m_maxSize );
		if ( !VirtualAlloc( m_pCommitLimit, initialCommit, VA_COMMIT_FLAGS, PAGE_READWRITE ) )
			return false;
		m_minCommit = initialCommit;
		m_pCommitLimit += initialCommit;
		MemAlloc_RegisterExternalAllocation( CMemoryStack, GetBase(), GetSize() );
	}

#elif defined( PLATFORM_VITA )
	m_reserveBlock = -1;
	if ( m_maxSize >= VITA_COMMIT_CHUNK )
	{
		m_commitSize = VITA_COMMIT_CHUNK;
		m_maxSize = AlignValue( m_maxSize, m_commitSize );

		// Starts at a megabyte boundary (the fallback block off a boundary
		// has a megabyte to spare), so commit and decommit points, which are
		// aligned addresses, are boundaries too
		void *pReserved = NULL;
		m_reserveBlock = VitaReserve( &pReserved, m_maxSize );
		if ( m_reserveBlock >= 0 )
		{
			m_pBase = AlignValue( (unsigned char *)pReserved, m_commitSize );
			m_pCommitLimit = m_pNextAlloc = m_pBase;
			so_reserve_register( this, __builtin_return_address( 0 ) );
			if ( initialCommit )
			{
				initialCommit = AlignValue( initialCommit, m_commitSize );
				if ( VitaCommitChunks( m_pCommitLimit, initialCommit ) < 0 )
					return false;
				so_reserve_commit_add( this, initialCommit / 1024 );
				m_minCommit = initialCommit;
				m_pCommitLimit += initialCommit;
			}
		}
		else
		{
			Warning( "CMemoryStack: reserving %u bytes failed (0x%08x), allocating outright\n", m_maxSize, m_reserveBlock );
			m_reserveBlock = -1;
		}
	}
	if ( m_reserveBlock < 0 )
	{
		// under a megabyte (the ropes' 128 KB stacks), or no reservation
		m_pBase = (byte *)MemAlloc_AllocAligned( m_maxSize, alignment ? alignment : 1 );
		m_pNextAlloc = m_pBase;
		m_pCommitLimit = m_pBase + m_maxSize;
	}
#else
	m_pBase = (byte *)MemAlloc_AllocAligned( m_maxSize, alignment ? alignment : 1 );
	m_pNextAlloc = m_pBase;
	m_pCommitLimit = m_pBase + m_maxSize;
#endif

	m_pAllocLimit = m_pBase + m_maxSize;

	return ( m_pBase != NULL );
}

//-------------------------------------

#ifdef _X360
bool CMemoryStack::InitPhysical( unsigned size, unsigned alignment )
{
	m_bPhysical = true;

	m_maxSize = m_commitSize = size;
	m_alignment = AlignValue( alignment, 4 );

	int flags = PAGE_READWRITE;
	if ( size >= 16*1024*1024 )
	{
		flags |= MEM_16MB_PAGES;
	}
	else
	{
		flags |= MEM_LARGE_PAGES;
	}
	m_pBase = (unsigned char *)XPhysicalAlloc( m_maxSize, MAXULONG_PTR, 4096, flags );
	Assert( m_pBase );
	m_pNextAlloc = m_pBase;
	m_pCommitLimit = m_pBase + m_maxSize;
	m_pAllocLimit = m_pBase + m_maxSize;

	MemAlloc_RegisterExternalAllocation( CMemoryStack, GetBase(), GetSize() );
	return ( m_pBase != NULL );
}
#endif

//-------------------------------------

void CMemoryStack::Term()
{
	FreeAll();
	if ( m_pBase )
	{
#if defined(_WIN32)
		VirtualFree( m_pBase, 0, MEM_RELEASE );
#elif defined( PLATFORM_VITA )
		if ( m_reserveBlock >= 0 )
		{
			kuKernelMemDecommit( m_pBase, m_pCommitLimit - m_pBase );
			so_reserve_commit_add( this, -(int)( ( m_pCommitLimit - m_pBase ) / 1024 ) );
			so_reserve_unregister( this );
			sceKernelFreeMemBlock( m_reserveBlock );
			m_reserveBlock = -1;
		}
		else
		{
			MemAlloc_FreeAligned( m_pBase );
		}
#else
		MemAlloc_FreeAligned( m_pBase );
#endif
		m_pBase = NULL;
	}
}

//-------------------------------------

int CMemoryStack::GetSize()
{ 
#if defined( _WIN32 ) || defined( PLATFORM_VITA )
	return m_pCommitLimit - m_pBase; 
#else
	return m_maxSize;
#endif
}


//-------------------------------------

bool CMemoryStack::CommitTo( byte *pNextAlloc ) RESTRICT
{
#ifdef _X360
	if ( m_bPhysical )
	{
		return NULL;
	}
#endif
#if defined(_WIN32)
	unsigned char *	pNewCommitLimit = AlignValue( pNextAlloc, m_commitSize );
	unsigned 		commitSize 		= pNewCommitLimit - m_pCommitLimit;
	
	if ( GetSize() )
		MemAlloc_RegisterExternalDeallocation( CMemoryStack, GetBase(), GetSize() );

	if( m_pCommitLimit + commitSize > m_pAllocLimit )
	{
		return false;
	}

	if ( !VirtualAlloc( m_pCommitLimit, commitSize, VA_COMMIT_FLAGS, PAGE_READWRITE ) )
	{
		Assert( 0 );
		return false;
	}
	m_pCommitLimit = pNewCommitLimit;

	if ( GetSize() )
		MemAlloc_RegisterExternalAllocation( CMemoryStack, GetBase(), GetSize() );
	return true;
#elif defined( PLATFORM_VITA )
	if ( m_reserveBlock < 0 )
		return false; // allocated outright: the commit limit is the end

	unsigned char *pNewCommitLimit = AlignValue( pNextAlloc, m_commitSize );
	unsigned commitSize = pNewCommitLimit - m_pCommitLimit;
	if ( m_pCommitLimit + commitSize > m_pAllocLimit )
		return false;
	int res = VitaCommitChunks( m_pCommitLimit, commitSize );
	if ( res < 0 )
	{
		// out of kernel memory (the hunk commits from what vitaGL and the
		// newlib heap leave): callers turn this into "hunk overflow" errors
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof( info );
		sceKernelGetFreeMemorySize( &info );
		Warning( "CMemoryStack: committing %u more bytes failed (0x%08x) with %u bytes committed; free main %u KB\n",
			commitSize, res, (unsigned)( m_pCommitLimit - m_pBase ), (unsigned)( info.size_user / 1024 ) );
		return false;
	}
	so_reserve_commit_add( this, commitSize / 1024 );
	m_pCommitLimit = pNewCommitLimit;
	return true;
#else
	Assert( 0 );
	return false;
#endif
}

//-------------------------------------

void CMemoryStack::FreeToAllocPoint( MemoryStackMark_t mark, bool bDecommit )
{
	void *pAllocPoint = m_pBase + mark;
	Assert( pAllocPoint >= m_pBase && pAllocPoint <= m_pNextAlloc );
	
	if ( pAllocPoint >= m_pBase && pAllocPoint < m_pNextAlloc )
	{
		if ( bDecommit )
		{
#if defined(_WIN32)
			unsigned char *pDecommitPoint = AlignValue( (unsigned char *)pAllocPoint, m_commitSize );

			if ( pDecommitPoint < m_pBase + m_minCommit )
			{
				pDecommitPoint = m_pBase + m_minCommit;
			}

			unsigned decommitSize = m_pCommitLimit - pDecommitPoint;

			if ( decommitSize > 0 )
			{
				MemAlloc_RegisterExternalDeallocation( CMemoryStack, GetBase(), GetSize() );

				VirtualFree( pDecommitPoint, decommitSize, MEM_DECOMMIT );
				m_pCommitLimit = pDecommitPoint;

				if ( mark > 0 )
				{
					MemAlloc_RegisterExternalAllocation( CMemoryStack, GetBase(), GetSize() );
				}
			}
#elif defined( PLATFORM_VITA )
			if ( m_reserveBlock >= 0 )
			{
				unsigned char *pDecommitPoint = AlignValue( (unsigned char *)pAllocPoint, m_commitSize );
				if ( pDecommitPoint < m_pBase + m_minCommit )
					pDecommitPoint = m_pBase + m_minCommit;
				if ( pDecommitPoint < m_pCommitLimit )
				{
					kuKernelMemDecommit( pDecommitPoint, m_pCommitLimit - pDecommitPoint );
					so_reserve_commit_add( this, -(int)( ( m_pCommitLimit - pDecommitPoint ) / 1024 ) );
					m_pCommitLimit = pDecommitPoint;
				}
			}
#endif
		}
		m_pNextAlloc = (unsigned char *)pAllocPoint;
	}
}

//-------------------------------------

void CMemoryStack::FreeAll( bool bDecommit )
{
	if ( m_pBase && m_pCommitLimit - m_pBase > 0 )
	{
		if ( bDecommit )
		{
#if defined(_WIN32)
			MemAlloc_RegisterExternalDeallocation( CMemoryStack, GetBase(), GetSize() );

			VirtualFree( m_pBase, m_pCommitLimit - m_pBase, MEM_DECOMMIT );
			m_pCommitLimit = m_pBase;
#elif defined( PLATFORM_VITA )
			if ( m_reserveBlock >= 0 && m_pCommitLimit > m_pBase + m_minCommit )
			{
				kuKernelMemDecommit( m_pBase + m_minCommit, m_pCommitLimit - ( m_pBase + m_minCommit ) );
				so_reserve_commit_add( this, -(int)( ( m_pCommitLimit - ( m_pBase + m_minCommit ) ) / 1024 ) );
				m_pCommitLimit = m_pBase + m_minCommit;
			}
#endif
		}
		m_pNextAlloc = m_pBase;
	}
}

//-------------------------------------

void CMemoryStack::Access( void **ppRegion, unsigned *pBytes )
{
	*ppRegion = m_pBase;
	*pBytes = ( m_pNextAlloc - m_pBase);
}

//-------------------------------------

void CMemoryStack::PrintContents()
{
	Msg( "Total used memory:      %d\n", GetUsed() );
	Msg( "Total committed memory: %d\n", GetSize() );
}

//-----------------------------------------------------------------------------

//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: sys/ioctl.h replacement for PS Vita. newlib has no ioctl; the
//          engine only uses it on sockets for FIONBIO and FIONREAD.
//
//=============================================================================//
#ifndef VITA_SYS_IOCTL_H
#define VITA_SYS_IOCTL_H

#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>

#define FIONBIO  0x5421
#define FIONREAD 0x541B

static inline int ioctl( int fd, unsigned long request, ... )
{
	va_list args;
	va_start( args, request );
	void *arg = va_arg( args, void * );
	va_end( args );

	switch ( request )
	{
	case FIONBIO:
	{
		int nonblock = *(unsigned long *)arg ? 1 : 0;
		return setsockopt( fd, SOL_SOCKET, SO_NONBLOCK, &nonblock, sizeof( nonblock ) );
	}
	case FIONREAD:
	{
		// No pending-byte query on Vita sockets; peek at the next packet.
		// Large enough for any UDP datagram and the rcon TCP reads.
		static char s_PeekBuf[65536];
		int ret = recv( fd, s_PeekBuf, sizeof( s_PeekBuf ), MSG_PEEK | MSG_DONTWAIT );
		*(unsigned long *)arg = ret > 0 ? (unsigned long)ret : 0;
		return 0;
	}
	default:
		errno = EINVAL;
		return -1;
	}
}

#endif // VITA_SYS_IOCTL_H

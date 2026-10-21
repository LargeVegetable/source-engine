//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: POSIX functions engine modules import that newlib on Vita doesn't
//          provide. Each one either does the closest sensible thing or fails
//          the way the real one would when the feature is unavailable.
//
//=============================================================================//
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>

struct passwd;

// no processes or shells
FILE *popen( const char *command, const char *type )
{
	errno = ENOSYS;
	return NULL;
}

int pclose( FILE *stream )
{
	errno = ECHILD;
	return -1;
}

int execlp( const char *file, const char *arg, ... )
{
	errno = ENOSYS;
	return -1;
}

// no signals to schedule; returns "no previous alarm"
unsigned int alarm( unsigned int seconds )
{
	return 0;
}

// no user database
struct passwd *getpwuid( uid_t uid )
{
	errno = ENOENT;
	return NULL;
}

int futimes( int fd, const struct timeval tv[2] )
{
	errno = ENOSYS;
	return -1;
}

// only used for server CPU usage stats; report none
int getrusage( int who, struct rusage *usage )
{
	memset( usage, 0, sizeof( *usage ) );
	return 0;
}

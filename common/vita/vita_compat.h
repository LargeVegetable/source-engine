//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Force-included on PS Vita to fill gaps in newlib's POSIX surface.
//
//=============================================================================//
#ifndef VITA_COMPAT_H
#define VITA_COMPAT_H

#include <limits.h>
#include <time.h>

// VitaSDK's sys/resource.h lacks C linkage guards; include it once here so
// later includes from C++ hit the include guard instead of declaring
// getrusage etc. as C++ functions
#ifdef __cplusplus
extern "C" {
#endif
#include <sys/resource.h>
#ifdef __cplusplus
}
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

// newlib has no timegm; this is the inverse of gmtime for the proleptic
// Gregorian calendar, normalising out-of-range month values like timegm does
static inline time_t timegm( struct tm *tm )
{
	long long year = tm->tm_year + 1900LL;
	long long mon = tm->tm_mon;
	year += mon / 12;
	mon %= 12;
	if ( mon < 0 )
	{
		mon += 12;
		year--;
	}

	// days from 1970-01-01 using the days-from-civil algorithm
	long long y = year - ( mon < 2 );
	long long era = ( y >= 0 ? y : y - 399 ) / 400;
	long long yoe = y - era * 400;
	long long mp = ( mon + 9 ) % 12;
	long long doy = ( 153 * mp + 2 ) / 5 + tm->tm_mday - 1;
	long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long days = era * 146097 + doe - 719468;

	return (time_t)( days * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec );
}

#ifdef __cplusplus
}
#endif

#endif // VITA_COMPAT_H

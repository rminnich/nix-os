#include <u.h>
#include <libc.h>

char dbg[256];

int
dbgclr(uchar flag)
{
	int x;

	x = dbg[flag];
	dbg[flag] = 0;
	return x;
}

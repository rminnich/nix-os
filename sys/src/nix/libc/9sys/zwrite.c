#include	<u.h>
#include	<libc.h>

int
zwrite(int fd, Zio io[], int nio)
{
	return zpwrite(fd, io, nio, -1LL);
}

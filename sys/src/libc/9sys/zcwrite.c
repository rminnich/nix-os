#include	<u.h>
#include	<libc.h>

int
zcwrite(int fd, Zio io[], int nio)
{
	return zpwrite(fd, io, nio, -1LL);
}

#include	<u.h>
#include	<libc.h>

int
zread(int fd, Zio io[], int nio, usize count)
{
	return zpread(fd, io, nio, count, -1LL);
}

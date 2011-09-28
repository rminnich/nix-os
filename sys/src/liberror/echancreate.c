#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>

Channel*
echancreate(int sz, int n)
{
	Channel *c;

	c = chancreate(sz, n);
	if(c == nil)
		sysfatal("chancreate: %r");
	return c;
}

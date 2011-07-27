#include <u.h>
#include <libc.h>
#include <tos.h>

int
getcore(int *type)
{
	if (type != nil)
		*type = _tos->nixtype;
	return _tos->core;
}

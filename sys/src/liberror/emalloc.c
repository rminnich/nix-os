#include <u.h>
#include <libc.h>
#include <error.h>

char*
estrdup(char* s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("estrdup: not enough memory");
	setmalloctag(s, getcallerpc(&s));
	return s;
}

void*
emalloc(int sz)
{
	void*	s;

	s = mallocz(sz, 1);
	if(s == nil)
		sysfatal("emalloc: not enough memory");
	setmalloctag(s, getcallerpc(&sz));
	return s;
}

void*
emallocz(int sz, int zero)
{
	void*	s;

	s = mallocz(sz, zero);
	if(s == nil)
		sysfatal("emalloc: not enough memory");
	setmalloctag(s, getcallerpc(&sz));
	return s;
}

void*
erealloc(void* p, int sz)
{

	p = realloc(p, sz);
	if(p == nil)
		sysfatal("erealloc: not enough memory");
	setmalloctag(p, getcallerpc(&p));
	return p;
}

char*
esmprint(char *fmt, ...)
{
	va_list arg;
	char *m;

	va_start(arg, fmt);
	m = vsmprint(fmt, arg);
	va_end(arg);
	if(m == nil)
		sysfatal("smprint: %r");
	setmalloctag(m, getcallerpc(&fmt));
	return m;
}

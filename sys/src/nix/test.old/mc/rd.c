#include <u.h>
#include <libc.h>

/*
 * read from a segment via segattach
 * (otherwise you can just use xd to see /mnt/segment/<name>/data)
 */
void
main(int argc, char **argv)
{
	char *ep;
	usize offset, length;
	void *va, *buf;

	ARGBEGIN{
	}ARGEND
	if(argc < 2)
		sysfatal("usage: rd name length [offset]");

	if(sizeof(length) < sizeof(vlong))
		length = strtoul(argv[1], &ep, 0);
	else
		length = strtoull(argv[1], &ep, 0);
	if(*ep)
		sysfatal("non-numeric length: %s", argv[1]);
	if(argc > 2){
		if(sizeof(offset) < sizeof(vlong))
			offset = strtoul(argv[2], &ep, 0);
		else
			offset = strtoull(argv[2], &ep, 0);
		if(*ep)
			sysfatal("non-numeric offset: %s", argv[2]);
	}else
		offset = 0;

	va = segattach(0, argv[0], nil, 0);
	if(va == (void*)~(uintptr)0)
		sysfatal("can't segattach %s: %r", argv[0]);
	buf = malloc(length);
	if(buf == nil)
		sysfatal("can't allocate buffer of %lld bytes: %r", (uvlong)length);
	memmove(buf, (char*)va+offset, length);
	write(1, buf, length);
	exits(nil);
}

#include <u.h>
#include <libc.h>

/*
 * write to a segment via segattach
 * (otherwise you can just use echo to /mnt/segment/<name>/data)
 */
void
main(int argc, char **argv)
{
	char *ep;
	usize offset;
	void *va;

	ARGBEGIN{
	}ARGEND
	if(argc < 2)
		sysfatal("usage: wr name data [offset]");

	if(argc > 2){
		if(sizeof(offset) < sizeof(vlong))
			offset = strtoul(argv[1], &ep, 0);
		else
			offset = strtoull(argv[1], &ep, 0);
		if(*ep)
			sysfatal("non-numeric offset: %s", argv[1]);
	}else
		offset = 0;
	va = segattach(0, argv[0], nil, 0);
	if(va == (void*)~(uintptr)0)
		sysfatal("can't segattach %s: %r", argv[0]);
	memmove((char*)va+offset, argv[1], strlen(argv[1]));	/* goes bang if offset is silly */
	exits(nil);
}

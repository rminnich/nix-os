#include <u.h>
#include <libc.h>

/*
 * make a segment
 */
void
main(int argc, char **argv)
{
	int fd, cfd, dfd;
	char name[128], ctlname[128];
	char *ep;
	uintptr va;
	usize length;

	ARGBEGIN{
	}ARGEND
	if(argc < 3)
		sysfatal("usage: seg name va len [initdata]");
	if(sizeof(va) < sizeof(vlong))
		va = strtoul(argv[1], &ep, 0);
	else
		va = strtoull(argv[1], &ep, 0);
	if(*ep)
		sysfatal("non-numeric address: %s", argv[1]);
	if(va & (va-1))
		sysfatal("implausible virtual address: %s", argv[1]);

	if(sizeof(length) < sizeof(vlong))
		length = strtoul(argv[2], &ep, 0);
	else
		length = strtoull(argv[2], &ep, 0);
	if(*ep)
		sysfatal("non-numeric length: %s", argv[2]);
	if(length & (length-1))
		sysfatal("implausible length: %s", argv[2]);
	
	snprint(name, sizeof(name), "/mnt/segment/%s", argv[0]);
	fd = create(name, ORDWR, DMDIR|0777);
	if(fd < 0)
		sysfatal("can't create %s: %r\n", name);
	snprint(ctlname, sizeof(ctlname), "%s/ctl", name);
	cfd = open(ctlname, OWRITE);
	if(cfd < 0)
		sysfatal("can't open %s: %r\n", ctlname);
	if(fprint(cfd, "va %#llux %#llux\n", (uvlong)va, (uvlong)length) < 0)
		sysfatal("can't set segment va/length: %r");
	if(argc > 3){
		snprint(ctlname, sizeof(ctlname), "%s/data", name);
		dfd = open(ctlname, OWRITE);
		if(dfd < 0)
			sysfatal("can't write initial data to %s: %r", name);
		if(write(dfd, argv[3], strlen(argv[3])) < 0)
			sysfatal("error writing initial data: %r");
		close(dfd);
	}
	exits(nil);
}

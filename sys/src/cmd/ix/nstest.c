/*
 * Test program for nsfile.
 *
 	X/\[/D
 ! mk 8.nstest
 ! rm /tmp/tmeasure/new
 ! rm /tmp/tmeasure/.ixd
 ! 8.nstest -Dn /tmp/tmeasure >[2=1]
 ! slay 8.nstest | rc
 */

#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>
#include <fcall.h>

#include "conf.h"
#include "dbg.h"
#include "file.h"


static void
usage(void)
{
	fprint(2, "usage: %s [-s] [-D flags] dir\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *flags;
	int dosleep;
	File *f;
	Dir *d;
	Dir wd;
	int fd, n;
	char buf[128];

	dosleep = 0;
	ARGBEGIN{
	case 'D':
		flags = EARGF(usage());
		for(;*flags != 0; flags++)
			dbg[*flags]++;
		dbg['d']++;
		break;
	case 's':
		dosleep = 1;
		break;
	}ARGEND;
	if(argc != 1)
		usage();

	fmtinstall('M', dirmodefmt);
	fmtinstall('D', shortdirfmt);
	fileinit(argv[0], 1);
	f = rootfile();
	walkfile(&f, "plotnumb");
	walkfile(&f, "resultclose");
	d = statfile(f, 0);
	print("got %D\n", d);
	free(d);
	fd = openfile(f, OWRITE);
	if(fd < 0)
		print("got %r\n");
	else{
		pwritefile(f, fd, "hola", 4, 0ULL);
		print("file %T\n", f);
		closefile(f, fd);
	}
	print("file %T\n", f);
	putfile(f);
	f = rootfile();
	if(walkfile(&f, "foo") < 0)
		print("walk failed; ok\n");
	print("file %T\n", f);
	walkfile(&f, "plotnumb");
	walkfile(&f, "resultclose");
	fd = openfile(f, OREAD);
	if(fd < 0)
		sysfatal("openfile: %r");
	n = preadfile(f, fd, buf, 4, 0ULL);
	if(n != 4)
		sysfatal("preadfile: %r");
	buf[4] = 0;
	print("read: '%s'\n", buf);
	closefile(f, fd);
	print("file %T\n", f);
	putfile(f);
	f = rootfile();
	if((fd = createfile(&f, "new", OREAD, 0775|DMDIR)) < 0)
		print("create: %r\n");
	else
		closefile(f, fd);
	print("created: %T\n", f);
	putfile(f);
	f = rootfile();
	walkfile(&f, "new");
	removefile(f);
	print("file is %T\n", f);
	putfile(f);
	f = rootfile();
	if(removefile(f) < 0)
		print("remove root: %r\n");
	putfile(f);
	nulldir(&wd);
	wd.mode = 0700;
	f = rootfile();
	if(walkfile(&f, "hola") < 0)
		print("can't walk to hola\n");
	else if(wstatfile(f, &wd) < 0)
		print("wstat: %r\n");
	print("file is %T\n", f);
	putfile(f);
	filesync();
	dumptree();
	if(dosleep){
		print("%s: sleeping forever\n", argv0);
		for(;;)
			sleep(3600);
	}
	
	threadexits(nil);
}

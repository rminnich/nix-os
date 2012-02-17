#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "fns.h"

static void
usage(void)
{
	fprint(2, "usage: %s [-DFLAGS] [-dv]\n", argv0);
	exits("usage");
}

static char xdbg[256];
static char zdbg[256];

void
threadmain(int argc, char *argv[])
{
	int verb;
	char *dev;

	dev = "disk";
	verb = 0;
	ARGBEGIN{
	case 'v':
		verb++;
		break;
	default:
		if(ARGC() >= 'A' && ARGC() <= 'Z'){
			xdbg['d'] = 1;
			xdbg[ARGC()] = 1;
		}else
			usage();
	}ARGEND;
	if(argc == 1)
		dev = argv[0];
	else if(argc > 0)
		usage();
	fmtinstall('H', mbfmt);
	fmtinstall('M', dirmodefmt);
	errinit(Errstack);
	if(catcherror())
		fatal("error: %r");
	memmove(dbg, xdbg, sizeof xdbg);
	fsfmt(dev);
	memmove(dbg, zdbg, sizeof zdbg);
	if(verb)
		fsdump(0);
	else
		fslist();
	noerror();
	exits(nil);
}


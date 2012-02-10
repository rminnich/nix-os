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

void
main(int argc, char *argv[])
{
	Fsys *fs;
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
			dbg['d'] = 1;
			dbg[ARGC()] = 1;
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
		sysfatal("error: %r");
	fs = fsfmt(dev);
	if(verb)
		fsdump(fs);
	noerror();
	exits(nil);
}


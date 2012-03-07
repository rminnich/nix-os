#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "ix.h"
#include "net.h"
#include "fns.h"

int
member(char *uid, char *member)
{
	return strcmp(uid, member);
}

void
meltfids(void)
{
}

static void
usage(void)
{
	fprint(2, "usage: %s [-DFLAGS] [disk]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *dev;
	int verb;

	dev = "disk";
	verb = 0;
	ARGBEGIN{
	case 'v':
		verb = 1;
		break;
	default:
		if((ARGC() >= 'A' && ARGC() <= 'Z') || ARGC() == '9'){
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
		fatal("error: %r");
	fsfmt(dev);
	if(verb)
		fsdump(0, 0);
	noerror();
	exits(nil);
}


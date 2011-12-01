#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>

#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "tses.h"
#include "ch.h"
#include "dbg.h"
#include "ix.h"
#include "ixfile.h"

#define SRVADDR	"tcp!*!9999"
#define TESTDIR "/sys/src/cmd/ix/Testfs"

int mainstacksize = Stack;

static void
usage(void)
{
	fprint(2, "usage: %s [-d] [-D flags] [-s srv] [addr]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *addr, *flags, *srv;

	addr = SRVADDR;
	srv = "ix";

	ARGBEGIN{
	case 'd':
		dbg['d']++;
		break;
	case 'D':
		flags = EARGF(usage());
		for(;*flags != 0; flags++)
			dbg[*flags]++;
		dbg['d']++;
		break;
	case 's':
		srv = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;
	if(argc == 1)
		addr = argv[1];
	else if(argc > 1)
		usage();

	outofmemoryexits(1);
	fmtinstall('G', ixcallfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('T', filefmt);
	fileinit(TESTDIR);
	ixinit(addr, srv);
	ixsrv();
	dtprint("testsrv: exiting\n");
	threadexits(nil);
}

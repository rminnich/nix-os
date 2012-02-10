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

void
main(int, char *argv[])
{
	Fsys *fs;

	argv0 = argv[0];
	fmtinstall('H', mbfmt);
	fmtinstall('M', dirmodefmt);
	errinit(Errstack);
	if(catcherror())
		sysfatal("error: %r");
	fs = fsopen("disk");
	fsdump(fs);
	dbg['D'] = 1;
	fsreclaim(fs);
	fsdump(fs);
	noerror();
	exits(nil);
}


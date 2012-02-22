#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>
#include <worker.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "ix.h"
#include "net.h"
#include "fns.h"

static void
postfd(char *name, int pfd)
{
	int fd;

	remove(name);
	fd = create(name, OWRITE|ORCLOSE|OCEXEC, 0600);
	if(fd < 0)
		fatal("postfd: %r\n");
	if(fprint(fd, "%d", pfd) < 0){
		close(fd);
		fatal("postfd: %r\n");
	}
	close(pfd);
}

static char*
getremotesys(char *ndir)
{
	char buf[128], *serv, *sys;
	int fd, n;

	snprint(buf, sizeof buf, "%s/remote", ndir);
	sys = nil;
	fd = open(buf, OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof(buf)-1);
		if(n>0){
			buf[n-1] = 0;
			serv = strchr(buf, '!');
			if(serv)
				*serv = 0;
			sys = strdup(buf);
		}
		close(fd);
	}
	if(sys == nil)
		sys = strdup("unknown");
	return sys;
}

void
srv9pix(char *srv, char* (*cliworker)(void *arg, void **aux))
{
	Cli *cli;
	int fd[2];
	char *name;

	name = smprint("/srv/%s", srv);
	if(pipe(fd) < 0)
		fatal("pipe: %r");
	postfd(name, fd[0]);
	consprint("listen %s\n", srv);
	cli = newcli(name, fd[1], -1);
	getworker(cliworker, cli, nil);
}

void
listen9pix(char *addr,  char* (*cliworker)(void *arg, void **aux))
{
	Cli *cli;
	char ndir[NETPATHLEN], dir[NETPATHLEN];
	int ctl, data, nctl;

	ctl = announce(addr, dir);
	if(ctl < 0)
		fatal("announce %s: %r", addr);
	consprint("listen %s\n", addr);
	for(;;){
		nctl = listen(dir, ndir);
		if(nctl < 0)
			fatal("listen %s: %r", addr);
		data = accept(nctl, ndir);
		if(data < 0){
			fprint(2, "%s: accept %s: %r\n", argv0, ndir);
			continue;
		}
		cli = newcli(getremotesys(ndir), data, nctl);
		getworker(cliworker, cli, nil);
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-DFLAGS] [-n addr] disk\n", argv0);
	exits("usage");
}

int mainstacksize = Stack;

void
threadmain(int argc, char *argv[])
{
	char *addr, *dev, *srv;

	addr = nil;
	srv = "9pix";
	ARGBEGIN{
	case 'n':
		addr = EARGF(usage());
		break;
	default:
		if(ARGC() >= 'A' && ARGC() <= 'Z' || ARGC() == '9'){
			dbg['d'] = 1;
			dbg[ARGC()] = 1;
		}else
			usage();
		dbg['x'] = dbg['X'];
	}ARGEND;
	if(argc != 1)
		usage();
	dev = argv[0];

	workerthreadcreate = proccreate;
	fmtinstall('H', mbfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('F', fcallfmt);
	fmtinstall('G', ixcallfmt);
	fmtinstall('X', fidfmt);
	fmtinstall('R', rpcfmt);
	errinit(Errstack);
	if(catcherror())
		fatal("uncatched error: %r");
	rfork(RFNAMEG);
	parseusers(defaultusers);
	fsopen(dev);
	if(srv != nil)
		srv9pix(srv, cliworker9p);
	if(addr != nil)
		listen9pix(addr, cliworker9p);

	/*
	 * fsstats();
	 * ninestats();
	 * ixstats();
	 */
	consinit();
	noerror();
	threadexits(nil);
}


/*
 * Test program for ix.
 * Get files or directories.
 *
 	X/\[/D
 ! mk all
 ! slay 8.ix 8.ixget|rc
 ! slay 8.ixget|rc
 ! 8.ix -Dfn
 ! 8.ix -Dfsn >/tmp/LOG >[2=1] &
 tail -F /tmp/LOG &
 >/tmp/SRV.out >[2=1]
 ! ls -lq /usr/nemo/acme.dump /tmp/acme.dump
 ! cd /tmp ; /usr/nemo/src/tram/8.ixget -fDsf acme.dump >[2=1]
 ! cd /tmp ; /usr/nemo/src/tram/8.ixget -cDsf acme.dump>[2=1]
 >/tmp/GET.out>[2=1]
 ! cd /tmp ; /usr/nemo/src/tram/8.ixget -Df acme.dump
 ! touch $home/acme.dump
 >/tmp/PUT.out>[2=1]
 ! tstack 8.ix
 ! tstack 8.ixget
 ! leak -s 8.ix|rc|acid 8.ix
 ! leak -s 8.ixget|rc|acid 8.ixget
 ! unmount /tmp/acme.dump /usr/nemo/acme.dump
 ! cmp /tmp/ohist /usr/nemo/ohist
 ! cmp /tmp/adump /usr/nemo/adump
 ! rm -f /tmp/^(acme.dump PULL ohist) /usr/nemo/adump
 *
 * Issues:
 * - test unexpected server errors
 * - test unexpected client errors
 * - adapt to test cond
 *	e.g.: get if newer, get if vers newer
 *   and clunk
 */

#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>
#include <fcall.h>

#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "tses.h"
#include "ch.h"
#include "dbg.h"
#include "fs.h"
#include "ixreqs.h"

#define CLIADDR	"tcp!localhost!9999"

int mainstacksize = Stack;

static Cmux *cm;
static Ses *ses;
Mpool *pool, *spool;
static Channel *wc;
static int rootfid = -1;
static ulong msz;
static int doflush;
static int twritehdrsz;

static long
wdata(int fd, void *a, long cnt, uvlong off)
{

	return pwrite(fd, a, cnt, off);
}

static long
wdent(int fd, void *a, long cnt, uvlong)
{
	Dir d;
	int n;
	char buf[512];
	uchar *data;
	long tot;

	data = a;
	for(tot = 0; tot < cnt; tot += n){
		n = convM2D(data+tot, cnt-tot, &d, buf);
		if(n <= BIT32SZ)
			break;
		fprint(fd, "%D\n", &d);
	}
	return cnt;
}

/*
 * If doflush, this will test a flush (abortch) before
 * retrieving all data.
 * Because ch does not convey more data to the receiver when it
 * enters the flushing state, we can only flush when we are sure
 * regarding the state of the server (i.e., if we sent a clone,
 * should either clone OCERR|OCEND or wait until receiving the Rclone,
 * to learn of the allocated fid, before flushing).
 */
static void
getproc(void *a)
{
	char *path, *els[64], *fn;
	int nels, fd, i;
	Ch *ch;
	Dir d;
	char buf[512];
	long nr;
	uvlong offset;
	Msg *m;

	path = a;
	threadsetname("getproc %s", path);
	if(*path == '/')
		path++;
	path = estrdup(path);
	nels = getfields(path, els, nelem(els), 1, "/");
	if(nels < 1)
		sysfatal("short path");
	fn = els[nels-1];
	ch = newch(cm);
	xtfid(ch, rootfid, 0);
	xtclone(ch, OCEND|OCERR, 0);
	for(i = 0; i < nels; i++)
		xtwalk(ch, els[i], 0);
	xtstat(ch, 0);
	xtopen(ch, OREAD, 0);
	xtread(ch, -1, 0ULL, msz, 1);
	/* fid automatically clunked on errors and eof */

	fd = -1;
	if(xrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		goto Done;
	}
	if(xrclone(ch) < 0){
		fprint(2, "%s: clone: %r\n", a);
		goto Done;
	}
	for(i = 0; i < nels; i++)
		if(xrwalk(ch, nil) < 0){
			fprint(2, "%s: walk[%s]: %r\n", a, els[i]);
			goto Done;
		}
	if(xrstat(ch, &d, buf) < 0){
		fprint(2, "%s: stat: %r\n", a);
		goto Done;
	}
	dtprint("get: %D\n", &d);
	remove(fn);	/* ignore errors */
	fd = create(fn, OWRITE, d.mode&~DMDIR);
	if(fd < 0){
		fprint(2, "create %s: %r\n", fn);
		goto Done;
	}
	if(xropen(ch) < 0){
		fprint(2, "%s: open: %r\n", a);
		goto Done;
	}
	offset = 0ULL;
	do{
		if(doflush){	/* flush testing */
			doflush = 0;
			abortch(ch);
			goto Done;
		}
		m = xrread(ch);
		if(m == nil){
			fprint(2, "%s: read: %r\n", a);
			goto Done;
		}
		nr = IOLEN(m->io);
		if(nr > 0)
			if(d.qid.type&QTDIR)
				nr = wdent(fd, m->io->rp, nr, offset);
		else
				nr = wdata(fd, m->io->rp, nr, offset);
		offset += nr;
		freemsg(m);
	}while(nr > 0);
	close(fd);
	fd = -1;

Done:
	if(fd >= 0){
		close(fd);
		remove(fn);
	}
	sendul(wc, 0);
	free(path);
	threadexits(nil);
	close(fd);
}

static void
condproc(void *a)
{
	char *path, *els[64], *fn;
	int nels, fd, i;
	Ch *ch;
	Dir d;
	char buf[512];
	long nr;
	uvlong offset;
	Dir *ld;
	Msg *m;

	path = a;
	threadsetname("condproc %s", path);
	if(*path == '/')
		path++;
	path = estrdup(path);
	nels = getfields(path, els, nelem(els), 1, "/");
	if(nels < 1)
		sysfatal("short path");
	fn = els[nels-1];
	ld = dirstat(fn);
	if(ld == nil)
		sysfatal("%s: dirstat: %r", fn);
	ch = newch(cm);
	xtfid(ch, rootfid, 0);
	xtclone(ch, OCEND|OCERR, 0);
	for(i = 0; i < nels; i++)
		xtwalk(ch, els[i], 0);
	nulldir(&d);
	d.qid = ld->qid;
	xtcond(ch, CNE, &d, 0);
	free(ld);
	xtstat(ch, 0);
	xtopen(ch, OREAD, 0);
	xtread(ch, -1, 0ULL, msz, 1);
	/* fid automatically clunked on errors and eof */

	fd = -1;
	if(xrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		goto Done;
	}
	if(xrclone(ch) < 0){
		fprint(2, "%s: clone: %r\n", a);
		goto Done;
	}
	for(i = 0; i < nels; i++)
		if(xrwalk(ch, nil) < 0){
			fprint(2, "%s: walk[%s]: %r\n", a, els[i]);
			goto Done;
		}
	if(xrcond(ch) < 0){
		fprint(2, "%s: cond: %r\n", a);
		goto Done;
	}
	if(xrstat(ch, &d, buf) < 0){
		fprint(2, "%s: stat: %r\n", a);
		goto Done;
	}
	dtprint("get: %D\n", &d);
	remove(fn);	/* ignore errors */
	fd = create(fn, OWRITE, d.mode&~DMDIR);
	if(fd < 0){
		fprint(2, "create %s: %r\n", fn);
		goto Done;
	}
	if(xropen(ch) < 0){
		fprint(2, "%s: open: %r\n", a);
		goto Done;
	}
	offset = 0ULL;
	do{
		m = xrread(ch);
		if(m == nil){
			fprint(2, "%s: read: %r\n", a);
			goto Done;
		}
		nr = IOLEN(m->io);
		if(nr > 0)
			if(d.qid.type&QTDIR)
				nr = wdent(fd, m->io->rp, nr, offset);
		else
				nr = wdata(fd, m->io->rp, nr, offset);
		offset += nr;
		freemsg(m);
	}while(nr > 0);
	close(fd);
	fd = -1;

Done:
	if(fd >= 0){
		close(fd);
		remove(fn);
	}
	sendul(wc, 0);
	free(path);
	threadexits(nil);
	close(fd);
}
static void
settwritehdrsz(void)
{
	Fscall t;

	t.type = Twrite;
	twritehdrsz = packedsize(&t);
}

static void
putproc(void *a)
{
	char *path, *els[64], *fn;
	int nels, fd, i, nw;
	Ch *ch;
	Dir *d;
	long nr;
	Msg *m;
	uvlong offset;

	path = a;
	threadsetname("putproc %s", path);
	settwritehdrsz();
	if(*path == '/')
		path++;
	path = estrdup(path);
	nels = getfields(path, els, nelem(els), 1, "/");
	if(nels < 1)
		sysfatal("short path");
	fn = els[nels-1];
	fd = open(fn, OREAD);
	if(fd < 0)
		sysfatal("%s: %r", fn);
	d = dirfstat(fd);
	if(d == nil)
		sysfatal("%s: %r", fn);
	ch = newch(cm);
	xtfid(ch, rootfid, 0);
	xtclone(ch, OCEND|OCERR, 0);
	for(i = 0; i < nels - 1; i++)
		xtwalk(ch, els[i], 0);
	if(d->qid.type&QTDIR)
		xtcreate(ch, els[nels-1], OREAD, d->mode, 1);
	else
		xtcreate(ch, els[nels-1], OWRITE, d->mode, 0);
	/* fid automatically clunked on errors and eof */

	if(xrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		closech(ch);
		goto Done;
	}
	if(xrclone(ch) < 0){
		fprint(2, "%s: clone: %r\n", a);
		closech(ch);
		goto Done;
	}
	for(i = 0; i < nels-1; i++)
		if(xrwalk(ch, nil) < 0){
			fprint(2, "%s: walk[%s]: %r\n", a, els[i]);
			closech(ch);
			goto Done;
		}
	if(xrcreate(ch) < 0){
		fprint(2, "%s: create: %r\n", a);
		closech(ch);
		goto Done;
	}
	if((d->qid.type&QTDIR) == 0){
		nw = 0;
		offset = 0ULL;
		for(;;){
			m = newmsg(pool);
			nr = IOCAP(m->io) - Chhdrsz - twritehdrsz;
			m->io->wp += twritehdrsz;
			nr = read(fd, m->io->wp, nr);
			if(nr <= 0){
				if(nr < 0)
					fprint(2, "%s: read: %r", fn);
				xtclunk(ch, 1);
				break;
			}
			m->io->wp += nr;
			if(xtwrite(ch, m, nr, offset, 0) < 0){
				closech(ch);
				drainch(ch);
				goto Done;
			}
			nw++;
			offset += nr;	

			/* read replies so that no more than
			 * 10 outstanding writes are going.
			 */
			if(nw > 10){
				nw--;
				if(xrwrite(ch) < 0){
					fprint(2, "%s: write: %r\n", a);
					closech(ch);
					goto Done;
				}
			}
		}
		while(nw-- > 0)
			if(xrwrite(ch) < 0){
				fprint(2, "%s: write: %r\n", a);
				closech(ch);
				goto Done;
			}
		xrclunk(ch);
	}
Done:
	if(fd >= 0)
		close(fd);
	sendul(wc, 0);
	free(path);
	threadexits(nil);
	close(fd);
}

static void
ixattach(char *user)
{
	Ch *ch;

	ch = newch(cm);
	xtversion(ch, 0);
	xtattach(ch, user, "main", 1);
	if(xrversion(ch, &msz) < 0)
		sysfatal("wrong ix version: %r");
	if(xrattach(ch, &rootfid) < 0)
		sysfatal("can't attach: %r");
}

static void
usage(void)
{
	fprint(2, "usage: %s [-fhpc] [-D flags] [-m msz] [-a addr] file...\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *addr, *flags;
	int i, dosleep, doput, docond;

	addr = CLIADDR;
	dosleep = doput = docond = 0;
	ARGBEGIN{
	case 'D':
		flags = EARGF(usage());
		for(;*flags != 0; flags++)
			dbg[*flags]++;
		dbg['d']++;
		break;
	case 'a':
		addr = EARGF(usage());
		break;
	case 'm':
		msz = atoi(EARGF(usage()));
		if(msz < 1)
			sysfatal("message size too short");
		break;
	case 's':
		dosleep = 1;
		break;
	case 'f':
		doflush = 1;
		break;
	case 'p':
		doput = 1;
		break;
	case 'c':
		docond = 1;
		break;
	default:
		usage();
	}ARGEND;
	if(argc == 0 || doput + docond + doflush> 1)
		usage();
	fmtinstall('G', fscallfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('D', dirfmt);
	ses = dialsrv(addr);
	if(ses == nil)
		sysfatal("dialsrv: %r");
	pool = newpool(Msgsz, argc*Nmsgs);
	spool = newpool(Smsgsz, argc*Nmsgs);
	startses(ses, pool, spool);
	cm = muxses(ses->rc, ses->wc, ses->ec);
	ixattach(getuser());
	wc = chancreate(sizeof(ulong), argc);
	for(i = 0; i < argc; i++)
		if(doput)
			proccreate(putproc, argv[i], Stack);
		else if(docond)
			proccreate(condproc, argv[i], Stack);
		else
			proccreate(getproc, argv[i], Stack);
	for(i = 0; i < argc; i++)
		recvul(wc);
	chanfree(wc);
	closemux(cm);
	if(dosleep){
		print("%s: sleeping forever\n", argv0);
		for(;;)
			sleep(3600);
	}
	threadexits(nil);
}

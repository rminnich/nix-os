/*
 * Test program for ix.
 *
 ! mk all
 ! slay 8.ix 8.ixget 8.ixp |rc
 	X/\[/D
 ! rm -r /usr/nemo/src/9/ixp/Testfs/*
 ! 8.ixp -DDfn
 ! slay 8.ixget|rc
 ! cd /tmp ; /usr/nemo/src/9/ixp/8.ixget -Dstf acme.dump >[2=1]
 ! cmp /tmp/acme.dump /sys/src/cmd/ix/test/acme.dump
 ! cd /tmp ; /usr/nemo/src/9/ixp/8.ixget -pDstf adump>[2=1]
 ! cmp /tmp/adump /sys/src/cmd/ix/test/adump
 ! tstack 8.ix
 ! tstack 8.ixget
 ! 8.ix -Dfn

 */

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
#include "ixreqs.h"

#define CLIADDR	"tcp!localhost!9999"

int mainstacksize = Stack;

static Cmux *cm;
static Con *ses;
Mpool *pool, *spool;
static Channel *wc;
static int rootfid = -1;
static ulong msz;
static int doflush;
static int twritehdrsz;
static int tcondhdrsz;
static int ssid;

static long
wdata(int fd, void *a, long cnt, uvlong off)
{

	return pwrite(fd, a, cnt, off);
}

static void
printfile(char *s, char *e)
{
	char *str, *val;

	print("dir:\n");
	while(s < e){
		s = (char*)gstring((uchar*)s, (uchar*)e, &str);
		if(s == nil)
			return;
		s = (char*)gstring((uchar*)s, (uchar*)e, &val);
		if(s == nil)
			return;
		if(strcmp(str, "name") == 0 || strcmp(str, "uid") == 0 ||
		   strcmp(str, "gid") == 0 || strcmp(str, "muid") == 0)
			print("\t[%s] = '%s'\n", str, val);
		else if(strcmp(str, "mode") == 0)
			print("\t[%s] = %M\n", str, GBIT32(val));
		else
			print("\t[%s] = %#ullx\n", str, GBIT64(val));
	}
}

static void
printdir(char *buf, int count)
{
	int tot, n;

	for(tot = 0; tot < count; tot += n){
		if(count-tot < BIT32SZ)
			break;
		n = GBIT32(buf+tot);
		tot += BIT32SZ;
		if(tot+n > count){
			print("wrong count in dir entry\n");
			break;
		}
		printfile(buf+tot, buf+tot+n);
	}
}

static void
getstat(Ch *ch, int last)
{
	ixtattr(ch, "name", last);
	ixtattr(ch, "path", last);
	ixtattr(ch, "vers", last);
	ixtattr(ch, "atime", last);
	ixtattr(ch, "prev", last);
	ixtattr(ch, "length", last);
	ixtattr(ch, "mode", last);
	ixtattr(ch, "uid", last);
	ixtattr(ch, "gid", last);
	ixtattr(ch, "muid", last);
}

static int
gotu64(Ch *ch, uvlong *ip)
{
	Msg *m;

	m = ixrattr(ch);
	if(m == nil)
		return -1;
	if(IOLEN(m->io) < BIT64SZ){
		freemsg(m);
		return -1;
	}
	*ip = GBIT64(m->io->rp);
	freemsg(m);
	return 0;
}

static int
gotu32(Ch *ch, ulong *ip)
{
	Msg *m;

	m = ixrattr(ch);
	if(m == nil)
		return -1;
	if(IOLEN(m->io) < BIT32SZ){
		freemsg(m);
		return -1;
	}
	*ip = GBIT32(m->io->rp);
	freemsg(m);
	return 0;
}

static int
gotstr(Ch *ch, char **cp)
{
	Msg *m;

	m = ixrattr(ch);
	if(m == nil)
		return -1;
	if(IOLEN(m->io) == 0){
		freemsg(m);
		return -1;
	}
	*m->io->wp = 0;	/* BUG, but ok for this */
	*cp = strdup((char*)m->io->rp);
	freemsg(m);
	return 0;
}

static int
gotstat(Ch *ch, Dir *d)
{
	uvlong x;

	nulldir(d);
	if(gotstr(ch, &d->name) < 0)
		return -1;
	if(gotu64(ch, &d->qid.path) < 0)
		return -1;
	if(gotu64(ch, &x) < 0)
		return -1;
	d->qid.vers = x;
	if(gotu64(ch, &x) < 0)
		return -1;
	d->atime = x;
	if(gotu64(ch, &x) < 0)
		return -1;
	/* prev */
	if(gotu64(ch, (uvlong*)&d->length) < 0)
		return -1;
	if(gotu32(ch, &d->mode) < 0)
		return -1;
	if(gotstr(ch, &d->uid) < 0)
		return -1;
	if(gotstr(ch, &d->gid) < 0)
		return -1;
	if(gotstr(ch, &d->muid) < 0)
		return -1;

	d->qid.type = ((d->mode >>24) & 0xFF);
	return 0;
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
	char *path, *els[64], *fn, *dirbuf;
	int nels, fd;
	Ch *ch;
	Dir d;
	long nr;
	uvlong offset;
	Msg *m;

	path = a;
	threadsetname("getproc %s", path);
	if(*path == '/')
		path++;
	path = strdup(path);
	nels = getfields(path, els, nelem(els), 1, "/");
	if(nels < 1)
		sysfatal("short path");
	fn = els[nels-1];
	ch = newch(cm);
	ixtsid(ch, ssid, 0);
	ixtfid(ch, rootfid, 0);
	ixtclone(ch, OCEND|OCERR, 0);
	ixtwalk(ch, nels, els, 0);
	getstat(ch, 0);
	ixtopen(ch, OREAD, 0);
	ixtread(ch, -1, msz, 0ULL, 1);
	/* fid automatically clunked on errors and eof */
	dirbuf = nil;

	fd = -1;
	if(ixrsid(ch) < 0){
		fprint(2, "%s: sid: %r\n", a);
		goto Done;
	}
	if(ixrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		goto Done;
	}
	if(ixrclone(ch, nil) < 0){
		fprint(2, "%s: clone: %r\n", a);
		goto Done;
	}
	if(ixrwalk(ch) < 0){
		fprint(2, "%s: walk: %r\n", a);
		goto Done;
	}
	if(gotstat(ch, &d) < 0){
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
	if(ixropen(ch) < 0){
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
		m = ixrread(ch);
		if(m == nil){
			fprint(2, "%s: read: %r\n", a);
			goto Done;
		}
		nr = IOLEN(m->io);
		if(nr > 0)
			if(d.qid.type&QTDIR){
				dirbuf = realloc(dirbuf, offset+nr);
				memmove(dirbuf+offset, m->io->rp, nr);
			}else
				nr = wdata(fd, m->io->rp, nr, offset);
		offset += nr;
		freemsg(m);
	}while(nr > 0);
	close(fd);
	fd = -1;
	if(dirbuf != nil){
		printdir(dirbuf, offset);
		free(dirbuf);
		dirbuf = nil;
	}
Done:
	if(fd >= 0){
		close(fd);
		remove(fn);
	}
	free(dirbuf);
	sendul(wc, 0);
	free(path);
	threadexits(nil);
	close(fd);
}

static void
condproc(void *a)
{
	char *path, *els[64], *fn, *dirbuf;
	int nels, fd;
	Ch *ch;
	Dir d;
	long nr;
	uvlong offset;
	Dir *ld;
	Msg *m;

	path = a;
	threadsetname("condproc %s", path);
	if(*path == '/')
		path++;
	path = strdup(path);
	nels = getfields(path, els, nelem(els), 1, "/");
	if(nels < 1)
		sysfatal("short path");
	fn = els[nels-1];
	ld = dirstat(fn);
	if(ld == nil)
		sysfatal("%s: dirstat: %r", fn);
	ch = newch(cm);
	ixtsid(ch, ssid, 0);
	ixtfid(ch, rootfid, 0);
	ixtclone(ch, OCEND|OCERR, 0);
	ixtwalk(ch, nels, els, 0);
	nulldir(&d);
	d.qid = ld->qid;
	m = newmsg(pool);
	m->io->wp += tcondhdrsz;
	PBIT64(m->io->wp, d.qid.path);	m->io->wp += BIT64SZ;
	ixtcond(ch, m, CEQ, "path", BIT64SZ+BIT32SZ+BIT8SZ, 0);
	free(ld);
	getstat(ch, 0);
	ixtopen(ch, OREAD, 0);
	ixtread(ch, -1, msz, 0ULL, 1);
	/* fid automatically clunked on errors and eof */
	dirbuf = nil;

	fd = -1;
	if(ixrsid(ch) < 0){
		fprint(2, "%s: sid: %r\n", a);
		goto Done;
	}
	if(ixrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		goto Done;
	}
	if(ixrclone(ch, nil) < 0){
		fprint(2, "%s: clone: %r\n", a);
		goto Done;
	}
	if(ixrwalk(ch) < 0){
		fprint(2, "%s: walk: %r\n", a);
		goto Done;
	}
	if(ixrcond(ch) < 0){
		fprint(2, "%s: cond: %r\n", a);
		goto Done;
	}
	if(gotstat(ch, &d) < 0){
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
	if(ixropen(ch) < 0){
		fprint(2, "%s: open: %r\n", a);
		goto Done;
	}
	offset = 0ULL;
	do{
		m = ixrread(ch);
		if(m == nil){
			fprint(2, "%s: read: %r\n", a);
			goto Done;
		}
		nr = IOLEN(m->io);
		if(nr > 0)
			if(d.qid.type&QTDIR){
				dirbuf = realloc(dirbuf, offset+nr);
				memmove(dirbuf+offset, m->io->rp, nr);
			}else
				nr = wdata(fd, m->io->rp, nr, offset);
		offset += nr;
		freemsg(m);
	}while(nr > 0);
	close(fd);
	fd = -1;
	if(dirbuf != nil){
		printdir(dirbuf, offset);
		free(dirbuf);
		dirbuf = nil;
	}

Done:
	if(fd >= 0){
		close(fd);
		remove(fn);
	}
	free(dirbuf);
	sendul(wc, 0);
	free(path);
	threadexits(nil);
	close(fd);
}

static void
settwritehdrsz(void)
{
	IXcall t;

	t.type = IXTwrite;
	twritehdrsz = ixpackedsize(&t);
	t.type = IXTcond;
	t.attr = "qid";
	tcondhdrsz = ixpackedsize(&t);
}

static void
putproc(void *a)
{
	char *path, *els[64], *fn;
	int nels, fd, nw;
	Ch *ch;
	Dir *d;
	long nr;
	Msg *m;
	uvlong offset;
	uchar buf[BIT32SZ];

	path = a;
	threadsetname("putproc %s", path);
	settwritehdrsz();
	if(*path == '/')
		path++;
	path = strdup(path);
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
	ixtsid(ch, ssid, 0);
	ixtfid(ch, rootfid, 0);
	ixtclone(ch, OCEND|OCERR, 0);
	if(nels > 1)
		ixtwalk(ch, nels-1, els, 0);
	if(d->qid.type&QTDIR)
		ixtcreate(ch, els[nels-1], OREAD, d->mode, 1);
	else
		ixtcreate(ch, els[nels-1], OWRITE, d->mode, 0);
	/* fid automatically clunked on errors and eof */
	PBIT32(buf, 0777);
	ixtwattr(ch, "mode", buf, BIT32SZ, 0);
	ixtwattr(ch, "gid", "planb", 5, 0);
	if(ixrsid(ch) < 0){
		fprint(2, "%s: sid: %r\n", a);
		goto Done;
	}
	if(ixrfid(ch) < 0){
		fprint(2, "%s: fid: %r\n", a);
		closech(ch);
		goto Done;
	}
	if(ixrclone(ch, nil) < 0){
		fprint(2, "%s: clone: %r\n", a);
		closech(ch);
		goto Done;
	}
	if(nels > 1 && ixrwalk(ch) < 0){
		fprint(2, "%s: walk: %r\n", a);
		goto Done;
	}
	if(ixrcreate(ch) < 0){
		fprint(2, "%s: create: %r\n", a);
		closech(ch);
		goto Done;
	}
	if(ixrwattr(ch) < 0){
		fprint(2, "%s: wattr: %r\n", a);
		closech(ch);
		goto Done;
	}
	if(ixrwattr(ch) < 0){
		fprint(2, "%s: wattr: %r\n", a);
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
				ixtclose(ch, 1);
				break;
			}
			m->io->wp += nr;
			if(ixtwrite(ch, m, nr, offset, offset+nr, 0) < 0){
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
				if(ixrwrite(ch, nil) < 0){
					fprint(2, "%s: write: %r\n", a);
					closech(ch);
					goto Done;
				}
			}
		}
		while(nw-- > 0)
			if(ixrwrite(ch, nil) < 0){
				fprint(2, "%s: write: %r\n", a);
				closech(ch);
				goto Done;
			}
		ixrclose(ch);
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
ixattach(char *)
{
	Ch *ch;
	char *u;
	int afid;

	ch = newch(cm);
	ixtversion(ch, 0);
	ixtsession(ch, Nossid, getuser(), 0, 0);
	ixtattach(ch, "main", 1);
	if(ixrversion(ch, &msz) < 0)
		sysfatal("wrong ix version: %r");
	if(ixrsession(ch, &ssid, &afid, &u) < 0)
		sysfatal("can't set session: %r");
	print("session is %d %s %s \n", ssid, getuser(), u);
	if(ixrattach(ch, &rootfid) < 0)
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
	outofmemoryexits(1);
	fmtinstall('G', ixcallfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('D', dirfmt);
	ses = dialsrv(addr);
	if(ses == nil)
		sysfatal("dialsrv: %r");
	pool = newpool(Msgsz, Nmsgs);
	spool = newpool(Smsgsz, Nmsgs);
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

#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <error.h>
#include "conf.h"
#include "msg.h"
#include "ch.h"
#include "mpool.h"
#include "tses.h"
#include "fs.h"
#include "file.h"
#include "cfile.h"
#include "dbg.h"

/*
	8.ix -Dfn
!	slay 8.ix|rc
	8.ixc -Dfn cache tcp!localhost!9999

 */

typedef struct Cfid Cfid;
typedef struct Rpc Rpc;

struct Cfid
{
	Cfid	*next;
	short	busy;
	int	omode;
	int	fid;
	int	fd;
	char	*user;
	File*	file;
};

struct Rpc
{
	Fcall t;
	Fcall r;
	Msg *m;
	Cfid *fid;
	int fd;
};

static Cfid	*fids;
static QLock	fidslk;

ulong	msz = Msgsz;

static void	io(int fd);

static int	rflush(Rpc*), rversion(Rpc*), rauth(Rpc*),
	rattach(Rpc*), rwalk(Rpc*),
	ropen(Rpc*), rcreate(Rpc*),
	rread(Rpc*), rwrite(Rpc*), rclunk(Rpc*),
	rremove(Rpc*), rstat(Rpc*), rwstat(Rpc*);

static int (*fcalls[])(Rpc*) = {
	[Tversion]	rversion,
	[Tflush]	rflush,
	[Tauth]		rauth,
	[Tattach]	rattach,
	[Twalk]		rwalk,
	[Topen]		ropen,
	[Tcreate]	rcreate,
	[Tread]		rread,
	[Twrite]		rwrite,
	[Tclunk]	rclunk,
	[Tremove]	rremove,
	[Tstat]		rstat,
	[Twstat]		rwstat,
};

static char	Enofid[] = 	"fid not found";
static char	Eperm[] =	"permission denied";
static char	Enotdir[] =	"not a directory";
static char	Enoauth[] =	"ramfs: authentication not required";
static char	Enotexist[] =	"file does not exist";
static char	Einuse[] =	"file in use";
static char	Eexist[] =	"file exists";
static char	Eisdir[] =	"file is a directory";
static char	Enotowner[] =	"not owner";
static char	Eisopen[] = 	"file already open for I/O";
static char	Enotopen[] = 	"file not open for I/O";
static char	Excl[] = 	"exclusive use file already open";
static char	Ename[] = 	"illegal name";
static char	Eversion[] =	"unknown 9P version";
static char	Enotempty[] =	"directory not empty";

static Ses *ses;
static Cmux *cmux;
Mpool *pool, *spool;

static char *cdir;

void
notifyf(void *a, char *s)
{
	USED(a);
	if(strncmp(s, "interrupt", 9) == 0)
		noted(NCONT);
	noted(NDFLT);
}

static Cfid*
newfid(int fid, int mk)
{
	Cfid *f, *ff;

	qlock(&fidslk);
	ff = 0;
	for(f = fids; f; f = f->next)
		if(f->fid == fid)
			return f;
		else if(!ff && !f->busy)
			ff = f;
	if(mk == 0)
		return nil;
	if(ff){
		ff->busy = 1;
		ff->fid = fid;
		ff->file = nil;
		ff->omode = -1;
		free(ff->user);
		ff->user = nil;
		qunlock(&fidslk);
		return ff;
	}
	f = emalloc(sizeof *f);
	f->fid = fid;
	f->next = fids;
	fids = f;
	f->busy = 1;
	f->omode = -1;
	qunlock(&fidslk);
	return f;
}

static int
p9reply(Rpc *rpc)
{
	int n;
	Io *io;

	io = rpc->m->io;
	ioreset(io);
	rpc->m->hdr = nil;
	n = convS2M(&rpc->r, io->wp, IOCAP(io));
	if(n <= 0)
		sysfatal("p9reply: convS2M");
	io->wp += n;
	if(write(rpc->fd, io->rp, IOLEN(io)) != IOLEN(io))
		return -1;
	freemsg(rpc->m);
	rpc->m = nil;
	return 0;
}

static int
p9error(Rpc *rpc, char *e)
{
	rpc->r.type = Rerror;
	rpc->r.ename = e;
	return p9reply(rpc);
}

static int
p9syserror(Rpc *rpc)
{
	char buf[128];

	rerrstr(buf, sizeof buf);
	buf[sizeof buf - 1] = 0;
	return p9error(rpc, buf);
}

static int
rversion(Rpc *rpc)
{
	if(rpc->t.msize > msz)
		rpc->t.msize = msz;
	else
		msz = rpc->t.msize;
	rpc->r.msize = msz;
	if(strncmp(rpc->t.version, "9P2000", 6) != 0)
		return p9error(rpc, Eversion);
	rpc->r.version = "9P2000";
	return p9reply(rpc);
}

static int
rauth(Rpc *rpc)
{
	return p9error(rpc, "no authentication required");
}

static int
rflush(Rpc *rpc)
{
	return p9reply(rpc);
}

static int
rattach(Rpc *rpc)
{
	Cfid *fid;

	fid = newfid(rpc->t.fid, 1);
	/* no authentication! */
	fid->file = crootfile();
	if(rpc->t.uname[0])
		fid->user = estrdup(rpc->t.uname);
	else
		fid->user = estrdup("none");
	rpc->r.qid = fileqid(fid->file);
	return p9reply(rpc);
}

static Cfid*
clone(Cfid *f, int nfid)
{
	Cfid *nf;

	nf = newfid(nfid, 1);
	nf->busy = 1;
	nf->omode = -1;
	nf->file = f->file;
	incref(f->file);
	nf->user = estrdup(f->user);
	return nf;
}

static int
rwalk(Rpc *rpc)
{
	File *file;
	char *name;
	Cfid *fid, *nfid;
	Qid q;
	char *err;
	int i;

	nfid = nil;
	rpc->r.nwqid = 0;
	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	if(fid->omode >= 0)
		return p9error(rpc, Eisopen);
	if(rpc->t.newfid != NOFID){
		nfid = clone(fid, rpc->t.newfid);
		if(nfid == nil)
			return p9syserror(rpc);
		fid = nfid;	/* walk the new fid */
	}
	file = fid->file;
	err = nil;
	incref(file);	/* temp. ref used during walk */
	if(rpc->t.nwname > 0){
		for(i=0; i<rpc->t.nwname && i<MAXWELEM; i++){
			q = fileqid(file);
			if((q.type & QTDIR) == 0){
				err = Enotdir;
 				break;
			}
			name = rpc->t.wname[i];
			if(!perm(file, fid->user, AEXEC)){
				err = Eperm;
				break;
			}
			if(cwalkfile(&file, name) < 0){
				err = Enotexist;
				break;
			}
			rpc->r.nwqid++;
			rpc->r.wqid[i] = q;
		}
		if(i==0 && err == nil)
			err = Enotexist;
	}
	if(nfid != nil && (err!=nil || rpc->r.nwqid < rpc->t.nwname)){
		/* clunk the new fid, which is the one we walked */
		nfid->busy = 0;
		cputfile(nfid->file);
		nfid->file = nil;
	}
	if(rpc->r.nwqid > 0)
		err = nil;	/* didn't get everything in 9P2000 right! */
	if(rpc->r.nwqid == rpc->t.nwname){
		/* update the fid after a successful walk */
		cputfile(fid->file);
		fid->file = file;
	}else
		cputfile(file);	/* temp ref used during walk */
	if(err != nil)
		return p9error(rpc, err);
	return p9reply(rpc);
}

static int
ropen(Rpc *rpc)
{
	Cfid *fid;
	int mode, omode;
	File *file;
	Dir *d;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	if(fid->omode >= 0)
		return p9error(rpc, Eisopen);
	file = fid->file;
	d = cstatfile(file, 0);
	mode = rpc->t.mode;
	if(d->qid.type & QTDIR){
		if(mode != OREAD)
			return p9error(rpc, Eperm);
		rpc->r.qid = d->qid;
		free(d);
		return 0;
	}
	if((d->mode&DMEXCL) && file->nopens){
		free(d);
		return p9error(rpc, Excl);
	}
	free(d);
	if(mode & ORCLOSE){
		/* can't remove root; must be able to write parent */
		if(file->parent == nil || !perm(file, fid->user, AWRITE))
			return p9error(rpc, Eperm);
	}
	omode = mode&3;
	if(omode != OREAD || (mode&OTRUNC))
		if(!perm(file, fid->user, AWRITE))
			return p9error(rpc, Eperm);
	if(omode != OWRITE)
		if(!perm(file, fid->user, AREAD))
			return p9error(rpc, Eperm);
	fid->fd = copenfile(file, mode);
	if(fid->fd < 0)
		return p9syserror(rpc);
	fid->omode = mode;
	rpc->r.qid = fileqid(file);
	rpc->r.iounit = msz - Chhdrsz - IOHDRSZ;
	return p9reply(rpc);
}


static int
rcreate(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	Dir *d;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	if(fid->omode >= 0)
		return p9error(rpc, Eisopen);
	file = fid->file;
	d = cstatfile(file, 0);
	if((d->qid.type&QTDIR) == 0){
		free(d);
		return p9error(rpc, Enotdir);
	}
	/* must be able to write parent */
	if(!perm(file, fid->user, AWRITE)){
		free(d);
		return p9error(rpc, Eperm);
	}
	rpc->t.perm &= d->mode;
	free(d);
	fid->fd = ccreatefile(&file, rpc->t.name, rpc->t.mode, rpc->t.perm);
	if(fid->fd < 0)
		return p9syserror(rpc);
	fid->omode = rpc->t.mode;
	fid->file = file;
	
	rpc->r.qid = fileqid(file);
	rpc->r.iounit = msz-Chhdrsz-IOHDRSZ;
	return p9reply(rpc);
}

static int
rread(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	Io *io;
	long fcsz, nr;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	if(fid->omode < 0)
		return p9error(rpc, Enotopen);
	if((fid->omode&3) == OWRITE)
		return p9error(rpc, "file not open for reading");
	file = fid->file;

	io = rpc->m->io;
	ioreset(io);
	/*
	 * read the data directly in the place where convS2M will
	 * copy it (it should not do the memmove in that case).
	 */
	rpc->r.count = 0;
	fcsz = sizeS2M(&rpc->r);
	rpc->r.data = (char*)io->wp+fcsz;
	if(rpc->t.count > IOCAP(io))
		rpc->t.count = IOCAP(io);
	if(rpc->t.count > msz)
		rpc->t.count = msz;
	nr = cpreadfile(file, fid->fd, rpc->r.data, rpc->t.count, rpc->t.offset);
	if(nr < 0)
		return p9syserror(rpc);
	rpc->r.count = nr;
	return p9reply(rpc);
}

static int
rwrite(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	long nw;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	if(fid->omode < 0)
		return p9error(rpc, Enotopen);
	if((fid->omode&3) == OREAD)
		return p9error(rpc, "file not open for writing");
	file = fid->file;

	nw = cpwritefile(file, fid->fd, rpc->t.data, rpc->t.count, rpc->t.offset);
	if(nw < 0)
		return p9syserror(rpc);
	rpc->r.count = nw;
	return p9reply(rpc);
}

static int
rclunk(Rpc *rpc)
{
	Cfid *fid;
	File *file;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	file = fid->file;
	if(fid->omode >= 0){
		cclosefile(file, fid->fd);
		fid->fd = -1;
		if(fid->omode&ORCLOSE)
			cremovefile(file);
		fid->omode = 0;
	}
	cputfile(file);
	fid->file = nil;
	return p9reply(rpc);
}

static int
rremove(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	int rc;

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	file = fid->file;
	if(fid->omode >= 0){
		cclosefile(file, fid->fd);
		fid->fd = -1;
		fid->omode = 0;
	}
	if(file->parent != nil && perm(file->parent, fid->user, AWRITE))
		rc = cremovefile(file);
	else{
		werrstr(Eperm);
		rc = -1;
	}
	cputfile(file);
	fid->file = nil;
	if(rc < 0)
		return p9syserror(rpc);
	else
		return p9reply(rpc);
}

static int
rstat(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	Dir *d;
	uchar buf[512];

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	file = fid->file;
	d = cstatfile(file, 1);
	rpc->r.nstat = convD2M(d, buf, sizeof buf);
	rpc->r.stat = buf;
	free(d);
	return p9reply(rpc);
}

static int
rwstat(Rpc *rpc)
{
	Cfid *fid;
	File *file;
	Dir wd, *d;
	char buf[512];

	fid = newfid(rpc->t.fid, 0);
	if(fid == nil)
		return p9error(rpc, Enofid);
	file = fid->file;
	d = cstatfile(file, 0);
	convM2D(rpc->t.stat, rpc->t.nstat, &wd, buf);


	/*
	 * To change length, must have write permission on file.
	 */
	if(wd.length!=~0 && d->length!=wd.length)
	 	if(!perm(file, fid->user, AWRITE)){
			free(d);
			return p9error(rpc, Eperm);
		}

	/*
	 * To change name, must have write permission in parent
	 * and name must be unique.
	 */
	if(wd.name[0]!='\0' && strcmp(wd.name, d->name)!=0)
	 	if(!file->parent || !perm(file->parent, fid->user, AWRITE)){
			free(d);
			return p9error(rpc, Eperm);
		}

	/*
	 * To change mode, must be owner or group leader.
	 * Because of lack of users file, leader=>group itself.
	 */
	if(wd.mode!=~0 && wd.mode!=d->mode &&
	   strcmp(fid->user, d->uid) != 0 && strcmp(fid->user, d->gid) != 0){
		free(d);
		return p9error(rpc, Enotowner);
	}

	/*
	 * To change group, must be owner and member of new group,
	 * or leader of current group and leader of new group.
	 */
	if(wd.gid[0]!='\0' && strcmp(wd.gid, d->gid)!=0 &&
	   strcmp(fid->user, d->uid) != 0){
		free(d);
		return p9error(rpc, Enotowner);
	}
	free(d);
	/* all ok; do it */
	if(cwstatfile(file, &wd) < 0)
		return p9syserror(rpc);
	return p9reply(rpc);
}

static void
p9io(int fd)
{
	char buf[40];
	int n;
	Rpc rpc;

	for(;;){
		/*
		 * reading from a pipe or a network device
		 * will give an error after a few eof reads.
		 * however, we cannot tell the difference
		 * between a zero-length read and an interrupt
		 * on the processes writing to us,
		 * so we wait for the error.
		 */
		memset(&rpc, 0, sizeof rpc);
		rpc.m = newmsg(pool);
		rpc.fd = fd;
		n = read9pmsg(fd, rpc.m->io->wp, IOCAP(rpc.m->io));
		if(n < 0){
			rerrstr(buf, sizeof buf);
			if(buf[0]=='\0' || strstr(buf, "hungup"))
				exits("");
			sysfatal("mount read: %r");
		}
		if(n == 0){
			freemsg(rpc.m);
			continue;
		}
		rpc.m->io->wp += n;
		if(convM2S(rpc.m->io->rp, IOLEN(rpc.m->io), &rpc.t) == 0){
			freemsg(rpc.m);
			continue;
		}
		dfprint("-9-> %F\n", &rpc.t);

		if(rpc.t.type>=nelem(fcalls) || fcalls[rpc.t.type] == nil){
			if(p9error(&rpc, "bad fcall type") < 0)
				sysfatal("p9error: %r");
			continue;
		}
		rpc.r.tag = rpc.t.tag;
		rpc.r.type = rpc.t.type+1;
		if(fcalls[rpc.t.type](&rpc) < 0)
			sysfatal("fscalls[%d]: %r", rpc.t.type);
		dfprint("<-9- %F\n", &rpc.r);
	}
}

static void
fsproc(void *a)
{
	int *p;

	p = a;
	close(p[1]);
	fileinit(".", 1);
	pool = newpool(Msgsz, Nmsgs);
	spool = newpool(Smsgsz, Nmsgs);
	startses(ses, pool, spool);
	cmux = muxses(ses->rc, ses->wc, ses->ec);
	cfileinit(cmux);
	p9io(p[0]);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D flags] [-s] [-m mnt] [-S srv] dir addr\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *defmnt, *service, *addr, *flags;
	int p[2];
	int fd;

	service = "ix";
	defmnt = "/n/ix";

	ARGBEGIN{
	case 'm':
		defmnt = EARGF(usage());
		break;
	case 's':
		defmnt = 0;
		break;
	case 'D':
		flags = EARGF(usage());
		for(;*flags != 0; flags++)
			dbg[*flags]++;
		dbg['d']++;
		break;
	case 'S':
		defmnt = 0;
		service = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 2)
		usage();
	cdir = cleanname(argv[0]);
	addr = argv[1];
	if(chdir(cdir) < 0)
		sysfatal("%s: %r", cdir);
	if(pipe(p) < 0)
		sysfatal("pipe failed: %r");
	if(defmnt == 0){
		char buf[64];
		snprint(buf, sizeof buf, "#s/%s", service);
		fd = create(buf, OWRITE|ORCLOSE, 0666);
		if(fd < 0)
			sysfatal("create failed: %r");
		sprint(buf, "%d", p[0]);
		if(write(fd, buf, strlen(buf)) < 0)
			sysfatal("writing service file: %r");
	}

	notify(notifyf);

	fmtinstall('G', fscallfmt);
	fmtinstall('F', fcallfmt);
	fmtinstall('D', shortdirfmt);
	fmtinstall('M', dirmodefmt);
	fmtinstall('T', filefmt);

	ses = dialsrv(addr);
	if(ses == nil)
		sysfatal("dialsrv: %r");
	procrfork(fsproc, p, Stack, RFFDG|RFNAMEG|RFNOTEG);
	close(p[0]);	/* don't deadlock if child fails */
	if(defmnt && mount(p[1], -1, defmnt, MREPL|MCREATE, "") < 0)
		sysfatal("mount failed: %r");
	threadexits(0);
}


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
#include "fns.h"

/*
 * 9p server for creepy
 */

typedef struct Fid Fid;
typedef struct Rpc Rpc;
typedef struct Cli Cli;

enum
{
	Maxmdata = 8*KiB
};

/*
 * One reference kept because of existence and another per req using it.
 */
struct Fid
{
	Ref;
	QLock;
	Fid	*next;		/* in hash or free list */
	void*	clino;		/* no is local to a client */
	int	no;
	Memblk	*file;		/* used by this fid */
	int	omode;		/* -1 if closed */
	int	rclose;
	int	archived;
	char	*uid;

	uvlong	loff;		/* last offset, for dir reads */
	long	lidx;		/* next dir entry index to read */
};

struct Rpc
{
	Cli	*cli;
	Rpc	*next;		/* in client or free list */
	Fid	*fid;
	Fcall	t;
	Fcall	r;
	uchar	data[IOHDRSZ+Maxmdata];
};

struct Cli
{
	Ref;
	int fd;
	int cfd;
	char *addr;
	int errors;
	ulong msize;

	QLock wlk;	/* lock for writing replies to the client */
	uchar wdata[IOHDRSZ+Maxmdata];

	QLock rpclk;
	Rpc *rpcs;
};

static void	rflush(Rpc*), rversion(Rpc*), rauth(Rpc*),
		rattach(Rpc*), rwalk(Rpc*),
		ropen(Rpc*), rcreate(Rpc*),
		rread(Rpc*), rwrite(Rpc*), rclunk(Rpc*),
		rremove(Rpc*), rstat(Rpc*), rwstat(Rpc*);

static void (*fcalls[])(Rpc*) =
{
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
	[Twstat]	rwstat,
};

static RWLock fidlk;
static Fid *fidhash[Fidhashsz];
static Fid *fidfree;
static ulong nfids, nfreefids;

static QLock rpclk;
static Rpc *rpcfree;
static ulong nrpcs, nfreerpcs;

static Rpc*
newrpc(void)
{
	Rpc *rpc;

	qlock(&rpclk);
	if(rpcfree != nil){
		rpc = rpcfree;
		rpcfree = rpc->next;
		rpc->next = nil;
		nfreerpcs--;
	}else{
		rpc = malloc(sizeof *rpc);
		nrpcs++;
	}
	qunlock(&rpclk);
	rpc->next = nil;
	rpc->fid = nil;
	memset(&rpc->t, 0, sizeof rpc->t);
	memset(&rpc->r, 0, sizeof rpc->r);
	return rpc;
}

static void
freerpc(Rpc *rpc)
{
	qlock(&rpclk);
	rpc->next = rpcfree;
	rpcfree = rpc;
	nfreerpcs++;
	qunlock(&rpclk);
}

static Fid*
newfid(void* clino, int no)
{
	Fid *fid, **fidp;

	wlock(&fidlk);
	if(catcherror()){
		wunlock(&fidlk);
		error(nil);
	}
	for(fidp = &fidhash[no%Fidhashsz]; *fidp != nil; fidp = &(*fidp)->next)
		if((*fidp)->clino == clino && (*fidp)->no == no)
			error("fid in use");
	if(fidfree != nil){
		fid = fidfree;
		fidfree = fidfree->next;
		nfreefids--;
	}else{
		fid = mallocz(sizeof *fid, 1);
		nfids++;
	}
	*fidp = fid;
	fid->omode = -1;
	fid->no = no;
	fid->rclose = 0;
	fid->clino = clino;
	fid->ref = 2;	/* one for the caller; another because it's kept */
	noerror();
	wunlock(&fidlk);
	return fid;
}

static Fid*
getfid(void* clino, int no)
{
	Fid *fid;

	rlock(&fidlk);
	if(catcherror()){
		runlock(&fidlk);
		error(nil);
	}
	for(fid = fidhash[no%Fidhashsz]; fid != nil; fid = fid->next)
		if(fid->clino == clino && fid->no == no){
			incref(fid);
			noerror();
			runlock(&fidlk);
			return fid;
		}
	error("fid not found");
	return fid;
}

static void
putfid(Fid *fid)
{
	Fid **fidp;

	if(fid == nil || decref(fid) > 0)
		return;
	mbput(fid->file);
	fid->file = nil;
	free(fid->uid);
	fid->uid = nil;
	fid->rclose = fid->archived = 0;
	fid->omode = -1;
	fid->loff = 0;
	fid->lidx = 0;
	wlock(&fidlk);
	if(catcherror()){
		wunlock(&fidlk);
		error(nil);
	}
	for(fidp = &fidhash[fid->no%Fidhashsz]; *fidp != nil; fidp = &(*fidp)->next)
		if(*fidp == fid){
			*fidp = fid->next;
			memset(fid, 0, sizeof *fid);
			fid->next = fidfree;
			noerror();
			wunlock(&fidlk);
			return;
		}
	fatal("putfid: fid not found");
}

static void
putcli(Cli *c)
{
	if(decref(c) == 0){
		close(c->fd);
		close(c->cfd);
		free(c->addr);
		free(c);
	}
}

static Qid
mkqid(Memblk *f)
{
	Qid q;
	
	q.path = f->mf->id;
	q.vers = f->mf->mtime;
	q.type = 0;
	if(f->mf->mode&DMDIR)
		q.type |= QTDIR;
	if(f->mf->mode&DMTMP)
		q.type |= QTTMP;
	if(f->mf->mode&DMAPPEND)
		q.type |= QTAPPEND;
	return q;
}

static void
rversion(Rpc *rpc)
{
	rpc->r.msize = rpc->t.msize;
	if(rpc->r.msize > sizeof rpc->data)
		rpc->r.msize = sizeof rpc->data;
	rpc->cli->msize = rpc->r.msize;
	if(strncmp(rpc->t.version, "9P2000", 6) != 0)
		error("unknown protocol version");
	rpc->r.version = "9P2000";
}

static void
rflush(Rpc *)
{
	/* BUG: should reply to this after replying to the flushed request.
	 * Just look into rpc->c->rpcs
	 */
}

static void
rauth(Rpc*)
{
	/* BUG */
	error("no auth required");
}


static void
rattach(Rpc *rpc)
{
	Fid *fid;

	fid = newfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	fid->file = fs->active;
	incref(fid->file);
	rwlock(fid->file, Rd);
	fid->uid = strdup(rpc->t.uname);
	rpc->r.qid = mkqid(fid->file);
	rwunlock(fid->file, Rd);
}

static Fid*
clone(Rpc *rpc)
{
	Fid *nfid;

	nfid = newfid(rpc->cli, rpc->t.newfid);
	nfid->file = rpc->fid->file;
	incref(nfid->file);
	nfid->uid = strdup(rpc->fid->uid);
	nfid->archived = rpc->fid->archived;
	return nfid;
}

static void
rwalk(Rpc *rpc)
{
	Fid *fid, *nfid;
	Memblk *f;
	int i;

	rpc->fid = getfid(rpc->cli, rpc->t.fid);
	fid = rpc->fid;
	if(rpc->t.fid == rpc->t.newfid && rpc->t.nwname > 1)
		error("can't walk like a clone without one");
	nfid = nil;
	if(rpc->t.fid != rpc->t.newfid)
		nfid = clone(rpc);
	if(catcherror()){
		putfid(nfid);
		error(nil);
	}
	rpc->r.nwqid = 0;
	for(i=0; i < rpc->t.nwname; i++){
		rwlock(nfid->file, Rd);
		if(catcherror()){
			rwunlock(nfid->file, Rd);
			if(rpc->r.nwqid == 0)
				error(nil);
			break;
		}
		dfaccessok(nfid->file, fid->uid, AEXEC);
		f = dfwalk(nfid->file, rpc->t.wname[i], 0);
		if(f == fs->archive)
			fid->archived++;
		else if(f == fs->active)
			fid->archived = 0;
		rwunlock(nfid->file, Rd);
		mbput(nfid->file);
		nfid->file = f;
		noerror();
		rwlock(f, Rd);
		rpc->r.wqid[i] = mkqid(f);
		rwunlock(f, Rd);
		rpc->r.nwqid++;
		USED(rpc->r.nwqid);	/* damn error()s */
	}
	if(i < rpc->t.nwname){
		putfid(nfid);
		putfid(nfid);
	}else{
		putfid(fid);
		rpc->fid = nfid;
	}
	noerror();
}

static void
ropen(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;
	int mode, fmode, amode;
	uvlong z;

	rpc->fid = getfid(rpc->cli, rpc->t.fid);
	fid = rpc->fid;

	if(fid->omode != -1)
		error("fid already open");
	mode = rpc->t.mode;
	rpc->r.iounit = rpc->cli->msize - IOHDRSZ;
	amode = 0;
	if((mode&3) != OREAD || (mode&OTRUNC) != 0)
		amode |= AWRITE;
	if((mode&3) != OWRITE)
		amode |= AREAD;
	if(mode != AREAD)
		fid->file = dfmelt(fid->file);
	else
		rwlock(fid->file, Rd);
	f = fid->file;
	if(catcherror()){
		rwunlock(f, amode != AREAD);
		error(nil);
	}
	fmode = f->mf->mode;
	if(mode != OREAD){
		if((fmode&DMDIR) != 0)
			error("wrong open mode for a directory");
		if(fid->archived)
			error("can't write in /archive"); /* yes, we can! */
	}
	rpc->r.qid = mkqid(f);
	dfaccessok(f, fid->uid, amode);
	if(mode&ORCLOSE)
		dfaccessok(f->mf->parent, fid->uid, AWRITE);
	if(mode&ORCLOSE)
		fid->rclose++;
	if((fmode&DMEXCL) != 0 &&f->mf->open)
		error("exclusive use file already open");
	if(mode&OTRUNC){
		z = 0;
		dfwattr(fid->file, "length", &z, sizeof z);
	}
	f->mf->open++;
	fid->omode = mode&3;
	fid->loff = 0;
	fid->lidx = 0;
	noerror();
	rwunlock(f, amode != AREAD);
}

static void
rcreate(Rpc *rpc)
{
	Fid *fid;
	Memblk *f, *nf;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;

	if(fid->omode != -1)
		error("fid already open");
	fid->file = dfmelt(fid->file);
	f = fid->file;
	if(catcherror()){
		rwunlock(f, Wr);
		error(nil);
	}
	if((f->mf->mode&DMDIR) == 0)
		error("not a directory");
	dfaccessok(f, fid->uid, AWRITE);
	if(strcmp(rpc->t.name, ".") == 0 || strcmp(rpc->t.name, "..") == 0)
		error("that file name scares me");
	if(utfrune(rpc->t.name, '/') != nil)
		error("that file name is too creepy");
	if((rpc->t.perm&DMDIR) != 0 && rpc->t.mode != OREAD)
		error("wrong open mode for a directory");
	if(f == fs->root || f == fs->archive)
		error("can't create there");
	if(fid->archived)
		error("can't create in /archive"); /* yes, we can! */
	if(!catcherror()){
		mbput(dfwalk(f, rpc->t.name, 0));
		error("file already exists");
	}
	nf = dfcreate(f, rpc->t.name, fid->uid, rpc->t.perm);
	rpc->r.qid = mkqid(nf);
	rpc->r.iounit = rpc->cli->msize-IOHDRSZ;
	nf->mf->open++;
	noerror();
	rwunlock(f, Wr);
	mbput(fid->file);
	fid->file = nf;
	if(rpc->t.mode&ORCLOSE)
		fid->rclose++;
	fid->omode = rpc->t.mode&3;
	fid->loff = 0;
	fid->lidx = 0;
}

static ulong
readmf(Memblk *f, uchar *buf, int nbuf)
{
	Dir d;

	d.name = f->mf->name;
	d.qid = mkqid(f);
	d.mode = f->mf->mode;
	d.length = f->mf->length;
	d.uid = f->mf->uid;
	d.gid = f->mf->gid;
	d.muid = f->mf->muid;
	d.atime = f->mf->atime;
	d.mtime = f->mf->mtime;
	return convD2M(&d, buf, nbuf);
}

static ulong
readdir(Fid *fid, uchar *data, ulong ndata, uvlong)
{
	Memblk *d, *f;
	ulong tot, nr;

	d = fid->file;
	for(tot = 0; tot+2 < ndata; tot += nr){
		
		f = dfchild(d, fid->lidx);
		if(f == nil)
			break;
		nr = readmf(f, data+tot, ndata-tot);
		mbput(f);
		if(nr <= 2)
			break;
		fid->lidx++;
	}
	return tot;
}

static void
rread(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	f = fid->file;

	if(fid->omode == -1)
		error("fid not open");
	if(fid->omode == OWRITE)
		error("fid not open for reading");
	if(rpc->t.offset < 0)
		error("negative offset");
	if(rpc->t.count > rpc->cli->msize-IOHDRSZ)
		rpc->r.count = rpc->cli->msize-IOHDRSZ;
	rpc->r.data = (char*)rpc->data;
	rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	if(f->mf->mode&DMDIR){
		if(fid->loff != rpc->t.offset)
			error("non-sequential dir read");
		rpc->r.count = readdir(fid, rpc->data, rpc->t.count, rpc->t.offset);
		fid->loff += rpc->r.count;
	}else
		rpc->r.count = dfpread(f, rpc->data, rpc->t.count, rpc->t.offset);
	noerror();
	rwunlock(f, Rd);
}

static void
rwrite(Rpc *rpc)
{
	Fid *fid;
	ulong n;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;

	if(fid->omode == -1)
		error("fid not open");
	if(fid->omode == OREAD)
		error("fid not open for writing");
	if(rpc->t.offset < 0)
		error("negative offset");
	n = rpc->t.count;
	if(n > rpc->cli->msize)
		n = rpc->cli->msize;	/* hmmm */
	fid->file = dfmelt(fid->file);
	if(catcherror()){
		rwunlock(fid->file, Wr);
		error(nil);
	}
	rpc->r.count = dfpwrite(fid->file, rpc->t.data, n, rpc->t.offset);
	noerror();
}


static void
rclunk(Rpc *rpc)
{
	Fid *fid;
	Memblk *f, *p;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	f = fid->file;
	if(fid->omode != -1){
		rwlock(f, Wr);
		f->mf->open--;
		rwunlock(f, Wr);
		fid->omode = -1;
		if(fid->rclose){
			f->mf->parent = dfmelt(f->mf->parent);
			p = f->mf->parent;
			rwlock(f, Wr);
			if(catcherror()){
				rwunlock(f, Wr);
				mbput(f);
			}else{
				dfremove(p, f);
				noerror();
			}
			rwunlock(p, Wr);
		}
		fid->file = nil;
	}
	putfid(fid);
	putfid(fid);
	rpc->fid = nil;
}


static void
rremove(Rpc *rpc)
{
	Fid *fid;
	Memblk *f, *p;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	f = fid->file;
	if(f == fs->root || f == fs->active || f == fs->archive)
		error("can't remove that");
	if(fid->archived)
		error("can't remove in /archive"); /* yes, we can! */

	f->mf->parent = dfmelt(f->mf->parent);
	p = f->mf->parent;
	rwlock(f, Wr);
	if(catcherror()){
		rwunlock(f, Wr);
		rwunlock(p, Wr);
		error(nil);
	}
	dfaccessok(p, fid->uid, AWRITE);
	fid->omode = -1;
	dfremove(p, f);
	noerror();
	rwunlock(p, Wr);
	fid->file = nil;
	putfid(fid);
	putfid(fid);
	rpc->fid = nil;
}


static void
rstat(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	f = fid->file;
	rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	rpc->r.stat = rpc->data;
	rpc->r.nstat = readmf(f, rpc->data, sizeof rpc->data);
	if(rpc->r.nstat <= 2)
		fatal("rstat: convD2M");
	noerror();
	rwunlock(f, Rd);
}

static void
wstatint(Memblk *f, char *name, u64int v)
{
	dfwattr(f, name, &v, sizeof v);
}

static void
wstatstr(Memblk *f, char *name, char *s)
{
	dfwattr(f, name, s, strlen(s)+1);
}

static void
rwstat(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;
	Dir d, *sd;
	u64int n;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	f = fid->file;

	
	if(f == fs->root || f == fs->archive || fid->archived)
		error("can't wstat there");
	fid->file = dfmelt(fid->file);
	n = convM2D(rpc->t.stat, rpc->t.nstat, &d, nil);
	sd = malloc(n);
	if(catcherror()){
		rwunlock(fid->file, Wr);
		free(sd);
		error(nil);
	}
	f = fid->file;
	n = convM2D(rpc->t.stat, rpc->t.nstat, sd, (char*)&sd[1]);
	if(n <= BIT16SZ){
		free(sd);
		error("wstat: convM2D");
	}
	if(sd->length != ~0 && sd->length != f->mf->length){
		if(f->mf->mode&DMDIR)
			error("can't resize a directory");
		dfaccessok(f, fid->uid, AWRITE);
	}else
		sd->length = ~0;

	if(sd->name[0] && strcmp(f->mf->name, sd->name) != 0){
		if(f == fs->active)
			error("can't rename /active");
		dfaccessok(f->mf->parent, fid->uid, AWRITE);
		if(!catcherror()){
			mbput(dfwalk(f, sd->name, 0));
			error("file already exists");
		}
	}else
		sd->name[0] = 0;

	if(sd->uid[0] != 0 && strcmp(sd->uid, f->mf->uid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the owner may donate a file");
	}else
		sd->uid[0] = 0;
	if(sd->gid[0] != 0 && strcmp(sd->gid, f->mf->gid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the onwer may change group");
	}else
		sd->gid[0] = 0;
	if(sd->mode != ~0 && f->mf->mode != sd->mode){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0 &&
		   strcmp(fid->uid, f->mf->gid) != 0)
			error("only the onwer may change mode");
	}else
		sd->mode = ~0;

	if(sd->length != ~0)
		wstatint(f, "length", sd->length);
	if(sd->name[0])
		wstatstr(f, "name", sd->name);
	if(sd->uid[0])
		wstatstr(f, "name", sd->name);
	if(sd->gid[0])
		wstatstr(f, "name", sd->name);
	if(sd->mode != ~0)
		wstatint(f, "mode", sd->mode);
	if(fs->config && sd->atime != ~0)
		wstatint(f, "atime", sd->atime);
	if(fs->config && sd->mtime != ~0)
		wstatint(f, "mtime", sd->mtime);
	if(fs->config && sd->muid[0] != 0 && strcmp(sd->muid, f->mf->muid) != 0)
		wstatint(f, "mtime", sd->mtime);

	noerror();
	rwunlock(f, Wr);
	free(sd);
	
}

static void
replied(Rpc *rpc)
{
	Rpc **rl;

	qlock(&rpc->cli->rpclk);
	for(rl = &rpc->cli->rpcs; (*rl != nil); rl = &(*rl)->next)
		if(*rl == rpc){
			*rl = rpc->next;
			break;
		}
	qunlock(&rpc->cli->rpclk);
	rpc->next = nil;
	putfid(rpc->fid);
	rpc->fid = nil;
	putcli(rpc->cli);
	rpc->cli = nil;
	freerpc(rpc);
}

static char*
rpcworker(void *v, void**aux)
{
	Rpc *rpc;
	Cli *cli;
	char err[128];
	long n;

	rpc = v;
	cli = rpc->cli;
	threadsetname("cliproc %s rpc", cli->addr);
	d9print("cliproc %s rpc starting\n", cli->addr);

	if(*aux == nil){
		errinit(Errstack);
		*aux = v;		/* make it not nil */
	}

	rpc->r.tag = rpc->t.tag;
	rpc->r.type = rpc->t.type + 1;

	if(catcherror()){
		rpc->r.type = Rerror;
		rpc->r.ename = err;
		rerrstr(err, sizeof err);
		goto out;
	}

	fcalls[rpc->t.type](rpc);	
	noerror();

out:
	d9print("-> %F\n", &rpc->r);
	qlock(&cli->wlk);
	n = convS2M(&rpc->r, cli->wdata, sizeof cli->wdata);
	if(n == 0)
		fatal("rpcworker: convS2M");
	if(write(cli->fd, cli->wdata, n) != n)
		d9print("%s: %r\n", cli->addr);
	qunlock(&cli->wlk);

	d9print("cliproc %s rpc exiting\n", cli->addr);
	replied(rpc);
	return nil;
}

static char*
cliworker(void *v, void**)
{
	Cli *c;
	long n;
	Rpc *rpc;

	c = v;
	threadsetname("cliproc %s", c->addr);
	d9print("cliproc %s started\n", c->addr);

	rpc = nil;
	for(;;){
		if(rpc == nil)
			rpc = newrpc();
		n = read9pmsg(c->fd, rpc->data, sizeof rpc->data);
		if(n < 0){
			d9print("%s: read: %r\n", c->addr);
			break;
		}
		if(n == 0)
			continue;
		if(convM2S(rpc->data, n, &rpc->t) == 0){
			d9print("%s: convM2S failed\n", c->addr);
			continue;
		}
		if(rpc->t.type >= nelem(fcalls) || fcalls[rpc->t.type] == nil){
			d9print("%s: bad fcall type %d\n", c->addr, rpc->t.type);
			continue;
		}
		if(dbg['0'])
			fprint(2, "<-%F\n", &rpc->t);
		rpc->cli = c;
		incref(c);

		qlock(&c->rpclk);
		rpc->next = c->rpcs;
		c->rpcs = rpc;
		qunlock(&c->rpclk);

		getworker(rpcworker, rpc, nil);
	}
	d9print("cliproc %s exiting\n", c->addr);
	putcli(c);
	return nil;
};

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
}

void
srv9p(char *srv)
{
	Cli *cli;
	int fd[2];
	char *name;

	name = smprint("/srv/%s", srv);
	if(pipe(fd) < 0)
		fatal("pipe: %r");
	postfd(name, fd[1]);
	cli = mallocz(sizeof *cli, 1);
	cli->fd = fd[0];
	cli->cfd = -1;
	cli->addr = name;
	cli->ref = 1;
	getworker(cliworker, cli, nil);
}

void
listen9p(char *addr)
{
	Cli *cli;
	char ndir[NETPATHLEN], dir[NETPATHLEN];
	int ctl, data, nctl;

	ctl = announce(addr, dir);
	if(ctl < 0)
		fatal("announce %s: %r", addr);
	for(;;){
		nctl = listen(dir, ndir);
		if(nctl < 0)
			fatal("listen %s: %r", addr);
		data = accept(nctl, ndir);
		if(data < 0){
			fprint(2, "%s: accept %s: %r\n", argv0, ndir);
			continue;
		}
		cli = mallocz(sizeof *cli, 1);
		cli->fd = data;
		cli->cfd = nctl;
		cli->addr = getremotesys(ndir);
		cli->ref = 1;
		getworker(cliworker, cli, nil);
	}
}


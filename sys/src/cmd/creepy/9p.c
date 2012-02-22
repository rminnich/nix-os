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


/*
 * 9p server for creepy
 */


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

static RWLock fidhashlk;
static Fid *fidhash[Fidhashsz];
static uint fidgen;

static Alloc fidalloc =
{
	.elsz = sizeof(Fid),
	.zeroing = 1,
};
static Alloc rpcalloc =
{
	.elsz = sizeof(Largerpc),
	.zeroing = 0,
};
Alloc clialloc =
{
	.elsz = sizeof(Cli),
	.zeroing = 1,
};

static QLock clientslk;
static Cli *clients;

int
fidfmt(Fmt *fmt)
{
	Fid *fid;

	fid = va_arg(fmt->args, Fid*);
	if(fid == nil)
		return fmtprint(fmt, "<nil>");
	return fmtprint(fmt, "fid %#p no %d r%d, omode %d arch %d",
		fid, fid->no, fid->ref, fid->omode, fid->archived);
}

void
ninestats(void)
{
	print("fids:\t%4uld alloc %4uld free (%4uld bytes)\n",
		fidalloc.nalloc, fidalloc.nfree, fidalloc.elsz);
	print("rpcs:\t%4uld alloc %4uld free (%4uld bytes)\n",
		rpcalloc.nalloc, rpcalloc.nfree, rpcalloc.elsz);
	print("clis:\t%4uld alloc %4uld free (%4uld bytes)\n",
		clialloc.nalloc, clialloc.nfree, clialloc.elsz);
	
}

Rpc*
newrpc(void)
{
	Rpc *rpc;

	rpc = anew(&rpcalloc);
	rpc->next = nil;
	rpc->cli = nil;
	rpc->fid = nil;
	rpc->flushed = 0;
	rpc->closed = 0;
	rpc->chan = ~0;
	rpc->rpc0 = nil;
	/* ouch! union. */
	if(sizeof(Fcall) > sizeof(IXcall)){
		memset(&rpc->t, 0, sizeof rpc->t);
		memset(&rpc->r, 0, sizeof rpc->r);
	}else{
		memset(&rpc->xt, 0, sizeof rpc->xt);
		memset(&rpc->xr, 0, sizeof rpc->xr);
	}
	return rpc;	
}

void
freerpc(Rpc *rpc)
{
	afree(&rpcalloc, rpc);
}

Fid*
newfid(void* clino, int no)
{
	Fid *fid, **fidp;

	wlock(&fidhashlk);
	if(catcherror()){
		wunlock(&fidhashlk);
		error(nil);
	}
	if(no < 0)
		no = fidgen++;
	for(fidp = &fidhash[no%Fidhashsz]; *fidp != nil; fidp = &(*fidp)->next)
		if((*fidp)->clino == clino && (*fidp)->no == no)
			error("fid in use");
	fid = anew(&fidalloc);
	*fidp = fid;
	fid->omode = -1;
	fid->no = no;
	fid->clino = clino;
	fid->ref = 2;	/* one for the caller; another because it's kept */
	noerror();
	wunlock(&fidhashlk);
	d9print("new fid %X\n", fid);
	return fid;
}

Fid*
getfid(void* clino, int no)
{
	Fid *fid;

	rlock(&fidhashlk);
	if(catcherror()){
		runlock(&fidhashlk);
		error(nil);
	}
	for(fid = fidhash[no%Fidhashsz]; fid != nil; fid = fid->next)
		if(fid->clino == clino && fid->no == no){
			incref(fid);
			noerror();
			runlock(&fidhashlk);
			return fid;
		}
	error("fid not found");
	return fid;
}

void
putfid(Fid *fid)
{
	Fid **fidp;

	if(fid == nil || decref(fid) > 0)
		return;
	d9print("clunk fid %X\n", fid);
	putpath(fid->p);
	free(fid->uid);
	wlock(&fidhashlk);
	if(catcherror()){
		wunlock(&fidhashlk);
		error(nil);
	}
	for(fidp = &fidhash[fid->no%Fidhashsz]; *fidp != nil; fidp = &(*fidp)->next)
		if(*fidp == fid){
			*fidp = fid->next;
			noerror();
			wunlock(&fidhashlk);
			afree(&fidalloc, fid);
			return;
		}
	fatal("putfid: fid not found");
}

/* keeps addr, does not copy it */
Cli*
newcli(char *addr, int fd, int cfd)
{
	Cli *cli;

	cli = anew(&clialloc);
	cli->fd = fd;
	cli->cfd = cfd;
	cli->addr = addr;
	cli->ref = 1;

	qlock(&clientslk);
	cli->next = clients;
	clients = cli;
	qunlock(&clientslk);
	return cli;
}

void
putcli(Cli *cli)
{
	Cli **cp;

	if(decref(cli) == 0){
		qlock(&clientslk);
		for(cp = &clients; *cp != nil; cp = &(*cp)->next)
			if(*cp == cli)
				break;
		if(*cp == nil)
			fatal("client not found");
		*cp = cli->next;
		qunlock(&clientslk);
		close(cli->fd);
		close(cli->cfd);
		free(cli->addr);
		afree(&clialloc, cli);
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
	if(f->mf->mode&DMEXCL)
		q.type |= QTEXCL;
	if((q.type&QTEXCL) == 0)
		q.type |= QTCACHE;
	return q;
}

static void
rversion(Rpc *rpc)
{
	rpc->r.msize = rpc->t.msize;
	if(rpc->r.msize > Maxmdata)
		rpc->r.msize = Maxmdata;
	rpc->cli->msize = rpc->r.msize;
	if(strncmp(rpc->t.version, "9P2000", 6) != 0)
		error("unknown protocol version");
	rpc->r.version = "9P2000";
}

/*
 * Served in the main client process.
 */
static void
rflush(Rpc *rpc)
{
	Cli *cli;
	Rpc *r;

	cli = rpc->cli;
	qlock(&cli->wlk);	/* nobody replies now */
	qlock(&rpc->cli->rpclk);
	for(r = rpc->cli->rpcs; r != nil; r = r->next)
		if(r->t.tag == rpc->t.oldtag)
			break;
	if(r != nil){
		r->flushed = 1;
		if(r->t.type == Tread && r->fid->consopen)
			consprint("");	/* in case it's waiting... */
	}
	qunlock(&rpc->cli->rpclk);
	qunlock(&cli->wlk);
}

static void
rauth(Rpc*)
{
	/* BUG */
	error("no auth required");
}

void
attach(Fid *fid, char *aname, char *uname)
{
	Path *p;

	fid->uid = strdup(uname);
	p = newpath(fs->root);
	fid->p = p;
	if(strcmp(aname, "active") == 0 || strcmp(aname, "main/active") == 0){
		addelem(&p, fs->active);
		return;
	}
	fid->archived = 1;
	if(strcmp(aname, "archive") == 0 || strcmp(aname, "main/archive") == 0)
		addelem(&p, fs->archive);
	else if(strcmp(aname, "main") != 0 && strcmp(aname, "") != 0)
		error("unknown tree");
}

static void
rattach(Rpc *rpc)
{
	Fid *fid;
	Path *p;
	Memblk *f;

	fid = newfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	attach(fid, rpc->t.aname, rpc->t.uname);
	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	rpc->r.qid = mkqid(f);
	rwunlock(f, Rd);
}

Fid*
clone(Cli *cli, Fid *fid, int no)
{
	Fid *nfid;

	nfid = newfid(cli, no);
	nfid->p = clonepath(fid->p);
	nfid->uid = strdup(fid->uid);
	nfid->archived = fid->archived;
	nfid->consopen = fid->consopen;
	return nfid;
}

void
walk(Fid *fid, char *wname)
{
	Path *p;
	Memblk *f, *nf;

	p = fid->p;
	if(strcmp(wname, ".") == 0)
		goto done;
	if(strcmp(wname, "..") == 0){
		if(p->nf > 1)
			p = dropelem(&fid->p);
		goto done;
	}
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	dfaccessok(f, fid->uid, AEXEC);
	nf = dfwalk(f, wname, 0);
	rwunlock(f, Rd);
	p = addelem(&fid->p, nf);
	decref(nf);
done:
	f = p->f[p->nf-1];
	if(isro(f))
		fid->archived = f != fs->cons;
	else if(f == fs->active)
		fid->archived = 0;
}

static void
rwalk(Rpc *rpc)
{
	Fid *fid, *nfid;
	Path *p;
	Memblk *nf;
	int i;

	rpc->fid = getfid(rpc->cli, rpc->t.fid);
	fid = rpc->fid;
	if(rpc->t.fid == rpc->t.newfid && rpc->t.nwname > 1)
		error("can't walk like a clone without one");
	nfid = nil;
	if(rpc->t.fid != rpc->t.newfid)
		nfid = clone(rpc->cli, rpc->fid, rpc->t.newfid);
	if(catcherror()){
		putfid(nfid);
		putfid(nfid);		/* clunk */
		error(nil);
	}
	rpc->r.nwqid = 0;
	for(i=0; i < rpc->t.nwname; i++){
		if(catcherror()){
			if(rpc->r.nwqid == 0)
				error(nil);
			break;
		}
		walk(nfid, rpc->t.wname[i]);
		p = nfid->p;
		nf = p->f[p->nf-1];
		rwlock(nf, Rd);
		rpc->r.wqid[i] = mkqid(nf);
		rwunlock(nf, Rd);
		rpc->r.nwqid++;
		USED(rpc->r.nwqid);	/* damn error()s */
	}
	if(i < rpc->t.nwname){
		putfid(nfid);
		putfid(nfid);		/* clunk */
	}else{
		putfid(fid);
		rpc->fid = nfid;
	}
	noerror();
}

void
fidopen(Fid *fid, int mode)
{
	int fmode, amode;
	Memblk *f;
	Path *p;
	uvlong z;

	if(fid->omode != -1)
		error("fid already open");

	/* check this before we try to melt it */
	p = fid->p;
	f = p->f[p->nf-1];
	if(mode != OREAD)
		if(f == fs->root || f == fs->archive || fid->archived)
			error("can't write archived or built-in files");
	amode = 0;
	if((mode&3) != OREAD || (mode&OTRUNC) != 0)
		amode |= AWRITE;
	if((mode&3) != OWRITE)
		amode |= AREAD;
	if(amode != AREAD)
		if(f == fs->cons)
			rwlock(f, Wr);
		else{
			p = dfmelt(&fid->p, fid->p->nf);
			f = p->f[p->nf-1];
		}
	else
		rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, (amode!=AREAD)?Wr:Rd);
		error(nil);
	}
	fmode = f->mf->mode;
	if(mode != OREAD){
		if(f != fs->root && p->f[p->nf-2]->mf->mode&DMAPPEND)
			error("directory is append only");
		if((fmode&DMDIR) != 0)
			error("wrong open mode for a directory");
	}
	dfaccessok(f, fid->uid, amode);
	if(mode&ORCLOSE){
		if(f == fs->active || f == fs->cons || fid->archived)
			error("can't remove an archived or built-in file");
		dfaccessok(p->f[p->nf-2], fid->uid, AWRITE);
	}
	if(mode&ORCLOSE)
		fid->rclose++;
	if((fmode&DMEXCL) != 0 && f->mf->open)
		if(f != fs->cons || amode != AWRITE)	/* ok to write cons */
			error("exclusive use file already open");
	if((mode&OTRUNC) && f != fs->cons){
		z = 0;
		dfwattr(f, "length", &z, sizeof z);
	}
	f->mf->open++;
	fid->omode = mode&3;
	fid->loff = 0;
	fid->lidx = 0;
	fid->consopen = f == fs->cons;
	noerror();
	rwunlock(f, (amode!=AREAD)?Wr:Rd);
}

static void
ropen(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;

	rpc->fid = getfid(rpc->cli, rpc->t.fid);
	fid = rpc->fid;

	rpc->r.iounit = rpc->cli->msize - IOHDRSZ;
	fidopen(rpc->fid, rpc->t.mode);
	f = fid->p->f[fid->p->nf-1];
	rwlock(f, Rd);
	rpc->r.qid = mkqid(f);
	rwunlock(f, Rd);
}

void
fidcreate(Fid *fid, char *name, int mode, ulong perm)
{
	Path *p;
	Memblk *f, *nf;

	if(fid->omode != -1)
		error("fid already open");
	if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		error("that file name scares me");
	if(utfrune(name, '/') != nil)
		error("that file name is too creepy");
	if((perm&DMDIR) != 0 && mode != OREAD)
		error("wrong open mode for a directory");
	p = fid->p;
	f = p->f[p->nf-1];
	if(fid->archived)
		error("can't create in archived or built-in files");
	if((f->mf->mode&DMDIR) == 0)
		error("not a directory");
	p = dfmelt(&fid->p, fid->p->nf);
	f = p->f[p->nf-1];
	if(catcherror()){
		rwunlock(f, Wr);
		error(nil);
	}
	dfaccessok(f, fid->uid, AWRITE);
	if(!catcherror()){
		mbput(dfwalk(f, name, 0));
		error("file already exists");
	}
	nf = dfcreate(f, name, fid->uid, perm);
	addelem(&fid->p, nf);
	decref(nf);
	nf->mf->open++;
	noerror();
	rwunlock(f, Wr);
	fid->omode = mode&3;
	fid->loff = 0;
	fid->lidx = 0;
	if(mode&ORCLOSE)
		fid->rclose++;
}

static void
rcreate(Rpc *rpc)
{
	Fid *fid;
	Path *p;
	Memblk *f;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;

	fidcreate(fid, rpc->t.name, rpc->t.mode, rpc->t.perm);
	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	rpc->r.qid = mkqid(f);
	rwunlock(f, Rd);
	rpc->r.iounit = rpc->cli->msize-IOHDRSZ;
}

static ulong
packmeta(Memblk *f, uchar *buf, int nbuf)
{
	Dir d;

	nulldir(&d);
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
	
	d = fid->p->f[fid->p->nf-1];
	for(tot = 0; tot+2 < ndata; tot += nr){
		
		f = dfchild(d, fid->lidx);
		if(f == nil)
			break;
		nr = packmeta(f, data+tot, ndata-tot);
		mbput(f);
		if(nr <= 2)
			break;
		fid->lidx++;
	}
	return tot;
}

long
fidread(Fid *fid, void *data, ulong count, vlong offset)
{
	Memblk *f;
	Path *p;

	if(fid->omode == -1)
		error("fid not open");
	if(fid->omode == OWRITE)
		error("fid not open for reading");
	if(offset < 0)
		error("negative offset");
	p = fid->p;
	f = p->f[p->nf-1];
	if(f == fs->cons)
		return consread(data, count);
	rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	if(f->mf->mode&DMDIR){
		if(fid->loff != offset)
			error("non-sequential dir read not supported");
		count = readdir(fid, data, count, offset);
		fid->loff += count;
	}else
		count = dfpread(f, data, count, offset);
	noerror();
	rwunlock(f, Rd);
	return count;
}

static void
rread(Rpc *rpc)
{
	Fid *fid;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	if(rpc->t.count > rpc->cli->msize-IOHDRSZ)
		rpc->r.count = rpc->cli->msize-IOHDRSZ;
	rpc->r.data = (char*)rpc->data;
	rpc->r.count = fidread(fid, rpc->r.data, rpc->t.count, rpc->t.offset);

}

long
fidwrite(Fid *fid, void *data, ulong count, uvlong *offset)
{
	Memblk *f;
	Path *p;

	if(fid->omode == -1)
		error("fid not open");
	if(fid->omode == OREAD)
		error("fid not open for writing");
	p = fid->p;
	f = p->f[p->nf-1];
	if(f == fs->cons)
		return conswrite(data, count);
	p = dfmelt(&fid->p, fid->p->nf);
	f = p->f[p->nf-1];
	if(catcherror()){
		rwunlock(f, Wr);
		error(nil);
	}
	count = dfpwrite(f, data, count, offset);
	rwunlock(f, Wr);
	noerror();
	return count;
}

static void
rwrite(Rpc *rpc)
{
	Fid *fid;
	uvlong off;

	if(rpc->t.offset < 0)
		error("negative offset");
	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	off = rpc->t.offset;
	rpc->r.count = fidwrite(fid, rpc->t.data, rpc->t.count, &off);
}

void
fidclose(Fid *fid)
{
	Memblk *f, *fp;
	Path *p;

	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Wr);
	f->mf->open--;
	rwunlock(f, Wr);
	fid->omode = -1;
	if(fid->rclose){
		p = dfmelt(&fid->p, fid->p->nf-1);
		fp = p->f[p->nf-2];
		rwlock(f, Wr);
		if(catcherror()){
			rwunlock(f, Wr);
			mbput(f);
		}else{
			dfremove(fp, f);
			fid->p->nf--;
			noerror();
		}
		rwunlock(fp, Wr);
	}
	putpath(fid->p);
	fid->p = nil;
	fid->consopen = 0;
}

static void
rclunk(Rpc *rpc)
{
	Fid *fid;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	if(fid->omode != -1)
		fidclose(fid);
	d9print("clunking %X\n\n", fid);
	putfid(fid);
	putfid(fid);
	rpc->fid = nil;
}

void
fidremove(Fid *fid)
{
	Memblk *f, *fp;
	Path *p;

	p = fid->p;
	f = p->f[p->nf-1];
	if(fid->archived || f == fs->cons || f == fs->active)
		error("can't remove archived or built-in files");
	p = dfmelt(&fid->p, fid->p->nf-1);
	fp = p->f[p->nf-2];
	f = p->f[p->nf-1];
	rwlock(f, Wr);
	if(catcherror()){
		rwunlock(f, Wr);
		rwunlock(fp, Wr);
		error(nil);
	}
	if(fp->mf->mode&DMAPPEND)
		error("directory is append only");
	dfaccessok(fp, fid->uid, AWRITE);
	fid->omode = -1;
	dfremove(fp, f);
	fid->p->nf--;
	noerror();
	rwunlock(fp, Wr);
	putpath(fid->p);
	fid->p = nil;
}

static void
rremove(Rpc *rpc)
{
	Fid *fid;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	if(catcherror()){
		d9print("clunking %X\n\n", fid);
		putfid(fid);
		putfid(fid);
		rpc->fid = nil;
		error(nil);
	}
	fidremove(fid);
	noerror();
	d9print("clunking %X\n\n", fid);
	putfid(fid);
	putfid(fid);
	rpc->fid = nil;
}

static void
rstat(Rpc *rpc)
{
	Fid *fid;
	Memblk *f;
	Path *p;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	rpc->r.stat = rpc->data;
	rpc->r.nstat = packmeta(f, rpc->data, rpc->cli->msize-IOHDRSZ);
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
	Path *p;
	Dir d, *sd;
	u64int n;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	p = fid->p;
	f = p->f[p->nf-1];
	if(fid->archived || f == fs->cons)
		error("can't wstat archived or built-in files");
	p = dfmelt(&fid->p, fid->p->nf);
	f = p->f[p->nf-1];
	n = convM2D(rpc->t.stat, rpc->t.nstat, &d, nil);
	sd = malloc(n);
	if(catcherror()){
		rwunlock(f, Wr);
		free(sd);
		error(nil);
	}
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
		if(isro(f) || f == fs->active)
			error("can't rename built-in files");
		dfaccessok(p->f[p->nf-2], fid->uid, AWRITE);
		if(!catcherror()){
			mbput(dfwalk(f, sd->name, 0));
			error("file already exists");
		}
	}else
		sd->name[0] = 0;

	if(sd->uid[0] != 0 && strcmp(sd->uid, f->mf->uid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the owner may donate a file");
		if(!fs->config && !member(sd->uid, fid->uid) != 0)
			error("you are not in that group");
	}else
		sd->uid[0] = 0;
	if(sd->gid[0] != 0 && strcmp(sd->gid, f->mf->gid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the onwer may change group");
		if(!fs->config && !member(sd->gid, fid->uid) != 0)
			error("you are not in that group");
	}else
		sd->gid[0] = 0;
	if(sd->mode != ~0 && f->mf->mode != sd->mode){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0 &&
		   !member(f->mf->gid, fid->uid) != 0)
			error("only the onwer or members may change mode");
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

void
replied(Rpc *rpc)
{
	Rpc **rl;

	qlock(&rpc->cli->rpclk);
	for(rl = &rpc->cli->rpcs; (*rl != nil); rl = &(*rl)->next)
		if(*rl == rpc){
			*rl = rpc->next;
			break;
		}
	rpc->cli->nrpcs--;
	qunlock(&rpc->cli->rpclk);
	rpc->next = nil;
	putfid(rpc->fid);
	rpc->fid = nil;
	putcli(rpc->cli);
	rpc->cli = nil;
}

static char*
rpcworker9p(void *v, void**aux)
{
	Rpc *rpc;
	Cli *cli;
	char err[128];
	long n;

	rpc = v;
	cli = rpc->cli;
	threadsetname("rpcworker9p %s %R", cli->addr, rpc);
	dPprint("%s starting\n", threadgetname());

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
	qlock(&cli->wlk);
	if(rpc->flushed == 0){
		d9print("-> %F\n", &rpc->r);
		n = convS2M(&rpc->r, cli->wdata, sizeof cli->wdata);
		if(n == 0)
			fatal("rpcworker: convS2M");
		if(write(cli->fd, cli->wdata, n) != n)
			d9print("%s: %r\n", cli->addr);
	}else
		d9print("flushed: %F\n", &rpc->r);
	qunlock(&cli->wlk);

	replied(rpc);
	freerpc(rpc);
	dPprint("%s exiting\n", threadgetname());
	return nil;
}

char*
cliworker9p(void *v, void**aux)
{
	Cli *cli;
	long n;
	Rpc *rpc;

	cli = v;
	threadsetname("cliworker9p %s", cli->addr);
	dPprint("%s started\n", threadgetname());
	if(*aux == nil){
		errinit(Errstack);
		*aux = v;		/* make it not nil */
	}

	if(catcherror())
		fatal("worker: uncatched: %r");

	rpc = nil;
	for(;;){
		if(rpc == nil)
			rpc = newrpc();
		n = read9pmsg(cli->fd, rpc->data, Maxmdata+IOHDRSZ);
		if(n < 0){
			d9print("%s: read: %r\n", cli->addr);
			break;
		}
		if(n == 0)
			continue;
		if(convM2S(rpc->data, n, &rpc->t) == 0){
			d9print("%s: convM2S failed\n", cli->addr);
			continue;
		}
		if(rpc->t.type >= nelem(fcalls) || fcalls[rpc->t.type] == nil){
			d9print("%s: bad fcall type %d\n", cli->addr, rpc->t.type);
			continue;
		}
		d9print("<-%F\n", &rpc->t);
		rpc->cli = cli;
		incref(cli);

		qlock(&cli->rpclk);
		rpc->next = cli->rpcs;
		cli->rpcs = rpc;
		cli->nrpcs++;
		qunlock(&cli->rpclk);

		fspolicy();
		if(rpc->t.type == Tflush ||
		   (Rpcspercli != 0 && cli->nrpcs >= Rpcspercli))
			rpcworker9p(rpc, aux);
		else
			getworker(rpcworker9p, rpc, nil);
		rpc = nil;
	}
	putcli(cli);
	noerror();
	dPprint("%s exiting\n", threadgetname());
	return nil;
};



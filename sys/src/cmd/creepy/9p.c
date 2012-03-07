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

void
ninestats(int clr)
{
	int i;

	fprint(2, "fids:\t%4uld alloc %4uld free (%4uld bytes)\n",
		fidalloc.nalloc, fidalloc.nfree, fidalloc.elsz);
	fprint(2, "rpcs:\t%4uld alloc %4uld free (%4uld bytes)\n",
		rpcalloc.nalloc, rpcalloc.nfree, rpcalloc.elsz);
	fprint(2, "clis:\t%4uld alloc %4uld free (%4uld bytes)\n",
		clialloc.nalloc, clialloc.nfree, clialloc.elsz);
	for(i = 0; i < nelem(fcalls); i++)
		if(fcalls[i] != nil && ncalls[i] > 0){
			fprint(2, "%-8s\t%5uld calls\t%11ulld µs\n",
				callname[i], ncalls[i],
				(calltime[i]/ncalls[i])/1000);
			if(clr){
				ncalls[i] = 0;
				calltime[i] = 0;
			}
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
	xqlock(&cli->wlk);	/* nobody replies now */
	xqlock(&rpc->cli->rpclk);
	for(r = rpc->cli->rpcs; r != nil; r = r->next)
		if(r->t.tag == rpc->t.oldtag)
			break;
	if(r != nil){
		r->flushed = 1;
		if(r->t.type == Tread && r->fid->consopen)
			consprint("");	/* in case it's waiting... */
	}
	xqunlock(&rpc->cli->rpclk);
	xqunlock(&cli->wlk);
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
	Path *p;
	Memblk *f;

	fid = newfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	fidattach(fid, rpc->t.aname, rpc->t.uname);
	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	rpc->r.qid = mkqid(f);
	rwunlock(f, Rd);
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
		nfid = fidclone(rpc->cli, rpc->fid, rpc->t.newfid);
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
		fidwalk(nfid, rpc->t.wname[i]);
		noerror();
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
pack9dir(Memblk *f, uchar *buf, int nbuf)
{
	Dir d;

	nulldir(&d);
	d.name = f->mf->name;
	d.qid = mkqid(f);
	d.mode = f->mf->mode;
	d.length = f->mf->length;
	if(d.mode&DMDIR)
		d.length = 0;
	d.uid = f->mf->uid;
	d.gid = f->mf->gid;
	d.muid = f->mf->muid;
	d.atime = f->mf->atime;
	d.mtime = f->mf->mtime;
	return convD2M(&d, buf, nbuf);
}

static void
rread(Rpc *rpc)
{
	Fid *fid;
	vlong off;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	if(rpc->t.count > rpc->cli->msize-IOHDRSZ)
		rpc->r.count = rpc->cli->msize-IOHDRSZ;
	rpc->r.data = (char*)rpc->data;
	off = rpc->t.offset;
	rpc->r.count = fidread(fid, rpc->r.data, rpc->t.count, off, pack9dir);

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

static void
rclunk(Rpc *rpc)
{
	Fid *fid;

	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	if(fid->omode != -1)
		fidclose(fid);
	putfid(fid);
	putfid(fid);
	rpc->fid = nil;
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
	xqlock(fid);
	if(catcherror()){
		xqunlock(fid);
		error(nil);
	}
	p = fid->p;
	f = p->f[p->nf-1];
	rwlock(f, Rd);
	noerror();
	xqunlock(fid);
	if(catcherror()){
		rwunlock(f, Rd);
		error(nil);
	}
	rpc->r.stat = rpc->data;
	rpc->r.nstat = pack9dir(f, rpc->data, rpc->cli->msize-IOHDRSZ);
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
	Dir sd;
	u64int n;

	n = convM2D(rpc->t.stat, rpc->t.nstat, &sd, (char*)rpc->t.stat);
	if(n != rpc->t.nstat)
		error("convM2D: bad stat");
	fid = getfid(rpc->cli, rpc->t.fid);
	rpc->fid = fid;
	xqlock(fid);
	if(catcherror()){
		xqunlock(fid);
		error(nil);
	}
	p = fid->p;
	f = p->f[p->nf-1];
	if(fid->archived || f == fs->cons)
		error("can't wstat archived or built-in files");
	p = dfmelt(&fid->p, fid->p->nf);
	f = p->f[p->nf-1];
	noerror();
	xqunlock(fid);
	if(catcherror()){
		rwunlock(f, Wr);
		error(nil);
	}
	if(sd.length != ~0 && sd.length != f->mf->length){
		if(f->mf->mode&DMDIR)
			error("can't resize a directory");
		dfaccessok(f, fid->uid, AWRITE);
	}else
		sd.length = ~0;

	if(sd.name[0] && strcmp(f->mf->name, sd.name) != 0){
		if(isro(f) || f == fs->active)
			error("can't rename built-in files");
		dfaccessok(p->f[p->nf-2], fid->uid, AWRITE);
		if(!catcherror()){
			mbput(dfwalk(f, sd.name, 0));
			error("file already exists");
		}
	}else
		sd.name[0] = 0;

	if(sd.uid[0] != 0 && strcmp(sd.uid, f->mf->uid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the owner may donate a file");
		if(!fs->config && !member(sd.uid, fid->uid) != 0)
			error("you are not in that group");
	}else
		sd.uid[0] = 0;
	if(sd.gid[0] != 0 && strcmp(sd.gid, f->mf->gid) != 0){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0)
			error("only the onwer may change group");
		if(!fs->config && !member(sd.gid, fid->uid) != 0)
			error("you are not in that group");
	}else
		sd.gid[0] = 0;
	if(sd.mode != ~0 && f->mf->mode != sd.mode){
		if(!fs->config && strcmp(fid->uid, f->mf->uid) != 0 &&
		   !member(f->mf->gid, fid->uid) != 0)
			error("only the onwer or members may change mode");
	}else
		sd.mode = ~0;

	if(sd.length != ~0)
		wstatint(f, "length", sd.length);
	if(sd.name[0])
		wstatstr(f, "name", sd.name);
	if(sd.uid[0])
		wstatstr(f, "name", sd.name);
	if(sd.gid[0])
		wstatstr(f, "name", sd.name);
	if(sd.mode != ~0)
		wstatint(f, "mode", sd.mode);
	if(fs->config && sd.atime != ~0)
		wstatint(f, "atime", sd.atime);
	if(fs->config && sd.mtime != ~0)
		wstatint(f, "mtime", sd.mtime);
	if(fs->config && sd.muid[0] != 0 && strcmp(sd.muid, f->mf->muid) != 0)
		wstatint(f, "mtime", sd.mtime);

	noerror();
	rwunlock(f, Wr);
}

static char*
rpcworker9p(void *v, void**aux)
{
	Rpc *rpc;
	Cli *cli;
	Fid *fid;
	char err[128];
	long n;
	int nerr;

	rpc = v;
	cli = rpc->cli;
	threadsetname("rpcworker9p %s %R", cli->addr, rpc);
	dPprint("%s starting\n", threadgetname());

	if(*aux == nil){
		errinit(Errstack);
		*aux = v;		/* make it not nil */
	}
	nerr = nerrors();


	rpc->r.tag = rpc->t.tag;
	rpc->r.type = rpc->t.type + 1;

	if(catcherror()){
		rpc->r.type = Rerror;
		rpc->r.ename = err;
		rerrstr(err, sizeof err);
	}else{
		fcalls[rpc->t.type](rpc);	
		noerror();
	}

	fid = nil;
	if(rpc->fid != nil && rpc->fid->ref > 1){
		/* The fid is not clunked by this rpc; ok to read/walk ahead */
		fid = rpc->fid;
		incref(fid);
	}
	if(catcherror()){
		if(fid != nil)
			putfid(fid);
	}

	xqlock(&cli->wlk);
	putfid(rpc->fid);	/* release rpc fid before replying */
	rpc->fid = nil;

	if(rpc->flushed == 0){
		d9print("-> %F\n", &rpc->r);
		n = convS2M(&rpc->r, cli->wdata, sizeof cli->wdata);
		if(n == 0)
			fatal("rpcworker: convS2M");
		if(write(cli->fd, cli->wdata, n) != n)
			d9print("%s: %r\n", cli->addr);
	}else
		dprint("flushed: %F\n", &rpc->r);
	calltime[rpc->t.type] += nsec() - rpc->t0;
	ncalls[rpc->t.type]++;
	xqunlock(&cli->wlk);

	if(fid != nil){
		switch(rpc->t.type){
		case Tread:	/* read ahead? */
			if(rpc->r.type == Rread && rpc->r.count == rpc->t.count)
				fidrahead(fid, rpc->t.offset + rpc->t.count);
			break;
		case Twalk:	/* walk ahead? */
			if(rpc->r.type == Rwalk)
				fidwahead(fid);
			break;
		}
		putfid(fid);
	}
	noerror();

	replied(rpc);
	freerpc(rpc);
	dPprint("%s exiting\n", threadgetname());

	if(nerrors() != nerr)
		fatal("%s: unbalanced error stack", threadgetname());
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
		if(dbg['E'])
			dumpfids();
		if(rpc == nil)
			rpc = newrpc();
		n = read9pmsg(cli->fd, rpc->data, Maxmdata+IOHDRSZ);
		if(n < 0){
			d9print("%s: read: %r\n", cli->addr);
			break;
		}
		if(n == 0)
			continue;
		rpc->t0 = nsec();
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

		xqlock(&cli->rpclk);
		rpc->next = cli->rpcs;
		cli->rpcs = rpc;
		cli->nrpcs++;
		xqunlock(&cli->rpclk);

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



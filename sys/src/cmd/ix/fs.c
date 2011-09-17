#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>
#include <fcall.h>
#include <bio.h>

#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "tses.h"
#include "ch.h"
#include "dbg.h"
#include "fs.h"
#include "file.h"

/*
 * Mbuf convention:
 * Msg.io[0] always refers to the message buffer.
 */

/*
 * Flushing is done by aborting the Ch where the RPC is being sent.
 * When ch is flushing, it will not deliver more messages to the
 * reader.
 * Thus, the client should not flush before receiving replies to
 * requests like Tclone or Topen which allocate new resources.
 * It should flush only when replies received from us make it clear
 * what's the state on its behalf.
 *
 * This makes flush processing a lot simpler than it could be.
 */

typedef struct Fs Fs;
typedef struct Rpc Rpc;
typedef struct Fid Fid;


struct Fs
{
	QLock;
	Fid **fids;
	int nfids;
	Fid *free;

	Ses *s;		/* session (i.e., tcp connection) */
	Cmux *cm;	/* channel multiplexor */
	Channel *argc;	/* to pass args to workers */
	ulong msize;	/* maximum data size */
};

struct Fid
{
	Ref;
	Fid *next;	/* in free list */
	File *file;
	int id;
	int fd;		/* meaningful to File only */
	int cflags;	/* OCEND|OCERR */
	int omode;
};

struct Rpc
{
	Fs *fs;
	Ch *ch;
	Msg *m;
	Fid *fid;
	Fscall t, r;
	int last;
};

enum
{
	Large = 0,
	Small,
};

static Ssrv *ssrv;
static Channel *poolc[2];
static int npools[2];
static ulong poolmsz[2] = 
{
	[Large]	Msgsz,
	[Small] Smsgsz,
};


static Fid*
newfid(Fs *fs)
{
	Fid *fid;

	qlock(fs);
	if(fs->free != nil){
		fid = fs->free;
		fs->free = fid->next;
		qunlock(fs);
		return fid;
	}
	if((fs->nfids%Incr) == 0)
		fs->fids = erealloc(fs->fids, (fs->nfids+Incr)*sizeof(Fid*));
	fid = emalloc(sizeof *fid);
	fs->fids[fs->nfids] = fid;
	fid->id = fs->nfids++;
	fid->ref = 1;
	fid->omode = -1;
	fid->fd = -1;
	qunlock(fs);
	return fid;
}

static void
putfid(Fs *fs, Fid *fid)
{
	if(decref(fid) == 0){
		qlock(fs);
		fid->next = fs->free;
		fs->free = fid;
		if(fid->file != nil){
			if(fid->fd != -1)
				closefile(fid->file, fid->fd);
			putfile(fid->file);
		}
		fid->file = nil;
		fid->fd = -1;
		fid->omode = -1;
		qunlock(fs);
	}
}

static Fid*
getfid(Fs *fs, int id)
{
	Fid *fid;

	qlock(fs);
	if(id < 0 || id >= fs->nfids || fs->fids[id]->file == nil){
		qunlock(fs);
		return nil;
	}
	fid = fs->fids[id];
	incref(fid);
	qunlock(fs);
	return fid;
}

static void
putpool(Mpool *mp)
{
	poolstats(mp);
	sendp(poolc[Large], mp);
}

static void
putspool(Mpool *mp)
{
	poolstats(mp);
	sendp(poolc[Small], mp);
}

static Mpool*
getpool(int sz)
{
	Mpool *mp;

	mp = nbrecvp(poolc[sz]);
	if(mp != nil)
		return mp;

	if(ainc(&npools[sz]) < Nses){
		mp = newpool(poolmsz[sz], Nmsgs);
		if(mp == nil){
			adec(&npools[sz]);
			return nil;
		}
		mp->freepool = putpool;
		if(sz == Small)
			mp->freepool = putspool;
		return mp;
	}
	return recvp(poolc[sz]);
}

static int
fsreply(Rpc *rpc)
{
	Msg *m;
	Io *io;
	ulong n;
	int rc;

	m = rpc->m;
	m->hdr = nil;
	io = &m->io[0];
	/*
	 * Rread reads data directly after the packed reply Fscall
	 * and sets io's rp and wp by itself.
	 */
	dfprint("%cch%d-> %G\n", rpc->last?'|':'-', rpc->ch->id, &rpc->r);
	if(rpc->r.type == Rread)
		n = pack(&rpc->r, io->bp, packedsize(&rpc->r));
	else{
		ioreset(io);
		n = pack(&rpc->r, io->wp, IOCAP(io));
		io->wp += n;
	}
	if(n <= 0)
		sysfatal("fsreply: pack (Smsgsz too small?)");
	rc = chsend(rpc->ch, rpc->m, rpc->last);
	if(rc < 0){
		dfprint("fsreply: %r\n");
		rpc->last = 1;
	}
	if(rpc->last != 0 && rpc->fid != nil && (rpc->fid->cflags&OCEND) != 0){
		putfid(rpc->fs, rpc->fid);	/* held by client */
		putfid(rpc->fs, rpc->fid);	/* held by rpc */
		rpc->fid = nil;
	}
	return rc;
}

static int
fserror(Rpc *rpc, char *e)
{
	rpc->r.type = Rerror;
	rpc->r.ename = e;
	rpc->last = 1;
	fsreply(rpc);
	return -1;
}

static int
fssyserror(Rpc *rpc)
{
	char *e;

	e = smprint("%r");
	rpc->r.type = Rerror;
	rpc->r.ename = e;
	rpc->last = 1;
	if(rpc->fid != nil && (rpc->fid->cflags&OCERR) != 0)
		rpc->fid->cflags |= OCEND;
	fsreply(rpc);
	free(e);
	return -1;
}

static int
fsversion(Rpc *rpc)
{
	if(rpc->t.msize < rpc->fs->msize){
		rpc->fs->msize = rpc->t.msize;
		dfprint("fsversion: msize %uld\n", rpc->fs->msize);
	}
	rpc->r.msize = rpc->fs->msize;
	if(strcmp(rpc->t.version, "ix") != 0)
		return fserror(rpc, "wrong version");
	rpc->r.version = "ix";
	return fsreply(rpc);
}

static int
fsauth(Rpc *rpc)
{
	return fserror(rpc, "no auth required");
}

static int
fsattach(Rpc *rpc)
{
	if(strcmp(rpc->t.uname, getuser()) != 0)
		return fserror(rpc, "bad user in attach");
	/*
	 * rpc->t.aname ignored
	 * rpc->t.fid is not used
	 */
	if(rpc->t.afid != NOFID)
		return fserror(rpc, "auth not supported");
	rpc->fid = newfid(rpc->fs);
	incref(rpc->fid);	/* client's reference */
	rpc->r.fid = rpc->fid->id;
	rpc->fid->file = rootfile();
	rpc->r.qid = fileqid(rpc->fid->file);
	return fsreply(rpc);
}


static int
fsclone(Rpc *rpc)
{
	Fid *nfid;
	Fid *fid;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	nfid = newfid(rpc->fs);	/* ref held by client */
	rpc->r.newfid = nfid->id;
	nfid->omode = fid->omode;
	nfid->file = fid->file;
	incref(nfid->file);
	putfid(rpc->fs, fid);		/* held by rpc */
	rpc->fid = nfid;
	nfid->cflags = rpc->t.cflags;
	incref(nfid);		/* held by rpc */
	return fsreply(rpc);
}

static int
fswalk(Rpc *rpc)
{
	Fid *fid;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode != -1)
		return fserror(rpc, "can't walk an open fid");
	if(walkfile(&fid->file, rpc->t.wname) < 0)
		return fssyserror(rpc);
	rpc->r.wqid = fileqid(fid->file);
	return fsreply(rpc);
}

static int
fsopen(Rpc *rpc)
{
	Fid *fid;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode != -1)
		return fserror(rpc, "fid already open");
	fid->fd = openfile(fid->file, rpc->t.mode);
	if(fid->fd < 0)
		return fssyserror(rpc);
	fid->omode = rpc->t.mode;
	rpc->r.qid = fileqid(fid->file);
	rpc->r.iounit = 0;
	return fsreply(rpc);
}

static int
fscreate(Rpc *rpc)
{
	int fd;
	Fid *fid;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode != -1)
		return fserror(rpc, "fid already open");
	fd = createfile(&fid->file, rpc->t.name, rpc->t.mode, rpc->t.perm);
	if(fd < 0)
		return fssyserror(rpc);
	fid->fd = fd;
	fid->omode = rpc->t.mode;
	rpc->r.qid = fileqid(fid->file);
	rpc->r.iounit = 0;
	return fsreply(rpc);
}

static int
fsread(Rpc *rpc)
{
	long nr;
	Io *io;
	Fid *fid;
	int i, saved, last;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode < 0 || (fid->omode&3) == OWRITE)
		return fserror(rpc, "fid not open for reading");
	if(rpc->t.nmsg == 0)
		rpc->t.nmsg = 1;
	/*
	 * Get a large buffer; we are probably using a small one.
	 */
	freemsg(rpc->m);
	rpc->m = newmsg(rpc->fs->s->pool);
	for(i = 0; i != rpc->t.nmsg; i++){
		io = &rpc->m->io[0];
		nr = rpc->t.count;
		rpc->m->hdr = nil;
		ioreset(io);
		io->wp += packedsize(&rpc->r);
		if(nr < 0 || nr > IOCAP(io))
			nr = IOCAP(io);
		if(IOLEN(io)+nr > rpc->fs->msize)
			nr = rpc->fs->msize - IOLEN(io);
		rpc->r.data = (char*)io->wp;
		nr = preadfile(fid->file, fid->fd, io->wp, nr, rpc->t.offset);
		if(nr < 0)
			return fssyserror(rpc);
		io->wp += nr;
		rpc->t.offset += nr;

		rpc->r.count = nr;
		saved = rpc->last;
		rpc->last = 0;
		last = saved && (rpc->last = i == rpc->t.nmsg || nr == 0);
		if(fsreply(rpc) < 0)
			return -1;
		rpc->last = saved;
		if(last)
			break;
		rpc->m = newmsg(rpc->fs->s->pool);
	}
	return 0;
}

static int
fswrite(Rpc *rpc)
{
	long nw;
	Fid *fid;
	Fscall *t;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode < 0 || (fid->omode&3) == OREAD)
		return fserror(rpc, "fid not open for writing");
	t = &rpc->t;
	nw = pwritefile(fid->file, fid->fd, t->data, t->count, t->offset);
	if(nw < 0)
		return fssyserror(rpc);
	rpc->r.count = nw;
	return fsreply(rpc);
}

static int
fsclunk(Rpc *rpc)
{
	Fid * fid;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	putfid(rpc->fs, fid);
	rpc->fid = nil;
	return fsreply(rpc);
}

static int
fsremove(Rpc *rpc)
{
	Fid * fid;
	int r;

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode >= 0){
		closefile(fid->file, fid->fd);
		fid->fd = -1;
		fid->omode = -1;
	}
	putfid(rpc->fs, fid);	/* held by client */

	/* file goes when rpc->fid reference is released later */
	r = removefile(fid->file);

	putfid(rpc->fs, fid);	/* held by rpc */
	rpc->fid = nil;
	if(r < 0)
		return fssyserror(rpc);
	else
		return fsreply(rpc);
}

static int
fsstat(Rpc *rpc)
{
	Fid *fid;
	Dir *d;
	uchar buf[512];	/* bug: fixes stat size */

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	d = statfile(fid->file, 1);
	if(d == nil)
		return fssyserror(rpc);
	rpc->r.nstat = convD2M(d, buf, sizeof buf);
	if(rpc->r.nstat <= BIT32SZ)
		sysfatal("fsstat: buf too short");
	rpc->r.stat = buf;
	free(d);

	/*
	 * We are probably using a small buffer, get a large
	 * one for the reply.
	 */
	freemsg(rpc->m);
	rpc->m = newmsg(rpc->fs->s->pool);
	return fsreply(rpc);
}

static int
fswstat(Rpc *rpc)
{
	Fid *fid;
	Dir d;
	char buf[512];	/* bug: fixes stat size */

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(convM2D(rpc->t.stat, rpc->t.nstat, &d, buf) <= BIT32SZ)
		return fserror(rpc, "bad stat data");
	if(wstatfile(fid->file, &d) < 0)
		return fssyserror(rpc);
	return fsreply(rpc);
}

static int
cmpulong(uvlong u1, uvlong u2, int cond)
{
	switch(cond){
	case CEQ:
		return u1 == u2;
	case CNE:
		return u1 != u2;
	case CLE:
		return u1 >= u2;
	case CLT:
		return u1 > u2;
	case CGT:
		return u1 < u2;
	case CGE:
		return u2 <= u2;
	default:
		return 0;
	}
}

static int
cmpstr(char *s1, char *s2, int cond)
{
	switch(cond){
	case CEQ:
		return strcmp(s1, s2) == 0;
	case CNE:
		return strcmp(s1, s2) != 0;
	case CLE:
		return strcmp(s1, s2) >= 0;
	case CLT:
		return strcmp(s1, s2) > 0;
	case CGT:
		return strcmp(s1, s2) < 0;
	case CGE:
		return strcmp(s1, s2) <= 0;
	default:
		return 0;
	}
}

static int
fscond(Rpc *rpc)
{
	Fid *fid;
	Dir cd, *d;
	char buf[512];	/* bug: fixes stat size */

	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(convM2D(rpc->t.stat, rpc->t.nstat, &cd, buf) <= BIT32SZ)
		sysfatal("fswstat: bug");
	d = statfile(fid->file, 1);
	if(d == nil)
		return fssyserror(rpc);
	if(cd.qid.path != ~0ULL && cmpulong(cd.qid.path, d->qid.path, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false qid.path");
	}
	if(cd.qid.vers != ~0UL && cmpulong(cd.qid.vers, d->qid.vers, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false qid.vers");
	}
	if(cd.mode != ~0 && cmpulong(cd.mode, d->mode, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false mode");
	}
	if(cd.atime != ~0 && cmpulong(cd.atime, d->atime, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false atime");
	}
	if(cd.mtime != ~0 && cmpulong(cd.mtime, d->mtime, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false mtime");
	}
	if(cd.length != ~0ULL && cmpulong(cd.length, d->length, rpc->t.cond)==0){
		free(d);
		return fserror(rpc, "false length");
	}
	if(cd.name != nil && cmpstr(cd.name, d->name, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false name");
	}
	if(cd.uid != nil && cmpstr(cd.uid, d->uid, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false uid");
	}
	if(cd.gid != nil && cmpstr(cd.gid, d->gid, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false gid");
	}
	if(cd.muid != nil && cmpstr(cd.muid, d->muid, rpc->t.cond) == 0){
		free(d);
		return fserror(rpc, "false muid");
	}
	free(d);
	return fsreply(rpc);
}

static int
fsfid(Rpc *rpc)
{
	if(rpc->fid != nil)
		putfid(rpc->fs, rpc->fid);
	rpc->fid = getfid(rpc->fs, rpc->t.fid);
	if(rpc->fid == nil)
		return fserror(rpc, "no such fid");
	rpc->fid->cflags = rpc->t.cflags;
	return fsreply(rpc);
}

typedef int (*Rpcfn)(Rpc*);

static Rpcfn reqfn[Tmax] = {
[Tversion]	fsversion,
[Tauth]		fsauth,
[Tattach]	fsattach,
[Tclone]	fsclone,
[Twalk]		fswalk,
[Topen]		fsopen,
[Tcreate]	fscreate,
[Tread]		fsread,
[Twrite]		fswrite,
[Tclunk]	fsclunk,
[Tremove]	fsremove,
[Tstat]		fsstat,
[Twstat]		fswstat,
[Tfid]		fsfid,
[Tcond]		fscond,
};

static int
fsrpc(Rpc *rpc)
{
	Msg *m;
	int r;

	m = rpc->m;
	if(IOLEN(&m->io[0]) == 0){
		dfprint("ch%d: eof\n", rpc->ch->id);
		goto Fail;
	}
	if(unpack(m->io[0].rp, IOLEN(&m->io[0]), &rpc->t) <= 0){
		dfprint("ch%d: unknown message: %r\n", rpc->ch->id);
		goto Fail;
	}
	dfprint("<-ch%d%c %G\n", rpc->ch->id, rpc->last?'|':'-', &rpc->t);
	if(rpc->t.type >= nelem(reqfn) || reqfn[rpc->t.type] == nil){
		dfprint("bad msg type %d\n", rpc->t.type);
		goto Fail;
	}
	rpc->r.type = rpc->t.type+1;
	r =  reqfn[rpc->t.type](rpc);
	return r;
Fail:
	m->hdr = nil;
	ioreset(m->io);
	rpc->last = 1;
	chsend(rpc->ch, m, 1);
	return -1;
}

static void
fsrpcproc(void *a)
{
	Channel *c;
	int id, waslast;
	Rpc rpc;

	c = a;
	rpc.fid = nil;
	rpc.fs = recvp(c);
	rpc.ch = recvp(c);
	rpc.last = 0;
	threadsetname("fsrpcproc fs %p ch %p", rpc.fs, rpc.ch);
	id = rpc.ch->id;
	dfprint("fsrpc[%p!%d]: new chan %s\n", rpc.fs->s, id, rpc.fs->s->addr);
	while((rpc.m = chrecv(rpc.ch, &rpc.last)) != nil){
		waslast = rpc.last;
		if(fsrpc(&rpc) < 0 && !waslast){
			drainch(rpc.ch);
			break;
		}
		if(rpc.last)
			break;
	}
	if(rpc.fid != nil)
		putfid(rpc.fs, rpc.fid);
	dfprint("fsrpc[%p!%d]: done\n", rpc.fs->s, id);
	threadexits(nil);
}

static void
fssrvproc(void *a)
{
	Fs fs;
	Ch *ch;
	int i;

	threadsetname("fssrvproc %p", a);
	dfprint("fssrvproc[%p]: started\n", &fs);
	memset(&fs, 0, sizeof fs);
	fs.s = a;
	fs.msize = Msgsz - Chhdrsz;
	startses(fs.s, getpool(Large), getpool(Small));
	fs.argc = echancreate(sizeof(void*), 0);
	fs.cm = muxses(fs.s->rc, fs.s->wc, fs.s->ec);
	dfprint("fssrvproc: %p %s\n", &fs, fs.s->addr);
	while((ch = recvp(fs.cm->newc)) != nil){
		threadcreate(fsrpcproc, fs.argc, Stack);
		sendp(fs.argc, &fs);
		sendp(fs.argc, ch);
	}
	closemux(fs.cm);
	chanfree(fs.argc);
	/* mux released by itself */
	for(i = 0; i < fs.nfids; i++)
		free(fs.fids[i]);
	free(fs.fids);
	dfprint("fssrvproc[%p]: done\n", &fs);
	threadexits(nil);
}


static int consfd = -1;
static int constid = -1;
static Biobuf bcmd;

static void
consproc(void *a)
{
	int fd;
	char *cmd;

	threadsetname("consproc");
	dfprint("consproc started\n");
	fd = (int)a;
	Binit(&bcmd, fd, OREAD);
	constid = threadid();
	for(;;){
		fprint(fd, "> ");
		cmd = Brdstr(&bcmd, '\n', 1);
		if(cmd == nil)
			break;
		if(cmd[0] == 0){
			free(cmd);
			continue;
		}
		dfprint("cmd: %s\n", cmd);
		if(strcmp(cmd, "sync") == 0){
			filesync();
			fprint(fd, "synced\n");
			fprint(2, "synced\n");
		}else if(strcmp(cmd, "halt") == 0){
			free(cmd);
			break;
		}else
			fprint(fd, "%s?\n", cmd);
		free(cmd);
	}
	fprint(2, "consproc: halting\n");
	filesync();
	fprint(fd, "halted\n");
	fprint(2, "halted\n");
	dfprint("consproc done\n");
	threadexitsall(nil);
}

static int
cons(char *srv)
{
	char *fn;
	int p[2], fd;

	fn = esmprint("/srv/%s", srv);
	fd = create(fn, OWRITE|ORCLOSE|OCEXEC, 0660);
	if(fd < 0){
		free(fn);
		return -1;
	}
	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	fprint(fd, "%d", p[0]);
	consfd = p[0];
	proccreate(consproc, (void*)p[1], Stack);
	free(fn);
	return 0;
}

void
fsinit(char *addr, char *srv)
{
	ssrv = newsrv(addr);
	if(ssrv == nil)
		sysfatal("fsinit: newsrv: %r");
	if(cons(srv) < 0)
		sysfatal("cons: %r");
	poolc[0] = echancreate(sizeof(Mpool*), Nses);
	poolc[1] = echancreate(sizeof(Mpool*), Nses);
}

void
fssrv(void)
{
	Ses *s;

	while((s = recvp(ssrv->newc)) != nil)
		threadcreate(fssrvproc, s, Stack);
	fprint(consfd, "halt\n");
	threadexitsall(nil);
}

#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <bio.h>

#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "tses.h"
#include "ch.h"
#include "dbg.h"
#include "ix.h"
#include "ixfile.h"

enum
{
	SSID = 42
};

/*
 * Mbuf convention:
 * Msg.io[0] always refers to the message buffer.
 */

typedef struct Fs Fs;
typedef struct Rpc Rpc;
typedef struct Fid Fid;
typedef struct Ses Ses;

struct Ses
{
	QLock;
	Ref;
	char *uname;
	int ssid;
	Ses *next;	/* in avail sessions list */
	Fid **fids;
	int nfids;
	Fid *free;
	void *tag;		/* to remove those not kept */
};

struct Fs
{
	QLock;
	Con *s;		/* transport session (i.e., tcp connection) */
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
	int aborting;
};

struct Rpc
{
	Fs *fs;
	Ch *ch;
	Msg *m;
	Ses *ix;
	Fid *fid;
	IXcall t, r;
	int last;
};

enum
{
	Large = 0,
	Small,
};

static QLock ixslk;
static Ssrv *ssrv;
static Ses *ixs;			/* sessions in server */

static int ssid = SSID;		/* server session id; any number would do */
static Channel *poolc[2];
static int npools[2];
static ulong poolmsz[2] = 
{
	[Large]	Msgsz,
	[Small] Smsgsz,
};

static QLock hugelock;	/* BUG: single lock for the fs */
static int nfids;

static Fid*
newfid(Ses *ix)
{
	Fid *fid;

	qlock(ix);
	nfids++;
	if(ix->free != nil){
		fid = ix->free;
		ix->free = fid->next;
		qunlock(ix);
		return fid;
	}
	if((ix->nfids%Incr) == 0)
		ix->fids = realloc(ix->fids, (ix->nfids+Incr)*sizeof(Fid*));
	fid = mallocz(sizeof *fid, 1);
	ix->fids[ix->nfids] = fid;
	fid->id = ix->nfids++;
	fid->ref = 1;
	fid->omode = -1;
	fid->fd = -1;
	fid->aborting = 0;
	qunlock(ix);
	return fid;
}

static void
clunkfid(Fid *fid)
{
	if(fid->file != nil){
		if(fid->fd != -1)
			closefile(fid->file, fid->fd, fid->aborting);
		putfile(fid->file);
	}
	fid->file = nil;
	fid->fd = -1;
	fid->omode = -1;
}


static void
putfid(Ses *ix, Fid *fid)
{
	assert(ix != nil);
	if(decref(fid) == 0){
		qlock(ix);
		nfids--;
		dfprint("free fid. %d fids in use\n", nfids);
		fid->next = ix->free;
		ix->free = fid;
		clunkfid(fid);
		qunlock(ix);
	}
}

static Fid*
getfid(Ses *ix, int id)
{
	Fid *fid;

	assert(ix != nil);
	qlock(ix);
	if(id < 0 || id >= ix->nfids || ix->fids[id]->file == nil){
		qunlock(ix);
		return nil;
	}
	fid = ix->fids[id];
	incref(fid);
	qunlock(ix);
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

static void
freeses(Ses *ix)
{
	int i;

	for(i = 0; i < ix->nfids; i++)
		free(ix->fids[i]);
	free(ix->fids);
	free(ix->uname);
	free(ix);
}

static Ses*
getses(int ssid)
{
	Ses *ix;

	qlock(&ixslk);
	for(ix = ixs; ix != nil; ix = ix->next)
		if(ix->ssid == ssid){
			incref(ix);
			break;
		}
	qunlock(&ixslk);
	return ix;
}

static void
_putses(Ses *ix)
{
	Ses **s;

	if(ix == nil || decref(ix) > 0)
		return;
	dfprint("putses: free session id %d\n", ix->ssid);
	for(s = &ixs; *s != nil; s = &(*s)->next)
		if(*s == ix){
			*s = ix->next;
			freeses(ix);
			return;
		}
	sysfatal("session not found in putses");
}

static void
putses(Ses *ix)
{
	Ses **s;

	if(ix == nil || decref(ix) > 0)
		return;
	dfprint("putses: free session id %d\n", ix->ssid);
	qlock(&ixslk);
	for(s = &ixs; *s != nil; s = &(*s)->next)
		if(*s == ix){
			*s = ix->next;
			freeses(ix);
			qunlock(&ixslk);
			return;
		}
	sysfatal("session not found in putses");
}

static Ses*
newses(void *tag)
{
	Ses *ix;
	static int ssidgen;

	assert(tag != nil);
	qlock(&ixslk);
	ix = mallocz(sizeof *ix, 1);
	ix->ref = 1;
	ix->next = ixs;
	ix->ssid = ++ssidgen;
	ix->tag = tag;
	ixs = ix;
	qunlock(&ixslk);
	return ix;
}

static void
killses(void *tag)
{
	Ses *ns, *s;

	qlock(&ixslk);
	for(s = ixs; s != nil; s = ns){
		ns = s->next;
		if(s->tag == tag)
			_putses(s);
	}
	qunlock(&ixslk);
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
	 * Rread puts data directly after the packed reply Fscall
	 * and sets io's rp and wp by itself.
	 * The same goes for Rattr.
	 */
	dfprint("%cch%d-> %G\n", rpc->last?'|':'-', rpc->ch->id, &rpc->r);
	if(rpc->r.type == IXRread || rpc->r.type == IXRattr)
		n = ixpack(&rpc->r, io->bp, ixpackedsize(&rpc->r));
	else{
		ioreset(io);
		n = ixpack(&rpc->r, io->wp, IOCAP(io));
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
		if(rpc->r.type == IXRerror)
			rpc->fid->aborting++;
		putfid(rpc->ix, rpc->fid);	/* held by client */
		putfid(rpc->ix, rpc->fid);	/* held by rpc */
		rpc->fid = nil;
	}
	return rc;
}

static int
fserror(Rpc *rpc, char *e)
{
	rpc->r.type = IXRerror;
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
	rpc->r.type = IXRerror;
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
	if(strcmp(rpc->t.version, "3.1415926P") != 0)
		return fserror(rpc, "wrong version");
	rpc->r.version = "3.1415926P";
	return fsreply(rpc);
}

static int
fssession(Rpc *rpc)
{
	Ses *s;

	/* only the owner can set a session by now */
	if(strcmp(rpc->t.uname, getuser()) != 0)
		return fserror(rpc, "bad user in session");

	if(rpc->t.ssid != Nossid)
		s = getses(rpc->t.ssid);
	else{
		s = newses(rpc->fs);	/* use fs as the tag */
		incref(s);		/* it's allocated now until endsession */
	}
	if(rpc->t.keep)
		s->tag = nil;	/* forget thet tag: keep it when done */
	putses(rpc->ix);
	rpc->ix = s;
	if(s == nil)
		return fserror(rpc, "unknown session id");
	rpc->ix->uname = strdup(rpc->t.uname);
	rpc->r.afid = ~0;
	rpc->r.ssid = s->ssid;
	rpc->r.uname = getuser();
	dfprint("fssession: session id %d ref %d\n", s->ssid, s->ref);
	return fsreply(rpc);
}

static int
fssid(Rpc *rpc)
{
	Ses *ix;

	ix = getses(rpc->t.ssid);
	putses(rpc->ix);
	rpc->ix = ix;
	if(ix == nil)
		return fserror(rpc, "unknown session id");
	return fsreply(rpc);
}

static int
fsendsession(Rpc *rpc)
{
	Ses *ix;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	putses(rpc->ix);
	ix = rpc->ix;
	rpc->ix = nil;

	putses(ix);	/* should destroy it when unused */
	return fsreply(rpc);
}

static int
fsfid(Rpc *rpc)
{
	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	if(rpc->fid != nil)
		putfid(rpc->ix, rpc->fid);
	rpc->fid = getfid(rpc->ix, rpc->t.fid);
	if(rpc->fid == nil)
		return fserror(rpc, "no such fid");
	rpc->fid->cflags = rpc->t.cflags;
	return fsreply(rpc);
}

static int
fsattach(Rpc *rpc)
{
	if(rpc->ix == nil)
		return fserror(rpc, "session not set");

	/*
	 * rpc->t.aname ignored
	 */
	rpc->fid = newfid(rpc->ix);
	incref(rpc->fid);		/* client's reference */
	rpc->r.fid = rpc->fid->id;
	rpc->fid->file = rootfile();
	return fsreply(rpc);
}

static int
fsclone(Rpc *rpc)
{
	Fid *nfid;
	Fid *fid;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	nfid = newfid(rpc->ix);	/* ref held by client */
	rpc->r.fid = nfid->id;
	nfid->omode = fid->omode;
	nfid->file = fid->file;
	increffile(nfid->file);
	putfid(rpc->ix, fid);		/* held by rpc */
	rpc->fid = nfid;
	nfid->cflags = rpc->t.cflags;
	incref(nfid);		/* held by rpc */
	return fsreply(rpc);
}

static int
fswalk(Rpc *rpc)
{
	Fid *fid;
	int i;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode != -1)
		return fserror(rpc, "can't walk an open fid");
	/*
	 * NB: This differs from 9p. A walk succeeds only if all
	 * names can be walked.
	 */
	for(i = 0; i < rpc->t.nwname; i++)
		if(walkfile(&fid->file, rpc->t.wname[i]) < 0)
			return fssyserror(rpc);
	return fsreply(rpc);
}

static int
fsopen(Rpc *rpc)
{
	Fid *fid;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode != -1)
		return fserror(rpc, "fid already open");
	fid->fd = openfile(fid->file, rpc->t.mode);
	if(fid->fd < 0)
		return fssyserror(rpc);
	fid->omode = rpc->t.mode;
	return fsreply(rpc);
}

static int
fscreate(Rpc *rpc)
{
	int fd;
	Fid *fid;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
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
	return fsreply(rpc);
}

static int
fsread(Rpc *rpc)
{
	long nr;
	Io *io;
	Fid *fid;
	int i, saved, last;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
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
		io->wp += ixpackedsize(&rpc->r);
		if(nr < 0 || nr > IOCAP(io))
			nr = IOCAP(io);
		if(IOLEN(io)+nr > rpc->fs->msize)
			nr = rpc->fs->msize - IOLEN(io);
		rpc->r.data = io->wp;
		nr = preadfile(fid->file, fid->fd, io->wp, nr, rpc->t.offset);
		if(nr < 0)
			return fssyserror(rpc);
		io->wp += nr;
		rpc->t.offset += nr;

		rpc->r.count = nr;
		saved = rpc->last;
		rpc->last = last = (saved && (i == rpc->t.nmsg || nr == 0));
		if(fsreply(rpc) < 0){
			rpc->last = saved;
			return -1;
		}
		rpc->last = saved;
		if(last || nr == 0)
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
	IXcall *t;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode < 0 || (fid->omode&3) == OREAD)
		return fserror(rpc, "fid not open for writing");
	t = &rpc->t;
	nw = pwritefile(fid->file, fid->fd, t->data, t->count, &t->offset, t->offset+t->count);
	if(nw < 0)
		return fssyserror(rpc);
	rpc->r.count = nw;
	rpc->r.offset = t->offset;
	return fsreply(rpc);
}

static int
fsclunk(Rpc *rpc)
{
	Fid * fid;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	fid->aborting++;
	putfid(rpc->ix, fid);	/* in session */
	putfid(rpc->ix, fid);	/* in rpc */
	rpc->fid = nil;
	return fsreply(rpc);
}

static int
fsclose(Rpc *rpc)
{
	Fid * fid;
	int isopen;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	isopen = fid->omode >= 0;
	assert(fid->aborting == 0);
	putfid(rpc->ix, fid);	/* in session */
	putfid(rpc->ix, fid);	/* in rpc */
	rpc->fid = nil;
	if(!isopen)
		return fserror(rpc, "fid not open");
	return fsreply(rpc);
}

static int
fsremove(Rpc *rpc)
{
	Fid * fid;
	int r;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(fid->omode >= 0){
		closefile(fid->file, fid->fd, fid->aborting);
		fid->fd = -1;
		fid->omode = -1;
	}
	putfid(rpc->ix, fid);	/* held by client */

	/* file goes when rpc->fid reference is released later */
	r = removefile(fid->file);

	putfid(rpc->ix, fid);	/* held by rpc */
	rpc->fid = nil;
	if(r < 0)
		return fssyserror(rpc);
	else
		return fsreply(rpc);
}

static int
fsattr(Rpc *rpc)
{
	Fid *fid;
	Io *io;
	long nr;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	/*
	 * Get a large buffer; we are probably using a small one.
	 */
	freemsg(rpc->m);
	rpc->m = newmsg(rpc->fs->s->pool);
	io = &rpc->m->io[0];
	rpc->m->hdr = nil;
	ioreset(io);
	io->wp += ixpackedsize(&rpc->r);
	rpc->r.value = io->wp;
	nr = fileattr(fid->file, rpc->t.attr, rpc->r.value, IOCAP(io));
	if(nr < 0)
		return fssyserror(rpc);
	rpc->r.nvalue = nr;
	io->wp += nr;
	return fsreply(rpc);
}

static int
fswattr(Rpc *rpc)
{
	Fid *fid;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	if(filewattr(fid->file, rpc->t.attr, rpc->t.value, IOLEN(rpc->m->io)) < 0)
		return fssyserror(rpc);
	return fsreply(rpc);
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
	char buf[128];	/* bug: fixes stat size */
	char buf2[128];
	Msg *m;
	long n;

	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	fid = rpc->fid;
	if(fid == nil)
		return fserror(rpc, "fid not set");
	m = rpc->m;
	if(IOLEN(m->io) >= sizeof buf - 1)
		return fserror(rpc, "fscond: bug: attr value too large");
	memmove(buf, m->io->rp, IOLEN(m->io));
	buf[IOLEN(m->io)] = 0;
	n = fileattr(fid->file, rpc->t.attr, (uchar*)buf2, sizeof buf2 - 1);
	if(n < 0)
		return fssyserror(rpc);
	buf[n] = 0;
	if(cmpstr(buf, buf2, rpc->t.op) == 0)
		return fserror(rpc, "false");
	return fsreply(rpc);
}

static int
fsmove(Rpc *rpc)
{
	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	return fserror(rpc, "move not supported");
}

static int
fscopy(Rpc *rpc)
{
	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	return fserror(rpc, "copy not supported");
}

static int
fsflush(Rpc *rpc)
{
	if(rpc->ix == nil)
		return fserror(rpc, "session not set");
	return fserror(rpc, "abort the channel for flushing");
}

typedef int (*Rpcfn)(Rpc*);

static Rpcfn reqfn[Tmax] = {
[IXTversion]	fsversion,
[IXTsession]	fssession,
[IXTsid]		fssid,
[IXTendsession]	fsendsession,
[IXTfid]		fsfid,
[IXTattach]	fsattach,
[IXTclone]	fsclone,
[IXTwalk]	fswalk,
[IXTopen]	fsopen,
[IXTcreate]	fscreate,
[IXTread]	fsread,
[IXTwrite]	fswrite,
[IXTclunk]	fsclunk,
[IXTclose]	fsclose,
[IXTremove]	fsremove,
[IXTattr]	fsattr,
[IXTwattr]	fswattr,
[IXTcond]	fscond,
[IXTmove]	fsmove,
[IXTcopy]	fscopy,
[IXTflush]	fsflush,
};

static int
fsrpc(Rpc *rpc)
{
	Msg *m;
	int r, n;

	m = rpc->m;
	if(IOLEN(&m->io[0]) == 0){
		dfprint("ch%d: eof\n", rpc->ch->id);
		goto Fail;
	}
	n = ixunpack(m->io[0].rp, IOLEN(&m->io[0]), &rpc->t);
	if(n <= 0){
		dfprint("ch%d: unknown message: %r\n", rpc->ch->id);
		goto Fail;
	}
	m->io->rp += n;
	switch(rpc->t.type){
	case IXTwrite:
		rpc->t.count = IOLEN(m->io);
		break;
	case IXTwattr:
	case IXTcond:
		rpc->t.nvalue = IOLEN(m->io);
		break;
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
	threadsetname("fsrpcproc fs %p ch %p", rpc.fs, rpc.ch);
	memset(&rpc, 0, sizeof rpc);
	rpc.fid = nil;
	rpc.fs = recvp(c);
	rpc.ch = recvp(c);
	id = rpc.ch->id;
	dfprint("fsrpc[%p!%d]: new chan %s\n", rpc.fs->s, id, rpc.fs->s->addr);
	while((rpc.m = chrecv(rpc.ch, &rpc.last)) != nil){
		qlock(&hugelock);
		waslast = rpc.last;
		if(fsrpc(&rpc) < 0 && !waslast){
			drainch(rpc.ch);
			break;
		}
		if(rpc.last)
			break;
		qunlock(&hugelock);
	}
	if(rpc.fid != nil)
		putfid(rpc.ix, rpc.fid);
	if(rpc.ix != nil)
		putses(rpc.ix);
	qunlock(&hugelock);
	dfprint("fsrpc[%p!%d]: done.\n", rpc.fs->s, id);
	dfprint("%d fids in use; %d files in use\n", nfids, ixnfiles);
	threadexits(nil);
}

static void
fssrvproc(void *a)
{
	Fs fs;
	Ch *ch;

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
	/* mux released by itself */
	closemux(fs.cm);
	chanfree(fs.argc);
	killses(&fs);
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
		if(strcmp(cmd, "halt") == 0){
			free(cmd);
			break;
		}else
			fprint(fd, "%s?\n", cmd);
		free(cmd);
	}
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

	fn = smprint("/srv/%s", srv);
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
ixinit(char *addr, char *srv)
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
ixsrv(void)
{
	Con *s;

	while((s = recvp(ssrv->newc)) != nil)
		threadcreate(fssrvproc, s, Stack);
	fprint(consfd, "halt\n");
	threadexitsall(nil);
}

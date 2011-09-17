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

static int
sendreq(Ch *ch, Fscall *t, Msg *m, int last)
{
	int n;

	/*
	 * Twrite reads data directly after the packed T call,
	 * and sets io's rp and wp by itself.
	 * Requests using strings like Tattach, Twalk, may not
	 * fit in a small Msg if strings are large, in which case
	 * we try using a large buffer before failing.
	 */
	dfprint("%cch%d-> %G\n", last?'|':'-', ch->id, t);
	if(t->type != Twrite){
		n = pack(t, m->io->wp, IOCAP(m->io));
		if(n < 0 && IOCAP(m->io) < Msgsz){
			/* try with a large buffer */
			freemsg(m);
			m = newmsg(pool);
			n = pack(t, m->io->wp, IOCAP(m->io));
		}
	}else
		n = pack(t, m->io->bp, packedsize(t));
	if(n <= 0)
		sysfatal("sendreq: pack");
	if(t->type != Twrite)
		m->io->wp += n;
	return chsend(ch, m, last);
}


int
xtversion(Ch *ch, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tversion;
	t.msize = Msgsz;
	t.version = "ix";
	return sendreq(ch, &t, m, last);
}

int
xtattach(Ch *ch, char *uname, char *aname, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tattach;
	t.afid = -1;
	t.uname = uname;
	t.aname = aname;
	return sendreq(ch, &t, m, last);
}

int
xtfid(Ch *ch, int fid, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tfid;
	t.fid = fid;
	t.cflags = 0;
	return sendreq(ch, &t, m, last);
}

int
xtclone(Ch *ch, int cflags, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tclone;
	t.cflags = cflags;
	return sendreq(ch, &t, m, last);
}

int
xtclunk(Ch *ch, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tclunk;
	return sendreq(ch, &t, m, last);
}

int
xtremove(Ch *ch, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tremove;
	return sendreq(ch, &t, m, last);
}

int
xtwalk(Ch *ch, char *elem, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Twalk;
	t.wname = elem;
	return sendreq(ch, &t, m, last);
}


int
xtstat(Ch *ch, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tstat;
	return sendreq(ch, &t, m, last);
}

int
xtopen(Ch *ch, int mode, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Topen;
	t.mode = mode;
	return sendreq(ch, &t, m, last);
}

int
xtread(Ch *ch, long count, uvlong offset, long msz, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tread;
	t.offset = offset;
	t.count = count;
	if(msz != 0 && t.count > msz)
		t.count = msz;
	t.nmsg = -1;
	return sendreq(ch, &t, m, last);
}

int
xtcreate(Ch *ch, char *name, int mode, int perm, int last)
{
	Fscall t;
	Msg *m;

	m = newmsg(spool);
	t.type = Tcreate;
	t.name = name;
	t.perm = perm;
	t.mode = mode;
	return sendreq(ch, &t, m, last);
}

int
xtwrite(Ch *ch, Msg *m, long count, uvlong offset, int last)
{
	Fscall t;

	t.type = Twrite;
	t.offset = offset;
	t.count = count;
	return sendreq(ch, &t, m, last);
}

int
xtcond(Ch *ch, int op, Dir *d, int last)
{
	Fscall t;
	Msg *m;
	uchar buf[512];

	m = newmsg(pool);
	t.type = Tcond;
	t.cond = op;
	t.nstat = convD2M(d, buf, sizeof buf);
	t.stat = buf;
	return sendreq(ch, &t, m, last);
}

int
xtwstat(Ch *ch, Dir *d, int last)
{
	Fscall t;
	Msg *m;
	uchar buf[512];

	m = newmsg(pool);
	t.type = Twstat;
	t.nstat = convD2M(d, buf, sizeof buf);
	t.stat = buf;
	return sendreq(ch, &t, m, last);
}

static Msg*
getreply(Ch *ch, int type, Fscall *r)
{
	Msg *m;
	int last, id, n;

	id = ch->id;		/* ch is released if last */
	m = chrecv(ch, &last);
	if(m == nil){
		werrstr("eof");
		dfprint("getreply: %r\n");
		return nil;
	}
	n = unpack(m->io->rp, IOLEN(m->io), r);
	if(n <= 0){
		werrstr("getreply: rp %#p wp %#p ep %#p %r", m->io->rp, m->io->wp, m->io->ep);
		dfprint("getreply: %r\n");
		freemsg(m);
		return nil;
	}
	m->io->rp += n;
	dfprint("<-ch%d%c %G\n", id, last?'|':'-', r);
	if(r->type == type)
		return m;
	if(r->type == Rerror)
		werrstr("%s", r->ename);
	else
		werrstr("wrong reply type %d", r->type);
	freemsg(m);
	return nil;
}

int
xrversion(Ch *ch, ulong *mszp)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rversion, &r);
	if(m == nil)
		return -1;
	if(strcmp(r.version, "ix") != 0){
		werrstr("wrong version %s", r.version);
		freemsg(m);
		return -1;
	}
	if(mszp != nil && (*mszp == 0 || r.msize < *mszp)){
		*mszp = r.msize;
		dfprint("msize %uld\n", *mszp);
	}
	freemsg(m);
	return 0;
}

int
xrattach(Ch *ch, int *fidp)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rattach, &r);
	if(m == nil)
		return -1;
	*fidp = r.fid;
	freemsg(m);
	return 0;
}

int
xrfid(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rfid, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
xrclunk(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rclunk, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
xrremove(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rremove, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
xrclone(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rclone, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return r.fid;
}

int
xrwalk(Ch *ch, Qid *qp)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rwalk, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	if(qp != nil)
		*qp = r.wqid;
	return 0;
}

int
xropen(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Ropen, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
xrstat(Ch *ch, Dir *d, char buf[])
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rstat, &r);
	if(m == nil)
		return -1;
	if(convM2D(r.stat, r.nstat, d, buf) <= BIT32SZ)
		return -1;
	freemsg(m);
	return 0;
}


Msg*
xrread(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rread, &r);
	if(m == nil)
		return nil;
	m->io->rp -= r.count;
	return m;
}

int
xrcreate(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rcreate, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

long
xrwrite(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rwrite, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return r.count;
}

int
xrcond(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rcond, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
xrwstat(Ch *ch)
{
	Msg *m;
	Fscall r;

	m = getreply(ch, Rwstat, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}




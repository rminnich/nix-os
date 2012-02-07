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

static int
sendreq(Ch *ch, IXcall *t, Msg *m, int last)
{
	int n;

	/*
	 * Twrite and Twattr read data directly after the packed T call,
	 * and set io's rp and wp by themselves.
	 * Requests using strings like Tattach, Twalk, may not
	 * fit in a small Msg if strings are large, in which case
	 * we try using a large buffer before failing.
	 */
	dfprint("%cch%d-> %G\n", last?'|':'-', ch->id, t);
	if(t->type != IXTwrite && t->type != IXTwattr && t->type != IXTcond){
		n = ixpack(t, m->io->wp, IOCAP(m->io));
		if(n < 0 && IOCAP(m->io) < Msgsz){
			/* try with a large buffer */
			freemsg(m);
			m = newmsg(pool);
			n = ixpack(t, m->io->wp, IOCAP(m->io));
		}
	}else
		n = ixpack(t, m->io->bp, ixpackedsize(t));
	if(n <= 0)
		sysfatal("sendreq: pack");
	if(t->type != IXTwrite && t->type != IXTwattr && t->type != IXTcond)
		m->io->wp += n;
	return chsend(ch, m, last);
}


int
ixtversion(Ch *ch, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTversion;
	t.msize = Msgsz;
	t.version = "3.1415926P";
	return sendreq(ch, &t, m, last);
}

int
ixtsession(Ch *ch, int ssid, char *u, int keep, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTsession;
	t.ssid = ssid;
	t.uname = u;
	t.keep = keep;
	return sendreq(ch, &t, m, last);
}

int
ixtsid(Ch *ch, int ssid, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTsid;
	t.ssid = ssid;
	return sendreq(ch, &t, m, last);
}

static int
ixtmsg(Ch *ch, int type, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = type;
	return sendreq(ch, &t, m, last);
}

int
ixtendsession(Ch *ch, int last)
{
	return ixtmsg(ch, IXTendsession, last);
}

int
ixtfid(Ch *ch, int fid, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTfid;
	t.fid = fid;
	return sendreq(ch, &t, m, last);
}

int
ixtattach(Ch *ch, char *aname, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTattach;
	t.aname = aname;
	return sendreq(ch, &t, m, last);
}

int
ixtclone(Ch *ch, int cflags, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTclone;
	t.cflags = cflags;
	return sendreq(ch, &t, m, last);
}

int
ixtwalk(Ch *ch, int nel, char **elem, int last)
{
	IXcall t;
	Msg *m;
	int i;

	m = newmsg(spool);
	t.type = IXTwalk;
	t.nwname = nel;
	if(nel > Nwalks)
		sysfatal("ixtwalk: bug: too many elems");

	for(i = 0; i < nel; i++)
		t.wname[i] = elem[i];
	return sendreq(ch, &t, m, last);
}

int
ixtopen(Ch *ch, int mode, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTopen;
	t.mode = mode;
	return sendreq(ch, &t, m, last);
}

int
ixtcreate(Ch *ch, char *name, int mode, int perm, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTcreate;
	t.name = name;
	t.perm = perm;
	t.mode = mode;
	return sendreq(ch, &t, m, last);
}

int
ixtread(Ch *ch, int nmsg, long count, uvlong offset, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTread;
	t.nmsg = nmsg;
	t.count = count;
	t.offset = offset;
	return sendreq(ch, &t, m, last);
}

int
ixtwrite(Ch *ch, Msg *m, long count, uvlong offset, uvlong endoffset, int last)
{
	IXcall t;

	t.type = IXTwrite;
	t.offset = offset;
	t.endoffset = endoffset;
	t.count = count;
	t.data = m->io->bp + ixpackedsize(&t);	/* so fmt works */
	return sendreq(ch, &t, m, last);
}

int
ixtclunk(Ch *ch, int last)
{
	return ixtmsg(ch, IXTclunk, last);
}

int
ixtclose(Ch *ch, int last)
{
	return ixtmsg(ch, IXTclose, last);
}

int
ixtremove(Ch *ch, int last)
{
	return ixtmsg(ch, IXTremove, last);
}

int
ixtattr(Ch *ch, char *attr, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTattr;
	t.attr = attr;
	return sendreq(ch, &t, m, last);
}

int
ixtwattr(Ch *ch, char *attr, void *data, long nvalue, int last)
{
	IXcall t;
	Msg *m;
	uchar *value;

	value = data;
	m = newmsg(pool);
	t.type = IXTwattr;
	t.attr = attr;
	t.nvalue = nvalue;
	m->io->wp += ixpackedsize(&t);
	t.value = m->io->wp;			/* for fmt */
	memmove(m->io->wp, value, nvalue);
	m->io->wp += nvalue;
	return sendreq(ch, &t, m, last);
}

int
ixtcond(Ch *ch, Msg *m, int op, char *attr, long nvalue, int last)
{
	IXcall t;

	t.type = IXTcond;
	t.op = op;
	t.attr = attr;
	t.nvalue = nvalue;
	return sendreq(ch, &t, m, last);
}

int
ixtmove(Ch *ch, int dirfid, char *newname, int last)
{
	IXcall t;
	Msg *m;

	m = newmsg(spool);
	t.type = IXTmove;
	t.dirfid = dirfid;
	t.newname = newname;
	return sendreq(ch, &t, m, last);
}

static Msg*
getreply(Ch *ch, int type, IXcall *r)
{
	Msg *m;
	int last, id, n;

Again:
	id = ch->id;		/* ch is released if last */
	m = chrecv(ch, &last);
	if(m == nil){
		werrstr("eof");
		dfprint("getreply: %r\n");
		return nil;
	}
	n = ixunpack(m->io->rp, IOLEN(m->io), r);
	if(n <= 0){
		werrstr("getreply: rp %#p wp %#p ep %#p %r", m->io->rp, m->io->wp, m->io->ep);
		dfprint("getreply: %r\n");
		freemsg(m);
		return nil;
	}
	m->io->rp += n;

	switch(r->type){	/* needed just for printing the reply */
	case IXRread:
	case IXTwrite:
		r->count = IOLEN(m->io);
		break;
	case IXRattr:
	case IXTwattr:
	case IXTcond:
		r->nvalue = IOLEN(m->io);
		break;
	}

	dfprint("<-ch%d%c %G\n", id, last?'|':'-', r);
	if(r->type == type)
		return m;
	if(r->type == IXRerror)
		werrstr("%s", r->ename);
	else{
		werrstr("wrong reply type %d", r->type);
		/*
		 * we could fail always here because we got an unexpected reply,
		 * but, this permits issuing requests, and then check the
		 * status on a later request without having to call the
		 * receive function for each request unless we are really
		 * interested in the reply.
		 */
		if(!last){
			freemsg(m);
			goto Again;
		}
	}
	freemsg(m);
	return nil;
}

int
ixrversion(Ch *ch, ulong *mszp)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRversion, &r);
	if(m == nil)
		return -1;
	if(strcmp(r.version, "3.1415926P") != 0){
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
ixrsession(Ch *ch, int *ssid, int *afid, char **u)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRsession, &r);
	if(m == nil)
		return -1;
	if(ssid != nil)
		*ssid = r.ssid;
	if(afid != nil)
		*afid = r.afid;
	if(u != nil)
		*u = strdup(r.uname);
	freemsg(m);
	return 0;
}

static int
ixrmsg(Ch *ch, int type)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, type, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	return 0;
}

int
ixrsid(Ch *ch)
{
	return ixrmsg(ch, IXRsid);
}

int
ixrendsession(Ch *ch)
{
	return ixrmsg(ch, IXRendsession);
}

int
ixrfid(Ch *ch)
{
	return ixrmsg(ch, IXRfid);
}

int
ixrattach(Ch *ch, int *fidp)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRattach, &r);
	if(m == nil)
		return -1;
	if(fidp != nil)
		*fidp = r.fid;
	freemsg(m);
	return 0;
}

int
ixrclone(Ch *ch, int *fidp)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRclone, &r);
	if(m == nil)
		return -1;
	freemsg(m);
	if(fidp != nil)
		*fidp = r.fid;
	return 0;
}

int
ixrwalk(Ch *ch)
{
	return ixrmsg(ch, IXRwalk);
}

int
ixropen(Ch *ch)
{
	return ixrmsg(ch, IXRopen);
}

int
ixrcreate(Ch *ch)
{
	return ixrmsg(ch, IXRcreate);
}

Msg*
ixrread(Ch *ch)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRread, &r);
	if(m == nil)
		return nil;
	r.count = IOLEN(m->io);	/* unused */
	return m;
}

long
ixrwrite(Ch *ch, uvlong *offsetp)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRwrite, &r);
	if(m == nil)
		return -1;
	if(offsetp)
		*offsetp = r.offset;
	freemsg(m);
	return r.count;
}

int
ixrclunk(Ch *ch)
{
	return ixrmsg(ch, IXRclunk);
}

int
ixrclose(Ch *ch)
{
	return ixrmsg(ch, IXRclose);
}

int
ixrremove(Ch *ch)
{
	return ixrmsg(ch, IXRremove);
}

Msg*
ixrattr(Ch *ch)
{
	Msg *m;
	IXcall r;

	m = getreply(ch, IXRattr, &r);
	if(m == nil)
		return nil;
	r.nvalue = IOLEN(m->io);	/* unused */
	return m;
}

int
ixrwattr(Ch *ch)
{
	return ixrmsg(ch, IXRwattr);
}

int
ixrcond(Ch *ch)
{
	return ixrmsg(ch, IXRcond);
}

int
ixrmove(Ch *ch)
{
	return ixrmsg(ch, IXRmove);
}

/*
 * NB: Tflush, Rflush are done by aborting the channel
 * in this implementation.
 */

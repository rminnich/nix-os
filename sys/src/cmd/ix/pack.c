#include	<u.h>
#include	<libc.h>
#include	<fcall.h>

#include "fs.h"

/*
 * This is convS2M, convM2S from libc, but:
 * - without copying bytes in the case of packing a Rread or Twrite request.
 * - Tattach as no fid, Rattach has a fid.
 * - Twalk has an uchar (bool) to ask for a newfid, and has only wname
 * - Rwalk has newfid and only a wqid.
 */


static
uchar*
pstring(uchar *p, char *s)
{
	uint n;

	if(s == nil){
		PBIT16(p, 0);
		p += BIT16SZ;
		return p;
	}

	n = strlen(s);
	/*
	 * We are moving the string before the length,
	 * so you can S2M a struct into an existing message
	 */
	memmove(p + BIT16SZ, s, n);
	PBIT16(p, n);
	p += n + BIT16SZ;
	return p;
}

static
uchar*
pqid(uchar *p, Qid *q)
{
	PBIT8(p, q->type);
	p += BIT8SZ;
	PBIT32(p, q->vers);
	p += BIT32SZ;
	PBIT64(p, q->path);
	p += BIT64SZ;
	return p;
}

static
uint
stringsz(char *s)
{
	if(s == nil)
		return BIT16SZ;

	return BIT16SZ+strlen(s);
}

uint
packedsize(Fscall *f)
{
	uint n;

	n = 0;
	n += BIT8SZ;	/* type */

	switch(f->type)
	{
	case Tcond:
		n += BIT8SZ;
		n += BIT16SZ;
		n += f->nstat;
		break;

	case Tfid:
		n += BIT32SZ;
		n += BIT8SZ;
		break;

	case Tclone:
		n += BIT8SZ;
		break;

	case Tversion:
		n += BIT32SZ;
		n += stringsz(f->version);
		break;

	case Tauth:
		n += BIT32SZ;
		n += stringsz(f->uname);
		n += stringsz(f->aname);
		break;

	case Tattach:
		n += BIT32SZ;
		n += stringsz(f->uname);
		n += stringsz(f->aname);
		break;

	case Twalk:
		n += stringsz(f->wname);
		break;

	case Topen:
		n += BIT8SZ;
		break;

	case Tcreate:
		n += stringsz(f->name);
		n += BIT32SZ;
		n += BIT8SZ;
		break;

	case Tread:
		n += BIT32SZ;
		n += BIT64SZ;
		n += BIT32SZ;
		break;

	case Twrite:
		n += BIT64SZ;
		n += BIT32SZ;
		/* n += f->count; */
		break;

	case Tclunk:
	case Tremove:
	case Tstat:
		break;

	case Twstat:
		n += BIT16SZ;
		n += f->nstat;
		break;
/*
 */
	case Rcond:
	case Rfid:
		break;

	case Rclone:
		n += BIT32SZ;
		break;

	case Rversion:
		n += BIT32SZ;
		n += stringsz(f->version);
		break;

	case Rerror:
		n += stringsz(f->ename);
		break;

	case Rauth:
		n += QIDSZ;
		break;

	case Rattach:
		n += BIT32SZ;
		n += QIDSZ;
		break;

	case Rwalk:
		n += QIDSZ;
		break;

	case Ropen:
	case Rcreate:
		n += QIDSZ;
		n += BIT32SZ;
		break;

	case Rread:
		n += BIT32SZ;
		/* n += f->count; */
		break;

	case Rwrite:
		n += BIT32SZ;
		break;

	case Rclunk:
	case Rremove:
		break;

	case Rstat:
		n += BIT16SZ;
		n += f->nstat;
		break;

	case Rwstat:
		break;

	default:
		sysfatal("packedsize: type %d", f->type);

	}
	return n;
}

uint
pack(Fscall *f, uchar *ap, uint nap)
{
	uchar *p;
	uint size;

	size = packedsize(f);
	if(size == 0)
		return 0;
	if(size > nap)
		return 0;

	p = (uchar*)ap;

	PBIT8(p, f->type);
	p += BIT8SZ;

	switch(f->type)
	{
	case Tcond:
		PBIT8(p, f->cond);
		p += BIT8SZ;
		PBIT16(p, f->nstat);
		p += BIT16SZ;
		memmove(p, f->stat, f->nstat);
		p += f->nstat;
		break;

	case Tfid:
		PBIT32(p, f->fid);
		p += BIT32SZ;
		PBIT8(p, f->cflags);
		p += BIT8SZ;
		break;

	case Tclone:
		PBIT8(p, f->cflags);
		p += BIT8SZ;
		break;

	case Tversion:
		PBIT32(p, f->msize);
		p += BIT32SZ;
		p = pstring(p, f->version);
		break;

	case Tauth:
		PBIT32(p, f->afid);
		p += BIT32SZ;
		p  = pstring(p, f->uname);
		p  = pstring(p, f->aname);
		break;

	case Tattach:
		PBIT32(p, f->afid);
		p += BIT32SZ;
		p  = pstring(p, f->uname);
		p  = pstring(p, f->aname);
		break;

	case Twalk:
		p = pstring(p, f->wname);
		break;

	case Topen:
		PBIT8(p, f->mode);
		p += BIT8SZ;
		break;

	case Tcreate:
		p = pstring(p, f->name);
		PBIT32(p, f->perm);
		p += BIT32SZ;
		PBIT8(p, f->mode);
		p += BIT8SZ;
		break;

	case Tread:
		PBIT32(p, f->nmsg);
		p += BIT32SZ;
		PBIT64(p, f->offset);
		p += BIT64SZ;
		PBIT32(p, f->count);
		p += BIT32SZ;
		break;

	case Twrite:
		PBIT64(p, f->offset);
		p += BIT64SZ;
		PBIT32(p, f->count);
		p += BIT32SZ;
		/*
		 * Data added by the caller.
		 * memmove(p, f->data, f->count);
		 * p += f->count;
		 */
		break;

	case Tclunk:
	case Tremove:
	case Tstat:
		break;

	case Twstat:
		PBIT16(p, f->nstat);
		p += BIT16SZ;
		memmove(p, f->stat, f->nstat);
		p += f->nstat;
		break;
/*
 * replies
 */

	case Rcond:
	case Rfid:
		break;

	case Rclone:
		PBIT32(p, f->newfid);
		p += BIT32SZ;
		break;

	case Rversion:
		PBIT32(p, f->msize);
		p += BIT32SZ;
		p = pstring(p, f->version);
		break;

	case Rerror:
		p = pstring(p, f->ename);
		break;

	case Rauth:
		p = pqid(p, &f->aqid);
		break;

	case Rattach:
		PBIT32(p, f->fid);
		p += BIT32SZ;
		p = pqid(p, &f->qid);
		break;

	case Rwalk:
		p = pqid(p, &f->wqid);
		break;

	case Ropen:
	case Rcreate:
		p = pqid(p, &f->qid);
		PBIT32(p, f->iounit);
		p += BIT32SZ;
		break;

	case Rread:
		PBIT32(p, f->count);
		p += BIT32SZ;
		/*
		 * data is added by the caller.
		 * memmove(p, f->data, f->count);
		 * p += f->count;
		 */
		break;

	case Rwrite:
		PBIT32(p, f->count);
		p += BIT32SZ;
		break;

	case Rclunk:
	case Rremove:
		break;

	case Rstat:
		PBIT16(p, f->nstat);
		p += BIT16SZ;
		memmove(p, f->stat, f->nstat);
		p += f->nstat;
		break;

	case Rwstat:
		break;

	default:
		sysfatal("pack: type %d", f->type);

	}
	if(size != p-ap)
		return 0;
	return size;
}

static
uchar*
gstring(uchar *p, uchar *ep, char **s)
{
	uint n;

	if(p+BIT16SZ > ep)
		return nil;
	n = GBIT16(p);
	p += BIT16SZ - 1;
	if(p+n+1 > ep)
		return nil;
	/* move it down, on top of count, to make room for '\0' */
	memmove(p, p + 1, n);
	p[n] = '\0';
	*s = (char*)p;
	p += n+1;
	return p;
}

static
uchar*
gqid(uchar *p, uchar *ep, Qid *q)
{
	if(p+QIDSZ > ep)
		return nil;
	q->type = GBIT8(p);
	p += BIT8SZ;
	q->vers = GBIT32(p);
	p += BIT32SZ;
	q->path = GBIT64(p);
	p += BIT64SZ;
	return p;
}

/*
 * no syntactic checks.
 * three causes for error:
 *  1. message size field is incorrect
 *  2. input buffer too short for its own data (counts too long, etc.)
 *  3. too many names or qids
 * gqid() and gstring() return nil if they would reach beyond buffer.
 * main switch statement checks range and also can fall through
 * to test at end of routine.
 */
uint
unpack(uchar *ap, uint nap, Fscall *f)
{
	uchar *p, *ep;

	p = ap;
	ep = p + nap;

	if(p+BIT8SZ > ep){
		werrstr("msg too short");
		return 0;
	}

	f->type = GBIT8(p);
	p += BIT8SZ;

	switch(f->type)
	{
	default:
		werrstr("unknown type %d", f->type);
		return 0;

	case Tcond:
		if(p+BIT8SZ+BIT16SZ > ep)
			return 0;
		f->cond = GBIT8(p);
		p += BIT8SZ;
		f->nstat = GBIT16(p);
		p += BIT16SZ;
		if(p+f->nstat > ep)
			return 0;
		f->stat = p;
		p += f->nstat;
		break;

	case Tfid:
		if(p+BIT32SZ+BIT8SZ > ep)
			return 0;
		f->fid = GBIT32(p);
		p += BIT32SZ;
		f->cflags = GBIT8(p);
		p += BIT8SZ;
		break;

	case Tclone:
		if(p+BIT8SZ > ep)
			return 0;
		f->cflags = GBIT8(p);
		p += BIT8SZ;
		break;

	case Tversion:
		if(p+BIT32SZ > ep)
			return 0;
		f->msize = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->version);
		break;

	case Tauth:
		if(p+BIT32SZ > ep)
			return 0;
		f->afid = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->uname);
		if(p == nil)
			break;
		p = gstring(p, ep, &f->aname);
		if(p == nil)
			break;
		break;

	case Tattach:
		if(p+BIT32SZ > ep)
			return 0;
		f->afid = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->uname);
		if(p == nil)
			break;
		p = gstring(p, ep, &f->aname);
		if(p == nil)
			break;
		break;

	case Twalk:
		p = gstring(p, ep, &f->wname);
		if(p == nil)
			break;
		break;

	case Topen:
		if(p+BIT8SZ > ep)
			return 0;
		f->mode = GBIT8(p);
		p += BIT8SZ;
		break;

	case Tcreate:
		p = gstring(p, ep, &f->name);
		if(p == nil)
			break;
		if(p+BIT32SZ+BIT8SZ > ep)
			return 0;
		f->perm = GBIT32(p);
		p += BIT32SZ;
		f->mode = GBIT8(p);
		p += BIT8SZ;
		break;

	case Tread:
		if(p+BIT32SZ+BIT64SZ+BIT32SZ > ep)
			return 0;
		f->nmsg = GBIT32(p);
		p += BIT32SZ;
		f->offset = GBIT64(p);
		p += BIT64SZ;
		f->count = GBIT32(p);
		p += BIT32SZ;
		break;

	case Twrite:
		if(p+BIT64SZ+BIT32SZ > ep)
			return 0;
		f->offset = GBIT64(p);
		p += BIT64SZ;
		f->count = GBIT32(p);
		p += BIT32SZ;
		if(p+f->count > ep)
			return 0;
		f->data = (char*)p;
		p += f->count;
		break;

	case Tclunk:
	case Tremove:
	case Tstat:
		break;

	case Twstat:
		if(p+BIT16SZ > ep)
			return 0;
		f->nstat = GBIT16(p);
		p += BIT16SZ;
		if(p+f->nstat > ep)
			return 0;
		f->stat = p;
		p += f->nstat;
		break;

/*
 */
	case Rfid:
	case Rcond:
		break;

	case Rclone:
		if(p+BIT32SZ > ep)
			return 0;
		f->newfid = GBIT32(p);
		p += BIT32SZ;
		break;

	case Rversion:
		if(p+BIT32SZ > ep)
			return 0;
		f->msize = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->version);
		break;

	case Rerror:
		p = gstring(p, ep, &f->ename);
		break;

	case Rauth:
		p = gqid(p, ep, &f->aqid);
		if(p == nil)
			break;
		break;

	case Rattach:
		if(p+BIT32SZ > ep)
			return 0;
		f->fid = GBIT32(p);
		p += BIT32SZ;
		p = gqid(p, ep, &f->qid);
		if(p == nil)
			break;
		break;

	case Rwalk:
		p = gqid(p, ep, &f->wqid);
		if(p == nil)
			break;
		break;

	case Ropen:
	case Rcreate:
		p = gqid(p, ep, &f->qid);
		if(p == nil)
			break;
		if(p+BIT32SZ > ep)
			return 0;
		f->iounit = GBIT32(p);
		p += BIT32SZ;
		break;

	case Rread:
		if(p+BIT32SZ > ep)
			return 0;
		f->count = GBIT32(p);
		p += BIT32SZ;
		if(p+f->count > ep)
			return 0;
		f->data = (char*)p;
		p += f->count;
		break;

	case Rwrite:
		if(p+BIT32SZ > ep)
			return 0;
		f->count = GBIT32(p);
		p += BIT32SZ;
		break;

	case Rclunk:
	case Rremove:
		break;

	case Rstat:
		if(p+BIT16SZ > ep)
			return 0;
		f->nstat = GBIT16(p);
		p += BIT16SZ;
		if(p+f->nstat > ep)
			return 0;
		f->stat = p;
		p += f->nstat;
		break;

	case Rwstat:
		break;
	}

	if(p==nil || p>ep || p == ap){
		werrstr("unpack: p %#p ep %#p", p, ep);
		return 0;
	}
	return p - ap;
}

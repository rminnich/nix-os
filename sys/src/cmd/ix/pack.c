#include	<u.h>
#include	<libc.h>
#include	<fcall.h>

#include "ix.h"

uchar*
pdata(uchar *p, uchar *s, int ns)
{
	if(s == nil){
		PBIT16(p, 0);
		p += BIT16SZ;
		return p;
	}

	/*
	 * We are moving the string before the length,
	 * so you can S2M a struct into an existing message
	 */
	memmove(p + BIT16SZ, s, ns);
	PBIT16(p, ns);
	p += ns + BIT16SZ;
	return p;
}

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

uint
stringsz(char *s)
{
	if(s == nil)
		return BIT16SZ;

	return BIT16SZ+strlen(s);
}

/*
 * Does NOT include the data bytes added past the packed
 * message for IXRread, IXTwrite, IXTwattr, IXRattr
 * This is so to save copying.
 */
uint
ixpackedsize(IXcall *f)
{
	uint n;
	int i;

	n = BIT8SZ;	/* type */

	switch(f->type){
	case IXTversion:
	case IXRversion:
		n += BIT32SZ;
		n += stringsz(f->version);
		break;

	case IXTsession:
		n += BIT16SZ;
		n += stringsz(f->uname);
		n += BIT8SZ;
		break;
	case IXRsession:
		n += BIT16SZ;
		n += BIT32SZ;
		n += stringsz(f->uname);
		break;

	case IXTsid:
		n += BIT16SZ;
		break;
	case IXRsid:
		break;

	case IXTendsession:
	case IXRendsession:
		break;

	case IXTfid:
		n += BIT32SZ;
		break;
	case IXRfid:
		break;

	case IXTattach:
		n += stringsz(f->aname);
		break;
	case IXRattach:
		n += BIT32SZ;
		break;

	case IXRerror:
		n += stringsz(f->ename);
		break;

	case IXTclone:
		n += BIT8SZ;
		break;
	case IXRclone:
		n += BIT32SZ;
		break;

	case IXTwalk:
		n += BIT16SZ;
		for(i=0; i<f->nwname; i++)
			n += stringsz(f->wname[i]);
		break;
	case IXRwalk:
		break;

	case IXTopen:
		n += BIT8SZ;
		break;
	case IXRopen:
		break;

	case IXTcreate:
		n += stringsz(f->name);
		n += BIT32SZ;
		n += BIT8SZ;
		break;
	case IXRcreate:
		break;

	case IXTread:
		n += BIT16SZ;
		n += BIT64SZ;
		n += BIT32SZ;
		break;
	case IXRread:
		/* data follows */
		break;

	case IXTwrite:
		n += BIT64SZ;
		n += BIT64SZ;
		/* data follows */
		break;
	case IXRwrite:
		n += BIT64SZ;
		n += BIT32SZ;
		break;

	case IXTclunk:
	case IXRclunk:
	case IXTclose:
	case IXRclose:
	case IXTremove:
	case IXRremove:
		break;

	case IXTattr:
		n += stringsz(f->attr);
		break;
	case IXRattr:
		/* data follows */
		break;

	case IXTwattr:
		n += stringsz(f->attr);
		/* value data follows */
		break;
	case IXRwattr:
		break;

	case IXTcond:
		n += BIT8SZ;
		n += stringsz(f->attr);
		/* value data follows */
		break;
	case IXRcond:
		break;

	case IXTmove:
		n += BIT32SZ;
		n += stringsz(f->newname);
		break;
	case IXRmove:
		break;

	case IXTflush:
	case IXRflush:
		break;
	default:
		sysfatal("packedsize: unknown type %d", f->type);

	}
	return n;
}

uint
ixpack(IXcall *f, uchar *ap, uint nap)
{
	uchar *p;
	uint size;
	int i;

	size = ixpackedsize(f);
	if(size == 0)
		return 0;
	if(size > nap)
		return 0;

	p = (uchar*)ap;

	PBIT8(p, f->type);
	p += BIT8SZ;

	switch(f->type){
	case IXTversion:
	case IXRversion:
		PBIT32(p, f->msize);
		p += BIT32SZ;
		p  = pstring(p, f->version);
		break;

	case IXTsession:
		PBIT16(p, f->ssid);
		p += BIT16SZ;
		p  = pstring(p, f->uname);
		PBIT8(p, f->keep);
		p += BIT8SZ;
		break;
	case IXRsession:
		PBIT16(p, f->ssid);
		p += BIT16SZ;
		PBIT32(p, f->afid);
		p += BIT32SZ;
		p  = pstring(p, f->uname);
		break;

	case IXTsid:
		PBIT16(p, f->ssid);
		p += BIT16SZ;
		break;
	case IXRsid:
		break;

	case IXTendsession:
	case IXRendsession:
		break;

	case IXTfid:
		PBIT32(p, f->fid);
		p += BIT32SZ;
		break;
	case IXRfid:
		break;

	case IXTattach:
		p  = pstring(p, f->aname);
		break;
	case IXRattach:
		PBIT32(p, f->fid);
		p += BIT32SZ;
		break;

	case IXRerror:
		p  = pstring(p, f->ename);
		break;

	case IXTclone:
		PBIT8(p, f->cflags);
		p += BIT8SZ;
		break;
	case IXRclone:
		PBIT32(p, f->fid);
		p += BIT32SZ;
		break;

	case IXTwalk:
		PBIT16(p, f->nwname);
		p += BIT16SZ;
		for(i=0; i<f->nwname; i++)
			p  = pstring(p, f->wname[i]);
		break;
	case IXRwalk:
		break;

	case IXTopen:
		PBIT8(p, f->mode);
		p += BIT8SZ;
		break;
	case IXRopen:
		break;

	case IXTcreate:
		p  = pstring(p, f->name);
		PBIT32(p, f->perm);
		p += BIT32SZ;
		PBIT8(p, f->mode);
		p += BIT8SZ;
		break;
	case IXRcreate:
		break;

	case IXTread:
		PBIT16(p, f->nmsg);
		p += BIT16SZ;
		PBIT64(p, f->offset);
		p += BIT64SZ;
		PBIT32(p, f->count);
		p += BIT32SZ;
		break;
	case IXRread:
		/* data follows */
		break;

	case IXTwrite:
		PBIT64(p, f->offset);
		p += BIT64SZ;
		PBIT64(p, f->endoffset);
		p += BIT64SZ;
		/* data follows */
		break;
	case IXRwrite:
		PBIT64(p, f->offset);
		p += BIT64SZ;
		PBIT32(p, f->count);
		p += BIT32SZ;
		break;

	case IXTclunk:
	case IXRclunk:
	case IXTclose:
	case IXRclose:
	case IXTremove:
	case IXRremove:
		break;

	case IXTattr:
		p  = pstring(p, f->attr);
		break;
	case IXRattr:
		/* value data follows */
		break;

	case IXTwattr:
		p  = pstring(p, f->attr);
		/* value data follows */
		break;
	case IXRwattr:
		break;

	case IXTcond:
		PBIT8(p, f->op);
		p += BIT8SZ;
		p  = pstring(p, f->attr);
		/* value data follows */
		break;
	case IXRcond:
		break;

	case IXTmove:
		PBIT32(p, f->dirfid);
		p += BIT32SZ;
		p  = pstring(p, f->newname);
		break;
	case IXRmove:
		break;

	case IXTflush:
	case IXRflush:
		break;

	default:
		sysfatal("pack: type %d", f->type);

	}
	if(size != p-ap)
		return 0;
	return size;
}

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

uchar*
gdata(uchar *p, uchar *ep, uchar **s, int *ns)
{
	uint n;

	if(p+BIT16SZ > ep)
		return nil;
	n = GBIT16(p);
	*ns = n;
	p += BIT16SZ;
	if(p+n > ep)
		return nil;
	*s = p;
	p += n;
	return p;
}

uint
ixunpack(uchar *ap, uint nap, IXcall *f)
{
	uchar *p, *ep;
	int i;

	p = ap;
	ep = p + nap;

	if(p+BIT8SZ > ep){
		werrstr("msg too short");
		return 0;
	}

	f->type = GBIT8(p);
	p += BIT8SZ;

	switch(f->type){
	case IXTversion:
	case IXRversion:
		if(p+BIT32SZ > ep)
			return 0;
		f->msize = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->version);
		break;

	case IXTsession:
		if(p+BIT16SZ > ep)
			return 0;
		f->ssid = GBIT16(p);
		p += BIT16SZ;
		p = gstring(p, ep, &f->uname);
		if(p == nil)
			return 0;
		if(p+BIT8SZ > ep)
			return 0;
		f->keep = GBIT8(p);
		p += BIT8SZ;
		break;
	case IXRsession:
		if(p+BIT16SZ+BIT32SZ > ep)
			return 0;
		f->ssid = GBIT16(p);
		p += BIT16SZ;
		f->afid = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->uname);
		break;

	case IXTsid:
		if(p+BIT16SZ > ep)
			return 0;
		f->ssid = GBIT16(p);
		p += BIT16SZ;
		break;
	case IXRsid:
		break;

	case IXTendsession:
	case IXRendsession:
		break;

	case IXTfid:
		if(p+BIT32SZ > ep)
			return 0;
		f->fid = GBIT32(p);
		p += BIT32SZ;
		break;
	case IXRfid:
		break;

	case IXTattach:
		p = gstring(p, ep, &f->aname);
		break;
	case IXRattach:
		if(p+BIT32SZ > ep)
			return 0;
		f->fid = GBIT32(p);
		p += BIT32SZ;
		break;

	case IXRerror:
		p = gstring(p, ep, &f->ename);
		break;

	case IXTclone:
		if(p+BIT8SZ > ep)
			return 0;
		f->cflags = GBIT8(p);
		p += BIT8SZ;
		break;
	case IXRclone:
		if(p+BIT32SZ > ep)
			return 0;
		f->fid = GBIT32(p);
		p += BIT32SZ;
		break;

	case IXTwalk:
		if(p+BIT16SZ > ep)
			return 0;
		f->nwname = GBIT16(p);
		p += BIT16SZ;
		if(f->nwname > Nwalks)
			sysfatal("unpack: bug: too many walk elems");
		for(i=0; i<f->nwname && p != nil; i++)
			p  = gstring(p, ep, &f->wname[i]);
		break;
	case IXRwalk:
		break;

	case IXTopen:
		if(p+BIT8SZ > ep)
			return 0;
		f->mode = GBIT8(p);
		p += BIT8SZ;
		break;
	case IXRopen:
		break;

	case IXTcreate:
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
	case IXRcreate:
		break;

	case IXTread:
		if(p+BIT16SZ+BIT64SZ+BIT32SZ > ep)
			return 0;
		f->nmsg = GBIT16(p);
		p += BIT16SZ;
		f->offset = GBIT64(p);
		p += BIT64SZ;
		f->count = GBIT32(p);
		p += BIT32SZ;
		break;
	case IXRread:
		f->data = p;
		break;

	case IXTwrite:
		if(p+BIT64SZ > ep)
			return 0;
		f->offset = GBIT64(p);
		p += BIT64SZ;
		f->endoffset = GBIT64(p);
		p += BIT64SZ;
		f->data = p;
		break;
	case IXRwrite:
		if(p+BIT32SZ+BIT64SZ > ep)
			return 0;
		f->offset = GBIT64(p);
		p += BIT64SZ;
		f->count = GBIT32(p);
		p += BIT32SZ;
		break;

	case IXTclunk:
	case IXRclunk:
	case IXTclose:
	case IXRclose:
	case IXTremove:
	case IXRremove:
		break;

	case IXTattr:
		p = gstring(p, ep, &f->attr);
		break;
	case IXRattr:
		f->value = p;
		break;

	case IXTwattr:
		p = gstring(p, ep, &f->attr);
		if(p == nil)
			return 0;
		f->value = p;
		break;
	case IXRwattr:
		break;

	case IXTcond:
		if(p+BIT8SZ > ep)
			return 0;
		f->op = GBIT8(p);
		p += BIT8SZ;
		p = gstring(p, ep, &f->attr);
		if(p == nil)
			return 0;
		f->value = p;
		break;
	case IXRcond:
		break;

	case IXTmove:
		if(p+BIT32SZ > ep)
			return 0;
		f->dirfid = GBIT32(p);
		p += BIT32SZ;
		p = gstring(p, ep, &f->newname);
		break;
	case IXRmove:
		break;

	case IXTflush:
	case IXRflush:
		break;

	default:
		werrstr("unpack: unknown type %d", f->type);
		return 0;
	}

	if(p==nil || p>ep || p == ap){
		werrstr("unpack: p %#p ep %#p", p, ep);
		return 0;
	}
	return p - ap;
}

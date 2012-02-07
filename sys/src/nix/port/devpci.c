#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

enum {
	Qtopdir = 0,

	Qpcidir,
	Qpcictl,
	Qpciraw,
};

#define TYPE(q)		((ulong)(q).path & 0x0F)
#define QID(c, t)	(((c)<<4)|(t))

static Dirtab topdir[] = {
	".",	{ Qtopdir, 0, QTDIR },	0,	0555,
	"pci",	{ Qpcidir, 0, QTDIR },	0,	0555,
};

extern Dev pcidevtab;

static int
pcidirgen(Chan *c, int t, int tbdf, Dir *dp)
{
	Qid q;

	q = (Qid){BUSBDF(tbdf)|t, 0, 0};
	switch(t) {
	case Qpcictl:
		snprint(up->genbuf, sizeof up->genbuf, "%d.%d.%dctl",
			BUSBNO(tbdf), BUSDNO(tbdf), BUSFNO(tbdf));
		devdir(c, q, up->genbuf, 0, eve, 0444, dp);
		return 1;
	case Qpciraw:
		snprint(up->genbuf, sizeof up->genbuf, "%d.%d.%draw",
			BUSBNO(tbdf), BUSDNO(tbdf), BUSFNO(tbdf));
		devdir(c, q, up->genbuf, 128, eve, 0664, dp);
		return 1;
	}
	return -1;
}

static int
pcigen(Chan *c, char *, Dirtab*, int, int s, Dir *dp)
{
	int tbdf;
	Pcidev *p;
	Qid q;

	switch(TYPE(c->qid)){
	case Qtopdir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, Qtopdir), 0, QTDIR};
			snprint(up->genbuf, sizeof up->genbuf, "#%C", pcidevtab.dc);
			devdir(c, q, up->genbuf, 0, eve, 0555, dp);
			return 1;
		}
		return devgen(c, nil, topdir, nelem(topdir), s, dp);
	case Qpcidir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, Qtopdir), 0, QTDIR};
			snprint(up->genbuf, sizeof up->genbuf, "#%C", pcidevtab.dc);
			devdir(c, q, up->genbuf, 0, eve, 0555, dp);
			return 1;
		}
		p = pcimatch(nil, 0, 0);
		while(s >= 2 && p != nil) {
			p = pcimatch(p, 0, 0);
			s -= 2;
		}
		if(p == nil)
			return -1;
		return pcidirgen(c, s+Qpcictl, p->tbdf, dp);
	case Qpcictl:
	case Qpciraw:
		tbdf = MKBUS(BusPCI, 0, 0, 0)|BUSBDF((ulong)c->qid.path);
		p = pcimatchtbdf(tbdf);
		if(p == nil)
			return -1;
		return pcidirgen(c, TYPE(c->qid), tbdf, dp);
	default:
		break;
	}
	return -1;
}

static Chan*
pciattach(char *spec)
{
	return devattach(pcidevtab.dc, spec);
}

Walkqid*
pciwalk(Chan* c, Chan *nc, char** name, int nname)
{
	return devwalk(c, nc, name, nname, (Dirtab *)0, 0, pcigen);
}

static long
pcistat(Chan* c, uchar* dp, long n)
{
	return devstat(c, dp, n, (Dirtab *)0, 0L, pcigen);
}

static Chan*
pciopen(Chan *c, int omode)
{
	c = devopen(c, omode, (Dirtab*)0, 0, pcigen);
	switch(TYPE(c->qid)){
	default:
		break;
	}
	return c;
}

static void
pciclose(Chan*)
{
}

static uchar *pcibase;

static void
pcireset0(void)
{
	pcibase = vmap(0xff00000000000000llu, 256*1024*1024);
print("***** PCIBASE %#p\n", pcibase);
	if(pcibase == nil)
		pcibase = vmap(0xe0000000, 32*1024*1024);
print("***** PCIBASE %#p\n", pcibase);
}

static long
xread(int tbdf, void *va, long n, vlong offset)
{
	uchar *a, *o;
	int i, r;
	ulong x;
	static int once;
	static uchar *p;

	if(once == 0){
		once = 1;
		p = pcibase; // vmap(0xe0000000, 256*1024*1024);
	}

	if(p == nil)
		error("can't vmap");
	a = va;
	o = p + (BUSBNO(tbdf)<<20 + BUSDNO(tbdf)<<15 | BUSFNO(tbdf)<<12);

	if(n+offset > 4096)
		n = 4096-offset;
	r = offset;
	if(!(r & 3) && n == 4){
		x = *(u32int*)(o+r);
		PBIT32(a, x);
		return 4;
	}
	if(!(r & 1) && n == 2){
		x = *(u16int*)(o+r);
		PBIT16(a, x);
		return 2;
	}
	for(i = 0; i <  n; i++){
		x = *(u8int*)(o+r);
		PBIT8(a, x);
		a++;
		r++;
	}
	return i;
}

static long
pciread(Chan *c, void *va, long n, vlong offset)
{
	char buf[256], *ebuf, *w, *a;
	int i, tbdf, r;
	ulong x;
	Pcidev *p;

	a = va;
	switch(TYPE(c->qid)){
	case Qtopdir:
	case Qpcidir:
		return devdirread(c, a, n, (Dirtab *)0, 0L, pcigen);
	case Qpcictl:
		tbdf = MKBUS(BusPCI, 0, 0, 0)|BUSBDF((ulong)c->qid.path);
		p = pcimatchtbdf(tbdf);
		if(p == nil)
			error(Egreg);
		ebuf = buf+sizeof buf-1;	/* -1 for newline */
		w = seprint(buf, ebuf, "%.2x.%.2x.%.2x %.4x/%.4x %3d",
			p->ccrb, p->ccru, p->ccrp, p->vid, p->did, p->intl);
		for(i=0; i<nelem(p->mem); i++){
			if(p->mem[i].size == 0)
				continue;
			w = seprint(w, ebuf, " %d:%.8lux %d", i, p->mem[i].bar, p->mem[i].size);
		}
		*w++ = '\n';
		*w = '\0';
		return readstr(offset, a, n, buf);
	case Qpciraw:
		tbdf = MKBUS(BusPCI, 0, 0, 0)|BUSBDF((ulong)c->qid.path);
		p = pcimatchtbdf(tbdf);
		if(p == nil)
			error(Egreg);
		if(n+offset > 256)
{
return xread(tbdf, va, n, offset);
			return 0;
}
		if(n+offset > 256)
			n = 256-offset;
		r = offset;
		if(!(r & 3) && n == 4){
			x = pcicfgr32(p, r);
			PBIT32(a, x);
			return 4;
		}
		if(!(r & 1) && n == 2){
			x = pcicfgr16(p, r);
			PBIT16(a, x);
			return 2;
		}
		for(i = 0; i <  n; i++){
			x = pcicfgr8(p, r);
			PBIT8(a, x);
			a++;
			r++;
		}
		return i;
	default:
		error(Egreg);
	}
	return n;
}

static long
pciwrite(Chan *c, void *va, long n, vlong offset)
{
	char buf[256];
	uchar *a;
	int i, r, tbdf;
	ulong x;
	Pcidev *p;

	if(n >= sizeof(buf))
		n = sizeof(buf)-1;
	a = va;
	strncpy(buf, (char*)a, n);
	buf[n] = 0;

	switch(TYPE(c->qid)){
	case Qpciraw:
		tbdf = MKBUS(BusPCI, 0, 0, 0)|BUSBDF((ulong)c->qid.path);
		p = pcimatchtbdf(tbdf);
		if(p == nil)
			error(Egreg);
		if(offset > 256)
			return 0;
		if(n+offset > 256)
			n = 256-offset;
		r = offset;
		if(!(r & 3) && n == 4){
			x = GBIT32(a);
			pcicfgw32(p, r, x);
			return 4;
		}
		if(!(r & 1) && n == 2){
			x = GBIT16(a);
			pcicfgw16(p, r, x);
			return 2;
		}
		for(i = 0; i <  n; i++){
			x = GBIT8(a);
			pcicfgw8(p, r, x);
			a++;
			r++;
		}
		return i;
	default:
		error(Egreg);
	}
	return n;
}

Dev pcidevtab = {
	'$',
	"pci",

	pcireset0,	/* arrg ... bad planning */
	devinit,
	devshutdown,
	pciattach,
	pciwalk,
	pcistat,
	pciopen,
	devcreate,
	pciclose,
	pciread,
	devbread,
	pciwrite,
	devbwrite,
	devremove,
	devwstat,
};

#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "ix.h"
#include "net.h"
#include "fns.h"

/*
 * Attribute handling
 *
 * BUG: we only support the predefined attributes.
 * Just store/parse a sequence of name[s] sz[2] value[sz]
 * after predefined attributes.
 */

typedef struct Adef Adef;

struct Adef
{
	char*	name;
	int	sz;
	long	(*wattr)(Memblk*, void*, long);
	long	(*rattr)(Memblk*, void*, long);
	void	(*cattr)(Memblk*, int, void*, long, void*, long);
};

long wname(Memblk*, void*, long);
static long rname(Memblk*, void*, long);
static long rid(Memblk*, void*, long);
long wid(Memblk*, void*, long);
long watime(Memblk*, void*, long);
static long ratime(Memblk*, void*, long);
long wmtime(Memblk*, void*, long);
static long rmtime(Memblk*, void*, long);
static long wlength(Memblk*, void*, long);
static long rlength(Memblk*, void*, long);
static long wuid(Memblk*, void*, long);
static long ruid(Memblk*, void*, long);
static long wgid(Memblk*, void*, long);
static long rgid(Memblk*, void*, long);
static long wmuid(Memblk*, void*, long);
static long rmuid(Memblk*, void*, long);
static long rstar(Memblk*, void*, long);
static void cstring(Memblk*, int, void*, long, void*, long);
static void cu64int(Memblk*, int, void*, long, void*, long);

static Adef adef[] =
{
	{"name", 0, wname, rname, cstring},
	{"id",	BIT64SZ, nil, rid, cu64int},
	{"atime", BIT64SZ, watime, ratime, cu64int},
	{"mtime", BIT64SZ, wmtime, rmtime, cu64int},
	{"length", BIT64SZ, wlength, rlength, cu64int},
	{"uid", 0, wuid, ruid, cstring},
	{"gid", 0, wgid, rgid, cstring},
	{"muid", 0, wuid, ruid, cstring},
	{"*", 0, nil, rstar, nil},
};

/*
 * Return size for attributes embedded in file.
 * At least Dminattrsz bytes are reserved in the file block,
 * at most Embedsz.
 * Size is rounded to the size of an address.
 */
ulong
embedattrsz(Memblk *f)
{
	ulong sz;

	sz = f->d.asize;
	sz = ROUNDUP(sz, BIT64SZ);
	if(sz < Dminattrsz)
		sz = Dminattrsz;
	else
		sz = Embedsz;
	return sz;
}

void
gmeta(Fmeta *meta, void *buf, ulong nbuf)
{
	Dmeta *d;
	char *p, *x;
	int i;

	if(nbuf < sizeof *d)
		error("metadata buffer too small");
	d = buf;
	meta->id = d->id;
	meta->mode = d->mode;
	meta->atime = d->atime;
	meta->mtime = d->mtime;
	meta->length = d->length;

	if(d->ssz[FMuid] + sizeof *d > nbuf ||
	   d->ssz[FMgid] + sizeof *d > nbuf ||
	   d->ssz[FMmuid] + sizeof *d > nbuf ||
	   d->ssz[FMname] + sizeof *d > nbuf)
		error("corrupt meta: wrong string size");

	p = (char*)(&d[1]);
	x = p;
	for(i = 0; i < nelem(d->ssz); i++){
		if(x[d->ssz[i]-1] != 0)
			error("corrupt meta: unterminated string");
		x += d->ssz[i];
	}

	meta->uid = p;
	p += d->ssz[FMuid];
	meta->gid = p;
	p += d->ssz[FMgid];
	meta->muid = p;
	p += d->ssz[FMmuid];
	meta->name = p;
}

static ulong
metasize(Fmeta *meta)
{
	ulong n;

	n = sizeof(Dmeta);
	n += strlen(meta->uid) + 1;
	n += strlen(meta->gid) + 1;
	n += strlen(meta->muid) + 1;
	n += strlen(meta->name) + 1;
	/*
	 * BUG: meta->attr
	 */
	return n;
}

/*
 * Pack the metadata into buf.
 * pointers in meta are changed to refer to the packed data.
 * Return pointer past the packed metadata.
 * The caller is responsible for ensuring that metadata fits in buf.
 */
ulong
pmeta(void *buf, ulong nbuf, Fmeta *meta)
{
	Dmeta *d;
	char *p, *bufp;
	ulong sz;

	sz = metasize(meta);
	if(sz > nbuf)
		error("attributes are too long");
	d = buf;
	bufp = buf;
	d->id = meta->id;
	d->mode = meta->mode;
	d->atime = meta->atime;
	d->mtime = meta->mtime;
	d->length = meta->length;

	p = (char*)(&d[1]);
	d->ssz[FMuid] = strlen(meta->uid) + 1;
	strcpy(p, meta->uid);
	meta->uid = p;
	p += d->ssz[FMuid];

	d->ssz[FMgid] = strlen(meta->gid) + 1;
	strcpy(p, meta->gid);
	meta->gid = p;
	p += d->ssz[FMgid];

	d->ssz[FMmuid] = strlen(meta->muid) + 1;
	strcpy(p, meta->muid);
	meta->muid = p;
	p += d->ssz[FMmuid];

	d->ssz[FMname] = strlen(meta->name) + 1;
	strcpy(p, meta->name);
	meta->name = p;
	p += d->ssz[FMname];

	assert(p - bufp <= sz);	/* can be <, to leave room for growing */
	return sz;
}

static void
ceval(int op, int v)
{
	switch(op){
	case CEQ:
		if(v != 0)
			error("false");
		break;
	case CGE:
		if(v < 0)
			error("false");
		break;
	case CGT:
		if(v <= 0)
			error("false");
		break;
	case CLE:
		if(v > 0)
			error("false");
		break;
	case CLT:
		if(v >= 0)
			error("false");
	case CNE:
		if(v == 0)
			error("false");
		break;
	}
}

static void
cstring(Memblk*, int op, void *buf, long, void *val, long len)
{
	char *p;

	p = val;
	if(len < 1 || p[len-1] != 0)
		error("value must end in \\0");
	ceval(op, strcmp(buf, val));
}

static void
cu64int(Memblk*, int op, void *buf, long, void *val, long)
{
	u64int v1, v2;
	uchar *p1, *p2;

	p1 = buf;
	p2 = val;
	v1 = GBIT64(p1);
	v2 = GBIT64(p2);
	/* avoid overflow */
	if(v1 > v2)
		ceval(op, 1);
	else if(v1 < v2)
		ceval(op, -1);
	else
		ceval(op, 0);
}

long 
wname(Memblk *f, void *buf, long len)
{
	char *p, *old;
	ulong maxsz;

	p = buf;
	if(len < 1 || p[len-1] != 0)
		error("name must end in \\0");
	old = f->mf->name;
	f->mf->name = p;
	maxsz = embedattrsz(f);
	if(metasize(f->mf) > maxsz){
		f->mf->name = old;
		fprint(2, "%s: bug: no attribute block implemented\n", argv0);
		error("no room to grow metadata");
	}
	/* name goes last, we can pack in place */
	pmeta(f->d.embed, maxsz, f->mf);
	return len;
}

static long
rstr(char *s, void *buf, long len)
{
	long l;

	l = strlen(s) + 1;
	if(l > len)
		error("buffer too short");
	strcpy(buf, s);
	return l;
}

static long 
rname(Memblk *f, void *buf, long len)
{
	return rstr(f->mf->name, buf, len);
}

static long 
ruid(Memblk *f, void *buf, long len)
{
	return rstr(f->mf->uid, buf, len);
}

static long 
wuid(Memblk *f, void *buf, long len)
{
	char *p;
	ulong maxsz;
	Fmeta m;

	p = buf;
	if(len < 1 || p[len-1] != 0)
		error("name must end in \\0");
	maxsz = embedattrsz(f);
	m = *f->mf;
	m.uid = buf;
	m.gid = strdup(m.gid);
	m.muid = strdup(m.muid);
	m.name = strdup(m.name);
	if(metasize(&m) > maxsz){
		fprint(2, "%s: bug: no attribute block implemented\n", argv0);
		error("no room to grow metadata");
	}
	f->mf->Fmeta = m;
	pmeta(f->d.embed, maxsz, f->mf);
	free(m.gid);
	free(m.muid);
	free(m.name);
	return len;
}

static long 
rgid(Memblk *f, void *buf, long len)
{
	return rstr(f->mf->gid, buf, len);
}

static long 
wgid(Memblk *f, void *buf, long len)
{
	char *p;
	ulong maxsz;
	Fmeta m;

	p = buf;
	if(len < 1 || p[len-1] != 0)
		error("name must end in \\0");
	maxsz = embedattrsz(f);
	m = *f->mf;
	m.uid = strdup(m.uid);
	m.gid = buf;
	m.muid = strdup(m.muid);
	m.name = strdup(m.name);
	if(metasize(&m) > maxsz){
		fprint(2, "%s: bug: no attribute block implemented\n", argv0);
		error("no room to grow metadata");
	}
	f->mf->Fmeta = m;
	pmeta(f->d.embed, maxsz, f->mf);
	free(m.uid);
	free(m.muid);
	free(m.name);
	return len;
}

static long 
rmuid(Memblk *f, void *buf, long len)
{
	return rstr(f->mf->muid, buf, len);
}

static long 
wmuid(Memblk *f, void *buf, long len)
{
	char *p;
	ulong maxsz;
	Fmeta m;

	p = buf;
	if(len < 1 || p[len-1] != 0)
		error("name must end in \\0");
	maxsz = embedattrsz(f);
	m = *f->mf;
	m.uid = strdup(m.uid);
	m.gid = strdup(m.gid);
	m.muid = buf;
	m.name = strdup(m.name);
	if(metasize(&m) > maxsz){
		fprint(2, "%s: bug: no attribute block implemented\n", argv0);
		error("no room to grow metadata");
	}
	f->mf->Fmeta = m;
	pmeta(f->d.embed, maxsz, f->mf);
	free(m.uid);
	free(m.gid);
	free(m.name);
	return len;
}

long 
wid(Memblk *f, void *buf, long)
{
	u64int *p;
	Dmeta *d;

	p = buf;
	d = (Dmeta*)f->d.embed;
	f->mf->id = *p;
	d->id = *p;
	return BIT64SZ;
}

static long 
rid(Memblk *f, void *buf, long)
{
	u64int *p;

	p = buf;
	*p = f->mf->id;
	return BIT64SZ;
}

long 
watime(Memblk *f, void *buf, long)
{
	u64int *p;
	Dmeta *d;

	p = buf;
	d = (Dmeta*)f->d.embed;
	f->mf->atime = *p;
	d->atime = *p;
	return BIT64SZ;
}

static long 
ratime(Memblk *f, void *buf, long)
{
	u64int *p;

	p = buf;
	*p = f->mf->atime;
	return BIT64SZ;
}

long 
wmtime(Memblk *f, void *buf, long)
{
	u64int *p;
	Dmeta *d;

	p = buf;
	d = (Dmeta*)f->d.embed;
	f->mf->mtime = *p;
	d->mtime = *p;
	return BIT64SZ;
}

static long 
rmtime(Memblk *f, void *buf, long)
{
	u64int *p;

	p = buf;
	*p = f->mf->mtime;
	return BIT64SZ;
}

static uvlong
fresize(Memblk *f, uvlong sz)
{
	ulong boff, bno, bend;

	if(f->mf->mode&DMDIR)
		error("can't resize a directory");

	if(sz > maxfsz)
		error("max file size exceeded");
	if(sz >= f->mf->length)
		return sz;
	bno = dfbno(f, sz, &boff);
	if(boff > 0)
		bno++;
	bend = dfbno(f, sz, &boff);
	if(boff > 0)
		bend++;
	dfdropblks(f, bno, bend);
	return sz;
}

static long 
wlength(Memblk *f, void *buf, long)
{
	u64int *p;
	Dmeta *d;

	p = buf;
	d = (Dmeta*)f->d.embed;
	f->mf->length = fresize(f, *p);
	d->length = *p;
	return BIT64SZ;
}

static long 
rlength(Memblk *f, void *buf, long)
{
	u64int *p;

	p = buf;
	*p = f->mf->length;
	return BIT64SZ;
}

static long 
rstar(Memblk *, void *buf, long len)
{
	char *s, *e;
	int i;

	s = buf;
	e = s + len;
	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, "*") != 0)
			s = seprint(s, e, "%s ", adef[i].name);
	if(s > buf)
		*--s = 0;
	return s - (char*)buf;
}

long
dfwattr(Memblk *f, char *name, void *val, long nval)
{
	int i;
	long tot;

	isfile(f);
	ismelted(f);
	isrwlocked(f, Wr);
	if(fsfull())
		error("file system full");

	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, name) == 0)
			break;
	if(i == nelem(adef))
		error("bug: user defined attributes not yet implemented");
	if(adef[i].wattr == nil)
		error("can't write %s", name);
	if(adef[i].sz != 0 && adef[i].sz != nval)
		error("wrong length for attribute");

	tot = adef[i].wattr(f, val, nval);
	changed(f);
	return tot;
}

long
dfrattr(Memblk *f, char *name, void *val, long count)
{
	int i;
	long tot;

	isfile(f);
	isrwlocked(f, Rd);
	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, name) == 0)
			break;
	if(i == nelem(adef))
		error("no such attribute");
	if(adef[i].sz != 0 && count < adef[i].sz)
		error("buffer too short for attribute");

	tot = adef[i].rattr(f, val, count);
	return tot;
}

void
dfcattr(Memblk *f, int op, char *name, void *val, long count)
{
	int i;
	long nbuf;
	char buf[128];

	isfile(f);
	isrwlocked(f, Rd);

	nbuf = dfrattr(f, name, buf, sizeof buf);

	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, name) == 0)
			break;
	if(i == nelem(adef))
		error("no such attribute");
	if(adef[i].sz != 0 && count != adef[i].sz)
		error("value size does not match");
	adef[i].cattr(f, op, buf, nbuf, val, count);
}

void
dfaccessok(Memblk *f, char *uid, int bits)
{
	uint mode;

	if(fs->config)
		return;

	bits &= 3;
	mode = f->mf->mode &0777;

	if((mode&bits) == bits)
		return;
	mode >>= 3;
	
	if(member(f->mf->gid, uid) && (mode&bits) == bits)
		return;
	mode >>= 3;
	if(strcmp(f->mf->uid, uid) == 0 && (mode&bits) == bits)
		return;
	error("permission denied");
}

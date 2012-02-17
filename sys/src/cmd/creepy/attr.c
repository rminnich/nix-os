#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "fns.h"

/*
 * Attribute handling
 */

typedef struct Adef Adef;

struct Adef
{
	char*	name;
	int	sz;
	long	(*wattr)(Memblk*, void*, long);
	long	(*rattr)(Memblk*, void*, long);
};

long wname(Memblk*, void*, long);
static long rname(Memblk*, void*, long);
static long rid(Memblk*, void*, long);
long watime(Memblk*, void*, long);
static long ratime(Memblk*, void*, long);
long wmtime(Memblk*, void*, long);
static long rmtime(Memblk*, void*, long);
static long wlength(Memblk*, void*, long);
static long rlength(Memblk*, void*, long);

static Adef adef[] =
{
	{"name", 0, wname, rname},
	{"id",	BIT64SZ, nil, rid},
	{"atime", BIT64SZ, watime, ratime},
	{"mtime", BIT64SZ, wmtime, rmtime},
	{"length", BIT64SZ, wlength, rlength},
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
	if(sz > nbuf){
		fatal("bug: allocate and use ablk");
		error("attributes are too long");
	}
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
rname(Memblk *f, void *buf, long len)
{
	long l;

	l = strlen(f->mf->name) + 1;
	if(l > len)
		error("buffer too short");
	strcpy(buf, f->mf->name);
	return l;
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
		error("user defined attributes not yet implemented");
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

static int
member(char *gid, char *uid)
{
	/* BUG: no groups */
	return strcmp(gid, uid) == 0;
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

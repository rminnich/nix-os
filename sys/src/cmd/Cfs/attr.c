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
	long	(*wattr)(Fsys*, Memblk*, void*, long);
	long	(*rattr)(Fsys*, Memblk*, void*, long);
};

static long wname(Fsys*, Memblk*, void*, long);
static long rname(Fsys*, Memblk*, void*, long);

static Adef adef[] =
{
	{"name", wname, rname},
};

ulong
embedattrsz(Memblk *f)
{
	ulong sz;

	sz = f->d.asize;
	if(sz < Dminattrsz)
		sz = Dminattrsz;
	if(sz > Embedsz || Embedsz - sz < sizeof(Dentry))
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
		sysfatal("bug: allocate and use ablk");
		error("attributes are too long");
	}
	d = buf;
	bufp = buf;
	d->id = meta->id;
	d->mode = meta->mode;
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

static long 
wname(Fsys *, Memblk *f, void *buf, long len)
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
	f->d.asize = pmeta(f->d.embed, maxsz, f->mf);
	changed(f);
	return len;
}

static long 
rname(Fsys *, Memblk *f, void *buf, long len)
{
	long l;

	l = strlen(f->mf->name) + 1;
	if(l > len)
		error("buffer too short");
	strcpy(buf, f->mf->name);
	return l;
}

long
dfwattr(Fsys *fs, Memblk *f, char *name, void *val, long nval)
{
	int i;

	isfile(f);
	iswlocked(f);
	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, name) == 0)
			return adef[i].wattr(fs, f, val, nval);
	error("user defined attributes not yet implemented");
	return -1;
}

long
dfrattr(Fsys *fs, Memblk *f, char *name, void *val, long count)
{
	int i;

	isfile(f);
	isrlocked(f);
	for(i = 0; i < nelem(adef); i++)
		if(strcmp(adef[i].name, name) == 0)
			return adef[i].rattr(fs, f, val, count);
	error("user defined attributes not yet implemented");
	return -1;
}


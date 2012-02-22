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
 * Misc tools.
 */

static Alloc pathalloc =
{
	.elsz = sizeof(Path),
	.zeroing = 0,
};

void
fsstats(void)
{
	print("blks:\t%4uld nblk %4uld nablk %4uld mused %4uld mfree %4ulld dfree\n",
		fs->nblk, fs->nablk, fs->nmused, fs->nmfree, fs->super->d.ndfree);
	print("paths:\t%4uld alloc %4uld free (%4uld bytes)\n",
		pathalloc.nalloc, pathalloc.nfree, pathalloc.elsz);
	print("mfs:\t%4uld alloc %4uld free (%4uld bytes)\n",
		mfalloc.nalloc, mfalloc.nfree, mfalloc.elsz);
	print("\n");
	print("Fsysmem:\t%uld\n", Fsysmem);
	print("Dminfree:\t%d\n", Dminfree);
	print("Dblksz:  \t%uld\n", Dblksz);
	print("Mblksz:  \t%ud\n", sizeof(Memblk));
	print("Dminattrsz:\t%uld\n", Dminattrsz);
	print("Nblkgrpsz:\t%uld\n", Nblkgrpsz);
	print("Dblkdatasz:\t%d\n", Dblkdatasz);
	print("Embedsz:\t%d\n", Embedsz);
	print("Dentryperblk:\t%d\n", Dblkdatasz/sizeof(Dentry));
	print("Dptrperblk:\t%d\n\n", Dptrperblk);
}

void*
anew(Alloc *a)
{
	Next *n;

	assert(a->elsz > 0);
	qlock(a);
	n = a->free;
	if(n != nil){
		a->free = n->next;
		a->nfree--;
	}else{
		a->nalloc++;
		n = mallocz(a->elsz, !a->zeroing);
	}
	qunlock(a);
	if(a->zeroing)
		memset(n, 0, a->elsz);
	return n;
	
}

void
afree(Alloc *a, void *nd)
{
	Next *n;

	if(nd == nil)
		return;
	n = nd;
	qlock(a);
	n->next = a->free;
	a->free = n;
	a->nfree++;
	qunlock(a);
}

static void
xaddelem(Path *p, Memblk *f)
{
	if(p->nf == p->naf){
		p->naf += Incr;
		p->f = realloc(p->f, p->naf*sizeof p->f[0]);
	}
	p->f[p->nf++] = f;
	incref(f);
}

static Path*
duppath(Path *p)
{
	Path *np;
	int i;

	np = newpath(p->f[0]);
	for(i = 1; i < p->nf; i++)
		xaddelem(np, p->f[i]);
	return np;
}

void
ownpath(Path **pp)
{
	Path *p;

	p = *pp;
	if(p->ref > 1){
		*pp = duppath(p);
		putpath(p);
	}
}

Path*
addelem(Path **pp, Memblk *f)
{
	Path *p;

	ownpath(pp);
	p = *pp;
	xaddelem(p, f);
	return p;
}

Path*
dropelem(Path **pp)
{
	Path *p;

	ownpath(pp);
	p = *pp;
	if(p->nf > 0)
		mbput(p->f[--p->nf]);
	return p;
}

Path*
newpath(Memblk *root)
{
	Path *p;

	p = anew(&pathalloc);
	p->ref = 1;
	xaddelem(p, root);
	return p;
}

void
putpath(Path *p)
{
	int i;

	if(p == nil || decref(p) > 0)
		return;
	for(i = 0; i < p->nf; i++)
		mbput(p->f[i]);
	p->nf = 0;
	afree(&pathalloc, p);
}

Path*
clonepath(Path *p)
{
	incref(p);
	return p;
}


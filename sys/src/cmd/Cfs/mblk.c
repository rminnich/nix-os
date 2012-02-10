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
 * memory blocks.
 * see dk.h
 */


/*
 * All the code assumes outofmemoryexits = 1.
 */

char*tname[] = {
[DBfree]	"free",
[DBdata]	"data",
[DBptr]		"ptr",
[DBref]		"ref",
[DBattr]	"attr",
[DBfile]		"file",
[DBsuper]	"super"
};

#define EP(e)	((e)&0xFFFFFFFFUL)
/*
 * NO LOCKS. debug only
 */
int
mbfmt(Fmt *fmt)
{
	Memblk *b;
	int type, i, n, once;

	b = va_arg(fmt->args, Memblk*);
	if(b == nil)
		return fmtprint(fmt, "<nil>\n");
	type = TAGTYPE(b->d.tag);
	fmtprint(fmt, "m%#p d%#ullx", b, b->addr);
	if(b->frozen)
		fmtprint(fmt, " FZ");
	if(b->dirty)
		fmtprint(fmt, " DT");
	if(b->written)
		fmtprint(fmt, " WR");
	fmtprint(fmt, " DB%s r%d", tname[type], b->ref);
	fmtprint(fmt, " tag %#ullx epoch %#ullx", EP(b->d.tag), EP(b->d.epoch));
	switch(type){
	case DBfree:
	case DBdata:
	case DBattr:
		fmtprint(fmt, "\n");
		break;
	case DBptr:
		fmtprint(fmt, "\n");
		for(i = n = 0; i < Dptrperblk; i++)
			if(b->d.ptr[i]){
				fmtprint(fmt, "\t[%d]=d%#ullx", i, b->d.ptr[i]);
				if(++n%4 == 0)
					fmtprint(fmt, "\n");
			}
		if(n%4 != 0)
			fmtprint(fmt, "\n");
		break;
	case DBref:
		fmtprint(fmt, " rnext m%#p\n", b->rnext);
		for(i = n = 0; i < Drefperblk; i++)
			if(b->d.ref[i]){
				fmtprint(fmt, "\t[%d]d%#ullx=%#ullx",
					i, addrofref(b->addr, i), b->d.ref[i]);
				if(++n%5 == 0)
					fmtprint(fmt, "\n");
			}
		if(n%5 != 0)
			fmtprint(fmt, "\n");
		break;
	case DBfile:
		fmtprint(fmt, "\n\tasz %#ullx aptr %#ullx\n", b->d.asize, b->d.aptr);
		if(b->mf == nil){
			fmtprint(fmt, "\tno mfile\n");
			break;
		}
		fmtprint(fmt, "\tid %#ullx mode %M mt %#ullx sz %#ullx '%s' '%s'\n",
			EP(b->mf->id), (ulong)b->mf->mode, EP(b->mf->mtime),
			b->mf->length, b->mf->uid, b->mf->name);
		fmtprint(fmt, "\tparent m%#p nr%d nw%d lastb m%#p lastbno %uld\n",
			b->mf->parent, b->mf->readers, b->mf->writer,
			b->mf->lastb, b->mf->lastbno);
		if(b->mf->nchild > 0){
			fmtprint(fmt, "\tchild:");
			for(i = 0; i < b->mf->nchild; i++)
				fmtprint(fmt, " m%#p", b->mf->child[i].f);
			fmtprint(fmt, "\n");
		}
		fmtprint(fmt, "\tdptr:");
		for(i = 0; i < nelem(b->d.dptr); i++)
			fmtprint(fmt, " d%#ullx", b->d.dptr[i]);
		fmtprint(fmt, "\n");
		fmtprint(fmt, "\tiptr:");
		for(i = 0; i < nelem(b->d.iptr); i++)
			fmtprint(fmt, " d%#ullx", b->d.iptr[i]);
		fmtprint(fmt, "\n");
		break;
	case DBsuper:
		fmtprint(fmt, "\n\tfree d%#ullx eaddr %#ullx root [", b->d.free, b->d.eaddr);
		once = 0;
		for(i = 0; i < nelem(b->d.root); i++)
			if(b->d.root[i] != 0){
				if(once++ != 0)
					fmtprint(fmt, " ");
				fmtprint(fmt, "d%#ullx", b->d.root[i]);
			}
		fmtprint(fmt, "]\n");
		break;
	}
	return 0;
}

void
clean(Memblk *b)
{
	b->dirty = 0;
}

void
ismelted(Memblk *b)
{
	if(b->frozen)
		sysfatal("frozen at pc %#p", getcallerpc(&b));
}

void
changed(Memblk *b)
{
	if(TAGTYPE(b->d.tag) != DBsuper)
		ismelted(b);
	b->d.epoch = now();
	b->dirty = 1;
}

Memblk*
mbhash(Fsys *fs, Memblk *b)
{
	Memblk **h;
	uint hv;

	hv = b->addr%nelem(fs->fhash);
	wlock(&fs->fhash[hv]);
	fs->nused++;
	for(h = &fs->fhash[hv].b; *h != nil; h = &(*h)->next)
		if((*h)->addr == b->addr){
			/* concurrent reads, use the first one */
			mbput(fs, b);
			b = *h;
			goto Found;
		}
	*h = b;
	if(b->next != nil)
		sysfatal("mbhash: next");
	if(TAGTYPE(b->d.tag) == DBref){
		qlock(fs);
		b->rnext = fs->refs;
		fs->refs = b;
		qunlock(fs);
	}
Found:
	incref(b);
	wunlock(&fs->fhash[hv]);
	return b;
}

static void
mbfree(Fsys *fs, Memblk *b)
{
	Mfile *mf;

	if(b == nil)
		return;
	dDprint("mbfree %H\n", b);
	if(b->ref > 0)
		sysfatal("mbfree: has refs");
	if(b->next != nil)
		sysfatal("mbfree: has next");
	if(TAGTYPE(b->d.tag) == DBref)
		sysfatal("mbfree: is DBref"); 

	if(TAGTYPE(b->d.tag) == DBfile && b->mf != nil){
		mf = b->mf;
		b->mf = nil;
		mf->nchild = 0;
		if(mf->lastb != nil)
			mbput(fs, mf->lastb);
		mf->lastb = nil;
		mf->lastbno = 0;
		mf->parent = nil;
		mf->next = nil;
		assert(mf->readers == mf->writer && mf->readers == 0);
		qlock(fs);
		mf->next = fs->mfree;
		fs->mfree = mf;
		qunlock(fs);
	}
	b->d.tag = DBfree;
	b->frozen = b->written = b->dirty = 0;
	b->addr = 0;

	qlock(fs);
	fs->nfree++;
	b->next = fs->free;
	fs->free = b;
	qunlock(fs);
}

void
mbunhash(Fsys *fs, Memblk *b)
{
	Memblk **h;
	uint hv;

	if(TAGTYPE(b->d.tag) == DBref)
		sysfatal("mbunhash: DBref");

	hv = b->addr%nelem(fs->fhash);
	wlock(&fs->fhash[hv]);
	for(h = &fs->fhash[hv].b; *h != nil; h = &(*h)->next)
		if((*h)->addr == b->addr){
			if(*h != b)
				sysfatal("mbunhash: dup block");
			*h = b->next;
			b->next = nil;
			fs->nused--;
			wunlock(&fs->fhash[hv]);
			mbput(fs, b);
			return;
		}
	sysfatal("mbunhash: not found");
}

Memblk*
mballoc(Fsys *fs, u64int addr)
{
	Memblk *b;

	b = nil;
	qlock(fs);
	if(fs->nblk < fs->nablk)
		b = &fs->blk[fs->nblk++];
	else if(fs->free != nil){
		b = fs->free;
		fs->free = b->next;
		fs->nfree--;
	}else{
		qunlock(fs);
		error("evict block not implemented");
	}
	qunlock(fs);
	memset(b, 0, sizeof *b);
	b->addr = addr;
	b->ref = 1;
	dDprint("mballoc %#ullx -> %H", addr, b);
	return b;
}

Memblk*
mbget(Fsys *fs, u64int addr)
{
	Memblk *b;
	uint hv;

	hv = addr%nelem(fs->fhash);
	rlock(&fs->fhash[hv]);
	for(b = fs->fhash[hv].b; b != nil; b = b->next)
		if(b->addr == b->addr){
			incref(b);
			break;
		}
	runlock(&fs->fhash[hv]);
	dDprint("mbget %#ullx -> %H", addr, b);
	return b;
}

void
mbput(Fsys *fs, Memblk *b)
{
	dDprint("mbput m%#p pc=%#p\n", b, getcallerpc(&fs));
	if(decref(b) == 0)
		mbfree(fs, b);
}

Memblk*
mbdup(Fsys *fs, Memblk *b)
{
	Memblk *nb;

	nb = mballoc(fs, b->addr);
	memmove(&nb->d, &b->d, sizeof b->d);
	return nb;
}

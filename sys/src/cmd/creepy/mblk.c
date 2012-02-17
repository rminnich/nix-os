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
 * For simplicity, functions in mblk.c do not raise errors.
 * (debug dump functions may be an exception).
 */

char*
tname(int t)
{
	static char*nms[] = {
	[DBfree]	"DBfree",
	[DBnew]		"DBnew",
	[DBsuper]	"DBsuper",
	[DBref]		"DBref",
	[DBdata]	"DBdata",
	[DBattr]	"DBattr",
	[DBfile]		"DBfile",
	[DBptr0]	"DBptr0",
	[DBptr0+1]	"DBptr1",
	[DBptr0+2]	"DBptr2",
	[DBptr0+3]	"DBptr3",
	[DBptr0+4]	"DBptr4",
	[DBptr0+5]	"DBptr5",
	[DBptr0+6]	"DBptr6",
	};

	if(t < 0 || t >= nelem(nms))
		return "BADTYPE";
	return nms[t];
}

#define EP(e)	((e)&0xFFFFFFFFUL)
/*
 * NO LOCKS. debug only
 */
static void
fmttab(Fmt *fmt, int t)
{
	if(t-- > 0)
		fmtprint(fmt, "\t");
	while(t-- > 0)
		fmtprint(fmt, "    ");
}
int mbtab;
static void
fmtptr(Fmt *fmt, u64int addr, char *tag, int n)
{
	Memblk *b;

	if(addr == 0)
		return;
	b = mbget(addr, 0);
	if(b == nil){
		fmttab(fmt, mbtab);
		fmtprint(fmt, "  %s[%d] = d%#ullx <unloaded>\n", tag, n, addr);
	}else{
		decref(b);
		fmtprint(fmt, "%H", b);
	}
}
static void
dumpsomedata(Fmt *fmt, Memblk *b)
{
	long doff;
	u64int *p;
	int i;

	if(b->mf->length == 0)
		return;
	doff = embedattrsz(b);
	if(doff < Embedsz){
		fmttab(fmt, mbtab);
		p = (u64int*)(b->d.embed+doff);
		for(i = 0; i < 5 && (uchar*)p < b->d.embed+Embedsz - BIT64SZ; i++)
			fmtprint(fmt, "%s%#ullx", i?" ":"  data: ", *p++);
		fmtprint(fmt, "\n");
	}
}

int
mbfmt(Fmt *fmt)
{
	Memblk *b;
	int type, i, n, xdbg;

	b = va_arg(fmt->args, Memblk*);
	if(b == nil)
		return fmtprint(fmt, "<nil>\n");
	type = TAGTYPE(b->d.tag);
	fmttab(fmt, mbtab);
	xdbg = dbg['D'];
	dbg['D'] = 0;
	fmtprint(fmt, "m%#p d%#ullx", b, b->addr);
	if(b->frozen)
		fmtprint(fmt, " FZ");
	if(b->dirty)
		fmtprint(fmt, " DT");
	if(b->written)
		fmtprint(fmt, " WR");
	fmtprint(fmt, " %s r%d", tname(type), b->ref);
	fmtprint(fmt, " tag %#ullx epoch %#ullx", EP(b->d.tag), EP(b->d.epoch));
	switch(type){
	case DBfree:
		fmtprint(fmt, "\n");
		break;
	case DBdata:
	case DBattr:
		fmtprint(fmt, " dr=%ulld\n", dbgetref(b->addr));
		break;
	case DBref:
		fmtprint(fmt, " rnext m%#p", b->rnext);
		for(i = n = 0; i < Drefperblk; i++)
			if(b->d.ref[i]){
				if(n++%4 == 0){
					fmtprint(fmt, "\n");
					fmttab(fmt, mbtab);
				}
				fmtprint(fmt, "  ");
				fmtprint(fmt, "[%d]d%#ullx=%#ullx",
					i, addrofref(b->addr, i), b->d.ref[i]);
			}
		if(n == 0 || --n%4 != 0)
			fmtprint(fmt, "\n");
		break;
	case DBfile:
		fmtprint(fmt, " dr=%ulld\n", dbgetref(b->addr));
		if(b->mf == nil){
			fmtprint(fmt, "  no mfile\n");
			break;
		}
		fmttab(fmt, mbtab);
		fmtprint(fmt, "  '%s' asz %#ullx aptr %#ullx melted m%#p\n",
			b->mf->name, b->d.asize,b->d.aptr, b->mf->melted);
		fmttab(fmt, mbtab);
		fmtprint(fmt, "  id %#ullx mode %M mt %#ullx sz %#ullx '%s'\n",
			EP(b->mf->id), (ulong)b->mf->mode, EP(b->mf->mtime),
			b->mf->length, b->mf->uid);
		fmttab(fmt, mbtab);
		fmtprint(fmt, "  parent m%#p nr%d nw%d\n",
			b->mf->parent, b->mf->readers, b->mf->writer);
		dumpsomedata(fmt, b);
		mbtab++;
		for(i = 0; i < nelem(b->d.dptr); i++)
			fmtptr(fmt, b->d.dptr[i], "d", i);
		for(i = 0; i < nelem(b->d.iptr); i++)
			fmtptr(fmt, b->d.iptr[i], "i", i);
		mbtab--;
		break;
	case DBsuper:
		fmtprint(fmt, "\n");
		fmttab(fmt, mbtab);
		fmtprint(fmt, "  free d%#ullx eaddr d%#ullx root d%#ullx\n",
			b->d.free, b->d.eaddr, b->d.root);
		break;
	default:
		if(type < DBptr0 || type >= DBptr0+Niptr)
			fatal("<bad type %d>", type);
		fmtprint(fmt, " dr=%ulld\n", dbgetref(b->addr));
		mbtab++;
		for(i = 0; i < Dptrperblk; i++)
			fmtptr(fmt, b->d.ptr[i], "p", i);
		mbtab--;
		break;
	}
	dbg['D'] = xdbg;
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
	if(b != fs->archive && b->frozen)
		fatal("frozen at pc %#p", getcallerpc(&b));
}

void
changed(Memblk *b)
{
	if(TAGTYPE(b->d.tag) != DBsuper)
		ismelted(b);
	b->d.epoch = now();
	b->dirty = 1;
	b->written = 0;
}

static void
lruunlink(Memblk *b)
{
	if(b->lprev != nil)
		b->lprev->lnext = b->lnext;
	else
		fs->mru = b->lnext;
	if(b->lnext != nil)
		b->lnext->lprev = b->lprev;
	else
		fs->lru = b->lprev;
	b->lnext = nil;
	b->lprev = nil;
}


static void
lrulink(Memblk *b)
{
	b->lnext = fs->mru;
	b->lprev = nil;
	if(fs->mru)
		fs->mru->lprev = b;
	else
		fs->lru = b;
	fs->mru = b;
}

static void
mbused(Memblk *b)
{
	qlock(&fs->llk);
	lruunlink(b);
	lrulink(b);
	qunlock(&fs->llk);
}

static void
linkblock(Memblk *b)
{
	if(TAGTYPE(b->d.tag) == DBref){
		qlock(fs);
		b->rnext = fs->refs;
		fs->refs = b;
		qunlock(fs);
	}
	qlock(&fs->llk);
	lrulink(b);
	qunlock(&fs->llk);
}

Memblk*
mbhash(Memblk *b)
{
	Memblk **h, *ob;
	uint hv;

	hv = b->addr%nelem(fs->fhash);
	qlock(&fs->fhash[hv]);
	fs->nused++;
	ob = nil;
	for(h = &fs->fhash[hv].b; *h != nil; h = &(*h)->next)
		if((*h)->addr == b->addr)
			fatal("mbhash: dup");
	*h = b;
	if(b->next != nil)
		fatal("mbhash: next");
	incref(b);
	linkblock(b);

	qunlock(&fs->fhash[hv]);
	mbput(ob);
	return b;
}

void
mbunhash(Memblk *b)
{
	Memblk **h;
	uint hv;

	if(TAGTYPE(b->d.tag) == DBref)
		fatal("mbunhash: DBref");

	hv = b->addr%nelem(fs->fhash);
	qlock(&fs->fhash[hv]);
	for(h = &fs->fhash[hv].b; *h != nil; h = &(*h)->next)
		if((*h)->addr == b->addr){
			if(*h != b)
				fatal("mbunhash: dup");
			*h = b->next;
			b->next = nil;
			fs->nused--;
			qlock(&fs->llk);
			lruunlink(b);
			qunlock(&fs->llk);
			qunlock(&fs->fhash[hv]);
			return;
		}
	fatal("mbunhash: not found");
}

static void
mbfree(Memblk *b)
{
	Mfile *mf;

	if(b == nil)
		return;
	dDprint("mbfree %H\n", b);
	if(b->ref > 0)
		fatal("mbfree: has %d refs", b->ref);
	if(b->next != nil)
		fatal("mbfree: has next");

	if(TAGTYPE(b->d.tag) != DBsuper)
		mbunhash(b);
	/* this could panic, but errors reading a block might cause it */
	if(TAGTYPE(b->d.tag) == DBref)
		fprint(2, "%s: free of DBref. i/o errors?\n", argv0);

	if(TAGTYPE(b->d.tag) == DBfile && b->mf != nil){
		mf = b->mf;
		b->mf = nil;
		mbput(mf->melted);
		mf->melted = nil;
		mbput(mf->parent);
		mf->parent = nil;
		mf->next = nil;
		assert(mf->writer == 0 && mf->readers == 0);
		mffree(mf);
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

Memblk*
mballoc(u64int addr)
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
		fatal("mballoc: evict block not implemented");
	}
	qunlock(fs);
	memset(b, 0, sizeof *b);
	b->addr = addr;
	b->ref = 1;
	dDprint("mballoc %#ullx -> %H", addr, b);
	return b;
}

Memblk*
mbget(u64int addr, int mkit)
{
	Memblk *b;
	uint hv;

	hv = addr%nelem(fs->fhash);
	qlock(&fs->fhash[hv]);
	for(b = fs->fhash[hv].b; b != nil; b = b->next)
		if(b->addr == addr){
			incref(b);
			break;
		}
	if(mkit)
		if(b == nil){
			b = mballoc(addr);
			b->d.tag = TAG(addr, DBnew);
			b->next = fs->fhash[hv].b;
			fs->fhash[hv].b = b;
			incref(b);
			linkblock(b);
			qlock(&b->newlk);	/* make others wait for it */
		}else if(TAGTYPE(b->d.tag) == DBnew){
			qunlock(&fs->fhash[hv]);
			qlock(&b->newlk);	/* wait for it */
			qunlock(&b->newlk);
			if(TAGTYPE(b->d.tag) == DBnew){
				mbput(b);
				dDprint("mbget %#ullx -> i/o error\n", addr);
				return nil;	/* i/o error reading it */
			}
			dDprint("mbget %#ullx -> waited for m%#p\n", addr, b);
			return b;
		}
	qunlock(&fs->fhash[hv]);
	if(b != nil)
		mbused(b);
	dDprint("mbget %#ullx -> m%#p\n", addr, b);
	return b;
}

void
mbput(Memblk *b)
{
	if(b == nil)
		return;
	dDprint("mbput m%#p pc=%#p\n", b, getcallerpc(&b));
	if(decref(b) == 0)
		mbfree(b);
}

Memblk*
mbdup(Memblk *b)
{
	Memblk *nb;

	nb = mballoc(b->addr);
	memmove(&nb->d, &b->d, sizeof b->d);
	return nb;
}

Mfile*
mfalloc(void)
{
	Mfile *mf;

	qlock(&fs->mlk);
	mf = fs->mfree;
	if(mf != nil){
		fs->mfree = mf->next;
		mf->next = nil;
	}
	qunlock(&fs->mlk);
	if(mf == nil)
		mf = mallocz(sizeof *mf, 1);
	return mf;
}

void
mffree(Mfile *mf)
{
	if(mf == nil)
		return;
	qlock(&fs->mlk);
	mf->next = fs->mfree;
	fs->mfree = mf;
	qunlock(&fs->mlk);
}


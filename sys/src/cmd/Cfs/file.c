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
 * Interface to handle files.
 * see dk.h
 */

/*
 * Ok if nelems is 0.
 */
Memblk*
walkpath(Fsys *fs, Memblk *f, char *elems[], int nelems)
{
	int i;
	Memblk *nf;

	isfile(f);
	rlock(f->mf);
	if(f->mf->length > 0 && f->mf->child == nil){
		runlock(f->mf);
		dfloaddir(fs, f, 0);
		rlock(f->mf);
	}
	if(catcherror()){
		runlock(f->mf);
		error(nil);
	}
	for(i = 0; i < nelems; i++){
		if((f->mf->mode&DMDIR) == 0)
			error("not a directory");
		nf = dfwalk(fs, f, elems[i], 0);
		runlock(f->mf);
		f = nf;
		USED(&f);	/* in case of error() */
	}
	noerror();
	incref(f);
	runlock(f->mf);
	return f;
}

Memblk*
dfcreate(Fsys *fs, Memblk *parent, char *name, char *uid, ulong mode)
{
	Memblk *b;
	Mfile *m;

	dDprint("dfcreate '%s' %M at %H", name, mode, parent);
	if(parent != nil){
		isdir(parent);
		wlock(parent->mf);
		if(parent->frozen){
			wunlock(parent->mf);
			parent = dfmelt(fs, parent);
		}else
			incref(parent);
		b = dballoc(fs, DBfile);
	}else
		b = dballoc(fs, Noaddr);	/* root */

	if(catcherror()){
		wunlock(b->mf);
		mbput(fs, b);
		if(parent != nil){
			wunlock(parent->mf);
			mbput(fs, parent);
		}
		error("create: %r");
	}
	m = b->mf;
	m->id = b->d.epoch;
	m->mode = mode;
	m->mtime = b->d.epoch;
	m->length = 0;
	m->uid = uid;
	m->gid = uid;
	m->muid = uid;
	m->name = name;
	b->d.asize = pmeta(b->d.embed, Embedsz, m);

	if(parent != nil){
		m->gid = parent->mf->uid;
		dflink(fs, parent, b);
		wunlock(parent->mf);
		mbput(fs, parent);
	}
	noerror();
	changed(b);
	dDprint("dfcreate-> %H\n", b);
	return b;
}

/*
 * returns a slice into a block for reading.
 */
Blksl
dfreadblk(Fsys *fs, Memblk *f, ulong len, uvlong off)
{
	Blksl sl;

	isfile(f);
	rlock(f->mf);
	if(catcherror()){
		runlock(f->mf);
		error("read: %r");
	}
	sl = dfslice(fs, f, len, off, 0);
	noerror();
	runlock(f->mf);
	return sl;
}

/*
 * returns a slice into a block for writing
 * the block is returned unlocked.
 */
Blksl
dfwriteblk(Fsys *fs, Memblk *f, uvlong off)
{
	Blksl sl;

	isnotdir(f);
	wlock(f->mf);
	if(f->frozen){
		wunlock(f->mf);
		f = dfmelt(fs, f);
	}else
		incref(f);
	if(catcherror()){
		wunlock(f->mf);
		error(nil);
	}
	sl = dfslice(fs, f, 0, off, 1);
	noerror();
	wunlock(f->mf);
	mbput(fs, f);
	return sl;
}

ulong
dfpread(Fsys *fs, Memblk *f, void *a, ulong count, uvlong off)
{
	Blksl sl;
	ulong tot;
	char *p;

	p = a;
	for(tot = 0; tot < count; tot += sl.len){
		sl = dfreadblk(fs, f, count, off);
		if(sl.len == 0)
			break;
		if(sl.data == nil){
			memset(p+tot, 0, sl.len);
			continue;
		}
		memmove(p+tot, sl.data, sl.len);
		mbput(fs, sl.b);
	}
	return tot;
}

ulong
dfpwrite(Fsys *fs, Memblk *f, void *a, ulong count, uvlong off)
{
	Blksl sl;
	ulong tot;
	char *p;

	p = a;
	for(tot = 0; tot < count; tot += sl.len){
		sl = dfwriteblk(fs, f, off);
		if(sl.len == 0 || sl.data == nil)
			sysfatal("dfpwrite: bug");
		memmove(sl.data, p+tot, sl.len);
		changed(sl.b);
		mbput(fs, sl.b);
	}
	return tot;
}

/*
 * Freezing should not fail.
 * If it does, we can't even freeze the tree to sync to disk,
 * so there's not much to do.
 * The caller with probably catch the error and sysfatal.
 */


/*
 * freeze a direct or indirect pointer and everything below it.
 */
static void
ptrfreeze(Fsys *fs, u64int addr, int nind)
{
	int i;
	Memblk *b;

	if(addr == 0)
		return;
	b = mbget(fs, addr);
	if(b == nil)
		return;	/* on disk: frozen */
	if(!b->frozen){
		b->frozen = 1;
		if(nind > 0)
			for(i = 0; i < Dptrperblk; i++)
				ptrfreeze(fs, b->d.ptr[i], nind-1);
	}
	mbput(fs, b);
}

/*
 * freeze a file.
 * Do not recur if children is found frozen.
 */
void
dffreeze(Fsys *fs, Memblk *f)
{
	int i;
	Memblk *b;

	iswlocked(f);
	isfile(f);
	dDprint("dffrezee m%#p\n", f);
	f->frozen = 1;
	for(i = 0; i < nelem(f->d.dptr); i++)
		ptrfreeze(fs, f->d.dptr[i], 0);
	for(i = 0; i < nelem(f->d.iptr); i++)
		ptrfreeze(fs, f->d.dptr[i], i+1);
	if((f->mf->mode&DMDIR) == 0)
		return;
	for(i = 0; i < f->mf->nchild; i++){
		b = f->mf->child[i].f;
		if(!b->frozen){
			wlock(b->mf);
			dffreeze(fs, b);
			wunlock(b->mf);
		}
	}
}

/*
 * freeze a direct or indirect pointer and everything below it.
 */
static void
ptrsync(Fsys *fs, u64int addr, int nind)
{
	int i;
	Memblk *b;

	if(addr == 0)
		return;
	b = mbget(fs, addr);
	if(b == nil)
		return;	/* on disk */
	if(!b->frozen)
		sysfatal("ptrsync: not frozen\n\t%H", b);
	if(b->dirty)
		dbwrite(fs, b);
	b->dirty = 0;
	mbput(fs, b);
	if(nind > 0)
		for(i = 0; i < Dptrperblk; i++)
			ptrsync(fs, b->d.ptr[i], nind-1);
}

/*
 * Ensure all frozen but dirty blocks are in disk.
 */
void
dfsync(Fsys *fs, Memblk *f)
{
	int i;

	isfile(f);
	if(f->written)
		return;
	if(!f->frozen)
		sysfatal("dfsync: not frozen\n\t%H", f);

	for(i = 0; i < nelem(f->d.dptr); i++)
		ptrsync(fs, f->d.dptr[i], 0);
	for(i = 0; i < nelem(f->d.iptr); i++)
		ptrsync(fs, f->d.dptr[i], i+1);
	for(i = 0; i < f->mf->nchild; i++)
		dfsync(fs, f->mf->child[i].f);

	rlock(f->mf);
	if(f->dirty)
		dbwrite(fs, f);
	f->dirty = 0;
	f->written = 1;
	runlock(f->mf);
}

/*
 * release a direct or indirect pointer and everything below it.
 */
static int
ptrreclaim(Fsys *fs, u64int addr, int nind)
{
	int i;
	Memblk *b;

	if(addr == 0)
		return 0;
	if(dbdecref(fs, addr) != 0)
		return 0;
	b = dbget(fs, nind == 0? DBdata: DBptr, addr);
	if(!b->frozen)
		sysfatal("ptrreclaim: not frozen\n\t%H", b);
	mbunhash(fs, b);
	if(b->ref != 1)
		sysfatal("dfreclaim: bug?");
	if(nind > 0)
		for(i = 0; i < Dptrperblk; i++)
			ptrreclaim(fs, b->d.ptr[i], nind-1);
	mbput(fs, b);
	return 1;
}

/*
 * remove f and all it's children.
 * It's safe to remove the parent before the children,
 * because no reference to f is kept in the disk when this
 * function is called.
 *
 * One problem here is that we have to load the blocks
 * to actually learn their references and remove them.
 * TODO: do this using an external cleaner program?
 */
int
dfreclaim(Fsys *fs, Memblk *f)
{
	int i, tot;

	tot = 0;
	dDprint("dfreclaim %H", f);
	if(dbdecref(fs, f->addr) != 0)
		return 0;
	tot++;
	if(!f->frozen)
		sysfatal("dfsync: not frozen\n\t%H", f);
	incref(f);
	mbunhash(fs, f);
	if(f->ref != 1)
		sysfatal("dfreclaim: ref is %d", f->ref);
	for(i = 0; i < nelem(f->d.dptr); i++)
		tot += ptrreclaim(fs, f->d.dptr[i], 0);
	for(i = 0; i < nelem(f->d.iptr); i++)
		tot += ptrreclaim(fs, f->d.dptr[i], i+1);
	if(f->mf->mode&DMDIR){
		isloaded(f);
		for(i = 0; i < f->mf->nchild; i++)
			tot += dfreclaim(fs, f->mf->child[i].f);
	}
	mbput(fs, f);
	return tot;
}


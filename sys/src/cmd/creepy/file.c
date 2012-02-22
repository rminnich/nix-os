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
 * Interface to handle files.
 * see dk.h
 */

static void
dfchanged(Memblk *f)
{
	isfile(f);
	changed(f);
	wmtime(f, &f->d.epoch, sizeof f->d.epoch);
	watime(f, &f->d.epoch, sizeof f->d.epoch);
}

static void
dfused(Memblk *f)
{
	u64int t;

	isfile(f);
	t = now();
	wmtime(f, &t, sizeof t);
}

Memblk*
dfcreate(Memblk *parent, char *name, char *uid, ulong mode)
{
	Memblk *b;
	Mfile *m;

	if(fsfull())
		error("file system full");

	if(parent != nil){
		dDprint("dfcreate '%s' %M at\n%H\n", name, mode, parent);
		isdir(parent);
		isrwlocked(parent, Wr);
		ismelted(parent);
		b = dballoc(DBfile);
	}else{
		dDprint("dfcreate '%s' %M", name, mode);
		b = dballoc(Noaddr);	/* root */
	}
	if(catcherror()){
		mbput(b);
		if(parent != nil)
			rwunlock(parent, Wr);
		error(nil);
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
	dfchanged(b);

	if(parent != nil){
		m->gid = parent->mf->uid;
		dflink(parent, b);
		dfchanged(parent);
	}
	noerror();
	dDprint("dfcreate-> %H\n", b);
	incref(b);	/* initial ref for tree; this for caller */
	return b;
}

void
dfremove(Memblk *p, Memblk *f)
{
	/* funny as it seems, we may need extra blocks to melt */
	if(fsfull())
		error("file system full");

	isrwlocked(f, Wr);
	isrwlocked(p, Wr);
	ismelted(p);
	if((f->mf->mode&DMDIR) != 0 && f->mf->length > 0)
		error("directory not empty");
	incref(p);
	if(catcherror()){
		mbput(p);
		error(nil);
	}
	dfunlink(p, f);
	/* can't fail now. it's unlinked */
	noerror();
	rwunlock(f, Wr);
	if(!catcherror()){
		dfreclaim(f);
		noerror();
	}
	mbput(f);
	mbput(p);
}

ulong
dfpread(Memblk *f, void *a, ulong count, uvlong off)
{
	Blksl sl;
	ulong tot;
	char *p;

	p = a;
	isrwlocked(f, Rd);
	for(tot = 0; tot < count; tot += sl.len){
		sl = dfslice(f, count-tot, off+tot, Rd);
		if(sl.len == 0)
			break;
		if(sl.data == nil){
			memset(p+tot, 0, sl.len);
			continue;
		}
		memmove(p+tot, sl.data, sl.len);
		mbput(sl.b);
	}
	dfused(f);
	return tot;
}

ulong
dfpwrite(Memblk *f, void *a, ulong count, uvlong *off)
{
	Blksl sl;
	ulong tot;
	char *p;

	if(fsfull())
		error("file system full");

	isrwlocked(f, Wr);
	ismelted(f);
	p = a;
	if(f->mf->mode&DMAPPEND)
		*off = f->mf->length;
	for(tot = 0; tot < count; tot += sl.len){
		sl = dfslice(f, count-tot, *off+tot, Wr);
		if(sl.len == 0 || sl.data == nil)
			fatal("dfpwrite: bug");
		memmove(sl.data, p+tot, sl.len);
		changed(sl.b);
		mbput(sl.b);
	}
	dfchanged(f);
	return tot;
}

/*
 * Called only by dfwattr(), for "length", to
 * adjust the file data structure before actually
 * updating the file length attribute.
 * Should return the size in use.
 */

static int
ptrmap(u64int addr, int nind, Blkf f, int isdisk)
{
	int i;
	Memblk *b;
	long tot;

	if(addr == 0)
		return 0;
	if(isdisk)
		b = dbget(DBdata+nind, addr);
	else{
		b = mbget(addr, 0);
		if(b == nil)
			return 0;	/* on disk */
	}
	if(catcherror()){
		mbput(b);
		error(nil);
	}
	tot = 0;
	if(f(b) == 0){
		tot++;
		if(nind > 0)
			for(i = 0; i < Dptrperblk; i++)
				tot += ptrmap(b->d.ptr[i], nind-1, f, isdisk);
	}
	noerror();
	mbput(b);
	return tot;
}

static int
fdumpf(Memblk *f)
{
	extern int mbtab;

	isfile(f);
	mbtab++;
	return 0;
}

static int
bdumpf(Memblk*)
{
	return 0;
}

static int
fdumpedf(Memblk *)
{
	extern int mbtab;

	mbtab--;
	return 0;
}

/*
 * XXX: We must get rid of dfmap.
 * There are few uses and they are already too different.
 * for example, for dfdump, we want to call fslowmem() now and then,
 * so that if we read the entire disk to dump it, we have no problem.
 */
int
dfdump(Memblk *f, int disktoo)
{
	int n;

	incref(f);
	n = dfmap(f, fdumpf, fdumpedf, bdumpf, disktoo, No);
	decref(f);
	return n;
}

int
dfmap(Memblk *f, Blkf pre, Blkf post, Blkf bf, int isdisk, int lk)
{
	int i;
	Memblk *b;
	Memblk *(*child)(Memblk*, int);
	long tot;
	extern int mbtab;

	isfile(f);
	rwlock(f, lk);
	if(catcherror()){
		rwunlock(f, lk);
		error(nil);
	}
	if(pre != nil && pre(f) < 0){
		noerror();
		rwunlock(f, lk);
		return 0;
	}
	tot = 1;
	if(bf != nil){
		for(i = 0; i < nelem(f->d.dptr); i++)
			tot += ptrmap(f->d.dptr[i], 0, bf, isdisk);
		for(i = 0; i < nelem(f->d.iptr); i++)
			tot += ptrmap(f->d.iptr[i], i+1, bf, isdisk);
	}
	if(pre == fdumpf){	/* kludge */
		mbtab--;
		print("%H\n", f);
		mbtab++;
	}
	if((f->mf->mode&DMDIR) != 0){
		child = dfchild;
		if(!isdisk)
			child = mfchild;
		for(i = 0; i < f->mf->length/sizeof(Dentry); i++){
			b = child(f, i);
			if(b == nil)
				continue;
			if(!catcherror()){
				tot += dfmap(b, pre, post, bf, isdisk, lk);
				noerror();
			}
			mbput(b);
		}
	}
	if(post != nil)
		post(f);
	noerror();
	rwunlock(f, lk);
	return tot;
}

static int
bfreezef(Memblk *b)
{
	if(b->frozen)
		return -1;
	b->frozen = 1;
	return 0;
}

static int
ffreezef(Memblk *f)
{
	/* see fsfreeze() */
	if(f->frozen && f != fs->active && f != fs->archive)
		return -1;
	f->frozen = 1;
	return 0;
}

int
dffreeze(Memblk *f)
{
	return dfmap(f, ffreezef, nil, bfreezef, Mem, Wr);
}

static int
bsyncf(Memblk *b)
{
	if(b->dirty)
		dbwrite(b);
	b->dirty = 0;
	return 0;
}

static int
fsyncf(Memblk *f)
{
	if(f->written)
		return -1;
	return 0;
}
static int
fsyncedf(Memblk *f)
{
	if((f != fs->archive && !f->frozen) || f->written)
		fatal("fsyncf: not frozen or written\n%H\n", f);
	if(f->dirty)
		dbwrite(f);
	f->dirty = 0;
	f->written = 1;	/* but for errors! */
	return 0;
}

int
dfsync(Memblk *f)
{
	return dfmap(f, fsyncf, fsyncedf, bsyncf, Mem, Rd);
}

static int
breclaimf(Memblk *b)
{
	if(catcherror())
		return -1;
	if(dbdecref(b->addr) != 0){
		noerror();
		return -1;
	}
	if(b->ref != 1)
		fatal("breclaimf: ref is %d", b->ref);
	noerror();
	return 0;
}

static int
freclaimf(Memblk *f)
{
	if(dbdecref(f->addr) != 0)
		return -1;
	if(f->ref != 1)
		print("freclaimf: ref is %d\n", f->ref);
	return 0;
}

/*
 * While reclaiming, we drop disk references from the parent
 * to the children, but, in memory,
 * the parent is never released before releasing the children,
 * so clients holding locks within the reclaimed tree should be safe.
 */
int
dfreclaim(Memblk *f)
{
	return dfmap(f, freclaimf, nil, breclaimf, Disk, Wr);
}

/*
 * DEBUG: no locks.
 */
void
dflist(Memblk *f, char *ppath)
{
	char *path;
	Mfile *m;
	int i;
	Memblk *cf;

	m = f->mf;
	if(ppath == nil){
		print("/");
		path = strdup(m->name);
	}else
		path = smprint("%s/%s", ppath, m->name);
	print("%-30s\t%M\t%5ulld\t%s mr=%d dr=%ulld\n",
		path, (ulong)m->mode, m->length, m->uid, f->ref, dbgetref(f->addr));
	if(m->mode&DMDIR)
		for(i = 0; (cf = dfchild(f, i)) != nil; i++){
			dflist(cf, path);
			mbput(cf);
		}
	free(path);
	if(ppath == nil)
		print("\n");
}

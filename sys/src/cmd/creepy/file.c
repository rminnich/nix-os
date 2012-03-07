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
dfused(Memblk *f)
{
	u64int t;

	isfile(f);
	t = now();
	wmtime(f, &t, sizeof t);
}

/*
 * May be called with null parent, for root and ctl files.
 * The first call with a null parent is root, all others are ctl
 * files linked at root.
 */
Memblk*
dfcreate(Memblk *parent, char *name, char *uid, ulong mode)
{
	Memblk *nf;
	Mfile *m;
	int isctl;

	if(fsfull())
		error("file system full");
	isctl = parent == nil;
	if(parent == nil)
		parent = fs->root;

	if(parent != nil){
		dprint("dfcreate '%s' %M at\n%H\n", name, mode, parent);
		isdir(parent);
		isrwlocked(parent, Wr);
		ismelted(parent);
	}else
		dprint("dfcreate '%s' %M", name, mode);

	if(isctl)
		nf = dballoc(DBctl);
	else
		nf = dballoc(DBfile);
	if(catcherror()){
		mbput(nf);
		if(parent != nil)
			rwunlock(parent, Wr);
		error(nil);
	}

	m = nf->mf;
	m->id = now();
	m->mode = mode;
	m->mtime = m->id;
	m->atime = m->id;
	m->length = 0;
	m->uid = uid;
	m->gid = uid;
	m->muid = uid;
	m->name = name;
	nf->d.asize = pmeta(nf->d.embed, Embedsz, m);
	changed(nf);

	if(parent != nil){
		m->gid = parent->mf->uid;
		dflink(parent, nf);
	}
	noerror();
	dprint("dfcreate-> %H\n", nf);
	return nf;
}

void
dfremove(Memblk *p, Memblk *f)
{
	vlong n;

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
	/* shouldn't fail now. it's unlinked */
	noerror();
	rwunlock(f, Wr);
	if(!catcherror()){
		n = dfreclaim(f);
		dprint("dfreclaim d%#ullx: %lld blks\n", f->addr, n);
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
	return tot;
}

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
		b = mbget(DBdata+nind, addr, 0);
		if(b == nil)
			return 0;	/* on disk */
	}
	if(catcherror()){
		mbput(b);
		error(nil);
	}
	tot = 0;
	if(f == nil || f(b) == 0){
		tot++;
		/* we might sweep an entire disk and run out of blocks */
		if(isdisk)
			fslru();
		if(nind > 0){
			for(i = 0; i < Dptrperblk; i++)
				tot += ptrmap(b->d.ptr[i], nind-1, f, isdisk);
		}
	}
	noerror();
	mbput(b);
	return tot;
}

/*
 * CAUTION: debug: no locks.
 */
int
dfdump(Memblk *f, int isdisk)
{
	int i;
	Memblk *b;
	Memblk *(*child)(Memblk*, int);
	long tot;
	extern int mbtab;

	isfile(f);
	tot = 1;
	/* visit the blocks to fetch them if needed. */
	for(i = 0; i < nelem(f->d.dptr); i++)
		tot += ptrmap(f->d.dptr[i], 0, nil, isdisk);
	for(i = 0; i < nelem(f->d.iptr); i++)
		tot += ptrmap(f->d.iptr[i], i+1, nil, isdisk);
	fprint(2, "%H\n", f);
	if((f->mf->mode&DMDIR) != 0){
		mbtab++;
		child = dfchild;
		if(!isdisk)
			child = mfchild;
		for(i = 0; i < f->mf->length/sizeof(Dentry); i++){
			b = child(f, i);
			if(b == nil)
				continue;
			if(!catcherror()){
				tot += dfdump(b, isdisk);
				noerror();
			}
			mbput(b);
		}
		mbtab--;
	}

	/* we might sweep an entire disk and run out of blocks */
	if(isdisk)
		fslru();
	return tot;
}

static int
bfreeze(Memblk *b)
{
	if(b->frozen)
		return -1;
	b->frozen = 1;
	return 0;
}

int
dffreeze(Memblk *f)
{
	int i;
	Memblk *b;
	long tot;

	isfile(f);
	if(f->frozen && f != fs->active && f != fs->archive)
		return 0;
	rwlock(f, Wr);
	if(catcherror()){
		rwunlock(f, Wr);
		error(nil);
	}
	f->frozen = 1;
	tot = 1;
	for(i = 0; i < nelem(f->d.dptr); i++)
		tot += ptrmap(f->d.dptr[i], 0, bfreeze, Mem);
	for(i = 0; i < nelem(f->d.iptr); i++)
		tot += ptrmap(f->d.iptr[i], i+1, bfreeze, Mem);
	if((f->mf->mode&DMDIR) != 0){
		for(i = 0; i < f->mf->length/sizeof(Dentry); i++){
			b = mfchild(f, i);
			if(b == nil)
				continue;
			if(!catcherror()){
				tot += dffreeze(b);
				noerror();
			}
			mbput(b);
		}
	}
	noerror();
	rwunlock(f, Wr);
	return tot;
}

static int
countref(u64int addr)
{
	ulong idx;
	int old;

	idx = addr/Dblksz;
	old = fs->chk[idx];
	if(fs->chk[idx] == 0xFE)
		fprint(2, "fscheck: d%#010ullx: too many refs, ignoring some\n",
			addr);
	else
		fs->chk[idx]++;
	return old;
}

static int
bcountrefs(Memblk *b)
{
	countref(b->addr);
	return 0;
}

static void
countfree(u64int addr)
{
	long i;

	i = addr/Dblksz;
	if(fs->chk[i] != 0 && fs->chk[i] <= 0xFE)
		fprint(2, "fscheck: d%#010ullx: free block in use\n", addr);
	else if(fs->chk[i] == 0xFF)
		fprint(2, "fscheck: d%#010ullx: double free\n", addr);
	else
		fs->chk[i] = 0xFF;
}

void
dfcountfree(void)
{
	u64int addr;

	dprint("list...\n");
	addr = fs->super->d.free;
	while(addr != 0){
		if(addr >fs->limit){
			fprint(2, "fscheck: d%#010ullx: free overflow\n", addr);
			break;
		}
		countfree(addr);
		addr = dbgetref(addr);
	}
	/* heading unused part */
	dprint("hdr...\n");
	for(addr = 0; addr < Dblk0addr; addr += Dblksz)
		countfree(addr);
	/* DBref blocks */
	dprint("refs...\n");
	for(addr = Dblk0addr; addr < fs->super->d.eaddr; addr += Dblksz*Nblkgrpsz){
		countfree(addr);	/* even DBref */
		countfree(addr+Dblksz);	/* odd DBref */
	}
}

void
dfcountrefs(Memblk *f)
{
	Memblk *b;
	int i;

	isfile(f);
	if((f->addr&Fakeaddr) == 0 && f->addr >= fs->limit){
		fprint(2, "fscheck: '%s' d%#010ullx: out of range\n",
			f->mf->name, f->addr);
		return;
	}
	if((f->addr&Fakeaddr) == 0)
		if(countref(f->addr) != 0)	/* already visited */
			return;			/* skip children */
	rwlock(f, Rd);
	if(catcherror()){
		fprint(2, "fscheck: '%s' d%#010ullx: data: %r\n",
			f->mf->name, f->addr);
		rwunlock(f, Rd);
		return;
	}
	for(i = 0; i < nelem(f->d.dptr); i++)
		ptrmap(f->d.dptr[i], 0, bcountrefs, Disk);
	for(i = 0; i < nelem(f->d.iptr); i++)
		ptrmap(f->d.iptr[i], i+1, bcountrefs, Disk);
	if(f->mf->mode&DMDIR)
		for(i = 0; i < f->mf->length/sizeof(Dentry); i++){
			b = dfchild(f, i);
			if(b == nil)
				continue;
			if(catcherror())
				fprint(2, "fscheck: '%s'  d%#010ullx:"
					" child[%d]: %r\n",
					f->mf->name, f->addr, i);
			else{
				dfcountrefs(b);
				noerror();
			}
			mbput(b);
		}
	noerror();
	rwunlock(f, Rd);
}

/*
 * Drop one disk reference for f and reclaim its storage if it's gone.
 * The given memory reference is not released.
 * For directories, all files contained have their disk references adjusted,
 * and they are also reclaimed if no further references exist.
 */
int
dfreclaim(Memblk *f)
{
	int i;
	Memblk *b;
	long tot;

	isfile(f);
	dKprint("dfreclaim %H\n", f);
	/*
	 * Remove children if it's the last disk ref before we drop data blocks.
	 * No new disk refs may be added, so there's no race here.
	 */
	tot = 0;
	if(dbgetref(f->addr) == 1 && (f->mf->mode&DMDIR) != 0){
		rwlock(f, Wr);
		if(catcherror()){
			rwunlock(f, Wr);
			error(nil);
		}
		for(i = 0; i < f->mf->length/sizeof(Dentry); i++){
			b = dfchild(f, i);
			if(b == nil)
				continue;
			if(!catcherror()){
				tot += dfreclaim(b);
				noerror();
			}
			mbput(b);
		}
		noerror();
		rwunlock(f, Wr);
	}

	if(dbput(f, f->type, f->addr) == 0)
		tot++;
	return tot;
}

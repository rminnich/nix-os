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
 * disk blocks.
 * see dk.h
 */

void
dbclear(Fsys *fs, u64int addr, int type)
{
	static Diskblk d;
	static QLock lk;

	dDprint("dbclear d%#ullx\n", addr);
	qlock(&lk);
	d.tag = TAG(addr, type);
	d.epoch = now();
	if(pwrite(fs->fd, &d, sizeof d, addr) != Dblksz){
		qlock(&lk);
		error("dbclear: %r");
	}
	qunlock(&lk);
}

void
meltedref(Fsys *fs, Memblk *rb)
{
	if(rb->frozen && rb->dirty){
		if(catcherror())
			sysfatal("writing ref: %r");
		dbwrite(fs, rb);
		noerror();
	}
	rb->frozen = rb->dirty = 0;
}

/*
 * BUG: the free list of blocks using entries in the ref blocks
 * shouldn't span all those blocks as it does now. To prevent
 * massive loses of free blocks each DBref block must keep its own
 * little free list, and all blocks with free entries must be linked
 * in the global list.
 * This keeps locality and makes it less likely that a failure in the
 * middle of the sync for the free list destroyes the entire list.
 */

u64int
newblkaddr(Fsys *fs)
{
	u64int addr, naddr;

	qlock(fs);

	/*
	 * Caution: can't acquire new locks while holding blklk
	 * only dbgetref may raise an error, but we don't hold the
	 * lock while calling it.
	 */
Again:
	if(fs->super == nil)
		addr = Dblksz;
	else if(fs->super->d.eaddr < fs->limit){
		addr = fs->super->d.eaddr;
		fs->super->d.eaddr += Dblksz;
		changed(fs->super);
	}else if(fs->super->d.free != 0){
		addr = fs->super->d.free;

		qunlock(fs);
		naddr = dbgetref(fs, addr);	/* acquires locks */
		qlock(fs);
		if(addr != fs->super->d.free){
			/* had a race */
			goto Again;
		}
		fs->super->d.free = naddr;
		fs->super->d.nfree -= 1;
		changed(fs->super);
		goto found;
	}else{
		/* backward compatible with fossil */
		error("disk is full");
	}
	/*
	 * ref blocks are allocated and initialized on demand,
	 * and they must be zeroed before used.
	 * do this holding the lock so others find everything
	 * initialized.
	 */
	if(((addr-Dblk0addr)/Dblksz)%Nblkgrpsz == 0){
		if(catcherror()){
			qunlock(fs);
			error(nil);
		}
		dbclear(fs, addr-Dblksz, DBref);	/* fs initialization */
		noerror();
		addr += Dblksz;
		fs->super->d.eaddr += Dblksz;
	}
found:
	qunlock(fs);
	okaddr(fs, addr);

	dDprint("newblkaddr = d%#ullx\n", addr);
	return addr;
}

u64int
addrofref(u64int refaddr, int idx)
{
	u64int bno;

	bno = (refaddr - Dblk0addr)/Dblksz;
	bno *= Nblkgrpsz;
	bno += idx;

	return Dblk0addr + bno*Dblksz;
}

u64int
refaddr(u64int addr, int *idx)
{
	u64int bno, refaddr;

	addr -= Dblk0addr;
	bno = addr/Dblksz;
	*idx = bno%Nblkgrpsz;
	refaddr = Dblk0addr + bno/Nblkgrpsz * Nblkgrpsz * Dblksz;
	dDprint("refaddr d%#ullx = d%#ullx[%d]\n",
		Dblk0addr + addr, refaddr, *idx);
	return refaddr;
}

/*
 * db*ref() functions update the on-disk reference counters.
 * memory blocks use Memblk.Ref instead.
 */

u64int
dbgetref(Fsys *fs, u64int addr)
{
	Memblk *rb;
	u64int raddr, ref;
	int i;

	if(addr == 0)
		return 0;

	if(addr == Noaddr )	/* root; 1 ref by the face */
		return 1;

	raddr = refaddr(addr, &i);
	rb = dbget(fs, DBref, raddr);
	qlock(fs);
	meltedref(fs, rb);
	ref = rb->d.ref[i];
	qunlock(fs);
	mbput(fs, rb);
	return ref;
}

void
dbsetref(Fsys *fs, u64int addr, u64int ref)
{
	Memblk *rb;
	u64int raddr;
	int i;

	dDprint("dbsetref %#ullx = %ulld\n", addr, ref);
	if(addr < Dblk0addr)
		sysfatal("dbsetref");
	raddr = refaddr(addr, &i);
	rb = dbget(fs, DBref, raddr);
	qlock(fs);
	meltedref(fs, rb);
	rb->d.ref[i] = ref;
	changed(rb);
	qunlock(fs);
	mbput(fs, rb);
}

void
dbincref(Fsys *fs, u64int addr)
{
	Memblk *rb;
	u64int raddr;
	int i;

	dDprint("dbincref %#ullx\n", addr);
	raddr = refaddr(addr, &i);
	rb = dbget(fs, DBref, raddr);
	qlock(fs);
	meltedref(fs, rb);
	rb->d.ref[i]++;
	changed(rb);
	qunlock(fs);
	mbput(fs, rb);
}

u64int
dbdecref(Fsys *fs, u64int addr)
{
	Memblk *rb;
	u64int raddr, ref;
	int i;

	if(addr < Dblk0addr)
		sysfatal("dbdecref");
	dDprint("dbdecref %#ullx\n", addr);
	raddr = refaddr(addr, &i);
	rb = dbget(fs, DBref, raddr);
	meltedref(fs, rb);
	qlock(fs);
	rb->d.ref[i]--;
	ref = rb->d.ref[i];
	changed(rb);
	if(ref == 0){
		rb->d.ref[i] = fs->super->d.free;
		fs->super->d.free = addr;
		fs->super->d.nfree += 1;
		changed(fs->super);
		changed(rb);
	}
	qunlock(fs);
	mbput(fs, rb);
	return ref;
}

static Mfile*
mfalloc(Fsys *fs)
{
	Mfile *mf;

	qlock(fs);
	mf = fs->mfree;
	if(mf != nil){
		fs->mfree = mf->next;
		mf->next = nil;
	}
	qunlock(fs);
	if(mf == nil)
		mf = mallocz(sizeof *mf, 1);
	return mf;
}

Memblk*
dballoc(Fsys *fs, uint type)
{
	Memblk *b;
	u64int addr;
	int root;

	root = (type == Noaddr);
	addr = Noaddr;
	if(root)
		type = DBfile;
	else
		addr = newblkaddr(fs);
	dDprint("dballoc DB%s\n", tname[type]);
	b = mballoc(fs, addr);
	b->d.tag = TAG(b->addr,type);
	changed(b);
	if(catcherror()){
		mbput(fs, b);
		error(nil);
	}
	if(addr != Noaddr && addr >= Dblk0addr)
		dbsetref(fs, addr, 1);
	if(type == DBfile)
		b->mf = mfalloc(fs);
	mbhash(fs, b);
	dDprint("dballoc DB%s -> %H\n", tname[type], b);
	noerror();
	return b;
}

/*
 * BUG: these should ensure that all integers are converted between
 * little endian (disk format) and the machine endianness.
 * We know the format of all blocks and the type of all file
 * attributes. Those are the integers to convert to fix the bug.
 */
Memblk*
hosttodisk(Memblk *b)
{
	incref(b);
	if(!TAGADDROK(b->d.tag, b->addr))
		sysfatal("hosttodisk: bad tag");
	return b;
}
void
disktohost(Memblk *b)
{
	static union
	{
		u64int i;
		uchar m[BIT64SZ];
	} u;

	u.i = 0x1122334455667788ULL;
	if(u.m[0] != 0x88)
		sysfatal("fix hosttodisk/disktohost for big endian");

	if(!TAGADDROK(b->d.tag, b->addr))
		error("disktohost: bad tag");
}

long
dbwrite(Fsys *fs, Memblk *b)
{
	Memblk *nb;

	dDprint("dbwrite %H",b);

	nb = hosttodisk(b);
	nb->d.epoch = now();
	if(pwrite(fs->fd, &nb->d, sizeof nb->d, nb->addr) != Dblksz){
		mbput(fs, nb);
		error("dbwrite: %r");
	}
	mbput(fs, nb);
	return Dblksz;
}

long
dbread(Fsys *fs, Memblk *b)
{
	long tot, nr;
	uchar *p;

	dDprint("dbread m%#p d%#ullx\n", b, b->addr);

	p = b->d.ddata;
	for(tot = 0; tot < Dblksz; tot += nr){
		nr = pread(fs->fd, p+tot, Dblksz-tot, b->addr + tot);
		if(nr == 0)
			error("eof on disk");
		if(nr <= 0){
			error("dbread: %r");
			return -1;
		}
	}
	assert(tot == sizeof b->d);

	disktohost(b);
	if(TAGTYPE(b->d.tag) != DBref)
		b->frozen = 1;
	dDprint("dbread %H", b);

	return tot;
}

/*
 * Directories are fully loaded by dbget.
 * Their data is never removed from memory unless the
 * entire directory is removed from memory.
 */
Memblk*
dbget(Fsys *fs, uint type, u64int addr)
{
	Memblk *b;
	u64int tag;

	dDprint("dbget d%#ullx\n",addr);
	okaddr(fs, addr);
	b = mbget(fs, addr);
	if(b != nil)
		return b;

	/*
	 * others might request the same block while we read it.
	 * the first one hashing it wins; no locks.
	 */
	tag = TAG(addr,type);
	b = mballoc(fs, addr);
	if(catcherror()){
		mbput(fs, b);
		error(nil);
	}
	dbread(fs, b);
	if(b->d.tag != tag)
		sysfatal("dbget: wrong tag");
	if(type == DBfile){
		assert(b->mf == nil);
		b->mf = mfalloc(fs);
		gmeta(b->mf, b->d.embed, Embedsz);
		b->written = 1;
	}
	noerror();
	b = mbhash(fs, b);
	return b;
}

/*
 * caller responsible for locking.
 */
Memblk*
dbdup(Fsys *fs, Memblk *b)
{
	Memblk *nb;
	uint type;
	int i;
	Mfile *nm, *m;

	type = TAGTYPE(b->d.tag);
	nb = dballoc(fs, type);
	switch(type){
	case DBfree:
	case DBref:
	case DBsuper:
	case DBattr:
		sysfatal("dbdup: DB%s", tname[type]);
	case DBdata:
		memmove(nb->d.data, b->d.data, Dblkdatasz);
		break;
	case DBptr:
		for(i = 0; i < Dptrperblk; i++){
			nb->d.ptr[i] = b->d.ptr[i];
			if(nb->d.ptr[i] != 0)
				dbincref(fs, b->d.ptr[i]);
		}
		break;
	case DBfile:
		isrlocked(b);
		isloaded(b);
		nb->d.asize = b->d.asize;
		nb->d.aptr = b->d.aptr;
		if(nb->d.aptr != 0)
			dbincref(fs, b->d.aptr);
		for(i = 0; i < nelem(b->d.dptr); i++){
			nb->d.dptr[i] = b->d.dptr[i];
			if(nb->d.dptr[i] != 0)
				dbincref(fs, b->d.dptr[i]);
		}
		for(i = 0; i < nelem(b->d.iptr); i++){
			nb->d.iptr[i] = b->d.iptr[i];
			if(nb->d.iptr[i] != 0)
				dbincref(fs, b->d.iptr[i]);
		}
		memmove(nb->d.embed, b->d.embed, Embedsz);
		nm = nb->mf;
		m = b->mf;
		gmeta(nm, nb->d.embed, Embedsz);
		if(m->nchild > 0){
			if(nm->nachild < m->nchild){
				nm->child = realloc(nm->child, m->nchild*sizeof(Child));
				nm->nachild = m->nchild;
				nm->nchild = m->nchild;
			}
			for(i = 0; i < nm->nchild; i++){
				nm->child[i] = m->child[i];
				nm->child[i].f->mf->parent = nb;
				incref(nm->child[i].f);
				incref(nm->child[i].b);
			}
		}
		break;
	default:
		sysfatal("dbdup: type");
	}
	changed(nb);
	return nb;
}


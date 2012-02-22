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
 * disk blocks, built upon memory blocks provided by mblk.c
 * see dk.h
 */

void
checktag(u64int tag, uint type, u64int addr)
{
	if(tag != TAG(addr,type)){
		fprint(2, "%s: bad tag: %#ullx != %#ux d%#ullx pc = %#p\n",
			argv0, tag, type, addr, getcallerpc(&tag));
abort();
		error("bad tag");
	}
}


void
dbclear(u64int addr, int type)
{
	static Diskblk d;
	static QLock lk;

	dDprint("dbclear d%#ullx type %s\n", addr, tname(type));
	qlock(&lk);
	d.tag = TAG(addr, type);
	d.epoch = now();
	if(pwrite(fs->fd, &d, sizeof d, addr) != Dblksz){
		qunlock(&lk);
		fprint(2, "%s: dbclear: d%#ullx: %r\n", argv0, addr);
		error("dbclear: d%#ullx: %r", addr);
	}
	qunlock(&lk);
}

void
meltedref(Memblk *rb)
{
	if(canqlock(&fs->rlk))
		fatal("meltedref rlk");
	if(rb->frozen && rb->dirty)
		dbwrite(rb);
	rb->frozen = rb->dirty = 0;
}

/*
 * BUG: the free list of blocks using entries in the ref blocks
 * shouldn't span all those blocks as it does now. To prevent
 * massive loses of free blocks each DBref block should keep its own
 * little free list, and all blocks with free entries should be linked
 * in the global list.
 * This would keep locality and make it less likely that a failure in the
 * middle of a sync destroyes the entire list.
 */

u64int
newblkaddr(void)
{
	u64int addr, naddr;

	qlock(fs);
	if(catcherror()){
		qunlock(fs);
		error(nil);
	}
Again:
	if(fs->super == nil)
		addr = Dblksz;
	else if(fs->super->d.eaddr < fs->limit){
		addr = fs->super->d.eaddr;
		fs->super->d.eaddr += Dblksz;
		changed(fs->super);
		/*
		 * ref blocks are allocated and initialized on demand,
		 * and they must be zeroed before used.
		 * do this holding the lock so others find everything
		 * initialized.
		 */
		if(((addr-Dblk0addr)/Dblksz)%Nblkgrpsz == 0){
			dDprint("new ref blk addr = d%#ullx\n", addr);
			dbclear(addr, DBref);	/* fs initialization */
			addr += Dblksz;
			fs->super->d.eaddr += Dblksz;
		}
	}else if(fs->super->d.free != 0){
		addr = fs->super->d.free;

		/*
		 * Caution: can't acquire new locks while holding the fs lock,
		 * but dbgetref may allocate blocks.
		 */
		qunlock(fs);
		if(catcherror()){
			qlock(fs);	/* restore the default in this fn. */
			error(nil);
		}
		naddr = dbgetref(addr);	/* acquires locks */
		noerror();
		qlock(fs);
		if(addr != fs->super->d.free){
			/* had a race */
			goto Again;
		}
		fs->super->d.free = naddr;
		fs->super->d.ndfree--;
		changed(fs->super);
	}else{
		addr = 0;
		/* preserve backward compatibility with fossil */
		sysfatal("disk is full");
	}

	noerror();
	qunlock(fs);
	okaddr(addr);
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
 * memory blocks use Memblk.Ref instead. Beware.
 */
static u64int
dbaddref(u64int addr, int delta, int set)
{
	Memblk *rb;
	u64int raddr, ref;
	int i;

	if(addr == 0)
		return 0;
	if(addr == Noaddr)	/* root doesn't count */
		return 0;

	if(set != 0)
		dDprint("dbsetref %#ullx = %d\n", addr, set);
	else if(delta != 0)
		dDprint("dbaddref %#ullx += %d\n", addr, delta);
	nodebug();
	raddr = refaddr(addr, &i);
	rb = dbget(DBref, raddr);
	qlock(&fs->rlk);
	if(catcherror()){
		mbput(rb);
		qunlock(&fs->rlk);
		debug();
		error(nil);
	}
	if(delta != 0 || set != 0){
		meltedref(rb);
		if(set)
			rb->d.ref[i] = set;
		else
			rb->d.ref[i] += delta;
		rb->dirty = 1;
		if(delta < 0 && rb->d.ref[i] == 0){
			qlock(fs);
			rb->d.ref[i] = fs->super->d.free;
			fs->super->d.free = addr;
			fs->super->d.ndfree++;
			qunlock(fs);
		}
	}
	ref = rb->d.ref[i];
	noerror();
	qunlock(&fs->rlk);
	mbput(rb);
	debug();
	return ref;
}

u64int
dbgetref(u64int addr)
{
	return dbaddref(addr, 0, 0);
}

void
dbsetref(u64int addr, int ref)
{
	dbaddref(addr, 0, ref);
}

u64int
dbincref(u64int addr)
{
	return dbaddref(addr, +1, 0);
}

u64int
dbdecref(u64int addr)
{
	return dbaddref(addr, -1, 0);
}

Memblk*
dballoc(uint type)
{
	Memblk *b;
	u64int addr;
	int root;

	nodebug();

	root = (type == Noaddr);
	addr = Noaddr;
	if(root)
		type = DBfile;
	else
		addr = newblkaddr();
	b = mballoc(addr);
	b->d.tag = TAG(b->addr,type);
	if(catcherror()){
		mbput(b);
		debug();
		error(nil);
	}
	changed(b);
	if(addr != Noaddr && addr >= Dblk0addr)
		dbsetref(addr, 1);
	if(type == DBfile)
		b->mf = anew(&mfalloc);
	b = mbhash(b);
	noerror();
	debug();
	dDprint("dballoc %s -> %H\n", tname(type), b);
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
	if(!TAGADDROK(b->d.tag, b->addr))
		fatal("hosttodisk: bad tag");
	incref(b);
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
		fatal("fix hosttodisk/disktohost for big endian");
	checktag(b->d.tag, TAGTYPE(b->d.tag), b->addr);
}

long
dbwrite(Memblk *b)
{
	Memblk *nb;

	dWprint("dbwrite %H\n",b);
	nb = hosttodisk(b);
	nb->d.epoch = now();
	if(pwrite(fs->fd, &nb->d, sizeof nb->d, nb->addr) != Dblksz){
		mbput(nb);
		fprint(2, "%s: dbwrite: d%#ullx: %r\n", argv0, b->addr);
		error("dbwrite: %r");
	}
	mbput(nb);

	return Dblksz;
}

long
dbread(Memblk *b)
{
	long tot, nr;
	uchar *p;


	p = b->d.ddata;
	for(tot = 0; tot < Dblksz; tot += nr){
		nr = pread(fs->fd, p+tot, Dblksz-tot, b->addr + tot);
		if(nr == 0)
			werrstr("eof on disk file");
		if(nr <= 0){
			fprint(2, "%s: dbread: d%#ullx: %r\n", argv0, b->addr);
			error("dbread: %r");
		}
	}
	assert(tot == sizeof b->d);

	disktohost(b);
	if(TAGTYPE(b->d.tag) != DBref)
		b->frozen = 1;
	dRprint("dbread %H\n", b);
	return tot;
}

Memblk*
dbget(uint type, u64int addr)
{
	Memblk *b;

	dMprint("dbget %s d%#ullx\n", tname(type), addr);
	okaddr(addr);
	b = mbget(addr, 1);
	if(b == nil)
		error("i/o error");
	if(TAGTYPE(b->d.tag) != DBnew){
		if(TAGTYPE(b->d.tag) != type)
			fatal("dbget: bug: type %d tag %#ullx", type, b->d.tag);
		return b;
	}

	/* the file is new, must read it */
	if(catcherror()){
		b->d.tag = TAG(addr, DBnew);
		qunlock(&b->newlk);	/* awake those waiting for it */
		mbput(b);		/* our ref and the hash ref */
		mbput(b);
		error(nil);
	}
	dbread(b);
	checktag(b->d.tag, type, addr);
	if(type == DBfile){
		assert(b->mf == nil);
		b->mf = anew(&mfalloc);
		gmeta(b->mf, b->d.embed, Embedsz);
		b->written = 1;
	}
	noerror();
	qunlock(&b->newlk);
	return b;
}

void
dupdentries(void *p, int n)
{
	int i;
	Dentry *d;

	d = p;
	for(i = 0; i < n; i++)
		if(d[i].file != 0){
			dDprint("add ref on melt d%#ullx\n", d[i].file);
			dbincref(d[i].file);
		}
}
/*
 * caller responsible for locking.
 * On errors we leak disk blocks because of added references.
 */
Memblk*
dbdup(Memblk *b)
{
	Memblk *nb;
	uint type;
	int i;
	Mfile *nm;
	ulong doff;

	type = TAGTYPE(b->d.tag);
	nb = dballoc(type);
	if(catcherror()){
		mbput(nb);
		error(nil);
	}
	switch(type){
	case DBfree:
	case DBref:
	case DBsuper:
	case DBattr:
		fatal("dbdup: %s", tname(type));
	case DBdata:
		memmove(nb->d.data, b->d.data, Dblkdatasz);
		break;
	case DBfile:
		if(!b->frozen)
			isrwlocked(b, Rd);
		nb->d.asize = b->d.asize;
		nb->d.aptr = b->d.aptr;
		if(nb->d.aptr != 0)
			dbincref(b->d.aptr);
		for(i = 0; i < nelem(b->d.dptr); i++){
			nb->d.dptr[i] = b->d.dptr[i];
			if(nb->d.dptr[i] != 0)
				dbincref(b->d.dptr[i]);
		}
		for(i = 0; i < nelem(b->d.iptr); i++){
			nb->d.iptr[i] = b->d.iptr[i];
			if(nb->d.iptr[i] != 0)
				dbincref(b->d.iptr[i]);
		}
		memmove(nb->d.embed, b->d.embed, Embedsz);
		nm = nb->mf;
		gmeta(nm, nb->d.embed, Embedsz);
		if((nm->mode&DMDIR) == 0)
			break;
		doff = embedattrsz(nb);
		dupdentries(nb->d.embed+doff, (Embedsz-doff)/sizeof(Dentry));
		/*
		 * no race: caller takes care.
		 */
		if(b->frozen && b->mf->melted == nil){
			incref(nb);
			b->mf->melted = nb;
		}
		break;
	default:
		if(type < DBptr0 || type >= DBptr0 + Niptr)
			fatal("dbdup: bad type %d", type);
		for(i = 0; i < Dptrperblk; i++){
			nb->d.ptr[i] = b->d.ptr[i];
			if(nb->d.ptr[i] != 0)
				dbincref(b->d.ptr[i]);
		}
	}
	changed(nb);
	noerror();
	return nb;
}


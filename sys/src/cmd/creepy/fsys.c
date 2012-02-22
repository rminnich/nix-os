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
 * All the code assumes outofmemoryexits = 1.
 */

Fsys *fs;
int fatalaborts = 1;
uvlong maxfsz;

void
fatal(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vfprint(2, fmt, arg);
	va_end(arg);
	fprint(2, "\n");
	if(fatalaborts)
		abort();
	threadexitsall("fatal");
}

uvlong
now(void)
{
	return nsec();
}

void
okaddr(u64int addr)
{
	if(addr < Dblksz || addr >= fs->limit)
		error("okaddr %#ullx", addr);
}

int
isro(Memblk *f)
{
	return f == fs->archive || f == fs->root || f == fs->cons;
}

/*
 * NO LOCKS. debug only
 */
void
fsdump(int disktoo)
{
	int i;
	Memblk *b;
	u64int a;

	nodebug();
	if(fs != nil){
		print("\n\nfsys '%s' limit %#ullx super m%#p root m%#p:\n",
			fs->dev, fs->limit, fs->super, fs->root);
		print("%H\n", fs->super);
		dfdump(fs->root, disktoo);
		for(b = fs->refs; b != nil; b = b->next)
			print("ref %H\n", b);
		if(1)
			for(i = 0; i < nelem(fs->fhash); i++)
				for(b = fs->fhash[i].b; b != nil; b = b->next)
					print("h[%d] = d%#ullx\n", i, b->addr);
		
	}
	b = fs->super;
	if(b->d.free != 0){
		print("free:");
		for(a = b->d.free; a != 0; a = dbgetref(a))
			print(" d%#ullx", a);
		print("\n");
	}
	print("mru:");
	for(b = fs->mru; b != nil; b = b->lnext)
		print(" d%#ullx", b->addr);
	print("\n");
	fsstats();
	debug();
}

void
fslist(void)
{
	nodebug();
	print("fsys '%s' blksz %ulld maxfsz %ulld:\n",
		fs->dev, fs->super->d.dblksz, maxfsz);
	dflist(fs->root, nil);
	print("\n");
	debug();
}

static usize
disksize(int fd)
{
	Dir *d;
	u64int sz;

	d = dirfstat(fd);
	if(d == nil)
		return 0;
	sz = d->length;
	free(d);
	return sz;
}

static void
freezerefs(void)
{
	Memblk *rb;

	qlock(&fs->rlk);
	for(rb = fs->refs; rb != nil; rb = rb->next)
		rb->frozen = 1;
	qunlock(&fs->rlk);
}

static void
writerefs(void)
{
	Memblk *rb;

	qlock(&fs->rlk);
	for(rb = fs->refs; rb != nil; rb = rb->next)
		meltedref(rb);
	qunlock(&fs->rlk);
}

static Memblk*
readsuper(void)
{
	Memblk *super;

	if(catcherror()){
		error("not a creepy disk: %r");
		error(nil);
	}
	fs->super = dbget(DBsuper, Dblksz);
	super = fs->super;
	if(super->d.magic != MAGIC)
		error("bad magic number");
	if(super->d.dblksz != Dblksz)
		error("bad Dblksz");
	if(super->d.nblkgrpsz != Nblkgrpsz)
		error("bad Nblkgrpsz");
	if(super->d.dminattrsz != Dminattrsz)
		error("bad Dminattrsz");
	if(super->d.ndptr != Ndptr)
		error("bad ndptr");
	if(super->d.niptr != Niptr)
		error("bad niptr");
	if(super->d.dblkdatasz != Dblkdatasz)
		error("bad Dblkdatasz");
	if(super->d.embedsz != Embedsz)
		error("bad Embedsz");
	if(super->d.dptrperblk != Dptrperblk)
		error("bad Dptrperblk");
	noerror();
	return super;
}

static void
freezesuper(void)
{
	Memblk *b;

	b = mbdup(fs->super);
	qlock(fs);
	b->d = fs->super->d;
	assert(fs->fzsuper == nil);
	fs->fzsuper = b;
	fs->fzsuper->frozen = 1;
	qunlock(fs);
}

static void
writezsuper(void)
{
	if(canqlock(&fs->fzlk))
		fatal("writezsuper: lock");
	assert(fs->fzsuper != nil);
	dbwrite(fs->fzsuper);
	dDprint("writezsuper: %H\n", fs->fzsuper);
	mbput(fs->fzsuper);
	fs->fzsuper = nil;
}

/*
 * Write any dirty frozen state after a freeze.
 * Only this function and initialization routines
 * may write to the disk.
 */
static void
fswrite(void)
{
	qlock(&fs->fzlk);
	if(fs->fzsuper == nil)
		fatal("can't fswrite if we didn't fsfreeze");
	if(catcherror()){
		qunlock(&fs->fzlk);
		error(nil);
	}
	writerefs();
	dfsync(fs->archive);
	writezsuper();
	noerror();
	qunlock(&fs->fzlk);
}

/*
 * Freeze the file tree, keeping active as a new melted file
 * that refers to frozen children now in the archive.
 * returns the just frozen tree.
 *
 * This requires two or three free blocks:
 * - one free block to dup the new active
 * - one to freeze the super block
 * -  an extra ref block if the new blocks come from a new block group.
 */
Memblk*
fsfreeze(void)
{
	Memblk *na, *oa, *arch;
	char name[50];

	qlock(&fs->fzlk);
	if(catcherror()){
		/*
		 * There was an error during freeze.
		 * It's better not to continue to prevent disk corruption.
		 * The user is expected to restart from the last frozen
		 * version of the tree.
		 */
		fatal("freeze: %r");
	}
	oa = fs->active;
	arch = fs->archive;
	rwlock(fs->root, Wr);
	rwlock(oa, Wr);
	rwlock(arch, Wr);

	/*
	 * move active into /archive/<epochnb>.
	 */
	seprint(name, name+sizeof(name), "%ulld", oa->d.epoch);
	wname(oa, name, strlen(name)+1);
	dflink(arch, oa);

	/* 1. Freeze the entire previously active.
	 */
	rwunlock(oa, Wr);	/* race */
	dffreeze(oa);
	rwunlock(arch, Wr);

	/* 2. Freeze the on-disk reference counters
	 * and the state of the super-block.
	 */
	freezerefs();
	freezesuper();

	/* 3. Make a new archive and replace the old one.
	 */
	na = dbdup(oa);
	rwlock(na, Wr);
	wname(na, "active", strlen("active")+1);
	fs->active = na;
	dfchdentry(fs->root, oa->addr, na->addr, 1);

	rwunlock(na, Wr);
	rwunlock(fs->root, Wr);
	qunlock(&fs->fzlk);
	noerror();
	return na;
}

static void
fsinit(char *dev, int nblk)
{
	uvlong fact;
	int i;

	maxfsz = Ndptr*Dblkdatasz;
	fact = 1;
	for(i = 0; i < Niptr; i++){
		maxfsz += Dptrperblk * fact;
		fact *= Dptrperblk;
	}

	fs = mallocz(sizeof *fs, 1);
	fs->dev = strdup(dev);
	fs->fd = open(dev, ORDWR);
	if(fs->fd < 0)
		fatal("can't open disk: %r");

	fs->nablk = Fsysmem / sizeof(Memblk);
	if(nblk > 0 && nblk < fs->nablk)
		fs->nablk = nblk;
	fs->limit = disksize(fs->fd);
	fs->limit = fs->limit/Dblksz*Dblksz;
	if(fs->limit < 10*Dblksz)
		fatal("buy a larger disk");
	if(fs->nablk > fs->limit/Dblksz){
		fs->nablk = fs->limit/Dblksz;
		print("%s: using only %uld blocks (small disk)\n", argv0, fs->nablk);
	}
	fs->blk = malloc(fs->nablk * sizeof fs->blk[0]);
	dDprint("fsys '%s' init\n", fs->dev);
}

/*
 * / is only in memory. It's `on-disk' address is Noaddr.
 *
 * /archive is the root on disk.
 * /active is allocated on disk, but not on disk. It will be linked into
 * /archive as a child in the future.
 */
void
fsfmt(char *dev)
{
	Memblk *super;

	fsinit(dev, 16);	/* enough # of blocks for fmt */

	if(catcherror())
		fatal("fsfmt: error: %r");

	fs->super = dballoc(DBsuper);
	super = fs->super;
	super->d.magic = MAGIC;
	super->d.eaddr = fs->super->addr + Dblksz;
	super->d.dblksz = Dblksz;
	super->d.nblkgrpsz = Nblkgrpsz;
	super->d.dminattrsz = Dminattrsz;
	super->d.ndptr = Ndptr;
	super->d.niptr = Niptr;
	super->d.dblkdatasz = Dblkdatasz;
	super->d.embedsz = Embedsz;
	super->d.dptrperblk = Dptrperblk;
	fs->root = dfcreate(nil, "", getuser(), DMDIR|0555);
	rwlock(fs->root, Wr);
	fs->active = dfcreate(fs->root, "active", getuser(), DMDIR|0775);
	fs->archive = dfcreate(fs->root, "archive", getuser(), DMDIR|0555);
	rwunlock(fs->root, Wr);
	super->d.root = fs->archive->addr;
	fsfreeze();
	fswrite();

	noerror();
}

void
fssync(void)
{
	/*
	 * TODO: If active has not changed and we are just going
	 * to dump a new archive for no change, do nothing.
	 */
	dDprint("syncing\n");
	fsfreeze();
	fswrite();
	dDprint("synced\n");
}

/*
 * One process per file system, so consume all the memory
 * for the cache.
 * To open more file systems, use more processes!
 */

void
fsopen(char *dev)
{
	Memblk *arch;
	Memblk *last, *c;
	int i;

	if(catcherror())
		fatal("fsopen: error: %r");

	fsinit(dev, 0);
	readsuper();

	qlock(&fs->fzlk);
	fs->root = dfcreate(nil, "", getuser(), DMDIR|0555);
	arch = dbget(DBfile, fs->super->d.root);
	fs->archive = arch;
	rwlock(fs->root, Wr);
	rwlock(arch, Wr);
	last = nil;
	for(i = 0; (c = dfchild(arch, i)) != nil; i++){
		if(last == nil || last->d.epoch < c->d.epoch){
			mbput(last);
			last = c;
			incref(c);
		}
		mbput(c);
	}
	if(last != nil){
		rwlock(last, Rd);
		fs->active = dbdup(last);
		wname(fs->active, "active", strlen("active")+1);
		rwlock(fs->active, Wr);
		dflink(fs->root, fs->active);
		rwunlock(fs->active, Wr);
		rwunlock(last, Rd);
		mbput(last);
	}else
		fs->active = dfcreate(fs->root, "active", getuser(), DMDIR|0775);
	dflink(fs->root, arch);
	rwunlock(arch, Wr);
	fs->cons = dfcreate(fs->root, "cons", getuser(), DMEXCL|600);
	fs->consc = chancreate(sizeof(char*), 256);
	rwunlock(fs->root, Wr);
	qunlock(&fs->fzlk);
	noerror();
}

uvlong
fsmemfree(void)
{
	uvlong nfree;

	qlock(fs);
	nfree = fs->nablk - fs->nblk;
	nfree += fs->nmfree;
	qunlock(fs);
	return nfree;
}

/*
 * Check if we are low on memory and move some blocks out in that case.
 * This does not acquire locks on blocks, so it's safe to call it while
 * keeping some files/blocks locked.
 */
int
fslowmem(void)
{
	int type;
	ulong n, tot;
	Memblk *b, *bprev;

	if(fsmemfree() > Mminfree)
		return 0;
	dDprint("low on memory\n");
	tot = 0;
	do{
		if(fsmemfree() > Mmaxfree)
			break;
		qlock(&fs->fzlk);
		qlock(&fs->llk);
		if(catcherror()){
			qunlock(&fs->llk);
			qunlock(&fs->fzlk);
			fprint(2, "%s: fslowmem: %r\n", argv0);
			break;
		}
	Again:
		n = 0;
		for(b = fs->lru; b != nil && tot < Mmaxfree; b = bprev){
			bprev = b->lprev;
			type = TAGTYPE(b->d.tag);
			switch(type){
			case DBfree:
				goto Again;
			case DBsuper:
			case DBref:
				dDprint("out: ignored: m%#p\n", b);
				continue;
			case DBfile:
				if(b == fs->root || b == fs->active || b == fs->archive){
					dDprint("out: ignored: m%#p\n", b);
					continue;
				}
				break;
			}
			if(b->dirty || b->ref > 1){
				dDprint("out: ignored: m%#p\n", b);
				continue;
			}
			/*
			 * Blocks here have one ref because of the hash table,
			 * which means they are are not used.
			 * We release the hash ref to let them go.
			 * bprev might move while we put b, but it would
			 * only go to another place in the lru list, or to
			 * the free list, but that's ok.
			 */
			qunlock(&fs->llk);
			dDprint("block out: m%#p d%#ullx\n", b, b->addr);
			mbput(b);
			qlock(&fs->llk);
			n++;
			tot++;
		}
		noerror();
		qunlock(&fs->llk);
		qunlock(&fs->fzlk);
	}while(n > 0);
	if(tot == 0)
		fprint(2, "%s: low mem and everything in use or dirty.\n", argv0);
	else
		dDprint("out: %uld blocks\n", tot);
	return 1;
}

static uvlong
fsdiskfree(void)
{
	uvlong nfree;

	qlock(fs);
	nfree = fs->super->d.ndfree;
	nfree += (fs->limit - fs->super->d.eaddr)/Dblksz;
	qunlock(fs);
	return nfree;
}

/*
 * Freeze requires 3 free blocks, but we declare the fs full
 * when less that Dzerofree are avail, to prevent freeze from
 * failing should we made a mistake counting 1, 2, 3.
 */
int
fsfull(void)
{
	if(fsdiskfree() > Dzerofree)
		return 0;

	if(1){
		fprint(2, "file system full:\n");
		fsdump(0);
		fatal("aborting");
	}
	return 1;
}

/*
 * This should be called if fs->super->d.nfree < some number.
 */
int
fsreclaim(void)
{
	Memblk *arch, *c, *victim;
	int i;
	u64int addr;
	Blksl sl;
	Dentry *de;
	ulong n, tot;

	if(fsdiskfree() > Dminfree)
		return 0;

	fprint(2, "%s: low on disk: reclaiming...\n", argv0);
	qlock(&fs->fzlk);
	arch = fs->archive;
	rwlock(arch, Wr);
	if(catcherror()){
		rwunlock(arch, Wr);
		qunlock(&fs->fzlk);
		error(nil);
	}
	tot = 0;
	for(;;){
		if(fsdiskfree() > Dmaxfree){
			dDprint("fsreclaim: got >= %d free\n", Dmaxfree);
			break;
		}
		dDprint("fsreclaim: reclaiming\n");
		victim = nil;
		for(i = 0; (c = dfchild(arch, i)) != nil; i++){
			if(victim == nil)
				victim = c;
			else if(victim->d.epoch > c->d.epoch){
				mbput(victim);
				victim = c;
			}else
				mbput(c);

		}
		if(i < 2){
			mbput(victim);
			dDprint("nothing to reclaim\n");
			break;
		}
		fprint(2, "%s: reclaiming /archive/%s\n", argv0, victim->mf->name);
		dDprint("victim is %H\n", victim);

		/*
		 * Don't make a new archive. Edit in-place the one we have to
		 * clear the reference to the victim.
		 */
		addr = dfchdentry(arch, victim->addr, 0, 0);
		assert(addr != Noaddr);
		sl = dfslice(arch, sizeof(Dentry), addr, 0);
		assert(sl.b);
		if(catcherror()){
			mbput(sl.b);
			error(nil);
		}
		de = sl.data;
		de->file = 0;
		dbwrite(sl.b);
		noerror();
		mbput(sl.b);

		n = dbgetref(victim->addr);
		if(n != 1)
			fatal("reclaim: victim disk ref is %d != 1", n);

		fs->super->d.root = fs->archive->addr;

		n = dfreclaim(victim);
		mbput(victim);
		dDprint("%uld block%s reclaimed\n", n, n?"s":"");
		tot += n;

		freezerefs();
		writerefs();
		freezesuper();
		writezsuper();
	}
	if(tot > 0)
		fprint(2, "%s: %uld block%s reclaimed\n", argv0, tot, tot?"s":"");
	rwunlock(arch, Wr);
	qunlock(&fs->fzlk);
	noerror();
	return 1;
}

void
fspolicy(void)
{
	/*
	 * If low on memory, move some blocks out.
	 * Otherwise, reclaim old snapshots if low on disk.
	 */
	if(fslowmem())
		fssync();
	else
		fsreclaim();
}


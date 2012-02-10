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

Fsys *fs;

/*
 * All the code assumes outofmemoryexits = 1.
 */

int
iserror(char *s)
{
	char err[128];

	rerrstr(err, sizeof err);
	return strstr(err, s) != nil;
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

/*
 * NO LOCKS. debug only
 */
void
fsdump(void)
{
	int i, flg;
	Memblk *b;
	u64int a;

	if(fs != nil){
		print("\n\nfsys '%s' limit %#ulx super m%#p root m%#p:\n",
			fs->dev, fs->limit, fs->super, fs->root);
		for(i = 0; i < nelem(fs->fhash); i++)
			for(b = fs->fhash[i].b; b != nil; b = b->next)
				print("h[%d] = %H", i, b);
		print("nblk %uld nablk %uld used %uld free %uld\n",
			fs->nblk, fs->nablk, fs->nused, fs->nfree);
	}
	b = fs->super;
	if(b->d.free != 0){
		print("free:");
		flg = dbg['D'];
		dbg['D'] = 0;
		for(a = b->d.free; a != 0; a = dbgetref(a))
			print(" d%#ullx", a);
		dbg['D'] = flg;
		print("\n");
	}
	print("Fsysmem\t= %uld\n", Fsysmem);
	print("Dminfree\t= %d\n", Dminfree);
	print("Dblksz\t= %uld\n", Dblksz);
	print("Dminattrsz\t= %uld\n", Dminattrsz);
	print("Nblkgrpsz\t= %uld\n", Nblkgrpsz);
	print("Dblkdatasz\t= %d\n", Dblkdatasz);
	print("Embedsz\t= %d\n", Embedsz);
	print("Dentryperblk\t= %d\n", Dentryperblk);
	print("Dptrperblk\t= %d\n\n", Dptrperblk);
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

	qlock(fs);
	for(rb = fs->refs; rb != nil; rb = rb->next)
		rb->frozen = 1;
	qunlock(fs);
}

static void
writerefs(void)
{
	Memblk *rb;

	qlock(fs);
	for(rb = fs->refs; rb != nil; rb = rb->next)
		meltedref(rb);
	qunlock(fs);
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
writesuper(void)
{
	qlock(fs);
	assert(fs->fzsuper != nil);
	qunlock(fs);
	dbwrite(fs->fzsuper);
	dDprint("fswrite: %H", fs->fzsuper);
	mbput(fs->fzsuper);
	fs->fzsuper = nil;
}

/*
 * Write any dirty frozen state after a freeze.
 * Only this function and initialization of previously unused DBref
 * blocks may write to the disk.
 */
static void
fswrite(void)
{
	if(fs->fzsuper == nil)
		sysfatal("can't fswrite if we didn't fsfreeze");
	writerefs();
	dfsync(fs->archive);
	writesuper();
}

/*
 * Freeze the file tree, keeping active as a new melted file
 * that refers to frozen children now in the archive.
 * returns the just frozen tree.
 */
Memblk*
fsfreeze(void)
{
	Memblk *na, *oa, *arch, *super;
	Child *ac;
	char name[50];
	int i;

	wlock(fs->active->mf);
	wlock(fs->archive->mf);
	if(fs->fzsuper != nil){
		wunlock(fs->archive->mf);
		wunlock(fs->active->mf);
		error("freeze already in progress");
	}
	dfloaddir(fs->active, 1);
	dfloaddir(fs->archive, 1);
	super = fs->super;
	if(catcherror()){
		/*
		 * Freeze can't fail. If it does, we better
		 * restart the file system from the last known
		 * frozen tree.
		 * The only reasing this should happen is because
		 * we run out of memory, or out of disk, or
		 * the disk fails.
		 */
		sysfatal("freeze: can't recover: %r");
	}

	/*
	 * move active into /archive/<epochnb> and create a new melted
	 * active.
	 */
	oa = fs->active;
	na = dbdup(oa);
	wlock(na->mf);
	seprint(name, name+sizeof(name), "%ulld", oa->d.epoch);
	dfwattr(oa, "name", name, strlen(name)+1);
	ac = fs->root->mf->child;
	assert(ac->f == oa);
	ac->f = na;		/* keeps the ref we have */
	ac->d->file = na->addr;
	if(fs->archive->frozen){
		arch = dbdup(fs->archive);
		wlock(arch->mf);
		wunlock(fs->archive->mf);
		mbput(fs->archive);
		fs->archive = arch;
		for(i = nelem(super->d.root)-1; i > 0; i--)
			super->d.root[i] = super->d.root[i-1];
		super->d.root[0] = fs->archive->addr;
	}
	dflink(fs->archive, oa);
	fs->active = na;
	fs->archive->frozen = 1;		/* for fsfmt */

	/* 1. Free the entire previously active
	 */
	dffreeze(oa);
	wunlock(oa->mf);

	/* 2. Freeze whatever new blocks are found in archive
	 */
	dffreeze(fs->archive);

	/* 3. Freeze the on-disk reference counters
	 * and the state of the super-block.
	 */
	freezerefs();
	freezesuper();

	/*
	/* 4. release locks, all done.
	 */
	wunlock(na->mf);
	wunlock(fs->archive->mf);
	noerror();
	return na;
}

static void
fsinit(char *dev, int nblk)
{
	fs = mallocz(sizeof *fs, 1);
	fs->dev = strdup(dev);
	fs->fd = open(dev, ORDWR);
	if(fs->fd < 0)
		sysfatal("can't open disk: %r");

	fs->nablk = Fsysmem / sizeof(Memblk);
	if(nblk > 0 && nblk < fs->nablk)
		fs->nablk = nblk;
	fs->limit = disksize(fs->fd);
	if(fs->nablk > fs->limit/Dblksz)
		fs->nablk = fs->limit/Dblksz;
	fs->limit = fs->nablk * Dblksz;
	if(fs->limit < 10*Dblksz)
		sysfatal("disk is ridiculous");
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

	if(catcherror())
		sysfatal("fsfmt: error: %r");

	fsinit(dev, 16);	/* enough # of blocks for fmt */

	fs->super = dballoc(DBsuper);
	super = fs->super;
	super->d.eaddr = fs->super->addr + Dblksz;
	super->d.dblksz = Dblksz;
	super->d.nblkgrpsz = Nblkgrpsz;
	super->d.dminattrsz = Dminattrsz;

	fs->root = dfcreate(nil, "", getuser(), DMDIR|0555);
	fs->active = dfcreate(fs->root, "active", getuser(), DMDIR|0775);
	fs->archive = dfcreate(fs->root, "archive", getuser(), DMDIR|0555);
	super->d.root[0] = fs->archive->addr;

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
	fsfreeze();
	fswrite();
}

static Memblk*
readsuper(void)
{
	Memblk *super;

	fs->super = dbget(DBsuper, Dblksz);
	super = fs->super;
	if(super->d.dblksz != Dblksz)
		error("bad Dblksz");
	if(super->d.nblkgrpsz != Nblkgrpsz)
		error("bad Nblkgrpsz");
	if(super->d.dminattrsz != Dminattrsz)
		error("bad Dminattrsz");
	return super;
}

/*
 * One process per file system, so consume all the memory
 * for the cache.
 * To open more file systems, use more processes!
 */

void
fsopen(char *dev)
{
	if(catcherror())
		sysfatal("fsopen: error: %r");

	fsinit(dev, 0);

	readsuper();

	fs->root = dfcreate(nil, "", getuser(), DMDIR|0555);
	fs->active = dfcreate(fs->root, "active", getuser(), DMDIR|0775);
	fs->archive = dbget(DBfile, fs->super->d.root[0]);
	wlock(fs->root->mf);
	wlock(fs->archive->mf);
	dflink(fs->root, fs->archive);
	wunlock(fs->archive->mf);
	wunlock(fs->root->mf);
	noerror();
}

/*
 * XXX: must revisit here:
 * - there are several things been done multiple times in different
 * functions. e.g., writing the super, compare
 *	fsfmt and fsopen
 *	fsreclaim and fsfreeze.
 * some inner functions are missing there.
 *
 * - it's not clear references end up ok after fsreclaim, must test that.
 * - perhaps we should reclaim in a loop until we are sure that
 * at least a min # of blocks are available or we can't reclaim anything else,
 * whatever happens first.
 *
 * - must implement a variant of the fsfmt function that reads an archived
 * tree and formats the file system according to it. Although that's
 * perhaps just a fsfmt() followed by the inverse o fthe archival tool,
 * and we may leave fsfmt alone.
 */

/*
 * This should be called if fs->super->d.nfree < some number.
 */
void
fsreclaim(void)
{
	uvlong nfree;
	Child *c, *victim;
	Memblk *arch, *gone;
	int i, n, tot;

	tot = 0;
	for(;;){
		qlock(fs);
		nfree = fs->super->d.nfree;
		nfree += (fs->limit - fs->super->d.eaddr)/Dblksz;
		qunlock(fs);
		if(nfree > Dminfree){
			dDprint("fsreclaim: got %ulld free\n", nfree);
			break;
		}
		dDprint("fsreclaim: reclaiming: %ulld free\n", nfree);
		arch = fs->archive;
		wlock(arch->mf);
		dfloaddir(arch, 1);
		if(arch->mf->nchild < 2){
			wunlock(arch->mf);
			dDprint("nothing to reclaim\n");
			break;
		}
		victim = arch->mf->child;
		for(i = 0; i < arch->mf->nchild; i++){
			c = &arch->mf->child[i];
			if(victim->f->d.epoch > c->f->d.epoch)
				victim = c;
		}
	
		gone = victim->f;
		fprint(2, "%s: reclaiming /archive/%s\n", argv0, gone->mf->name);
		dDprint("victim is %H", gone);
	
		/*
		 * Surgery: we don't want to allocate anything by now.
		 * Just clear the references on disk and memory to the victim.
	 	 * If we fail before finishing then RCs will be >= the
		 * value they should have (the reference is gone from disk).
		 */
		victim->d->file = 0;
		dbwrite(victim->b);
		delchild(arch, victim);
		wunlock(arch->mf);
	
		n = dbgetref(gone->addr);
		if(n != 1)
			sysfatal("reclaim: gone ref is %d != 1", n);
		n = dfreclaim(gone);
		dDprint("%d block%s reclaimed\n", n, n?"s":"");
		tot += n;

		/*
		 * Hopefully we have some free blocks.
		 * dump the reference blocks to disk.
		 * Gone blocks are in the free list, active blocks may end up
		 * with on-disk refs >= those matching the references on disk,
		 * the next snap will make the ref list coherent.
		 * We don't snap here because that is likely to allocate more
		 * blocks.
		 */
		freezerefs();
		writerefs();
		freezesuper();
		dDprint("fsreclaim: %H", fs->fzsuper);
		writesuper();
	}
	if(tot > 0)
		fprint(2, "%s: %d block%s reclaimed\n", argv0, tot, tot?"s":"");
	
}

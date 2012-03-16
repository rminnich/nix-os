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

enum
{
	Lru = 0,
	Freeze,
	Write,
	Nfsops,
};

Fsys *fs;
uvlong maxfsz;

vlong fsoptime[Nfsops];
ulong nfsopcalls[Nfsops];

static char* fsopname[] =
{
[Lru]		"lru",
[Freeze]	"freeze",
[Write]		"write",
};

char statstext[Statsbufsz], *statsp;

void
quiescent(int y)
{
	if(y == No)
		xrwlock(&fs->quiescence, Rd);
	else
		xrwunlock(&fs->quiescence, Rd);
}

static uvlong
fsdiskfree(void)
{
	uvlong nfree;

	xqlock(fs);
	nfree = fs->super->d.ndfree;
	nfree += (fs->limit - fs->super->d.eaddr)/Dblksz;
	xqunlock(fs);
	return nfree;
}

static char*
fsstats(char *s, char *e, int clr)
{
	int i;

	s = seprint(s, e, "mblks:\t%4uld nblk %4uld nablk %4uld mused %4uld mfree\n",
		fs->nblk, fs->nablk, fs->nmused, fs->nmfree);
	s = seprint(s, e, "lists:\t%4uld lru %#4uld dirty %#4uld refs %4uld total\n",
		fs->lru.n, fs->mdirty.n, fs->refs.n,
		fs->lru.n + fs->mdirty.n + fs->refs.n);
	s = seprint(s, e, "dblks:\t %4ulld dtot %4ulld dfree (%ulld list + %ulld rem)\n",
		fs->limit/Dblksz - 1, fsdiskfree(), fs->super->d.ndfree,
		(fs->limit - fs->super->d.eaddr)/Dblksz);
	s = seprint(s, e, "paths:\t%4uld alloc %4uld free (%4uld bytes)\n",
		pathalloc.nalloc, pathalloc.nfree, pathalloc.elsz);
	s = seprint(s, e, "mfs:\t%4uld alloc %4uld free (%4uld bytes)\n",
		mfalloc.nalloc, mfalloc.nfree, mfalloc.elsz);
	s = seprint(s, e, "nmelts:\t%d\n", fs->nmelts);
	s = seprint(s, e, "nindirs:\t");
	for(i = 0; i < nelem(fs->nindirs); i++){
		s = seprint(s, e, "%d ", fs->nindirs[i]);
		if(clr)
			fs->nindirs[i] = 0;
	}
	s = seprint(s, e, "\n");
	s = seprint(s, e, "\n");
	s = seprint(s, e, "Fsysmem:\t%uld\n", Fsysmem);
	s = seprint(s, e, "Mzerofree:\t%d\tMminfree:\t%d\tMmaxfree:\t%d\n",
		Mzerofree, Mminfree, Mmaxfree);
	s = seprint(s, e, "Dzerofree:\t%d\tDminfree:\t%d\tDmaxfree:\t%d\n",
		Dzerofree, Dminfree, Dmaxfree);
	s = seprint(s, e, "Mmaxdirtypcent:\t%d\n", Mmaxdirtypcent);
	s = seprint(s, e, "Dblksz:  \t%uld\n", Dblksz);
	s = seprint(s, e, "Mblksz:  \t%ud\n", sizeof(Memblk));
	s = seprint(s, e, "Dminattrsz:\t%uld\n", Dminattrsz);
	s = seprint(s, e, "Nblkgrpsz:\t%uld\n", Nblkgrpsz);
	s = seprint(s, e, "Dblkdatasz:\t%d\n", Dblkdatasz);
	s = seprint(s, e, "Embedsz:\t%d\n", Embedsz);
	s = seprint(s, e, "Dentryperblk:\t%d\n", Dblkdatasz/Daddrsz);
	s = seprint(s, e, "Dptrperblk:\t%d\n\n", Dptrperblk);

	for(i = 0; i < nelem(nfsopcalls); i++){
		if(nfsopcalls[i] == 0)
			s = seprint(s, e, "%s:\t0 calls\t0 µs\n", fsopname[i]);
		else
			s = seprint(s, e, "%s:\t%uld calls\t%ulld µs\n", fsopname[i],
				nfsopcalls[i], (fsoptime[i]/nfsopcalls[i])/1000);
		if(clr){
			nfsopcalls[i] = 0;
			fsoptime[i] = 0;
		}
	}
	return s;
}

char*
updatestats(int clr)
{
	static QLock statslk;

	if(clr)
		fprint(2, "%s: clearing stats\n", argv0);
	xqlock(&statslk);
	statsp = statstext;
	*statsp = 0;
	statsp = fsstats(statsp, statstext+sizeof statstext, clr);
	statsp = ninestats(statsp, statstext+sizeof statstext, clr);
	statsp = ixstats(statsp, statstext+sizeof statstext, clr);
	xqunlock(&statslk);
	return statstext;
}

int
isro(Memblk *f)
{
	return f == fs->archive || f == fs->root || f == fs->cons || f == fs->stats;
}

/*
 * NO LOCKS. debug only
 *
 */
void
fsdump(int full, int disktoo)
{
	int i, n, x;
	Memblk *b;
	daddrt a;
	extern int fullfiledumps;

	x = fullfiledumps;
	fullfiledumps = full;
	nodebug();
	if(fs != nil){
		fprint(2, "\n\nfsys '%s' limit %#ullx super m%#p root m%#p:\n",
			fs->dev, fs->limit, fs->super, fs->root);
		fprint(2, "%H\n", fs->super);
		dfdump(fs->root, disktoo);
		mlistdump("refs", &fs->refs);
		if(1){
			n = 0;
			fprint(2, "hash:");
			for(i = 0; i < nelem(fs->fhash); i++)
				for(b = fs->fhash[i].b; b != nil; b = b->next){
					if(n++ % 5 == 0)
						fprint(2, "\n\t");
					fprint(2, "d%#010ullx ", EP(b->addr));
				}
			fprint(2, "\n");
		}
	}
	if(fs->super->d.free != 0){
		fprint(2, "free:");
		i = 0;
		for(a = fs->super->d.free; a != 0; a = dbgetref(a)){
			if(i++ % 5 == 0)
				fprint(2, "\n\t");
			fprint(2, "d%#010ullx ", EP(a));
		}
		fprint(2, "\n");
	}
	mlistdump("mru", &fs->lru);
	mlistdump("dirty", &fs->mdirty);
	fprint(2, "%s\n", updatestats(0));
	fullfiledumps = x;
	debug();
}

/*
 * Failed checks are reported but not fixed (but for leaked blocks).
 * The user is expected to format the partition and restore contents from venti.
 * We might easily remove the dir entries for corrupt files, and restore
 */
int
fscheck(void)
{
	long i;
	daddrt n, addr;
	long nfails;

	xqlock(&fs->fzlk);
	xrwlock(&fs->quiescence, Wr);
	nfails = 0;
	if(fs->chk == nil)
		fs->chk = mallocz(fs->ndblk, 1);
	else
		memset(fs->chk, 0, fs->ndblk);
	if(catcherror()){
		xrwunlock(&fs->quiescence, Wr);
		xqunlock(&fs->fzlk);
		fprint(2, "fscheck: %r\n");
		nfails++;
		return nfails;
	}

	fprint(2, "%s: checking %s...\n", argv0, fs->dev);
	nfails += dfcountrefs(fs->root);
	dprint("countfree...\n");
	dfcountfree();

	dprint("checks...\n");
	for(addr = 0; addr < fs->super->d.eaddr; addr += Dblksz){
		i = addr/Dblksz;
		if(fs->chk[i] == 0){
			fprint(2, "fscheck: d%#010ullx: leak\n", addr);
			if(!catcherror()){
				dbsetref(addr, fs->super->d.free);
				fs->super->d.free = addr;
				noerror();
			}else{
				fprint(2, "%s: check: %r\n", argv0);
				nfails++;
			}
			continue;
		}
		if(fs->chk[i] == 0xFF)
			continue;
		n = dbgetref(addr);
		if(fs->chk[i] == 0xFE && n < (daddrt)0xFE){
			fprint(2, "fscheck: d%#010ullx: found >%ud != ref %ulld\n",
				addr, fs->chk[i], n);
			nfails++;
		}
		if(fs->chk[i] < 0xFE && n != fs->chk[i]){
			fprint(2, "fscheck: d%#010ullx: found %ud != ref %ulld\n",
				addr, fs->chk[i], n);
			nfails++;
		}
	}
	xrwunlock(&fs->quiescence, Wr);
	xqunlock(&fs->fzlk);
	noerror();
	fprint(2, "%s: %s check complete\n", argv0, fs->dev);
	return nfails;
}

static daddrt
disksize(int fd)
{
	Dir *d;
	daddrt sz;

	d = dirfstat(fd);
	if(d == nil)
		return 0;
	sz = d->length;
	free(d);
	return sz;
}

/*
 * To preserve coherency, blocks written are always frozen.
 * DBref blocks with RCs and the free block list require some care:
 *
 * On disk, the super block indicates that even (odd) DBref blocks are active.
 * On memory, the super selects even (odd) refs (we read refs from there.)
 * To sync...
 * 1. we make a frozen super to indicate that odd (even) DBrefs are active.
 * 2. we write odd (even) DBref blocks.
 * 3. the frozen super is written, indicating that odd (even) refs are in use.
 *    (The disk is coherent now, pretending to use odd (even) refs).
 * 4. The memory super is udpated to select odd (even) DBref blocks.
 *    (from now on, we are loading refs from odd (even) blocks.
 * 5. we update even (odd) DBref blocks, so we can get back to 1.
 *    with even/odd swapped.
 *
 */

static void
freezesuperrefs(void)
{
	Memblk *b, *rb;

	b = mballoc(fs->super->addr);
	xqlock(fs);
	b->type = fs->super->type;
	b->d = fs->super->d;
	b->d.oddrefs = !fs->super->d.oddrefs;
	assert(fs->fzsuper == nil);
	fs->fzsuper = b;
	b->frozen = 1;
	b->dirty = 1;	/* so it's written */
	xqlock(&fs->refs);
	for(rb = fs->refs.hd; rb != nil; rb = rb->lnext){
		rb->frozen = 1;
		rb->changed = rb->dirty;
	}
	xqunlock(&fs->refs);
	xqunlock(fs);
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

/*
 * Freeze the file tree, keeping active as a new melted file
 * that refers to frozen children now in the archive.
 * returns the just frozen tree or nil
 *
 * This requires two or three free blocks:
 * - one free block to dup the new active
 * - one to freeze the super block
 * -  an extra ref block if the new blocks come from a new block group.
 */
static Memblk*
fsfreeze(void)
{
	Memblk *na, *oa, *arch;
	char name[50];
	vlong t0;
	u64int id;

	dprint("freezing fs...\n");
	t0 = nsec();
	xqlock(&fs->fzlk);
	if(fs->fzsuper != nil){
		/*
		 * we did freeze/reclaim and are still writing, can't freeze now.
		 */
		xqunlock(&fs->fzlk);
		return nil;
	}
	xrwlock(&fs->quiescence, Wr);	/* not really required */
	nfsopcalls[Freeze]++;
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
	seprint(name, name+sizeof(name), "%ulld", oa->d.mtime);
	wname(oa, name, strlen(name)+1);
	dflink(arch, oa);

	/* 1. Freeze the entire previously active.
	 */
	oa->d.mtime = t0;
	oa->d.atime = t0;
	rwunlock(oa, Wr);	/* race */
	changed(oa);
	dffreeze(oa);
	rwunlock(arch, Wr);

	/* 2. Freeze the on-disk reference counters
	 * and the state of the super-block.
	 */
	dprint("freezing refs...\n");
	freezesuperrefs();

	/* 3. Make a new active and replace the old one.
	 */
	na = dbdup(oa);
	rwlock(na, Wr);
	id = nsec();
	na->d.id = nsec();
	wname(na, "active", strlen("active")+1);

	fs->active = na;

	dfchdentry(fs->root, oa->addr, na->addr, Mkit);

	assert(oa->ref > 1);	/* release fs->active */
	mbput(oa);

	rwunlock(na, Wr);
	rwunlock(fs->root, Wr);

	/* 4. Try to advance fids within active to their
	 * most recent melted files, to release refs to old frozen files.
	 */
	meltfids();

	fsoptime[Freeze] += nsec() - t0;
	xrwunlock(&fs->quiescence, Wr);
	xqunlock(&fs->fzlk);
	noerror();
	return na;
}

static long
writerefs(void)
{
	Memblk *rb;
	long n;

	n = 0;
	xqlock(&fs->refs);
	for(rb = fs->refs.hd; rb != nil; rb = rb->lnext){
		if(rb->dirty && rb->frozen)
			n++;
		meltedref(rb);
	}
	xqunlock(&fs->refs);
	return n;
}

static int
mustwrite(Memblk *b)
{
	return b->frozen != 0 || b == fs->archive || b->aflag != 0;
}

/*
 * Written blocks become mru, perhaps we should
 * consider keeping their location in the lru list, at the
 * expense of visiting them while scanning for blocks to move out.
 * We write only (dirty) blocks that are frozen or part of the "/archive" file.
 */
static long
writedata(void)
{
	Memblk *b;
	long nw;
	List dl;

	nw = 0;
	dl = mfilter(&fs->mdirty, mustwrite);
	while((b = dl.hd) != nil){
		munlink(&dl, b, 1);
		assert(b->dirty);
		if((b->addr&Fakeaddr) != 0)
			fatal("write data on fake address");
		dbwrite(b);
		nw++;
	}
	return nw;
}

static void
writezsuper(void)
{
	if(canqlock(&fs->fzlk))
		fatal("writezsuper: lock");
	assert(fs->fzsuper != nil);
	dbwrite(fs->fzsuper);
	dprint("writezsuper: %H\n", fs->fzsuper);
	mbput(fs->fzsuper);
	fs->fzsuper = nil;
}

static void
syncref(daddrt addr)
{
	static Memblk b;

	b.addr = addr;
	b.type = DBref;
	dbread(&b);
	if(fs->super->d.oddrefs == 0) /* then the old ones are odd */
		addr += Dblksz;
	dWprint("syncref d%#010ullx at d%#010ullx\n", b.addr, addr);
	if(pwrite(fs->fd, &b.d, sizeof b.d, addr) != sizeof b.d)
		error("syncref: write: %r");
}

static void
syncrefs(void)
{
	Memblk *rb;

	fs->super->d.oddrefs = !fs->super->d.oddrefs;
	xqlock(&fs->refs);
	rb = fs->refs.hd;
	xqunlock(&fs->refs);
	for(; rb != nil; rb = rb->lnext){
		if(rb->changed)
			syncref(rb->addr);
		rb->changed = 0;
	}
}


/*
 * Write any dirty frozen state after a freeze.
 * Only this function and initialization routines (i.e., super, refs)
 * may lead to writes.
 */
static void
fswrite(void)
{
	vlong t0;
	long nr, nb;

	dprint("writing fs...\n");
	t0 = nsec();
	xqlock(&fs->fzlk);
	nfsopcalls[Write]++;
	if(fs->fzsuper == nil)
		fatal("can't fswrite if we didn't fsfreeze");
	if(catcherror()){
		fsoptime[Write] += nsec() - t0;
		xqunlock(&fs->fzlk);
		error(nil);
	}
	nr = writerefs();
	nb = writedata();
	writezsuper();
	nb++;
	syncrefs();
	noerror();
	fs->wtime = nsec();
	fsoptime[Write] += fs->wtime - t0;
	xqunlock(&fs->fzlk);
	dprint("fs written (2*%ld refs %ld data)\n", nr, nb);
}

static void
fsinit(char *dev, int nblk)
{
	uvlong fact;
	int i;

	/* this is an invariant that must hold for directories */
	assert(Embedsz % Daddrsz == 0);

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
	fs->ndblk = fs->limit/Dblksz;
	fs->limit = fs->ndblk*Dblksz;
	if(fs->limit < 10*Dblksz)
		fatal("buy a larger disk");
	if(fs->nablk > fs->ndblk){
		fprint(2, "%s: using %uld blocks and not %uld (small disk)\n",
			argv0, fs->ndblk, fs->nablk);
		fs->nablk = fs->ndblk;
	}
	fs->blk = malloc(fs->nablk * sizeof fs->blk[0]);
	dprint("fsys '%s' init\n", fs->dev);
}

void
fssync(void)
{
	if(fsfreeze())
		fswrite();
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
	int uid;

	fsinit(dev, Mmaxfree);	/* enough # of blocks for fmt */

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
	uid = usrid(getuser());
	fs->root = dfcreate(nil, "", uid, DMDIR|0555);
	rwlock(fs->root, Wr);
	fs->active = dfcreate(fs->root, "active", uid, DMDIR|0775);
	fs->archive = dfcreate(fs->root, "archive", uid, DMDIR|0555);
	fs->archive->aflag = 1;
	rwunlock(fs->root, Wr);
	super->d.root = fs->archive->addr;
	fssync();

	noerror();
}

/*
 * One process per file system, so consume all the memory
 * for the cache.
 * To open more file systems, use more processes!
 */
void
fsopen(char *dev)
{
	Memblk *arch, *last, *c;
	int i, uid;

	if(catcherror())
		fatal("fsopen: error: %r");

	fsinit(dev, 0);
	readsuper();

	uid = usrid("sys");
	xqlock(&fs->fzlk);
	fs->root = dfcreate(nil, "", uid, DMDIR|0555);
	arch = dbget(DBfile, fs->super->d.root);
	arch->aflag = 1;
	fs->archive = arch;
	rwlock(fs->root, Wr);
	rwlock(arch, Wr);
	last = nil;
	for(i = 0; (c = dfchild(arch, i)) != nil; i++){
		if(last == nil || last->d.mtime < c->d.mtime){
			mbput(last);
			last = c;
			incref(c);
		}
		mbput(c);
	}
	if(last != nil){
		rwlock(last, Rd);
		fs->active = dbdup(last);
		mbput(last->mf->melted);	/* could keep it, but no need */
		last->mf->melted = nil;
		wname(fs->active, "active", strlen("active")+1);
		fs->active->d.id = nsec();
		rwlock(fs->active, Wr);
		dflink(fs->root, fs->active);
		rwunlock(fs->active, Wr);
		rwunlock(last, Rd);
		mbput(last);
	}else
		fs->active = dfcreate(fs->root, "active", uid, DMDIR|0775);
	dflink(fs->root, arch);
	rwunlock(arch, Wr);
	fs->cons = dfcreate(nil, "cons", uid, DMEXCL|0660);
	fs->cons->d.gid = usrid("adm");
	fs->cons->mf->gid = "adm";
	fs->stats = dfcreate(nil, "stats", uid, 0664);
	changed(fs->cons);
	fs->consc = chancreate(sizeof(char*), 256);
	rwunlock(fs->root, Wr);
	xqunlock(&fs->fzlk);

	noerror();

	/*
	 * Try to load the /active/users file, if any,
	 * but ignore errors. We already have a default table loaded
	 * and may operate using it.
	 */
	if(!catcherror()){
		c = dfwalk(fs->active, "users", Rd);
		rwlock(c, Wr);
		if(catcherror()){
			rwunlock(c, Wr);
			mbput(c);
			error(nil);
		}
		rwusers(c);
		noerror();
		rwunlock(c, Wr);
		mbput(c);
		noerror();
		fs->cons->d.uid = usrid(getuser());
		fs->cons->mf->uid = getuser();
	}
}

uvlong
fsmemfree(void)
{
	uvlong nfree;

	xqlock(fs);
	nfree = fs->nablk - fs->nblk;
	nfree += fs->nmfree;
	xqunlock(fs);
	return nfree;
}

/*
 * Check if we are low on memory and move some blocks out in that case.
 * This does not acquire locks on blocks, so it's safe to call it while
 * keeping some files/blocks locked.
 */
int
fslru(void)
{
	Memblk *b, *bprev;
	vlong t0;
	int x;
	long target, tot, n, ign;

	x = setdebug();
	dprint("fslru: low on memory %ulld free %d min\n", fsmemfree(), Mminfree);
	tot = ign = 0;
	do{
		target = Mmaxfree - fsmemfree();
		t0 = nsec();
		xqlock(&fs->lru);
		nfsopcalls[Lru]++;
		if(catcherror()){
			fsoptime[Lru] += t0 - nsec();
			xqunlock(&fs->lru);
			fprint(2, "%s: fslru: %r\n", argv0);
			break;
		}
		n = 0;
		for(b = fs->lru.tl; b != nil && target > 0; b = bprev){
			bprev = b->lprev;
			if(b->dirty)
				fatal("fslru: dirty block on lru\n");
			switch(b->type){
			case DBfree:
				/* can happen. but, does it? */
				fatal("fslru: DBfree on lru\n", argv0);
			case DBsuper:
			case DBref:
				fatal("fslru: type %d found on lru\n", b->type);
			case DBfile:
				if(b == fs->root || b == fs->active || b == fs->archive){
					ign++;
					continue;
				}
				break;
			}
			if(b->ref > 1){
				ign++;
				continue;
			}
			/*
			 * Blocks here have one ref because of the hash table,
			 * which means they are are not used.
			 * We release the hash ref to let them go.
			 * bprev can't move while we put b.
			 */
			dOprint("fslru: out: m%#p d%#010ullx\n", b, b->addr);
			mbunhash(b, 1);
			n++;
			tot++;
			target--;
		}
		noerror();
		fsoptime[Lru] += t0 - nsec();
		xqunlock(&fs->lru);
	}while(n > 0 && target > 0);
	if(tot == 0){
		fprint(2, "%s: low on mem (0 out; %uld ignored)\n", argv0, ign);
		tot = -1;
	}else
		dprint("fslru: %uld out %uld ignored %ulld free %d min %d max\n",
			tot, ign, fsmemfree(), Mminfree, Mmaxfree);
	rlsedebug(x);
	return tot;
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
		fsdump(0, Mem);
		fatal("aborting");
	}
	return 1;
}

int
fsreclaim(void)
{
	Memblk *arch, *c, *victim;
	int i;
	daddrt addr;
	Blksl sl;
	long n, tot;

	xqlock(&fs->fzlk);
	fprint(2, "%s: %ulld free: reclaiming...\n", argv0, fsdiskfree());
	if(fs->fzsuper != nil){
		/*
		 * we did freeze/reclaim and are still writing, can't reclaim now.
		 */
		xqunlock(&fs->fzlk);
		fprint(2, "%s: writing, skip reclaim\n", argv0);
		return 0;
	}

	arch = fs->archive;
	rwlock(arch, Wr);
	if(catcherror()){
		rwunlock(arch, Wr);
		xqunlock(&fs->fzlk);
		error(nil);
	}
	tot = 0;
	for(;;){
		dprint("fsreclaim: reclaiming\n");
		victim = nil;
		for(i = 0; (c = dfchild(arch, i)) != nil; i++){
			if(victim == nil)
				victim = c;
			else if(victim->d.mtime > c->d.mtime){
				mbput(victim);
				victim = c;
			}else
				mbput(c);

		}
		if(i < 2){
			mbput(victim);
			dprint("nothing to reclaim\n");
			break;
		}

		fprint(2, "%s: reclaiming /archive/%s\n", argv0, victim->mf->name);
		dprint("victim is %H\n", victim);
		addr = dfchdentry(arch, victim->addr, 0, Dontmk);

		/*
		 * Write the new /archive without the file reclaimed, in
		 * case we fail while reclaiming.
		 */
		sl = dfslice(arch, sizeof(u64int), addr, Rd);
		if(sl.b != nil && sl.b != arch){
			munlink(&fs->mdirty, sl.b, 0);
			if(!catcherror()){
				dbwrite(sl.b);
				noerror();
			}
		}
		mbput(sl.b);
		changed(arch);
		munlink(&fs->mdirty, arch, 0);
		dbwrite(arch);

		n = dfreclaim(victim);
		mbput(victim);

		fs->super->d.root = arch->addr;

		dprint("fsreclaim: %uld file%s reclaimed\n", n, n?"s":"");
		tot += n;

		if(fsdiskfree() > Dmaxfree){
			dprint("fsreclaim: %d free: done\n", Dmaxfree);
			break;
		}
	}
	if(tot == 0){
		fprint(2, "%s: low on disk: 0 files reclaimed %ulld blocks free\n",
			argv0, fsdiskfree());
		tot = -1;
	}else
		fprint(2, "%s: %uld file%s reclaimed %ulld blocks free\n",
			argv0, tot, tot?"s":"", fsdiskfree());
	rwunlock(arch, Wr);
	xqunlock(&fs->fzlk);
	noerror();
	return tot;
}

static int
fsdirtypcent(void)
{
	long n, ndirty;

	n = fs->lru.n;
	ndirty = fs->mdirty.n;

	return (ndirty*100)/(n + ndirty);
}
	
/*
 * Policy for memory and and disk block reclaiming.
 * Should be called from time to time to guarantee that there are free blocks.
 */
void
fspolicy(void)
{
	int lomem, lodisk, hidirty, longago;

	/*
	 * XXX: This will call fswrite() while blocking everyone calling
	 * fspolicy(), including all rpcs.
	 * 
	 * That's done so now because the sizes used for testing are a joke,
	 * so that a single rpc may fill the disk even while another is already
	 * running the policy!, despite water marks!
	 *
	 * Once testing is complete, we should insist on running the policy
	 * (qlock it) only if we are really low on resources, otherwise, we
	 * should canqlock or defer it for later (because the most likely
	 * reason we can't qlock it is that it's already running and perhaps
	 * writing the frozen fs to disk).
	 *
	 * If we forget to make that change, writes will become
	 * synchronous and that should be considered as a BUG.
	 * 
	 */
	xqlock(&fs->policy);
	if(catcherror()){
		xqunlock(&fs->policy);
		fatal("fspolicy: %r");	/* should just print and return */
	}

	lomem = fsmemfree() < Mminfree;
	lodisk = fsdiskfree() < Dminfree;
	hidirty = fsdirtypcent() > Mmaxdirtypcent;
	longago = (nsec() - fs->wtime)/1000000UL > Syncival;

	/* Ideal sequence for [lomem lodisk hidirty] might be:
	 * 111:	lru sync reclaim+sync lru
	 * 110:	lru reclaim+sync
	 * 101:	lru sync lru
	 * 100:	lru
	 * 011:	reclaim+sync
	 * 010:	reclaim+sync
	 * 001:	sync
	 * 000: -
	 * Plus: if we are still low on memory, after lru, try
	 * doing a sync to move blocks to the lru list, ie. fake  "hidirty".
	 */

	if(lomem || lodisk || hidirty)
		dprint("fspolicy: lomem=%d (%ulld) lodisk=%d (%ulld)"
			" hidirty=%d (%d%%)\n",
			lomem, fsmemfree(), lodisk, fsdiskfree(),
			hidirty, fsdirtypcent());
	if(lomem){
		fslru();
		lomem = fsmemfree() < Mminfree;
		if(lomem)
			hidirty++;
	}
	if(lodisk)
		fsreclaim();
	if(lodisk || hidirty || longago)
		fssync();
	if(lomem && hidirty)
		fslru();

	qunlock(&fs->policy);
	noerror();
}

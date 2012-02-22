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
 * File block tools.
 * Should be used mostly by file.c, where the interface is kept.
 * see dk.h
 */

void
rwlock(Memblk *f, int iswr)
{
	if(iswr == No)
		return;
	if(iswr)
		wlock(f->mf);
	else
		rlock(f->mf);
}

void
rwunlock(Memblk *f, int iswr)
{
	if(iswr == No)
		return;
	if(iswr)
		wunlock(f->mf);
	else
		runlock(f->mf);
}

void
isfile(Memblk *f)
{
	if(TAGTYPE(f->d.tag) != DBfile || f->mf == nil)
		fatal("isfile: not a file at pc %#p", getcallerpc(&f));
}

void
isrwlocked(Memblk *f, int iswr)
{
	if(TAGTYPE(f->d.tag) != DBfile || f->mf == nil)
		fatal("isrwlocked: not a file  at pc %#p", getcallerpc(&f));
	if(iswr == No)
		return;
	if((iswr && canrlock(f->mf)) || (!iswr && canwlock(f->mf)))
		fatal("is%clocked at pc %#p", iswr?'w':'r', getcallerpc(&f));
}

void
isdir(Memblk *f)
{
	if(TAGTYPE(f->d.tag) != DBfile || f->mf == nil)
		fatal("isdir: not a file at pc %#p", getcallerpc(&f));
	if((f->mf->mode&DMDIR) == 0)
		fatal("isdir: not a dir at pc %#p", getcallerpc(&f));
}

void
isnotdir(Memblk *f)
{
	if(TAGTYPE(f->d.tag) != DBfile || f->mf == nil)
		fatal("isnotdir: not a file at pc %#p", getcallerpc(&f));
	if((f->mf->mode&DMDIR) != 0)
		fatal("isnotdir: dir at pc %#p", getcallerpc(&f));
}

/* for dfblk only */
static Memblk*
getmelted(uint isdir, int isarch, uint type, u64int *addrp)
{
	Memblk *b, *nb;

	if(*addrp == 0){
		b = dballoc(type);
		*addrp = b->addr;
		return b;
	}

	b = dbget(type, *addrp);
	nb = nil;
	if(isarch)
		b->frozen = 0;	/* /archive always melted */
	if(!b->frozen)
		return b;
	if(catcherror()){
		mbput(b);
		mbput(nb);
		error(nil);
	}
	nb = dbdup(b);
	if(isdir && type == DBdata)
		dupdentries(nb->d.data, Dblkdatasz/sizeof(Dentry));
	USED(&nb);		/* for error() */
	*addrp = nb->addr;
	incref(nb);
	dbdecref(b->addr);
	noerror();
	return nb;
}

/*
 * Get a file data block, perhaps allocating it on demand
 * if mkit. The file must be r/wlocked and melted if mkit.
 *
 * Adds disk refs for dir entries copied during melts and
 * considers that /archive is always melted.
 *
 * Read-ahead is not considered here. The file only records
 * the last accessed block number, to help the caller do RA.
 */
static Memblk*
dfblk(Memblk *f, ulong bno, int mkit)
{
	ulong prev, nblks;
	int i, idx, nindir, type, isdir, isarch;
	Memblk *b, *pb;
	u64int *addrp;

	isarch = f == fs->archive;
	if(isarch)
		f->frozen = 0;
	if(mkit)
		ismelted(f);
	isdir = (f->mf->mode&DMDIR);

	f->mf->lastbno = bno;
	/*
	 * bno: block # relative to the the block we are looking at.
	 * prev: # of blocks before the current one.
	 */
	prev = 0;

	/*
	 * Direct block?
	 */
	if(bno < nelem(f->d.dptr))
		if(mkit)
			return getmelted(isdir, isarch, DBdata, &f->d.dptr[bno]);
		else
			return dbget(DBdata, f->d.dptr[bno]);

	bno -= nelem(f->d.dptr);
	prev += nelem(f->d.dptr);

	/*
	 * Indirect block
	 * nblks: # of data blocks addressed by the block we look at.
	 */
	nblks = Dptrperblk;
	for(i = 0; i < nelem(f->d.iptr); i++){
		if(bno < nblks)
			break;
		bno -= nblks;
		prev += nblks;
		nblks *= Dptrperblk;
	}
	if(i == nelem(f->d.iptr))
		error("offset exceeds file capacity");

	type = DBptr0+i;
	dFprint("dfblk: indirect %s nblks %uld (ppb %ud) bno %uld\n",
		tname(type), nblks, Dptrperblk, bno);

	addrp = &f->d.iptr[i];
	if(mkit)
		b = getmelted(isdir, isarch, type, addrp);
	else
		b = dbget(type, *addrp);
	pb = b;
	if(catcherror()){
		mbput(pb);
		error(nil);
	}

	/* at the loop header:
	 * 	pb: parent of b
	 * 	b: DBptr block we are looking at.
	 * 	addrp: ptr to b within fb.
	 * 	nblks: # of data blocks addressed by b
	 */
	for(nindir = i+1; nindir >= 0; nindir--){
		dFprint("indir %s d%#ullx nblks %uld ptrperblk %d bno %uld\n\n",
			tname(DBdata+nindir), *addrp, nblks, Dptrperblk, bno);
		idx = 0;
		if(nindir > 0){
			nblks /= Dptrperblk;
			idx = bno/nblks;
		}
		if(*addrp == 0 && !mkit){
			/* hole */
			b = nil;
		}else{
			assert(type >= DBdata);
			if(mkit)
				b = getmelted(isdir, isarch, type, addrp);
			else
				b = dbget(type, *addrp);
			addrp = &b->d.ptr[idx];
		}
		mbput(pb);
		pb = b;
		USED(&b);	/* force to memory in case of error */
		USED(&pb);	/* force to memory in case of error */
		bno -= idx * nblks;
		prev +=  idx * nblks;
		type--;
	}
	noerror();
	return b;
}

/*
 * Remove [bno:bend) file data blocks.
 * The file must be r/wlocked and melted.
 */
void
dfdropblks(Memblk *f, ulong bno, ulong bend)
{
	Memblk *b;

	isrwlocked(f, Wr);
	ismelted(f);
	isnotdir(f);

	dDprint("dfdropblks: could remove d%#ullx[%uld:%uld]\n",
		f->addr, bno, bend);
	/*
	 * Instead of releasing the references on the data blocks,
	 * considering that the file might grow again, we keep them.
	 * Consider recompiling again and again and...
	 *
	 * The length has been adjusted and data won't be returned
	 * before overwritten.
	 *
	 * We only have to zero the data, because the file might
	 * grow using holes and the holes must read as zero, and also
	 * for safety.
	 */
	for(; bno < bend; bno++){
		if(catcherror())
			continue;
		b = dfblk(f, bno, 0);
		noerror();
		memset(b->d.data, 0, Dblkdatasz);
		changed(b);
		mbput(b);
	}
}

/*
 * block # for the given offset (first block in file is 0).
 * embedded data accounts also as block #0.
 * If boffp is not nil it returns the offset within that block
 * for the given offset.
 */
ulong
dfbno(Memblk *f, uvlong off, ulong *boffp)
{
	ulong doff, dlen;

	doff = embedattrsz(f);
	dlen = Embedsz - doff;
	if(off < dlen){
		*boffp = doff + off;
		return 0;
	}
	off -= dlen;
	if(boffp != nil)
		*boffp = off%Dblkdatasz;
	return off/Dblkdatasz;
}

static void
updatesize(Memblk *f, uvlong nsize)
{
	Dmeta *d;

	isrwlocked(f, Wr);
	f->mf->length = nsize;
	d = (Dmeta*)f->d.embed;
	d->length = nsize;
}

/*
 * Return a block slice for data in f.
 * The slice returned is resized to keep in a single block.
 * If there's a hole in the file, Blksl.data == nil && Blksl.len > 0.
 *
 * If mkit, the data block (and any pointer block crossed)
 * is allocated/melted if needed, and the file length updated.
 *
 * The file must be r/wlocked by the caller, and melted if mkit.
 * The block is returned referenced but unlocked,
 * (it's still protected by the file lock.)
 */
Blksl
dfslice(Memblk *f, ulong len, uvlong off, int iswr)
{
	Blksl sl;
	ulong boff, doff, dlen, bno;

	memset(&sl, 0, sizeof sl);

	if(iswr)
		ismelted(f);
	else
		if(off >= f->mf->length)
			goto done;

	doff = embedattrsz(f);
	dlen = Embedsz - doff;

	if(off < dlen){
		sl.b = f;
		incref(f);
		sl.data = f->d.embed + doff + off;
		sl.len = dlen - off;
	}else{
		bno = (off-dlen) / Dblkdatasz;
		boff = (off-dlen) % Dblkdatasz;

		sl.b = dfblk(f, bno, iswr);
		if(iswr)
			ismelted(sl.b);
		if(sl.b != nil)
			sl.data = sl.b->d.data + boff;
		sl.len = Dblkdatasz - boff;
	}

	if(sl.len > len)
		sl.len = len;
	if(off + sl.len > f->mf->length)
		if(iswr)
			updatesize(f, off + sl.len);
		else
			sl.len = f->mf->length - off;
done:
	if(sl.b == nil){
		dFprint("slice m%#p[%#ullx:+%#ulx]%c -> 0[%#ulx]\n",
			f, off, len, iswr?'w':'r', sl.len);
		return sl;
	}
	if(TAGTYPE(sl.b->d.tag) == DBfile)
		dFprint("slice m%#p[%#ullx:+%#ulx]%c -> m%#p:e+%#uld[%#ulx]\n",
			f, off, len, iswr?'w':'r',
			sl.b, (uchar*)sl.data - sl.b->d.embed, sl.len);
	else
		dFprint("slice m%#p[%#ullx:+%#ulx]%c -> m%#p:%#uld[%#ulx]\n",
			f, off, len, iswr?'w':'r',
			sl.b, (uchar*)sl.data - sl.b->d.data, sl.len);

	assert(sl.b->ref > 1);
	return sl;
}

static void
compact(Memblk *d, Dentry *de, u64int off)
{
	Blksl sl;
	uvlong lastoff;
	Dentry *lastde;

	if(catcherror())
		return;
	assert(d->mf->length >= sizeof(Dentry));
	lastoff = d->mf->length - sizeof(Dentry);
	if(d->mf->length > sizeof(Dentry) && off < lastoff){
		sl = dfslice(d, sizeof(Dentry), lastoff, 0);
		assert(sl.b);
		lastde = sl.data;
		de->file = lastde->file;
		lastde->file = 0;
		changed(sl.b);
		mbput(sl.b);
	}
	noerror();
	updatesize(d, lastoff);
	changed(d);
}

/*
 * Find a dir entry for addr (perhaps 0 == avail) and change it to
 * naddr. If iswr, the entry is allocated if needed and the blocks
 * melted on demand.
 * Return the offset for the entry in the file or Noaddr
 * Does not adjust disk refs.
 */
u64int
dfchdentry(Memblk *d, u64int addr, u64int naddr, int iswr)
{
	Blksl sl;
	Dentry *de;
	uvlong off;
	int i;

	dDprint("dfchdentry d%#ullx -> d%#ullx\nin %H\n", addr, naddr, d);
	isrwlocked(d, iswr);
	isdir(d);

	off = 0;
	for(;;){
		sl = dfslice(d, Dblkdatasz, off, iswr);
		if(sl.len == 0)
			break;
		if(sl.b == 0){
			if(addr == 0 && !iswr)
				return off;
			continue;
		}
		de = sl.data;
		for(i = 0; i < sl.len/sizeof(Dentry); i++){
			if(de[i].file == addr){
				if(naddr != addr){
					if(iswr && naddr == 0)
						compact(d, &de[i], off+i*sizeof(Dentry));
					else
						de[i].file = naddr;
					changed(sl.b);
				}
				mbput(sl.b);
				return off + i*sizeof(Dentry);
			}
		}
		off += sl.len;
		mbput(sl.b);
	}
	if(iswr)
		fatal("dfchdentry: bug");
	return Noaddr;
}

static u64int
dfdirnth(Memblk *d, int n)
{
	Blksl sl;
	Dentry *de;
	uvlong off;
	int i, tot;

	isdir(d);
	off = 0;
	tot = 0;
	for(;;){
		sl = dfslice(d, Dblkdatasz, off, 0);
		if(sl.len == 0)
			break;
		if(sl.b == 0)
			continue;
		de = sl.data;
		for(i = 0; i < sl.len/sizeof(Dentry); i++)
			if(de[i].file != 0 && tot++ >= n){
				mbput(sl.b);
				dFprint("dfdirnth d%#ullx[%d] = d%#ullx\n",
					d->addr, n, de[i].file);
				return de[i].file;
			}
		off += sl.len;
		mbput(sl.b);
	}
	return 0;
}

static Memblk*
xfchild(Memblk *f, int n, int disktoo)
{
	u64int addr;
	Memblk *b;

	addr = dfdirnth(f, n);
	if(addr == 0)
		return nil;
	b = mbget(addr, 0);
	if(b != nil || disktoo == 0)
		return b;
	return dbget(DBfile, addr);
}

Memblk*
dfchild(Memblk *f, int n)
{
	return xfchild(f, n, 1);
}

Memblk*
mfchild(Memblk *f, int n)
{
	return xfchild(f, n, 0);
}

/*
 * does not dbincref(f)
 * caller locks both d and f
 */
void
dflink(Memblk *d, Memblk *f)
{
	ismelted(d);
	isdir(d);

	dfchdentry(d, 0, f->addr, Wr);
	changed(d);
}

/*
 * does not dbdecref(f)
 * caller locks both d and f
 */
void
dfunlink(Memblk *d, Memblk *f)
{
	ismelted(d);
	isdir(d);

	dfchdentry(d, f->addr, 0, Wr);
	changed(d);
}

/*
 * Walk to a child and return it referenced.
 * If iswr, d must not be frozen and the child is returned melted.
 */
Memblk*
dfwalk(Memblk *d, char *name, int iswr)
{
	Memblk *f;
	Blksl sl;
	Dentry *de;
	uvlong off;
	int i;

	dDprint("dfwalk '%s' at %H\n", name, d);
	if(strcmp(name, "..") == 0)
		fatal("dfwalk: '..'");
	isdir(d);
	if(iswr)
		ismelted(d);

	off = 0;
	f = nil;
	for(;;){
		sl = dfslice(d, Dblkdatasz, off, 0);
		if(sl.len == 0)
			break;
		if(sl.b == nil)
			continue;
		if(catcherror()){
			mbput(sl.b);
			error(nil);
		}
		for(i = 0; i < sl.len/sizeof(Dentry); i++){
			de = sl.data;
			de += i;
			if(de->file == 0)
				continue;
			f = dbget(DBfile, de->file);
			if(strcmp(f->mf->name, name) != 0){
				mbput(f);
				continue;
			}

			/* found  */
			noerror();
			mbput(sl.b);
			if(!iswr || !f->frozen)
				goto done;
			fatal("dfwalk: frozen");
		}
		noerror();
		mbput(sl.b);
		off += sl.len;
	}
	error("file not found");

done:
	return f;
}

/*
 * Return the last version for *fp, wlocked, be it frozen or melted.
 */
static void
followmelted(Memblk **fp)
{
	Memblk *f;

	f = *fp;
	isfile(f);
	rwlock(f, Wr);
	while(f->mf->melted != nil){
		incref(f->mf->melted);
		*fp = f->mf->melted;
		rwunlock(f, Wr);
		mbput(f);
		f = *fp;
		rwlock(f, Wr);
		if(!f->frozen)
			return;
	}
}

/*
 * Caller walked down p, and now requires the nth element to be
 * melted, and wlocked for writing. (nth count starts at 1);
 * 
 * Return the path with the version of f that we must use,
 * locked for writing and melted.
 * References kept in the path are traded for the ones returned.
 */
Path*
dfmelt(Path **pp, int nth)
{
	int i;
	Memblk *f, **fp, *nf;
	Path *p;

	ownpath(pp);
	p = *pp;
	assert(nth >= 1 && p->nf >= nth && p->nf >= 2);
	assert(p->f[0] == fs->root);
	fp = &p->f[nth-1];

	/*
	 * 1. Optimistic: Try to get a loaded melted version for f.
	 */
	followmelted(fp);
	f = *fp;
	if(!f->frozen)
		return p;
	rwunlock(f, Wr);

	/*
	 * 2. Realistic:
	 * walk down the path, melting every frozen thing until we
	 * reach f. Keep wlocks so melted files are not frozen while we walk.
	 * /active is special, because it's only frozen temporarily while
	 * creating a frozen version of the tree. Instead of melting it,
	 * we should just wait for it.
	 */
	followmelted(&p->f[1]);

	for(i = 2; i < nth; i++){
		followmelted(&p->f[i]);
		f = p->f[i];
		if(!f->frozen){
			rwunlock(p->f[i-1], Wr);
			continue;
		}
		if(catcherror()){
			rwunlock(p->f[i-1], Wr);
			rwunlock(p->f[i], Wr);
			error(nil);
		}
		nf = dbdup(f);
		rwlock(nf, Wr);
		if(catcherror()){
			rwunlock(nf, Wr);
			mbput(nf);
			error(nil);
		}
		dfchdentry(p->f[i-1], f->addr, nf->addr, 1);
		/* committed */
		rwunlock(f, Wr);
		rwunlock(p->f[i-1], Wr);
		if(!catcherror()){
			dbdecref(f->addr);
			noerror();
		}
	}
	return p;
}

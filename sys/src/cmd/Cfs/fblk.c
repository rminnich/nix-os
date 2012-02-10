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
 * File block tools.
 * Should be used mostly by file.c, where the interface is kept.
 * see dk.h
 */

void
isfile(Memblk *d)
{
	if(TAGTYPE(d->d.tag) != DBfile || d->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", d, getcallerpc(&d));
}

void
isdir(Memblk *d)
{
	if(TAGTYPE(d->d.tag) != DBfile || d->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", d, getcallerpc(&d));
	if((d->mf->mode&DMDIR) == 0)
		sysfatal("%Hnot a directory at %#p", d, getcallerpc(&d));
}

void
isnotdir(Memblk *d)
{
	if(TAGTYPE(d->d.tag) != DBfile || d->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", d, getcallerpc(&d));
	if((d->mf->mode&DMDIR) != 0)
		sysfatal("%His a directory at %#p", d, getcallerpc(&d));
}

void
isloaded(Memblk *d)
{
	if(TAGTYPE(d->d.tag) != DBfile || d->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", d, getcallerpc(&d));
	if((d->mf->mode&DMDIR) != 0)
		if(d->mf->length > 0 && d->mf->child == nil){
			abort();
			sysfatal("%Hnot loaded at %#p", d, getcallerpc(&d));
		}
}

void
iswlocked(Memblk *b)
{
	if(TAGTYPE(b->d.tag) != DBfile || b->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", b, getcallerpc(&b));
	if(canrlock(b->mf))
		sysfatal("iswlocked at %#p", getcallerpc(&b));
}

void
isrlocked(Memblk *b)
{
	if(TAGTYPE(b->d.tag) != DBfile || b->mf == nil)
		sysfatal("%Hnot a DBfile at %#p", b, getcallerpc(&b));
	if(canwlock(b->mf))
		sysfatal("isrlocked at %#p", getcallerpc(&b));
}


static Memblk*
getmelted(Fsys *fs, uint type, u64int *addrp)
{
	Memblk *b, *nb;

	if(*addrp == 0){
		b = dballoc(fs, type);
		*addrp = b->addr;
		return b;
	}

	b = dbget(fs, type, *addrp);
	if(b->frozen == 0)
		return b;

	nb = dbdup(fs, b);
	dbdecref(fs, b->addr);
	mbput(fs, b);
	*addrp = nb->addr;
	return nb;
}

/*
 * Get a file data block, perhaps allocating it on demand
 * if mkit. The file must be r/wlocked and melted if mkit.
 */
static Memblk*
dfblk(Fsys *fs, Memblk *f, ulong bno, int mkit)
{
	ulong prev, nblks;
	int i, idx, nindir, type;
	Memblk *b, *pb;
	u64int *addrp;
	Mfile *m;

	m = f->mf;
	if(mkit){
		iswlocked(f);
		ismelted(f);
	}else
		isrlocked(f);

	if(bno != 0 && m->lastb != nil){
		if(bno == m->lastbno){
			if(!mkit || !m->lastb->frozen){
				incref(m->lastb);
				return m->lastb;
			}
		}else if(bno == m->lastbno + 1 && !mkit){
			 /* BUG: read ahead */
		}
		if(m->lastb != nil)
			mbput(fs, m->lastb);
		m->lastb = nil;
	}
	m->lastbno = bno;

	/*
	 * bno: block # relative to the the block we are looking at.
	 * prev: # of blocks before the current one.
	 */
	prev = 0;

	/*
	 * Direct block?
	 */
	if(bno < nelem(f->d.dptr)){
		if(mkit)
			b = getmelted(fs, DBdata, &f->d.dptr[bno]);
		else
			b = dbget(fs, DBdata, f->d.dptr[bno]);
		goto Found;
	}

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
		sysfatal("fblkaddr");
	pb = f;
	incref(pb);
	addrp = &f->d.iptr[bno];
	if(mkit)
		b = getmelted(fs, DBptr, addrp);
	else
		b = dbget(fs, DBptr, *addrp);

	/* invariant at the loop header:
	 * b: ptr block we are looking at.
	 * nblks: # of data blocks addressed by b
	 * pb: parent of b
	 * addrp: ptr to b within fb.
	 */
	if(catcherror()){
		mbput(fs, pb);
		mbput(fs, b);
		error(nil);
	}
	for(nindir = i; nindir >= 0; nindir--){
		nblks /= Dptrperblk;
		idx = bno/nblks;
		if(*addrp == 0 && !mkit){
			/* hole */
			b = nil;
		}else{
			type = DBptr;
			if(nindir == 0)
				type = DBdata;
			if(mkit)
				b = getmelted(fs, type, addrp);
			else
				b = dbget(fs, type, *addrp);
			addrp = &b->d.ptr[idx];
			mbput(fs, pb);
			pb = b;
		}
		USED(&b);	/* force to memory in case of error */
		USED(&pb);	/* force to memory in case of error */
		bno -= idx * nblks;
		prev +=  idx * nblks;
	}
	mbput(fs, pb);
	noerror();

Found:
//	if(b != nil)
//		incref(b);
//	m->lastb = b;	/* memo the last search; beware of holes vs. memos */

	return b;
}

static void
fresize(Memblk *f, uvlong nsize)
{
	Dmeta *d;

	f->mf->length = nsize;
	d = (Dmeta*)f->d.embed;
	d->length = nsize;
}

/*
 * Return a block slice for data in f.
 *
 * Only a single block is returned, the caller may call dfslice
 * as many times as needed.
 * (If there's a hole in the file, Blksl.data == nil && Blksl.len > 0)
 *
 * If mkit, the data block (and middle pointer blocks)
 * are allocated/melted if needed, in such case, the length of the file
 * is updated to the given offset. Otherwise, the file length is a limit.
 *
 * The file must be r/wlocked by the caller, and melted if mkit.
 * The block is returned unlocked, but still protected by the file lock.
 */
Blksl
dfslice(Fsys *fs, Memblk *f, ulong len, uvlong off, int iswr)
{
	Blksl sl;
	ulong doff, dlen, bno, nsize;

	dDprint("slice off %#ullx len %#ulx wr=%d at m%#p\n", off, len, iswr, f);
	memset(&sl, 0, sizeof sl);

	nsize = off + 0;
	if(iswr){
		iswlocked(f);
		ismelted(f);
	}else{
		isrlocked(f);
		if(off >= f->mf->length)
			goto Done;
	}
	doff = embedattrsz(f);
	dlen = Embedsz - doff;
	if(off < dlen){
		sl.b = f;
		incref(f);
		sl.data = f->d.embed + doff + off;
		sl.len = dlen - off;
	}else{
		off -= dlen;
		bno = off / Dblkdatasz;
		off %= Dblkdatasz;

		sl.b = dfblk(fs, f, bno, iswr);

		dDprint("dfblk %H%uld -> %H", f, bno, sl.b);
		if(iswr)
			ismelted(sl.b);
		if(sl.b != nil)
			sl.data = sl.b->d.data + off;
	
		sl.len = Dblkdatasz - off;
		if(doff + bno*Dblkdatasz + off + sl.len > f->mf->length)
			sl.len = f->mf->length - (doff + bno*Dblkdatasz + off);
	}
	if(sl.len > len)
		sl.len = len;
	nsize += sl.len;
Done:
	if(iswr && nsize > f->mf->length)
		fresize(f, nsize);
	if(sl.b == nil)
		dDprint("slice-> null with len %#ulx\n", sl.len);
	else if(TAGTYPE(sl.b->d.tag) == DBfile)
		dDprint("slice-> off %#ulx len %#ulx at m%#p\n",
			(uchar*)sl.data - sl.b->d.embed, sl.len, sl.b);
	else
		dDprint("slice-> off %#ulx len %#ulx at m%#p\n",
			(uchar*)sl.data - sl.b->d.data, sl.len, sl.b);
	return sl;
}

/*
 * These two ones un/link f to the child list of d,
 * but the children do not imply further references to the parent.
 */

static Child*
addchild(Memblk *d, Memblk *f)
{
	Mfile *m;
	Child *c;

	isdir(d);
	isfile(f);
	m = d->mf;
	if(m->nchild == m->nachild){
		m->nachild += Incr;
		m->child = realloc(m->child, m->nachild*sizeof(Child));
	}
	c = &m->child[m->nchild++];
	memset(c, 0, sizeof *c);
	c->f = f;
	f->mf->parent = d;
	return c;
}

void
delchild(Memblk *d, Child *c)
{
	Mfile *m;
	int i;

	isdir(d);
	isloaded(d);
	m = d->mf;
	i = c - m->child;
	if(i < m->nchild-1)
		m->child[i] = m->child[m->nchild-1];
	m->nchild--;
	c->f->mf->parent = nil;
}

static Child*
getchild(Memblk *d, Memblk *f)
{
	Mfile *m;
	int i;

	isdir(d);
	isfile(f);
	isloaded(d);
	m = d->mf;
	for(i = 0; i < m->nchild; i++)
		if(m->child[i].f == f)
			break;
	if(i == m->nchild)
		sysfatal("findchild: not found");
	return &m->child[i];
}

/*
 * does not dbincref(f)
 * caller locks both d and f
 * No reference to the parent is added because of the new
 * e.g. block referenced by f->child[n].b must be loaded as long
 * as its child entry is alive.
 */
void
dflink(Fsys *fs, Memblk *d, Memblk *f)
{
	Blksl sl;
	Dentry *de;
	uvlong off;
	Child *c;

	dDprint("dflink\n  %H  %H", d, f);
	iswlocked(d);
	ismelted(d);
	isdir(d);
	if(d->mf->length > 0 && d->mf->child == nil)
		dfloaddir(fs, d, 1);

	c = addchild(d, f);
	if(catcherror()){
		delchild(d, c);
		error(nil);
	}
	off = 0;
	for(;;){
		sl = dfslice(fs, d, sizeof(Dentry), off, 1);
		if(sl.len == 0)
			break;
		ismelted(sl.b);
		off += sl.len;
		if(sl.len < sizeof(Dentry)){	/* trailing part in block */
			mbput(fs, sl.b);
			continue;
		}
		de = sl.data;
		if(de->file == 0){
			c->d = de;
			c->b = sl.b;
			de->file = f->addr;
			changed(sl.b);
			mbput(fs, sl.b);
			break;
		}
		mbput(fs, sl.b);
	}
	changed(d);
	noerror();
}

/*
 * does not dbdecref(f)
 * caller locks both d and f
 */
void
dfunlink(Fsys *fs, Memblk *d, Memblk *f)
{
	Dentry *de;
	Child *c;

	dDprint("dfunlink\n  %H  %H\n", d, f);
	iswlocked(d);
	ismelted(d);
	isdir(d);
	if(d->mf->length > 0 && d->mf->child == nil)
		dfloaddir(fs, d, 1);

	c = getchild(d, f);
	ismelted(c->b);
	de = c->d;
	de->file = 0;
	changed(c->b);
	mbput(fs, c->b);
	delchild(d, c);
	changed(d);
}

void
dfloaddir(Fsys *fs, Memblk *d, int locked)
{
	Blksl sl;
	Dentry *de;
	uvlong off;
	Memblk *f;
	Child *c;

	isdir(d);
	if(d->mf->length == 0 || d->mf->child != nil)	/* already loaded */
		return;
	if(locked)
		iswlocked(d);
	else
		wlock(d->mf);
	if(catcherror()){
		wunlock(d->mf);
		error(nil);
	}
	off = 0;
	for(;;){
		sl = dfslice(fs, d, sizeof(Dentry), off, 0);
		if(sl.len == 0)
			break;
		off += sl.len;
		if(sl.len < sizeof(Dentry)){	/* trailing part in block */
			mbput(fs, sl.b);
			continue;
		}
		if(catcherror()){
			mbput(fs, sl.b);
			error(nil);
		}
		de = sl.data;
		if(de->file == 0)
			continue;
		f = dbget(fs, DBfile, de->file);
		c = addchild(d, f);
		c->d = de;
		c->b = sl.b;
		mbput(fs, f);
		noerror();
	}
	if(!locked)
		wunlock(d->mf);
	noerror();
}

/*
 * Walk to a child and return it both referenced and locked.
 * If iswr, d must not be frozen and the child is returned melted.
 * caller locks d.
 * dir must be already loaded.
 */
Memblk*
dfwalk(Fsys *fs, Memblk *d, char *name, int iswr)
{
	Memblk *f, *nf;
	int i;
	Mfile *m;
	Child *c;

	dDprint("dfwalk '%s' at %H\n", name, d);
	isdir(d);
	if(iswr){
		iswlocked(d);
		ismelted(d);
	}else
		isrlocked(d);
	isloaded(d);

	m = d->mf;
	for(i = 0; i < m->nchild; i++){
		c = &m->child[i];
		f = c->f;
		if(strcmp(f->mf->name, name) == 0){
			if(iswr)
				wlock(f->mf);
			else
				rlock(f->mf);
			if(!iswr || !f->frozen){
				incref(f);
				return(f);
			}
			/* hard: it's frozen and iswr; must melt it */
			nf = dbdup(fs, f);
			wunlock(f->mf);
			wlock(nf->mf);
			dbdecref(fs, f->addr);
			mbput(fs, f);
			c->f = nf;
			incref(nf);
			c->d->file = nf->addr;
			changed(c->b);
			ismelted(nf);
			iswlocked(nf);
			return nf;
		}
	}
	error("file not found");
	return nil;
}

/*
 * Found a file b frozen, and need it to be melted.
 * travel up the tree and walk down to b ensuring that it's melted.
 * Return it wlocked, so nobody can freeze it again before we use it.
 */
Memblk*
dfmelt(Fsys *fs, Memblk *f)
{
	char **names;
	int nnames;
	Memblk *b, *pb, *nb;

	/*
	 * 1. travel up to a melted block or to the root, recording
	 * the names we will have to walk down to reach f.
	 */
	isfile(f);
	rlock(f->mf);
	names = nil;
	nnames = 0;
	for(b = f; b != nil; b = pb){
		if(b == fs->active || b == fs->archive)
			break;
		if(nnames%Incr == 0)
			names = realloc(names, (nnames+Incr)*sizeof(char*));
		names[nnames++] = strdup(b->mf->name);
		pb = b->mf->parent;
		runlock(b->mf);
		rlock(pb->mf);
	}
	runlock(b->mf);

	/*
	 * 2. walk down from active to f, ensuring everything is melted.
	 */
	for(;;){
		b = fs->active;
		wlock(b->mf);
		if(!b->frozen)
			break;
		wunlock(b->mf);
	}
	ismelted(b);
	incref(b);
	if(catcherror()){
		while(nnames-- > 0)
			free(names[nnames]);
		free(names);
		wunlock(b->mf);
		mbput(fs, b);
		error("can't melt: %r");
	}

	while(nnames-- > 0){
		if(b->mf->length > 0 && b->mf->child == nil)
			dfloaddir(fs, b, 1);
		nb = dfwalk(fs, b, names[--nnames], 1);
		free(names[nnames]);
		names[nnames] = nil;
		wunlock(b->mf);
		mbput(fs, b);
		b = nb;
		USED(&b);	/* flush b to memory for error()s */
	}
	noerror();
	free(names);
	iswlocked(b);
	return b;
}


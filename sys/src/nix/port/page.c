#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

enum
{
	Min4kpages = 30
};

/*
 * Changes for 2M pages:
 *
 * A new allocator, bigpalloc contains all 2M pages and divides
 * them into 4K pages as needed.
 *
 * Segment sizes are still in 4K pages.
 * The first page to attach to a segment fixes the segment pg sz.
 */

#define pghash(daddr)	palloc.hash[(daddr>>PGSHIFT)&(PGHSIZE-1)]

/*
 * Palloc contains PGSZ pages.
 * bigpalloc contains BIGPGSZ pages.
 * The first one is used for locking and hashing.
 * The second one contains just aggregated pages.
 */

struct	Palloc palloc;
struct	Palloc bigpalloc;
static void splitbigpage(void);

void
pageinit(void)
{
	int color, i, j;
	Page *p, *lastp, *lastbigp;
	Pallocmem *pm;
	uvlong pnp, np, pkb, nbigp; 

	np = 0;
	nbigp = 0;
	/*
	 * For each palloc memory map we have a bunch of 4K pages
	 * not aligned to make a full 2M page and then a bunch of 2M pages.
	 * BUG: We leak pages at the end if they are not enough to make a full 2M page.
	 * We also assume that each map is at least 2M.
	 */
	for(i=0; i<nelem(palloc.mem); i++){
		pm = &palloc.mem[i];
		DBG("COMPUTE: pm %d pm->npage %uld pm->base %#p \n", i, pm->npage, pm->base);
		if(pm->npage == 0)
			continue;
		pnp = (BIGPGROUND(pm->base) - pm->base) / PGSZ;
		pm->nbigpage = pm->npage - pnp;
		pm->npage = pnp;
		pm->nbigpage /= PGSPERBIG;
		if(1)
		DBG("pnp %#ullx pm npage %#ulx nbigpage %#ulx\n",
			pnp, pm->npage, pm->nbigpage);
		np += pm->npage;
		nbigp += pm->nbigpage;
	}
	palloc.pages = malloc(np*sizeof(Page));
	bigpalloc.pages = malloc(nbigp*sizeof(Page));
	if(palloc.pages == 0 || bigpalloc.pages == 0)
		panic("pageinit");
	DBG("npages %#ullx nbigpages %#ullx pgsz %d\n", np, nbigp, sizeof(Page));
	color = 0;
	lastp = nil;
	palloc.head = palloc.tail = nil;
	palloc.user = 0;
	lastbigp = nil;
	bigpalloc.head = bigpalloc.tail = nil;

	for(i=0; i<nelem(palloc.mem); i++){
		pm = &palloc.mem[i];
		DBG("ALLOCATE: npage %#ulx, nbigpage %#ulx\n", pm->npage, pm->nbigpage);
		if(pm->npage == 0 && pm->nbigpage == 0)
			continue;
		if(lastp == nil)
			p = palloc.pages;
		else
			p = palloc.tail+1;
		for(j=0; j<pm->npage; j++){
			assert(p >= palloc.pages && p < palloc.pages + np);
			p->prev = lastp;
			if(lastp != nil)
				lastp->next = p;
			else
				palloc.head = p;
			p->next = nil;
			p->pa = pm->base+j*PGSZ;
			p->color = color;
			p->lgsize = PGSHIFT;
			palloc.freecount++;
			color = (color+1)%NCOLOR;
			lastp = p++;
			palloc.user++;
		}
		palloc.tail = lastp;
		if(lastbigp == nil)
			p = bigpalloc.pages;
		else
			p = bigpalloc.tail+1;
		for(j = 0; j < pm->nbigpage; j++){
			assert(p >= bigpalloc.pages && p < bigpalloc.pages + nbigp);
			p->prev = lastbigp;
			if(lastbigp != nil)
				lastbigp->next = p;
			else
				bigpalloc.head = p;
			p->next = nil;
			p->pa = pm->base+pm->npage*PGSZ+j*BIGPGSZ;
			assert(p->pa == BIGPGROUND(p->pa));
			p->color = color;
			p->lgsize = BIGPGSHFT;
			bigpalloc.freecount++;
			color = (color+1)%NCOLOR;
			lastbigp = p++;
			bigpalloc.user++;
		}
		bigpalloc.tail = lastbigp;
	}

	DBG("%uld big pages; %uld small pages\n",
		bigpalloc.freecount, palloc.freecount);
	pkb = palloc.user*PGSZ/1024 + bigpalloc.user*BIGPGSZ/1024ULL;

	/* Paging numbers */
	swapalloc.highwater = (palloc.user*5)/100;
	swapalloc.headroom = swapalloc.highwater + (swapalloc.highwater/4);

	/* How to compute total and kernel memory in this kernel? */
	print("%lldM user memory\n", pkb/1024ULL);

	/*
	 * This is not necessary, but it makes bugs in memory scan/page init
	 * show up right now, so we split now a big page into 4K pages.
	 */
	lock(&palloc);
	splitbigpage();
	unlock(&palloc);
	DBG("pageinit done\n");
}

static Palloc*
getalloc(Page *p)
{
	if(p->lgsize == PGSHIFT)
		return &palloc;
	if(p->lgsize != BIGPGSHFT)
		panic("getalloc");
	return &bigpalloc;
}

static void
pageunchain(Page *p)
{
	Palloc *pp;

	if(canlock(&palloc))
		panic("pageunchain (palloc %#p)", &palloc);
	pp = getalloc(p);
	if(p->prev)
		p->prev->next = p->next;
	else
		pp->head = p->next;
	if(p->next)
		p->next->prev = p->prev;
	else
		pp->tail = p->prev;
	p->prev = p->next = nil;
	pp->freecount--;
}

void
pagechaintail(Page *p)
{
	Palloc *pp;

	if(canlock(&palloc))
		panic("pagechaintail");
	pp = getalloc(p);
	if(pp->tail) {
		p->prev = pp->tail;
		pp->tail->next = p;
	}
	else {
		pp->head = p;
		p->prev = 0;
	}
	pp->tail = p;
	p->next = 0;
	pp->freecount++;
}

void
pagechainhead(Page *p)
{
	Palloc *pp;

	if(canlock(&palloc))
		panic("pagechainhead");
	pp = getalloc(p);
	if(pp->head) {
		p->next = pp->head;
		pp->head->prev = p;
	}
	else {
		pp->tail = p;
		p->next = 0;
	}
	pp->head = p;
	p->prev = 0;
	pp->freecount++;
}

/*
 * allocator is locked.
 * Low on pages, split a big one a release all its pages
 * into the small page allocator.
 * The Page structs for the new pages are allocated within
 * the big page being split, so we don't have to allocate more memory.
 * For a 2M page we need 512 Page structs (for new 4K pages).
 * That's 80K if Page is 160 bytes.
 * 
 */
static void
splitbigpage(void)
{
	Page *bigp, *p;
	ulong arrysz, npage;
	KMap *k;
	int color;

	if(canlock(&palloc))
		panic("splitbigpage");
	if(bigpalloc.freecount == 0)
		panic("no big pages; no memory\n");
	bigp = bigpalloc.head;
	pageunchain(bigp);
	DBG("big page %#ullx split...\n", bigp->pa);

	arrysz = PGROUND(PGSPERBIG * sizeof(Page));	/* size consumed in Page array */
	npage = arrysz/PGSZ;
	k = KADDR(bigp->pa);
	memset(k, 0, BIGPGSZ);
	p = k;
	p += npage;
	p->next = nil;
	color = 0;
	for(; npage < PGSPERBIG; npage++){
		p->prev = palloc.tail;
		if(palloc.tail == nil)
			palloc.head = p;
		else
			palloc.tail->next = p;
		p->next = nil;
		p->color = color;
		color = (color+1)%NCOLOR;
		p->lgsize = PGSHIFT;
		p->pa = bigp->pa + npage*PGSZ;
		palloc.tail = p;
		palloc.freecount++;
	}

	/*
	 * We leak the big page, we will never coallesce
	 * small pages into a big page.
	 * Also, we must leave the bigpage mapped, or we won't
	 * be able to access its Page structs for inner 4K pages.
	 */
	DBG("big page split %#ullx done\n", bigp->pa);
}

Page*
newpage(int clear, Segment **s, uintptr va, uintptr pgsz)
{
	Page *p;
	KMap *k;
	uchar ct;
	int i, color, dontalloc;
	Palloc *pp;
	static int once, last;

	pp=&palloc;
	if(pgsz == BIGPGSZ)
		pp = &bigpalloc;
	lock(&palloc);
	color = getpgcolor(va);

	DBG("newpage up %#p va %#ullx pgsz %#ullx free %uld bigfree %uld\n",
		up, va, pgsz, palloc.freecount, bigpalloc.freecount);
	if(pp == &palloc && (pp->freecount % 100) == 0 && pp->freecount != last)
		DBG("newpage: %uld free 4K pages\n", palloc.freecount);
	if(pp == &bigpalloc && pp->freecount <= 5 && pp->freecount != last)
		DBG("newpage: %uld free 2M pages\n", bigpalloc.freecount);
	last = pp->freecount;

	for(;;){
		if(pp == &palloc && pp->freecount < Min4kpages)
			splitbigpage();
		if(pp->freecount > 1)
			break;

		unlock(&palloc);
		dontalloc = 0;
		if(s && *s) {
			qunlock(&((*s)->lk));
			*s = 0;
			dontalloc = 1;
		}
		kickpager();

		/*
		 * If called from fault and we lost the segment from
		 * underneath don't waste time allocating and freeing
		 * a page. Fault will call newpage again when it has
		 * reacquired the segment locks
		 */
		if(dontalloc)
			return 0;

		lock(&palloc);
	}

	/* First try for our colour */
	for(p = pp->head; p; p = p->next)
		if(p->color == color)
			break;

	ct = PG_NOFLUSH;
	if(p == 0) {
		p = pp->head;
		p->color = color;
		ct = PG_NEWCOL;
	}

	pageunchain(p);

	lock(p);
	if(p->ref != 0)
		panic("newpage pa %#ullx", p->pa);

	uncachepage(p);
	p->ref++;
	p->va = va;
	p->modref = 0;
	for(i = 0; i < MAXMACH; i++)
		p->cachectl[i] = ct;
	unlock(p);
	unlock(&palloc);

	if(clear) {
		k = kmap(p);
		memset((void*)VA(k), 0, 1<<p->lgsize);
		kunmap(k);
	}

	return p;
}

int
ispages(void *)
{
	return bigpalloc.freecount > 0;
}

void
putpage(Page *p)
{
	if(onswap(p)) {
		putswap(p);
		return;
	}

	lock(&palloc);
	lock(p);

	if(p->ref == 0)
		panic("putpage");

	if(--p->ref > 0) {
		unlock(p);
		unlock(&palloc);
		return;
	}

	if(p->image && p->image != &swapimage)
		pagechaintail(p);
	else
		pagechainhead(p);

	if(palloc.r.p != 0)
		wakeup(&palloc.r);

	unlock(p);
	unlock(&palloc);
}

Page*
auxpage(void)
{
	Page *p;

	lock(&palloc);
	p = palloc.head;
	if(palloc.freecount < swapalloc.highwater) {
		unlock(&palloc);
		return 0;
	}
	pageunchain(p);

	lock(p);
	if(p->ref != 0)
		panic("auxpage");
	p->ref++;
	uncachepage(p);
	unlock(p);
	unlock(&palloc);

	return p;
}

static int dupretries = 15000;

int
duppage(Page *p)				/* Always call with p locked */
{
	Palloc *pp;
	Page *np;
	int color;
	int retries;

	retries = 0;
retry:

	if(retries++ > dupretries){
		print("duppage %d, up %#p\n", retries, up);
		dupretries += 100;
		if(dupretries > 100000)
			panic("duppage\n");
		uncachepage(p);
		return 1;
	}


	/* don't dup pages with no image */
	if(p->ref == 0 || p->image == nil || p->image->notext)
		return 0;

	/*
	 *  normal lock ordering is to call
	 *  lock(&palloc) before lock(p).
	 *  To avoid deadlock, we have to drop
	 *  our locks and try again.
	 */
	if(!canlock(&palloc)){
		unlock(p);
		if(up)
			sched();
		lock(p);
		goto retry;
	}

	pp = getalloc(p);
	/* No freelist cache when memory is very low */
	if(pp->freecount < swapalloc.highwater) {
		unlock(&palloc);
		uncachepage(p);
		return 1;
	}

	color = getpgcolor(p->va);
	for(np = pp->head; np; np = np->next)
		if(np->color == color)
			break;

	/* No page of the correct color */
	if(np == 0) {
		unlock(&palloc);
		uncachepage(p);
		return 1;
	}

	pageunchain(np);
	pagechaintail(np);

/*
* XXX - here's a bug? - np is on the freelist but it's not really free.
* when we unlock palloc someone else can come in, decide to
* use np, and then try to lock it.  they succeed after we've
* run copypage and cachepage and unlock(np).  then what?
* they call pageunchain before locking(np), so it's removed
* from the freelist, but still in the cache because of
* cachepage below.  if someone else looks in the cache
* before they remove it, the page will have a nonzero ref
* once they finally lock(np).
*/
	lock(np);
	unlock(&palloc);

	/* Cache the new version */
	uncachepage(np);
	np->va = p->va;
	np->daddr = p->daddr;
	copypage(p, np);
	cachepage(np, p->image);
	unlock(np);
	uncachepage(p);

	return 0;
}

void
copypage(Page *f, Page *t)
{
	KMap *ks, *kd;

	if(f->lgsize != t->lgsize)
		panic("copypage");
	ks = kmap(f);
	kd = kmap(t);
	memmove((void*)VA(kd), (void*)VA(ks), 1<<t->lgsize);
	kunmap(ks);
	kunmap(kd);
}

void
uncachepage(Page *p)			/* Always called with a locked page */
{
	Page **l, *f;

	if(p->image == 0)
		return;

	lock(&palloc.hashlock);
	l = &pghash(p->daddr);
	for(f = *l; f; f = f->hash) {
		if(f == p) {
			*l = p->hash;
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
	putimage(p->image);
	p->image = 0;
	p->daddr = 0;
}

void
cachepage(Page *p, Image *i)
{
	Page **l;

	/* If this ever happens it should be fixed by calling
	 * uncachepage instead of panic. I think there is a race
	 * with pio in which this can happen. Calling uncachepage is
	 * correct - I just wanted to see if we got here.
	 */
	if(p->image)
		panic("cachepage");

	incref(i);
	lock(&palloc.hashlock);
	p->image = i;
	l = &pghash(p->daddr);
	p->hash = *l;
	*l = p;
	unlock(&palloc.hashlock);
}

void
cachedel(Image *i, ulong daddr)
{
	Page *f, **l;

	lock(&palloc.hashlock);
	l = &pghash(daddr);
	for(f = *l; f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			lock(f);
			if(f->image == i && f->daddr == daddr){
				*l = f->hash;
				putimage(f->image);
				f->image = 0;
				f->daddr = 0;
			}
			unlock(f);
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
}

Page *
lookpage(Image *i, ulong daddr)
{
	Page *f;

	lock(&palloc.hashlock);
	for(f = pghash(daddr); f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			unlock(&palloc.hashlock);

			lock(&palloc);
			lock(f);
			if(f->image != i || f->daddr != daddr) {
				unlock(f);
				unlock(&palloc);
				return 0;
			}
			if(++f->ref == 1)
				pageunchain(f);
			unlock(&palloc);
			unlock(f);

			return f;
		}
	}
	unlock(&palloc.hashlock);

	return 0;
}

uvlong
pagereclaim(int npages)
{
	Page *p;
	uvlong ticks;

	lock(&palloc);
	ticks = fastticks(nil);

	/*
	 * All the pages with images backing them are at the
	 * end of the list (see putpage) so start there and work
	 * backward.
	 */
	for(p = palloc.tail; p && p->image && npages > 0; p = p->prev) {
		if(p->ref == 0 && canlock(p)) {
			if(p->ref == 0) {
				npages--;
				uncachepage(p);
			}
			unlock(p);
		}
	}
	ticks = fastticks(nil) - ticks;
	unlock(&palloc);

	return ticks;
}

Pte*
ptecpy(Pte *old)
{
	Pte *new;
	Page **src, **dst;

	new = ptealloc();
	dst = &new->pages[old->first-old->pages];
	new->first = dst;
	for(src = old->first; src <= old->last; src++, dst++)
		if(*src) {
			if(onswap(*src))
				dupswap(*src);
			else {
				lock(*src);
				(*src)->ref++;
				unlock(*src);
			}
			new->last = dst;
			*dst = *src;
		}

	return new;
}

Pte*
ptealloc(void)
{
	Pte *new;

	new = smalloc(sizeof(Pte));
	new->first = &new->pages[PTEPERTAB];
	new->last = new->pages;
	return new;
}

void
freepte(Segment *s, Pte *p)
{
	int ref;
	void (*fn)(Page*);
	Page *pt, **pg, **ptop;

	switch(s->type&SG_TYPE) {
	case SG_PHYSICAL:
		fn = s->pseg->pgfree;
		ptop = &p->pages[PTEPERTAB];
		if(fn) {
			for(pg = p->pages; pg < ptop; pg++) {
				if(*pg == 0)
					continue;
				(*fn)(*pg);
				*pg = 0;
			}
			break;
		}
		for(pg = p->pages; pg < ptop; pg++) {
			pt = *pg;
			if(pt == 0)
				continue;
			lock(pt);
			ref = --pt->ref;
			unlock(pt);
			if(ref == 0)
				free(pt);
		}
		break;
	default:
		for(pg = p->first; pg <= p->last; pg++)
			if(*pg) {
				putpage(*pg);
				*pg = 0;
			}
	}
	free(p);
}

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * There's no pager process here.
 * One process waiting for memory becomes the pager,
 * during the call to kickpager()
 */

enum
{
	Minpages = 2
};

static QLock	pagerlck;
static struct
{
	ulong ntext;
	ulong nbig;
	ulong nall;
} pstats;

void
swapinit(void)
{
}

void
putswap(Page*)
{
	panic("putswap");
}

void
dupswap(Page*)
{
	panic("dupswap");
}

int
swapcount(ulong daddr)
{
	USED(daddr);
	return 0;
}

static int
canflush(Proc *p, Segment *s)
{
	int i, x;

	lock(s);
	if(s->ref == 1) {		/* Easy if we are the only user */
		s->ref++;
		unlock(s);
		return canpage(p);
	}
	s->ref++;
	unlock(s);

	/* Now we must do hardwork to ensure all processes which have tlb
	 * entries for this segment will be flushed if we succeed in paging it out
	 */
	for(x = 0; (p = psincref(x)) != nil; x++){
		if(p->state != Dead) {
			for(i = 0; i < NSEG; i++){
				if(p->seg[i] == s && !canpage(p)){
					psdecref(p);
					return 0;
				}
			}
		}
		psdecref(p);
	}
	return 1;
}

static int
pageout(Proc *p, Segment *s)
{
	int i, size, n;
	Pte *l;
	Page **pg, *entry;

	if((s->type&SG_TYPE) != SG_TEXT)
		panic("pageout");

	if(!canqlock(&s->lk))	/* We cannot afford to wait, we will surely deadlock */
		return 0;

	if(s->steal){		/* Protected by /dev/proc */
		qunlock(&s->lk);
		return 0;
	}

	if(!canflush(p, s)){	/* Able to invalidate all tlbs with references */
		qunlock(&s->lk);
		putseg(s);
		return 0;
	}

	if(waserror()){
		qunlock(&s->lk);
		putseg(s);
		return 0;
	}

	/* Pass through the pte tables looking for text memory pages to put */
	n = 0;
	size = s->mapsize;
	for(i = 0; i < size; i++){
		l = s->map[i];
		if(l == 0)
			continue;
		for(pg = l->first; pg < l->last; pg++){
			entry = *pg;
			if(pagedout(entry))
				continue;
			n++;
			if(entry->modref & PG_REF){
				entry->modref &= ~PG_REF;
				continue;
			}
			putpage(*pg);
			*pg = nil;
		}
	}
	poperror();
	qunlock(&s->lk);
	putseg(s);
	return n;
}

static void
pageouttext(int pgszi, int color)
{

	Proc *p;
	Pgsza *pa;
	int i, n, np, x;
	Segment *s;
	int prepaged;

	USED(color);
	pa = &pga.pgsza[pgszi];
	n = x = 0;
	prepaged = 0;

	/*
	 * Try first to steal text pages from non-prepaged processes,
	 * then from anyone.
	 */
Again:
	do{
		if((p = psincref(x)) == nil)
			break;
		np = 0;
		if(p->prepagemem == 0 || prepaged != 0)
		if(p->state != Dead && p->noswap == 0 && canqlock(&p->seglock)){
			for(i = 0; i < NSEG; i++){
				if((s = p->seg[i]) == nil)
					continue;
				if((s->type&SG_TYPE) == SG_TEXT)
					np = pageout(p, s);
			}
			qunlock(&p->seglock);
		}
		/*
		 * else process dead or locked or changing its segments
		 */
		psdecref(p);
		n += np;
		if(np > 0)
			DBG("pager: %d from proc #%d %#p\n", np, x, p);
		x++;
	}while(pa->freecount < Minpages);

	if(pa->freecount < Minpages && prepaged++ == 0)
		goto Again;
}

static void
freepages(int si, int once)
{
	Pgsza *pa;
	Page *p;

	for(; si < m->npgsz; si++){
		pa = &pga.pgsza[si];
		if(pa->freecount > 0){
			DBG("kickpager() up %#p: releasing %udK pages\n",
				up, m->pgsz[si]/KiB);
			lock(&pga);
			if(pa->freecount == 0){
				unlock(&pga);
				continue;
			}
			p = pa->head;
			pageunchain(p);
			unlock(&pga);
			if(p->ref != 0)
				panic("freepages pa %#ullx", p->pa);
			pgfree(p);
			if(once)
				break;
		}
	}
}

static int
tryalloc(int pgszi, int color)
{
	Page *p;

	p = pgalloc(m->pgsz[pgszi], color);
	if(p != nil){
		lock(&pga);
		pagechainhead(p);
		unlock(&pga);
		return 0;
	}
	return -1;
}

/*
 * Someone thinks pages of size m->pgsz[pgszi] are needed
 * and is trying to make them available.
 * Many processes may be calling this at the same time,
 * in which case they will enter one by one. Only when more than
 * Minpages are available they will simply return.
 */
void
kickpager(int pgszi, int color)
{
	Pgsza *pa;

	if(DBGFLG>1)
		DBG("kickpager() %#p\n", up);
	if(waserror())
		panic("error in kickpager");
	qlock(&pagerlck);
	pa = &pga.pgsza[pgszi];

	/*
	 * First try allocating from physical memory.
	 */
	tryalloc(pgszi, color);
	if(pa->freecount > Minpages)
		goto Done;

	/*
	 * If pgszi is <= page size for text (assumed to be 2M)
	 * try to release text pages.
	 */
	if(m->pgsz[pgszi] <= 2*MiB){
		pstats.ntext++;
		DBG("kickpager() up %#p: reclaiming text pages\n", up);
		pageouttext(pgszi, color);
		tryalloc(pgszi, color);
		if(pa->freecount > Minpages){
			DBG("kickpager() found %uld free\n", pa->freecount);
			goto Done;
		}
	}

	/*
	 * Try releasing memory from one bigger page, perhaps from text
	 * pages released in the previous step.
	 */
	pstats.nbig++;
	freepages(pgszi+1, 1);
	while(tryalloc(pgszi, color) != -1 && pa->freecount < Minpages)
		;
	if(pa->freecount > 1){
		DBG("kickpager() found %uld free\n", pa->freecount);
		goto Done;
	}
	/*
	 * Try releasing memory from all pages.
	 */
	pstats.nall++;
	DBG("kickpager() up %#p: releasing all pages\n", up);
	freepages(0, 0);
	tryalloc(pgszi, color);
	if(pa->freecount > 1){
		DBG("kickpager() found %uld free\n", pa->freecount);
		goto Done;
	}
	panic("kickpager(): no physical memory");
Done:
	poperror();
	qunlock(&pagerlck);
	if(DBGFLG>1)
		DBG("kickpager() done %#p\n", up);
}

void
pagersummary(void)
{
	print("ntext %uld nbig %uld nall %uld\n",
		pstats.ntext, pstats.nbig, pstats.nall);
	print("no swap\n");
}

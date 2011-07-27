#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum
{
	Minpages = 2
};

Image 	swapimage;
QLock	pagerlck;

void
swapinit(void)
{
	swapimage.notext = 1;
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

/*
 * Someone thinks memory is needed.
 * Try to page out some text pages and keep on doing so
 * while needpages() and we could do something about it.
 * We ignore prepaged processes in a first pass.
 */
void
kickpager(void)
{
	Proc *p;
	int i, n, np, x;
	Segment *s;
	int prepaged;
	static int nwant;

	DBG("kickpager() %#p\n", up);
	if(waserror())
		panic("error in kickpager");
	qlock(&pagerlck);
	if(bigpalloc.freecount > Minpages)
		goto Done;
	n = x = 0;
	prepaged = 0;
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
	}while(bigpalloc.freecount < Minpages);
	if(bigpalloc.freecount  < Minpages){
		if(prepaged++ == 0)
			goto Again;
		panic("no physical memory");
	}
Done:
	qunlock(&pagerlck);
	DBG("kickpager() done %#p\n", up);
}

void
pagersummary(void)
{
	print("no swap\n");
}


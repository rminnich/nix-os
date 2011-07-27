#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

int
fault(uintptr addr, int read)
{
	Segment *s;
	char *sps;

if(up->nlocks) print("fault nlocks %d\n", up->nlocks);

	sps = up->psstate;
	up->psstate = "Fault";
	spllo();

	m->pfault++;
	for(;;) {
		s = seg(up, addr, 1);		/* leaves s->lk qlocked if seg != nil */
		if(s == 0) {
			up->psstate = sps;
			return -1;
		}

		if(!read && (s->type&SG_RONLY)) {
			qunlock(&s->lk);
			up->psstate = sps;
			return -1;
		}

		if(fixfault(s, addr, read, 1) == 0)
			break;

		/*
		 * See the comment in newpage that describes
		 * how to get here.
		 */
	}

	up->psstate = sps;
	return 0;
}

static void
faulterror(char *s, Chan *c, int freemem)
{
	char buf[ERRMAX];

	if(c && c->path){
		snprint(buf, sizeof buf, "%s accessing %s: %s", s, c->path->s, up->errstr);
		s = buf;
	}
	if(up->nerrlab) {
		postnote(up, 1, s, NDebug);
		error(s);
	}
	pexit(s, freemem);
}

int
fixfault(Segment *s, uintptr addr, int read, int dommuput)
{
	int type;
	int ref;
	Pte **p, *etp;
	uintptr soff;
	physaddr mmuphys;
	Page **pg, *lkp, *new;
	Page *(*fn)(Segment*, uintptr);

	addr &= ~(BIGPGSZ-1);
	soff = addr-s->base;
	p = &s->map[soff/PTEMAPMEM];
	if(*p == 0)
		*p = ptealloc();

	etp = *p;
	pg = &etp->pages[(soff&(PTEMAPMEM-1))/BIGPGSZ];
	type = s->type&SG_TYPE;

	if(pg < etp->first)
		etp->first = pg;
	if(pg > etp->last)
		etp->last = pg;

	mmuphys = 0;
	switch(type) {
	default:
		panic("fault");
		break;

	case SG_TEXT: 			/* Demand load */
		if(pagedout(*pg))
			pio(s, addr, soff, pg);

		mmuphys = BIGPPN((*pg)->pa) | PTERONLY|PTEVALID;
		(*pg)->modref = PG_REF;
		break;

	case SG_BSS:
	case SG_SHARED:			/* Zero fill on demand */
	case SG_STACK:
		if(*pg == 0) {
			new = newpage(1, &s, addr, BIGPGSZ);
			if(s == 0)
				return -1;

			*pg = new;
		}
		goto common;

	case SG_DATA:
	common:			/* Demand load/pagein/copy on write */
		if(pagedout(*pg))
			pio(s, addr, soff, pg);

		/*
		 *  It's only possible to copy on write if
		 *  we're the only user of the segment.
		 */
		if(read && conf.copymode == 0 && s->ref == 1) {
			mmuphys = BIGPPN((*pg)->pa)|PTERONLY|PTEVALID;
			(*pg)->modref |= PG_REF;
			break;
		}

		lkp = *pg;
		lock(lkp);

		if(lkp->image == &swapimage)
			ref = lkp->ref + swapcount(lkp->daddr);
		else
			ref = lkp->ref;
		if(ref > 1) {
			unlock(lkp);

			new = newpage(0, &s, addr, BIGPGSZ);
			if(s == 0)
				return -1;
			*pg = new;
			copypage(lkp, *pg);
			putpage(lkp);
		}
		else {
			/* save a copy of the original for the image cache */
			if(lkp->image != nil)
				duppage(lkp);

			unlock(lkp);
		}
		mmuphys = BIGPPN((*pg)->pa) | PTEWRITE | PTEVALID;
		(*pg)->modref = PG_MOD|PG_REF;
		break;

	case SG_PHYSICAL:
		if(*pg == 0) {
			fn = s->pseg->pgalloc;
			if(fn)
				*pg = (*fn)(s, addr);
			else {
				new = smalloc(sizeof(Page));
				new->va = addr;
				new->pa = s->pseg->pa+(addr-s->base);
				new->ref = 1;
				new->lgsize = s->pseg->lgpgsize;
				*pg = new;
			}
		}

		mmuphys = BIGPPN((*pg)->pa) |PTEVALID;
		if((s->pseg->attr & SG_RONLY) == 0)
			mmuphys |= PTEWRITE;
		if((s->pseg->attr & SG_CACHED) == 0)
			mmuphys |= PTEUNCACHED;
		(*pg)->modref = PG_MOD|PG_REF;
		break;
	}
	qunlock(&s->lk);

	if(dommuput)
		mmuput(addr, mmuphys, *pg);

	return 0;
}

void
pio(Segment *s, uintptr addr, ulong soff, Page **p)
{
	Page *new;
	KMap *k;
	Chan *c;
	int n, ask;
	char *kaddr;
	ulong daddr;
	Page *loadrec;

retry:
	loadrec = *p;
	if(loadrec == nil) {	/* from a text/data image */
		daddr = s->fstart+soff;
		new = lookpage(s->image, daddr);
		if(new != nil) {
			*p = new;
			return;
		}

		c = s->image->c;
		ask = s->flen-soff;
		if(ask > BIGPGSZ)
			ask = BIGPGSZ;
	}
	else {			/* from a swap image */
		daddr = swapaddr(loadrec);
		new = lookpage(&swapimage, daddr);
		if(new != nil) {
			putswap(loadrec);
			*p = new;
			return;
		}

		c = swapimage.c;
		ask = BIGPGSZ;
	}
	qunlock(&s->lk);

	new = newpage(0, 0, addr, BIGPGSZ);
	k = kmap(new);
	kaddr = (char*)VA(k);

	while(waserror()) {
		if(strcmp(up->errstr, Eintr) == 0)
			continue;
		kunmap(k);
		putpage(new);
		faulterror(Eioload, c, 0);
	}

	n = c->dev->read(c, kaddr, ask, daddr);
	if(n != ask)
		faulterror(Eioload, c, 0);
	if(ask < BIGPGSZ)
		memset(kaddr+ask, 0, BIGPGSZ-ask);

	poperror();
	kunmap(k);

	qlock(&s->lk);
	if(loadrec == nil) {	/* This is demand load */
		/*
		 *  race, another proc may have gotten here first while
		 *  s->lk was unlocked
		 */
		if(*p == nil) {
			new->daddr = daddr;
			cachepage(new, s->image);
			*p = new;
		}
		else
			putpage(new);
	}
	else {			/* This is paged out */
		/*
		 *  race, another proc may have gotten here first
		 *  (and the pager may have run on that page) while
		 *  s->lk was unlocked
		 */
		if(*p != loadrec){
			if(!pagedout(*p)){
				/* another process did it for me */
				putpage(new);
				goto done;
			} else {
				/* another process and the pager got in */
				putpage(new);
				goto retry;
			}
		}

		new->daddr = daddr;
		cachepage(new, &swapimage);
		*p = new;
		putswap(loadrec);
	}

done:
	if(s->flushme)
		memset((*p)->cachectl, PG_TXTFLUSH, sizeof((*p)->cachectl));
}

/*
 * Called only in a system call
 */
int
okaddr(uintptr addr, long len, int write)
{
	Segment *s;

	if(len >= 0) {
		for(;;) {
			s = seg(up, addr, 0);
			if(s == 0 || (write && (s->type&SG_RONLY)))
				break;

			if(addr+len > s->top) {
				len -= s->top - addr;
				addr = s->top;
				continue;
			}
			return 1;
		}
	}
	return 0;
}

void*
validaddr(void* addr, long len, int write)
{
	if(!okaddr(PTR2UINT(addr), len, write)){
		pprint("suicide: invalid address %#p/%ld in sys call pc=%#p\n",
			addr, len, userpc(nil));
		pexit("Suicide", 0);
	}

	return UINT2PTR(addr);
}

/*
 * &s[0] is known to be a valid address.
 * Assume 4K pages, so it works for both 4K and 2M pages.
 */
void*
vmemchr(void *s, int c, int n)
{
	int m;
	uintptr a;
	void *t;

	a = PTR2UINT(s);
	while(PGROUND(a) != PGROUND(a+n-1)){
		/* spans pages; handle this page */
		m = PGSZ - (a & (PGSZ-1));
		t = memchr(UINT2PTR(a), c, m);
		if(t)
			return t;
		a += m;
		n -= m;
		if((a & KZERO) != KZERO)
			validaddr(UINT2PTR(a), 1, 0);
	}

	/* fits in one page */
	return memchr(UINT2PTR(a), c, n);
}

Segment*
seg(Proc *p, uintptr addr, int dolock)
{
	Segment **s, **et, *n;

	et = &p->seg[NSEG];
	for(s = p->seg; s < et; s++) {
		n = *s;
		if(n == 0)
			continue;
		if(addr >= n->base && addr < n->top) {
			if(dolock == 0)
				return n;

			qlock(&n->lk);
			if(addr >= n->base && addr < n->top)
				return n;
			qunlock(&n->lk);
		}
	}

	return 0;
}

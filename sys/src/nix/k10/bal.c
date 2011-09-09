#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/*
 * Buddy allocator from forsyth, possible tweaked to
 * be used in nix for physical memory allocation.
 *
 * - locks
 * - auxilary structure allocation and sizing
 * - largest size
 * - could instead coalesce free items on demand (cf. Wulf)
 * - or lazy buddy (cf. Barkley)
 */

#define	DPRINT	if(0)iprint

enum{
	MinK=		16,		/* 64k */
	MinBsize=	1<<MinK,
	Aspace=		2*GiB,
	MaxBlocks=	Aspace/MinBsize,
	MaxK=		30,		/* last usable k */

	Busy=		0x80,	/* bit set in byte map if block busy */
};

typedef struct Blk Blk;
struct Blk{
	Blk*	forw;	/* free list */
	Blk*	back;
};

typedef struct Bfree Bfree;
struct Bfree{
	Blk;	/* header */
	Lock;
};

static	Bfree	blist[32];		/* increasing powers of two */
static	uchar	kofb[MaxBlocks+1];	/* k(block_index) with top bit set if busy */
static	Blk	blocks[MaxBlocks+1];	/* free list pointers */
#define	aspace	0
static	uchar	log2v[256];

#define	BI(a)	(((a)-aspace)>>MinK)
#define	IB(x)	(((x)<<MinK)+aspace)

static	Lock	balk;	/* TO DO: localise lock (need CAS update of kofb) */

static void
loginit(void)
{
	int i;

	for(i=2; i<nelem(log2v); i++)
		log2v[i] = log2v[i/2] + 1;
}

static int
log2of(uint n)
{
	int r;

	r = (n & (n-1)) != 0;	/* not a power of two => round up */
	if((n>>8) == 0)
		return log2v[n] + r;
	if((n>>16) == 0)
		return 8 + log2v[n>>8] + r;
	if((n>>24) == 0)
		return 16 + log2v[n>>16] + r;
	return 24 + log2v[n>>24] + r;
}

void
balinit(physaddr base, usize size)
{
	int k;
	Blk *b;

	loginit();
	for(k = 0; k < nelem(blist); k++){
		b = &blist[k];
		b->forw = b->back = b;
	}
	memset(kofb, 0, sizeof(kofb));
	balfreephys(base, size);
	DPRINT("Aspace=%#ux MaxBlocks=%#ux (%d) len bdesc=%d\n",
		Aspace, MaxBlocks, MaxBlocks, nelem(blocks));
	baldump();
}

physaddr
bal(usize size)
{
	int j, k;
	Blk *b, *b2;
	physaddr a, a2;
	uint bi;

	k = log2of(size);
	if(k < MinK)
		k = MinK;
	DPRINT("size=%lud k=%d\n", size, k);
	lock(&balk);
	for(j = k;;){
		b = blist[j].forw;
		if(b != &blist[j])
			break;
		if(++j > MaxK){
			unlock(&balk);
			return 0;	/* out of space */
		}
	}
	b->forw->back = b->back;
	b->back->forw = b->forw;
	/* set busy state */
	bi = b-blocks;
	a = IB(bi);
	kofb[bi] = k | Busy;
	while(j != k){
		/* split */
		j--;
		a2 = a+((physaddr)1<<j);
		bi = BI(a2);
		DPRINT("split %#llux %#llux k=%d %#llux kofb=%#ux\n", a, a2, j, (physaddr)1<<j, kofb[bi]);
		if(kofb[bi] & Busy)
			panic("bal: busy block %#llux k=%d\n", a, kofb[bi] & ~Busy);
		kofb[bi] = j;	/* new size */
		b2 = &blocks[bi];
		b2->forw = &blist[j];
		b2->back = blist[j].back;
		blist[j].back = b2;
		b2->back->forw = b2;
	}
	unlock(&balk);
	return a;
}

void
bfree(physaddr a, usize size)
{
	int k;
	Blk *b, *b2;
	physaddr a2;
	uint bi, bi2;

	k = log2of(size);	/* could look it up in kofb */
	if(k < MinK)
		k = MinK;
	DPRINT("free %#llux %d\n", a, k);
	bi = BI(a);
	lock(&balk);
	if(kofb[bi] != 0 && kofb[bi] != (Busy|k)){
		unlock(&balk);
		panic("balfree: busy %#llux odd k k=%d kfob=%#ux\n", a, k, kofb[bi]);
	}
	for(; k != MaxK; k++){
		a2 = a ^ ((physaddr)1<<k);	/* buddy */
		bi2 = BI(a2);
		b2 = &blocks[bi2];
		if(kofb[bi2] != k)	/* note this also ensures not busy */
			break;
		DPRINT("combine %#llux %#llux %d %#llux\n", a, a2, k, (physaddr)1<<k);
		b2->back->forw = b2->forw;
		b2->forw->back = b2->back;
		if(a2 < a)
			a = a2;
	}
	kofb[bi] = k;	/* sets size and resets Busy */
	b = &blocks[bi];
	b->forw = &blist[k];
	b->back = blist[k].back;
	blist[k].back = b;
	b->back->forw = b;
	unlock(&balk);
}

void
balfreephys(physaddr base, usize size)
{
	physaddr top, a, lim;
	usize m;
	int i;

	/* round base to min block size */
	if(base & (MinBsize-1)){
		i = MinBsize - (base & (MinBsize-1));
		base += i;
		size -= i;
	}
	size &= ~(MinBsize-1);
	if(size < MinBsize)
		return;
	DPRINT("%#.8llux %#lux (%lud) start\n", base, size, size);
	if(BI(base+size) >= MaxBlocks)
		panic("balfreephys: address space too large");
	/* split base and size into suitable chunks */
	for(top = MinBsize; top < base+size; top <<= 1)
		{}
	/* free maximal power-of-2 chunks below mid-point */
	lim = base+size;
	a = top>>1;
	m = a>>1;
	DPRINT("a=%llux m=%#lux (%ld)\n", a, m, m);
	if(m > ((usize)1<<MaxK))
		m = 1<<MaxK;
	while(m >= MinBsize){
		DPRINT("a==%#llux m=%#lux base=%#llux\n", a, m, base);
		if(a-m >= base && a <= lim){
			a -= m;
			bfree(a, m);
		}else
			m >>= 1;
	}
	/* free chunks above mid-point */
	a = top>>1;
	m = a>>1;
	if(m > ((usize)1<<MaxK))
		m = 1<<MaxK;
	while(m >= MinBsize){
		DPRINT("a[2]==%#llux m=%#lux base=%#llux\n", a, m, base);
		if(a >= base && a+m <= lim){
			bfree(a, m);
			a += m;
		}else
			m >>= 1;
	}
}

void
baldump(void)
{
	physaddr a;
	uint bi;
	int i, k;
	Blk *b;

	for(i=0; i<nelem(blist); i++){
		b = blist[i].forw;
		if(b != &blist[i]){
			print("%d	", i);
			for(; b != &blist[i]; b = b->forw){
				bi = b-blocks;
				a = IB(bi);
				k = kofb[bi];
				print(" [%#llux %d %#ux b=%#llux]", a, k, 1<<k, a^((physaddr)1<<k));
			}
			print("\n");
		}
	}
}

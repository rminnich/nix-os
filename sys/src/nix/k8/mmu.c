#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "amd64.h"

#define PTEX(pi, lvl)	(((pi)>>((((lvl)-1)*9)+12)) & ((1<<PTPGSHFT)-1))

static Page mach0pml4;

/*
 * Nemo: NB:
 * m->pml4 is always the same.
 * sched->procsave->flushtbl zeroes the entries in pml4 up to
 * pml4->daddr (which is the number of entries used by the user, or
 * the index in the upper page table for this entry)
 * the new process will fault and we populate again the page table
 * as needed.
 *
 * mmuptp[0] is used to keep a free list of pages.
 * mmuptp[1-3] are used to keep PT pages for each level
 * 4K pages: pml4 -> lvl3 -> lvl2 -> lvl1 ->pg
 * 2M pages: pml4 -> lvl3 -> lvl2 -> pg
 *
 * Therefore, we can't use pml4 in other processors. Each one
 * has to play the same trick at least, using its own pml4.
 * For NIX, we have to fill up the pml4 of the application core
 * so it wont fault.
 */

void
iprintva(char *s, uintptr addr)
{
	iprint("%s = %#ullx = %#ullx:%#ullx:%#ullx:%#ullx", s, addr,
			(addr >> (21+9+9)) & 0x1FF,
			(addr >> (21+9)) & 0x1FF,
			(addr >> 21) & 0x1FF,
			addr & 0x1FFFFF);
}

void
mmuinit(void)
{
	u64int r;
	Page *page;
	uchar *p;

	if(m->machno == 0)
		page = &mach0pml4;
	else{
		/* NIX: KLUDGE: Has to go when each mach is using
		 * its own page table
		 */
		p = UINT2PTR(m->stack);
		p += MACHSTKSZ;
		memmove(p, UINT2PTR(mach0pml4.va), PTPGSZ);
		m->pml4 = &m->pml4kludge;
		m->pml4->va = PTR2UINT(p);
		m->pml4->pa = PADDR(p);
		m->pml4->daddr = mach0pml4.daddr;	/* # of user mappings in pml4 */

		r = rdmsr(Efer);
		r |= Nxe;
		wrmsr(Efer, r);
		cr3put(m->pml4->pa);
		DBG("m %#p pml4 %#p\n", m, m->pml4);
		return;
	}

	page->pa = cr3get();
	page->va = PTR2UINT(KADDR(page->pa));

	m->pml4 = page;

	r = rdmsr(Efer);
	r |= Nxe;
	wrmsr(Efer, r);
}

void
mmuflushtlb(u64int)
{
	m->tlbpurge++;
	if(m->pml4->daddr){
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->daddr*sizeof(PTE));
		m->pml4->daddr = 0;
	}
	cr3put(m->pml4->pa);
}

void
mmuflush(void)
{
	int s;

	s = splhi();
	up->newtlb = 1;
	mmuswitch(up);
	splx(s);
}

static void
mmuptpfree(Proc* proc, int clear)
{
	int l;
	PTE *pte;
	Page **last, *page;

	for(l = 1; l < 4; l++){
		last = &proc->mmuptp[l];
		if(*last == nil)
			continue;
		for(page = *last; page != nil; page = page->next){
			if(l <= 2 && clear)
				memset(UINT2PTR(page->va), 0, PTPGSZ);
			pte = UINT2PTR(page->prev->va);
			pte[page->daddr] = 0;
			last = &page->next;
		}
		*last = proc->mmuptp[0];
		proc->mmuptp[0] = proc->mmuptp[l];
		proc->mmuptp[l] = nil;
	}

	m->pml4->daddr = 0;
}

static void
tabs(int n)
{
	int i;

	for(i = 0; i < n; i++)
		print("  ");
}

void
dumpptepg(int lvl, uintptr pa)
{
	PTE *pte;
	int tab, i;

	tab = 4 - lvl;
	pte = UINT2PTR(KADDR(pa));
	for(i = 0; i < PTPGSZ/sizeof(PTE); i++)
		if(pte[i] & PteP){
			tabs(tab);
			print("l%d[%#05x]:\t%#ullx\n", lvl, i, pte[i]);
			if(lvl == 3 && i == 0x1FF){
				/* don't print lvl 2 kernel entries */
				tabs(tab+1);
				print("...kern...\n");
				continue;
			}
			if(lvl > 2)
				dumpptepg(lvl-1, PPN(pte[i]));
		}
	print("\n");
}

void
dumpmmu(Proc *p)
{
	int i;
	Page *pg;

	print("proc %#p\n", p);
	for(i = 3; i > 0; i--){
		print("mmuptp[%d]:\n", i);
		for(pg = p->mmuptp[i]; pg != nil; pg = pg->next)
			print("\tva %#ullx ppn %#ullx d %#ulx\n",
				pg->va, pg->pa, pg->daddr);
	}
	print("pml4 %#ullx\n", m->pml4->pa);
	dumpptepg(4, m->pml4->pa);
}

void
mmuswitch(Proc* proc)
{
	PTE *pte;
	Page *page;

	if(proc->newtlb){
		/*
 		 * NIX: We cannot clear our page tables if they are going to
		 * be used in the AC
		 */
		if(proc->ac == nil)
			mmuptpfree(proc, 1);
		proc->newtlb = 0;
	}

	if(m->pml4->daddr){
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->daddr*sizeof(PTE));
		m->pml4->daddr = 0;
	}

	pte = UINT2PTR(m->pml4->va);
	for(page = proc->mmuptp[3]; page != nil; page = page->next){
		pte[page->daddr] = PPN(page->pa)|PteU|PteRW|PteP;
		if(page->daddr >= m->pml4->daddr)
			m->pml4->daddr = page->daddr+1;
	}

	tssrsp0(STACKALIGN(PTR2UINT(proc->kstack+KSTACK)));
	cr3put(m->pml4->pa);
}

void
mmurelease(Proc* proc)
{
	Page *page, *next;

	mmuptpfree(proc, 0);

	for(page = proc->mmuptp[0]; page != nil; page = next){
		next = page->next;
		if(--page->ref)
			panic("mmurelease: page->ref %d\n", page->ref);
		pagechainhead(page);
	}
	if(proc->mmuptp[0] && palloc.r.p)
		wakeup(&palloc.r);
	proc->mmuptp[0] = nil;

	tssrsp0(STACKALIGN(m->stack+MACHSTKSZ));
	cr3put(m->pml4->pa);
}

/*
 * We assume that all user space is mapped using 2M pages.
 * In that case, we use three levels, not four.
 */
void
mmuput(uintptr va, physaddr pa, Page *pg)
{
	int user, lvl, x;
	PTE *pte;
	Page *page, *prev;

	DBG("up %#p mmuput %#ullx %#ullx\n", up, va, pa);

	user = (va < KZERO);
	x = PTEX(va, 4);
	pte = UINT2PTR(m->pml4->va);
	pte += x;
	prev = m->pml4;
	for(lvl = 3; lvl > 0; lvl--){
		if(user && lvl == 1)	/* use 2M entry */
			break;
		for(page = up->mmuptp[lvl]; page != nil; page = page->next){
			if(page->prev == prev && page->daddr == x)
				break;
		}
		if(page == nil){
			if(up->mmuptp[0] == 0){
				page = newpage(1, 0, 0, PTPGSZ);
				page->va = VA(kmap(page));
			}
			else {
				page = up->mmuptp[0];
				up->mmuptp[0] = page->next;
			}
			page->daddr = x;
			page->next = up->mmuptp[lvl];
			up->mmuptp[lvl] = page;
			page->prev = prev;
			*pte = PPN(page->pa)|PteU|PteRW|PteP;
			if(lvl == 3 && x >= m->pml4->daddr)
				m->pml4->daddr = x+1;
		}

		x = PTEX(va, lvl);
		pte = UINT2PTR(KADDR(PPN(*pte)));
		pte += x;
		prev = page;
	}

	*pte = pa|PteU;
	if(user){
		if(pg->lgsize == 12)
			panic("mmuput: using 4K page for user page");
		*pte |= PtePS;
	}
	DBG("up %#p new pte %#p = %#ullx\n", up, pte, *pte);

	invlpg(va);			/* only if old entry valid? */
}

PTE*
mmuwalk(PTE* pml4, uintptr va, int level, uintptr (*alloc)(int))
{
	int l, user;
	PTE *pte;
	uintptr pa;

	user = (va < KZERO);
	if(user && level == 1)	/* we now use 2M entries */
		level = 2;
	pte = &pml4[PTEX(va, 4)];
	for(l = 4; l > 0; l--){
		if(l == level)
			return pte;
		if(l == 2 && (*pte & PtePS))	/* 2M entry */
			break;
		if(!(*pte & PteP)){
			if(alloc == nil)
				break;
			pa = alloc(l);
			if(pa == ~0ull)
				break;
			*pte = PPN(pa)|PteRW|PteP;
		}
		pte = UINT2PTR(KADDR(PPN(*pte)));
		pte += PTEX(va, l-1);
	}

	return nil;
}

static Lock mmukmaplock;

int
mmukmapsync(uvlong va)
{
	PTE *pte;

	/*
	 * XXX - replace with new vm stuff.
	 */
	ilock(&mmukmaplock);
	pte = mmuwalk(UINT2PTR(m->pml4->va), va, 1, nil);
	if(pte == nil){
		iunlock(&mmukmaplock);
		return 0;
	}

	if(!(*pte & PteP)){
		iunlock(&mmukmaplock);
		return 0;
	}
	cr3put(m->pml4->pa);			/* need this? */
	iunlock(&mmukmaplock);

	return 1;
}

static uintptr
mmukmapptalloc(int)
{
	void *v;
	uchar *p, *pe;

	/*
	 * XXX - replace with new vm stuff.
	 */
	if((v = mallocalign(BY2PG, BY2PG, 0, 0)) == nil)
{
print("mmukmapptalloc fails\n");
		return ~0ull;
}
	memset(v, 0, BY2PG);

	p = v;
	pe = p+BY2PG;
	while(p < pe){
		if(*p != 0)
			panic("mmukmapptalloc non-zero\n");
		p++;
	}

	return PADDR(v);
}

uintptr
mmukmap(uintptr va, uintptr pa, usize size)
{
	PTE * pte;
	uintptr ova, pae;
	int sync;

	if(va < KZERO)
		panic("mmukmap: can't do va %#ullx; it is 2M now\n", va);
	/*
	 * XXX - replace with new vm stuff.
	 */
	pa = PPN(pa);
	if(va == 0)
		va = PTR2UINT(KADDR(pa));
	else
		va = PPN(va);
	ova = va;
	pae = pa + size;

	sync = 0;
//print("mmukmap(%#p, %#p, %lud) %#p %#p\n", va, pa, size, ova, pae);

	ilock(&mmukmaplock);
	while(pa < pae){
		pte = mmuwalk(UINT2PTR(m->pml4->va), va, 1, mmukmapptalloc);
		if(pte == nil){
//print("mmuwalk fails pa %#p pae %#p\n", pa, pae);
			iunlock(&mmukmaplock);
			return 0;
		}
		if((*pte & PteP) && PPN(*pte) != pa){
			panic("mmukmap: va %#p -> %#p already mapped with %#p\n",
				va, pa, *pte);
		}
		else{
			*pte = PPN(pa)|PtePCD|PteRW|PteP;
			sync++;
		}

		pa += PGSZ;
		va += PGSZ;
	}
	iunlock(&mmukmaplock);

	/*
	 * If something was added
	 * then need to sync up.
	 */
	if(sync)
		mmukmapsync(ova);

	return pa;
}

void*
vmap(uintptr pa, usize size)
{
	uintptr pae, va;
	usize o, osize;

	/*
	 * XXX - replace with new vm stuff.
	 * Crock after crock - the first 4MB is mapped with 2MB pages
	 * so catch that and return good values because the current mmukmap
	 * will fail.
	 */
	if(pa+size < 4*MiB)
		return UINT2PTR(kseg0|pa);

	osize = size;
	o = pa & (BY2PG-1);
	pa -= o;
	size += o;
	size = ROUNDUP(size, PGSZ);

	va = kseg0|pa;
	pae = mmukmap(va, pa, size);
	if(pae == 0 || pae-size != pa)
		print("vmap(%#p, %ld) called from %#p: mmukmap fails %#p\n",
			pa+o, osize, getcallerpc(&pa), pae);

	return UINT2PTR(va+o);
}

void
vunmap(void* v, usize size)
{
	/*
	 * XXX - replace with new vm stuff.
	 * Can't do this until do real vmap for all space that
	 * might be used, e.g. stuff below 1MB which is currently
	 * mapped automagically at boot but that isn't used (or
	 * at least shouldn't be used) by the kernel.
	upafree(PADDR(v), size);
	 */
	USED(v, size);
}

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"


#define _KADDRx(pa, x)	UINT2PTR(x+((uintptr)(pa)))
#define _PADDRx(va, x)	PTR2UINT(((uintptr)(va)) - x)
#define _KADDR(pa)	UINT2PTR(kseg0+((uintptr)(pa)))
#define _PADDR(va)	PTR2UINT(((uintptr)(va)) - kseg0)
static uvlong maxmem = 128 *MiB;
/* note: Can't really use DBG in here, sorry. */
void configmap(uvlong m)
{
//	DBG("configmap: max is %#p\n", maxmem);
	maxmem = m;
}
void*
KADDR(uintptr pa)
{
	void* va;


	if (pa < MAPATKZERO)
		va = _KADDR(pa);
	else
		va = _KADDRx(pa, 0xffffff8000000000ULL);
	DBG("Kaddr %#p is %#p\n", pa, va);
//	if(PTR2UINT(va) < kseg0)
//		print("pa %#p va #%p @ %#p\n", pa, va, getcallerpc(&pa));
	return va;
}

uintptr
PADDR(void* va)
{
	uvlong pa;

	if ((uvlong)va >= KZERO)
		pa =  _PADDR(va);
	else 
		pa = _PADDRx(va, 0xffffff8000000000ULL);
	return pa;
}

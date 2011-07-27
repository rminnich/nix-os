#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "apic.h"
#include "sipi.h"

#define SIPIHANDLER	(KZERO+0x3000)

void
sipi(void)
{
	Apic *apic;
	Mach *mach;
	int apicno, i;
	u32int *sipiptr;
	u8int *alloc, *p;
	extern void squidboy(int);

	/*
	 * Move the startup code into place,
	 * must be aligned properly.
	 */
	sipiptr = UINT2PTR(SIPIHANDLER);
	if(PADDR(sipiptr) & (4*KiB - 1))
		return;
	memmove(sipiptr, sipihandler, sizeof(sipihandler));

	/*
	 * Notes:
	 * The Universal Startup Algorithm described in the MP Spec. 1.4.
	 * The data needed per-processor is the sum of the stack, page
	 * table pages, vsvm page and the Mach page. The layout is similar
	 * to that described in data.h for the bootstrap processor, but
	 * with any unused space elided.
	 */
	for(apicno = 0; apicno < Napic; apicno++){
		apic = &xapic[apicno];
		if(!apic->useable || apic->addr || apic->machno == 0)
			continue;

		/*
		 * NOTE: for now, share the page tables with the
		 * bootstrap processor, until the lsipi code is worked out,
		 * so only the Mach and stack portions are used below.
		 */
		alloc = mallocalign(MACHSTKSZ+4*PTPGSZ+PGSZ+MACHSZ, PGSZ, 0, 0);
		if(alloc == nil)
			continue;
		memset(alloc, 0, MACHSTKSZ+4*PTPGSZ+PGSZ+MACHSZ);
		p = alloc+MACHSTKSZ;

		sipiptr[-1] = PADDR(p);			/* need mmuphysaddr */

		p += 4*PTPGSZ+PGSZ;

		/*
		 * Committed. If the AP startup fails, can't safely
		 * release the resources, who knows what mischief
		 * the AP is up to. Perhaps should try to put it
		 * back into the INIT state?
		 */
		mach = (Mach*)p;
		mach->machno = apic->machno;		/* NOT one-to-one... */
		mach->splpc = PTR2UINT(squidboy);
		mach->apicno = apicno;
		mach->stack = PTR2UINT(alloc);
		mach->vsvm = alloc+MACHSTKSZ+4*PTPGSZ;

		p = KADDR(0x467);
		*p++ = PADDR(sipiptr);
		*p++ = PADDR(sipiptr)>>8;
		*p++ = 0;
		*p = 0;

		nvramwrite(0x0f, 0x0a);
		apicsipi(apicno, PADDR(sipiptr));

		for(i = 0; i < 1000; i++){
			if(mach->splpc == 0)
				break;
			millidelay(5);
		}
		nvramwrite(0x0f, 0x00);

		DBG("mach %#p (%#p) apicid %d machno %2d %dMHz\n",
			mach, sys->machptr[mach->machno],
			apicno, mach->machno, mach->cpumhz);
	}
}

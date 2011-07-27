/*
 * Memory and machine-specific definitions.  Used in C and assembler.
 */
#define KiB		1024u			/* Kibi 0x0000000000000400 */
#define MiB		1048576u		/* Mebi 0x0000000000100000 */
#define GiB		1073741824u		/* Gibi 000000000040000000 */
#define TiB		1099511627776ull	/* Tebi 0x0000010000000000 */
#define PiB		1125899906842624ull	/* Pebi 0x0004000000000000 */
#define EiB		1152921504606846976ull	/* Exbi 0x1000000000000000 */

#define HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))
#define ROUNDDN(x, y)	(((x)/(y))*(y))
#define MIN(a, b)	((a) < (b)? (a): (b))
#define MAX(a, b)	((a) > (b)? (a): (b))

/*
 * Sizes
 */
#define BI2BY		8			/* bits per byte */
#define BY2V		8			/* bytes per double word */
#define BY2SE		8			/* bytes per stack element */
#define BY2PG		4096			/* bytes per page */
#define BLOCKALIGN	8

#define PGSZ		(4*KiB)			/* page size */
#define PGSHFT		12			/* log(PGSZ) */
#define PTPGSZ		(4*KiB)			/* page table page size */
#define PTPGSHFT	9			/*  */

#define MACHSZ		PGSZ			/* Mach+stack size */
#define MACHMAX		32			/* max. number of cpus */
#define MACHSTKSZ	(6*PGSZ)		/* Mach stack size */

#define KSTACK		(16*1024)		/* Size of Proc kernel stack */
#define STACKALIGN(sp)	((sp) & ~(BY2SE-1))	/* bug: assure with alloc */

/*
 * 2M pages
 */
#define	BIGPGSZ		(2ULL*MiB)
#define	BIGPGSHFT	21U
#define	BIGPGROUND(x)	ROUNDUP((x), BIGPGSZ)
#define	PGSPERBIG	(BIGPGSZ/PGSZ)
#define	BIGPPN(x)	((x)&~(BIGPGSZ-1))


/*
 * Time
 */
#define HZ		(100)			/* clock frequency */
#define MS2HZ		(1000/HZ)		/* millisec per clock tick */
#define TK2SEC(t)	((t)/HZ)		/* ticks to seconds */

/*
 *  Address spaces
 *
 *  User is at ??
 *  Kernel is at ??
 */
#define UTZERO		(0+2*MiB)		/* first address in user text */
#define UTROUND(t)	ROUNDUP((t), BIGPGSZ)
#define USTKTOP		(0x00007ffffffff000ull & ~(BIGPGSZ-1))
#define USTKSIZE	(16*1024*1024)		/* size of user stack */
#define TSTKTOP		(USTKTOP-USTKSIZE)	/* end of new stack in sysexec */
#define HEAPTOP		(TSTKTOP-USTKSIZE)
#define	NIXCALL		(HEAPTOP-BIGPGSZ)	/* nix syscall queues */
/* NOTE: MAKE SURE THESE STAY 1 GiB Aligned */
#define KZERO		(0xffffffff80000000ull)
#define KSEG0		(0xffffffff80000000ull)
#define PMAPADDR	(0xffffffffffe00000ull)	/* unused as of yet */
#define KTZERO		(KZERO+1*MiB+64*KiB)
/* 
 * Amount of memory to map at KZERO. 
 * Matched to an eventual 1 (or more) GiB TLB entry
 */
#define MAPATKZERO (1ULL*GiB)

/*
 *  virtual MMU
 */
#define PTEMAPMEM	(512*1024*1024)
#define PTEPERTAB	(PTEMAPMEM/BIGPGSZ)
#define SEGMAPSIZE	1984
#define SSEGMAPSIZE	16
#define PPN(x)		((x)&~(BY2PG-1))

/*
 * This is the interface between fixfault and mmuput.
 * Should be in port.
 */
#define PTEVALID	(1<<0)
#define PTEWRITE	(1<<1)
#define PTERONLY	(0<<1)
#define PTEUSER		(1<<2)
#define PTEUNCACHED	(1<<4)

#define getpgcolor(a)	0

/*
 * Oh, give it a fucking rest, will you.
 */
#define MAXMACH		MACHMAX

#define PGSIZE		PGSZ
#define PGSHIFT		PGSHFT			/* log(PGSIZE) */

#define PGROUND(s)	ROUNDUP(s, BY2PG)


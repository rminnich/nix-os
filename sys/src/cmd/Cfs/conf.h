

#define TESTING

enum
{
	KiB = 1024UL,
	MiB = KiB * 1024UL,
	GiB = MiB * 1024UL,

#ifdef TESTING
	Incr = 2,
	Fmemsz  = 1*MiB,		/* max size of in-memory file data */
	Fsysmem = 1*MiB,		/* size of fsys data in memory */
	Dminfree = 100000,	/* min nb. of free blocks in disk */

	/* disk parameters; don't change */
	Dblksz = 1*KiB,		/* disk block size */
	Dblkhdrsz = 2*BIT64SZ,
	Ndptr = 2,		/* # of direct data pointers */
	Niptr = 2,		/* # of indirect data pointers */

#else
	Incr = 16,
	Fmemsz  = 64 * MiB,	/* max size of in-memory file data */
	Fsysmem = 2*GiB,		/* size of fsys data in memory */
	Dminfree = 1000,		/* min nb. of free blocks in disk */

	/* disk parameters; don't change */
	Dblksz = 16*KiB,		/* disk block size */
	Dblkhdrsz = 2*BIT64SZ,
	Ndptr = 8,		/* # of direct data pointers */
	Niptr = 4,		/* # of indirect data pointers */

#endif

	Dminattrsz = Dblksz/2,	/* min size for attributes */

	/*
	 * The format of the disk is:
	 * blk 0: unused
	 * blk 1: super
	 * Nblkgrpsz blocks (1st is ref, Nblkgrpsz-1 are data)
	 * ...
	 * Nblkgrpsz blocks (1st is ref, Nblkgrpsz-1 are data)
	 *
	 */
	Nblkgrpsz = (Dblksz - Dblkhdrsz) / BIT64SZ,
	Dblk0addr = 2*Dblksz,

	/*
	 * Caution: Errstack also limits the max tree depth,
	 * because of recursive routines (in the worst case).
	 */
	Stack = 32*KiB,		/* stack size for threads */
	Errstack = 512,		/* max # of nested error labels */
	Fhashsz = 7919,		/* size of file hash (plan9 has 35454 files). */

};

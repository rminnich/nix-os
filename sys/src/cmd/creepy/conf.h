

#define TESTING

enum
{
	KiB = 1024UL,
	MiB = KiB * 1024UL,
	GiB = MiB * 1024UL,

#ifdef TESTING
	Incr = 2,
	Fsysmem = 1*MiB,		/* size for in-memory block array */
	Dzerofree = 10,		/* out of disk blocks */
	Dminfree = 100000,	/* low on disk  */
	Dmaxfree = 100000,	/* high on disk */
	Mminfree = 1000000ULL,	/* low on mem */
	Mmaxfree = 1000000ULL,	/* high on mem */

	/* disk parameters; don't change */
	Dblksz = 512UL,		/* disk block size */
	Dblkhdrsz = 2*BIT64SZ,
	Ndptr = 2,		/* # of direct data pointers */
	Niptr = 2,		/* # of indirect data pointers */
#else
	Incr = 16,
	Fsysmem = 2*GiB,		/* size for in-memory block array */
	Dzerofree = 10,		/* out of disk blocks */
	Dminfree = 1000,		/* low on disk blocks */
	Dmaxfree = 1000,	/* high on disk blocks */
	Mminfree = 50,		/* low on mem blocks */
	Mmaxfree = 500,		/* high on mem blocks */

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
	Errstack = 64,		/* max # of nested error labels */
	Fhashsz = 7919,		/* size of file hash (plan9 has 35454 files). */
	Fidhashsz = 97,		/* size of the fid hash size */

};


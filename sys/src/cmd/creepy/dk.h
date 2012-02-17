typedef struct Fmeta Fmeta;
typedef struct Ddatablk Ddatablk;
typedef struct Dptrblk Dptrblk;
typedef struct Drefblk Drefblk;
typedef struct Dattrblk Dattrblk;
typedef struct Dfileblk Dfileblk;
typedef struct Dsuperblk Dsuperblk;
typedef union Diskblk Diskblk;
typedef struct Diskblkhdr Diskblkhdr;
typedef struct Memblk Memblk;
typedef struct Fsys Fsys;
typedef struct Dentry Dentry;
typedef struct Dmeta Dmeta;
typedef struct Blksl Blksl;
typedef struct Mfile Mfile;

/*
 * these are used by several functions that have flags to indicate
 * mem-only, also on disk; and read-access/write-access. (eg. dfmap).
 */
enum{
	Mem=0,
	Disk,

	Rd=0,
	Wr,
	No,
};

#define HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))

/*
 * Conventions:
 *
 * References:
 *	- Ref is used for in-memory RCs. This has nothing to do with on-disk refs.
 * 	- Mem refs include the reference from the hash. That one keeps the file
 *	  loaded in memory while unused.
 *	- The hash ref also accounts for the lru list and list of DBref blocks.
 *	- Disk refs count only references within the tree on disk.
 *	- Children imply new refs to the parents. But not vice-versa.
 *
 * Assumptions:
 *	- /active is *never* found on disk, it's memory-only.
 *	- b->addr is worm.
 *	- blocks are added to the end of the hash chain.
 *	- We try not to hold more than one lock, using the
 *	  reference counters when we need to be sure that
 *	  an unlocked resource does not vanish.
 *	- parents of file blocks in memory are in memory (because of RCs)
 *	- reference blocks are never removed from memory.
 *	- disk refs are frozen while waiting to go to disk during a fs freeze.
 *	  in which case db*ref functions write the block in place and melt it.
 *	- frozen blocks are quiescent.
 *	- the block epoch number for a on-disk block is the time when it
 *	  was written (thus it's archived "/" has a newer epoch).
 *	- mb*() functions do not raise errors.
 *
 * Locking:
 *	- the caller to functions in [mbf]blk.c acquires the locks before
 *	  calling them, and makes sure the file is melted if needed.
 *	  This prevents races and deadlocks.
 *	- blocks are locked by the file responsible for them, when not frozen.
 *	- next fields in blocks are locked by the list they are used for.
 *
 * Lock order:
 *	- fs, super,... : while locked can't acquire fs or blocks.
 *	- parent -> child
 *	  (but a DBfile protects all ptr and data blocks under it).
 *	- block -> ref block
 *
 * All the code assumes outofmemoryexits = 1.
 */

enum
{
	/* block types */
	DBfree = 0,
	DBnew,			/* never found on disk */
	DBref,
	DBattr,
	DBfile,
	DBsuper,
	DBdata,			/* direct block */
	DBptr0 = DBdata+1,	/* simple-indirect block */
				/* double */
				/* triple */
				/*...*/
};

/*
 * ##### On disk structures. #####
 *
 * All on-disk integer values are little endian.
 *
 * blk 0: unused
 * blk 1: super
 * ref blk + Nblkgrpsz-1 blocks
 * ...
 * ref blk + Nblkgrpsz-1 blocks
 *
 * The code assumes these structures are packed.
 * Be careful if they are changed to make things easy for the
 * compiler and keep them naturally aligned.
 */

struct Ddatablk
{
	uchar	data[1];	/* raw memory */
};

struct Dptrblk
{
	u64int	ptr[1];		/* array of block addresses */
};

struct Drefblk
{
	u64int	ref[1];		/* disk RC or next block in free list */
};

struct Dattrblk
{
	u64int	next;		/* next block used for attribute data */
	uchar	attr[1];	/* raw attribute data */
};

/*
 * directory entry. contents of data blocks in directories.
 * Each block stores only an integral number of Dentries, for simplicity.
 */
struct Dentry
{
	u64int	file;		/* file address or 0 when unused */
};

/*
 * The trailing part of the file block is used to store attributes
 * and initial file data.
 * At least Dminattrsz is reserved for attributes, at most
 * all the remaining embedded space.
 * Past the attributes, starts the file data.
 * If more attribute space is needed, an attribute block is allocated.
 * For huge attributes, it is suggested that a file is allocated and
 * the attribute value refers to that file.
 * The pointer in iptr[n] is an n-indirect data pointer.
 *
 * Directories are also files, but their data is simply an array of
 * Dentries.
 */
struct Dfileblk
{
	u64int	asize;		/* attribute size */
	u64int	aptr;		/* attribute block pointer */
	u64int	dptr[Ndptr];	/* direct data pointers */
	u64int	iptr[Niptr];	/* indirect data pointers */
	uchar	embed[1];	/* embedded attrs and data */
};

enum
{
	FMuid = 0,	/* strings in mandatory attributes */
	FMgid,
	FMmuid,
	FMname,
	FMnstr,
};

struct Dmeta			/* mandatory metadata */
{
	u64int	id;		/* ctime, actually */
	u64int	mode;
	u64int	atime;
	u64int	mtime;
	u64int	length;
	u16int	ssz[FMnstr];
	/* uid\0gid\0muid\0name\0 */
};

#define	MAGIC	0x6699BCB06699BCB0ULL
/*
 * Superblock.
 * The stored tree is:
 *		archive/		root of the archived tree
 *			<epoch>
 *			...
 * (/ and /active are only memory and never on disk, parts
 * under /active that are on disk are shared with entries in /archive)
 */
struct Dsuperblk
{
	u64int	magic;		/* MAGIC */
	u64int	free;		/* first free block on list  */
	u64int	eaddr;		/* end of the assigned disk portion */
	u64int	root;		/* address of /archive in disk */
	u64int	nfree;		/* # of blocks in free list */
	u64int	dblksz;		/* only for checking */
	u64int	nblkgrpsz;	/* only for checking */
	u64int	dminattrsz;	/* only for checking */
	u64int	ndptr;		/* only for checking */
	u64int	niptr;		/* only for checking */
	u64int	dblkdatasz;	/* only for checking */
	u64int	embedsz;	/* only for checking */
	u64int	dptrperblk;	/* only for checking */
	uchar	vac0[24];	/* score for last venti archive + 4pad */
	uchar	vac1[24];	/* score for previous venti archive + 4pad */
};

enum
{
	Noaddr = ~0UL		/* null address, for / */
};

#define	TAG(addr,type)		((addr)<<8|((type)&0x7F))
#define	TAGTYPE(t)		((t)&0x7F)
#define	TAGADDROK(t,addr)	(((t)&~0xFF) == ((addr)<<8))

/*
 * disk blocks
 */

/*
 * header for all disk blocks.
 * Those using on-disk references keep them at a DBref block
 */
struct Diskblkhdr
{
	u64int	tag;		/* block tag */
	u64int	epoch;		/* block epoch */
};

union Diskblk
{
	struct{
		Diskblkhdr;
		union{
			Ddatablk;	/* data block */
			Dptrblk;	/* pointer block */
			Drefblk;	/* reference counters block */
			Dattrblk;	/* attribute block */
			Dfileblk;	/* file block */
			Dsuperblk;
		};
	};
	uchar	ddata[Dblksz];
};

/*
 * These are derived.
 */
enum
{
	Dblkdatasz = sizeof(Diskblk) - sizeof(Diskblkhdr),
	Embedsz	= Dblkdatasz - sizeof(Dfileblk),
	Dptrperblk = Dblkdatasz / sizeof(u64int),
	Drefperblk = Dblkdatasz / sizeof(u64int),
};


/*
 * File attributes are name/value pairs.
 * By now, only mandatory attributes are implemented, and
 * have names implied by their position in the Dmeta structure.
 */

/*
 * ##### On memory structures. #####
 */

/*
 * File metadata
 */
struct Fmeta
{
	Dmeta;
	char	*uid;
	char	*gid;
	char	*muid;
	char	*name;
};

/*
 * On memory file information.
 */
struct Mfile
{
	RWLock;
	Fmeta;
	union{
		Memblk	*parent;	/* most recent parent */
		Mfile	*next;		/* in free Mfile list */
	};

	Memblk*	melted;		/* next version for this one, if frozen */
	ulong	lastbno;	/* help for RA */
	int	open;		/* for DMEXCL */
};

/*
 * memory block
 */
struct Memblk
{
	Ref;
	u64int	addr;			/* block address */
	Memblk	*next;			/* in hash or free list */

	union{
		Memblk	*rnext;		/* in list of DBref blocks */
		Mfile	*mf;		/* DBfile on memory info. */
	};

	int	dirty;			/* must be written */
	int	frozen;			/* is frozen */
	int	written;			/* no need to scan this for dirties */

	Memblk	*lnext;			/* list from fs->mru -> fs->lru */
	Memblk	*lprev;

	QLock	newlk;			/* only to wait on DBnew blocks */
	Diskblk	d;
};

/*
 * Slice into a block, used to read/write file blocks.
 */
struct Blksl
{
	Memblk *b;
	void	*data;
	long	len;
};

struct Fsys
{
	QLock;

	struct{
		QLock;
		Memblk	*b;
	} fhash[Fhashsz];	/* hash of blocks by address */

	Memblk	*blk;		/* static global array of memory blocks */
	usize	nblk;		/* # of entries used */
	usize	nablk;		/* # of entries allocated */
	usize	nused;		/* blocks in use */
	usize	nfree;		/* free blocks */

	Memblk	*free;		/* free list of unused blocks in blk */

	QLock	llk;
	Memblk	*lru;
	Memblk	*mru;

	QLock	mlk;
	Mfile	*mfree;		/* unused list */

	QLock	rlk;
	Memblk	*refs;		/* list of DBref blocks (also hashed) */

	Memblk	*super;		/* locked by blklk */
	Memblk	*root;		/* only in memory */
	Memblk	*active;		/* /active */
	Memblk	*archive;	/* /archive */
	Memblk	*cons;		/* /cons */
	Channel	*consc;		/* of char*; output for /cons */

	Memblk	*fzsuper;	/* frozen super */

	QLock	fzlk;		/* free or reclaim in progress. */

	char	*dev;		/* name for disk */
	int	fd;		/* of disk */
	usize	limit;		/* address for end of disk */

	int	config;		/* config mode enabled */
};

#pragma	varargck	type	"H"	Memblk*

typedef int(*Blkf)(Memblk*);


extern Fsys*fs;
extern uvlong maxfsz;


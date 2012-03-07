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
typedef struct Cmd Cmd;
typedef struct Path Path;
typedef struct Alloc Alloc;
typedef struct Next Next;
typedef struct Lstat Lstat;
typedef struct List List;
typedef struct Link Link;

/*
 * Conventions:
 *
 * References:
 *	- Ref is used for in-memory RCs. This has nothing to do with on-disk refs.
 * 	- Mem refs include the reference from the hash. That one keeps the file
 *	  loaded in memory while unused.
 *	- The hash ref also accounts for refs from the lru/ref/dirty lists.
 *	- Disk refs count only references within the tree on disk.
 *	- There are two copies of disk references, even, and odd.
 *	  Only one of them is active. Every time the system is written,
 *	  the inactive copy becomes active and vice-versa. Upon errors,
 *	  the active copy on disk is always coherent because the super is
 *	  written last.
 *	- Children do not add refs to parents; parents do not add ref to children.
 *	- 9p, fscmd, ix, and other top-level shells for the fs are expected to
 *	  keep Paths for files in use, so that each file in the path
 *	  is referenced once by the path
 *	- example, on debug fsdump()s:
 *		r=2 -> 1 (from hash) + 1 (while dumping the file info).
 *		(block is cached, in the hash, but unused otherwise).
 *		r=3 in /active: 1 (hash) + 1(fs->active) + 1(dump)
 *		r is greater:
 *			- some fid is referencing the block
 *			- it's a melt and the frozen f->mf->melted is a ref.
 *			- some rpc is using it (reading/writing/...)
 *
 * Assumptions:
 *	- /active is *never* found on disk, it's memory-only.
 *	- b->addr is worm.
 *	- parents of files loaded in memory are also in memory.
 *	  (but this does not hold for pointer and data blocks).
 *	- We try not to hold more than one lock, using the
 *	  reference counters when we need to be sure that
 *	  an unlocked resource does not vanish.
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

/*
 * these are used by several functions that have flags to indicate
 * mem-only, also on disk; and read-access/write-access. (eg. dfmap).
 */
enum{
	Mem=0,
	Disk,

	Rd=0,
	Wr,

	Tqlock = 0,
	Trwlock,
	Tlock,
};


struct Lstat
{
	int	type;
	uintptr	pc;
	int	ntimes;
	int	ncant;
	vlong	wtime;
};



#define HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))

/*
 * ##### On disk structures. #####
 *
 * All on-disk integer values are little endian.
 *
 * blk 0: unused
 * blk 1: super
 * even ref blk + odd ref blk + Nblkgrpsz-2 blocks
 * ...
 * even ref blk + odd ref blk + Nblkgrpsz-2 blocks
 *
 * The code assumes these structures are packed.
 * Be careful if they are changed to make things easy for the
 * compiler and keep them naturally aligned.
 */

enum
{
	/* block types */
	DBfree = 0,
	DBref,
	DBattr,
	DBfile,
	DBsuper,
	DBdata,			/* direct block */
	DBptr0 = DBdata+1,	/* simple-indirect block */
				/* double */
				/* triple */
				/*...*/
	DBctl = ~0,		/* DBfile, never on disk. arg for dballoc */

	Dblkhdrsz = 2*BIT64SZ,
	Nblkgrpsz = (Dblksz - Dblkhdrsz) / BIT64SZ,
	Dblk0addr = 2*Dblksz,

};

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
	u64int	oddrefs;	/* use odd ref blocks? or even ref blocks? */
	u64int	ndfree;		/* # of blocks in free list */
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
	/* addresses for ctl files and / have this bit set, and are never
	 * found on disk.
	 */
	Fakeaddr = 0x8000000000000000ULL,
	Noaddr = ~0ULL,
};

#define	TAG(type,addr)		((addr)<<8|((type)&0x7F))
#define	TAGTYPE(t)		((t)&0x7F)
#define	TAGADDROK(t,addr)	(((t)&~0xFF) == ((addr)<<8))

/*
 * disk blocks
 */

/*
 * header for all disk blocks.
 */
struct Diskblkhdr
{
	u64int	tag;		/* block tag */
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
	Mfile*	next;		/* in free list */
	RWLock;
	Fmeta;

	Memblk*	melted;		/* next version for this one, if frozen */
	ulong	lastbno;	/* last accessed block nb within this file */
	ulong	sequential;	/* access has been sequential */

	int	open;		/* for DMEXCL */
	uvlong	raoffset;	/* we did read ahead up to this offset */
	int	wadone;		/* we did walk ahead here */
};

struct List
{
	QLock;
	Memblk	*hd;
	Memblk	*tl;
	long	n;
};

struct Link
{
	Memblk	*lprev;
	Memblk	*lnext;
};

/*
 * memory block
 */
struct Memblk
{
	Ref;
	u64int	addr;			/* block address */
	Memblk	*next;			/* in hash or free list */

	Link;				/* lru / dirty / ref lists */

	Mfile	*mf;			/* DBfile on-memory info. */

	int	type;
	int	dirty;			/* must be written */
	int	frozen;			/* is frozen */
	int	loading;			/* block is being read */
	int	changed;		/* for freerefs/writerefs */
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
	usize	nmused;		/* blocks in use */
	usize	nmfree;		/* free blocks */
	Memblk	*free;		/* free list of unused blocks in blk */

	List	lru;		/* hd: mru; tl: lru */
	List	mdirty;		/* dirty blocks, not on lru */
	List	refs;		/* DBref blocks, not in lru nor dirty lists */

	QLock	mlk;
	Mfile	*mfree;		/* unused list */


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
	u64int	limit;		/* address for end of disk */
	usize	ndblk;		/* # of disk blocks in dev */

	int	config;		/* config mode enabled */

	int	nindirs[Niptr];	/* stats */
	int	nmelts;

	uchar	*chk;		/* for fscheck() */
};

/*
 * Misc tools.
 */

struct Cmd
{
	char *name;
	void (*f)(int, char**);
	int nargs;
	char *usage;
};

struct Next
{
	Next *next;
};

struct Alloc
{
	QLock;
	Next *free;
	ulong nfree;
	ulong nalloc;
	usize elsz;
	int zeroing;
};

/*
 * Used to keep references to parents crossed to
 * reach files, to be able to build a melted version of the
 * children. Also to know the parent of a file for things like
 * removals.
 */
struct Path
{
	Path* next;	/* in free list */
	Ref;
	Memblk** f;
	int nf;
	int naf;
};


#pragma	varargck	type	"H"	Memblk*

/* used in debug prints to print just part of huge values */
#define EP(e)	((e)&0xFFFFFFFFUL)

typedef int(*Blkf)(Memblk*);


extern Fsys*fs;
extern uvlong maxfsz;
extern char*defaultusers;
extern Alloc mfalloc, pathalloc;

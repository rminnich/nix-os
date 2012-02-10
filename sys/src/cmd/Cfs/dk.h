typedef struct Fattr Fattr;
typedef struct Fmeta Fmeta;
typedef struct Child Child;
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
 * Conventions on the data structures:
 *
 * References:
 * 	- Mem refs include the reference from the hash (to keep the block)
 * 	  plus from external structures/users. Thus:
 *		- those with ref=1 are just kept cached in the hash
 *		- those with ref=2 are referenced also by the tree
 *		(or the superblock; does not apply to DBref blocks)
 *		- those with ref >2 are in use
 *	- Disk refs count only references within the tree on disk.
 *	(perhaps loaded in memory waiting for a further sync)
 *	- Children do not imply new refs to the parents.
 * Locking:
 *
 *   Assumptions:
 *	- /active is *never* found on disk, it's memory-only.
 *	- b->addr is worm.
 *	- b->next is locked by the hash bucked lock
 *	- blocks added to the end of the hash chain.
 *	- blocks are locked by the file responsible for them, when not frozen.
 *	- super, disk refs, block allocation, free list, ... protected by fs lock
 *	- We try not to hold more than one lock, using the
 *	  reference counters when we need to be sure that
 *	  an unlocked resource does not vanish.
 *	- parents of blocks in memory are in memory
 *	- reference blocks are never removed from memory.
 *	- disk refs frozen while waiting to be to disk during a fs freeze.
 *	  in which case db*ref functions write the block in place and melt it.
 *	- the block epoch number for a on-disk block is the time when it
 *	  was written (thus it's archived "/" has a newer epoch).
 *   Order:
 *	fs & super: while locked can't acquire fs or blocks.
 *	blocks: parent -> child; block -> ref block
 */

enum
{
	/* block types */
	DBfree = 0,
	DBdata,
	DBptr,
	DBref,
	DBattr,
	DBfile,
	DBsuper,
};

/*
 * ##### On disk structures. #####
 *
 * All on-disk integer values are little endian.
 *
 * blk 0: unused
 * blk 1: super
 * ref blk + Nblkgrpsz blocks
 * ...
 * ref blk + Nblkgrpsz blocks
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
	u64int	ref[1];		/* RC or next in free list */
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
	u64int	file;		/* file address or 0 when archived */
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
	FMuid = 0,
	FMgid,
	FMmuid,
	FMname,
	FMnstr,
};

struct Dmeta
{
	u64int	id;		/* ctime, actually */
	u64int	mode;
	u64int	mtime;
	u64int	length;
	u16int	ssz[FMnstr];
	/* uid\0gid\0muid\0name\0 */
};

/*
 * Superblock.
 * The stored tree is:
 *	/
 *		active/		root of the current or active tree
 *		archive/		root of the archived tree
 *			<epoch>
 *			...
 *		<epoch1>/	old root of active as of epoch#1
 *		...
 *		<epochn>/	old root of active as of epoch#n
 */
struct Dsuperblk
{
	u64int	free;		/* first free block on list  */
	u64int	eaddr;		/* end of the assigned disk portion */
	u64int	root[16];	/* address of /archive in disk */
	u64int	nfree;		/* # of blocks in free list */
	u64int	dblksz;		/* only for checking */
	u64int	nblkgrpsz;	/* only for checking */
	u64int	dminattrsz;	/* only for checking */
	uchar	vac0[24];	/* score for last venti archive + 4pad */
	uchar	vac1[24];	/* score for previous venti archive + 4pad */
};

enum
{
	Noaddr = ~0UL
};

#define	TAG(addr,type)		((addr)<<8|((type)&0x7F))
#define	TAGTYPE(t)		((t)&0x7F)
#define	TAGADDROK(t,addr)	(((t)&~0xFF) == ((addr)<<8))

/*
 * disk block
 */
struct Diskblkhdr
{
	u64int	tag;		/* block tag */
	u64int	epoch;		/* block epoch */
	/*	ref is kept on Dref blocks */
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

enum
{
	Dblkdatasz = sizeof(Diskblk) - sizeof(Diskblkhdr),
	Embedsz = Dblkdatasz - sizeof(Dfileblk),
	Dentryperblk = Dblkdatasz / sizeof(Dentry),
	Dptrperblk = Dblkdatasz / sizeof(u64int),
	Drefperblk = Dblkdatasz / sizeof(u64int),
};

/*
 * File attributes are name/value pairs.
 * A few ones have the name implied by their position.
 * All integer values are always kept LE.
 * addr	u64int
 * mode	u32int
 * mtime u64int
 * length u64int
 * uid  [n] + UTF8 + '\0'
 * gid  [n] + UTF8 + '\0'
 * muid  [n] + UTF8 + '\0'
 * name  [n] + UTF8 + '\0'
 */

/*
 * ##### On memory structures. #####
 */

/*
 * unpacked file attributes point into the Bfile embedded data.
 */
struct Fattr
{
	Fattr *next;
	char *name;
	uchar *val;
	long nval;
};

struct Child
{
	Memblk	*f;		/* actual child */
	Memblk	*b;		/* data block containing it's dentry */
	Dentry	*d;		/* loaded dentry */
};

struct Fmeta
{
	Dmeta;
	char	*uid;
	char	*gid;
	char	*muid;
	char	*name;
};

struct Mfile
{
	RWLock;
	Fmeta;
	union{
		Memblk	*parent;	/* most recent parent */
		Mfile	*next;		/* in free Mfile list */
	};

	Memblk*	lastb;		/* last returned data block */
	ulong	lastbno;	/*   for the last asked block # */

	Child	*child;		/* direct references to loaded children */
	int	nchild;		/* number of used children in child */
	int	nachild;		/* number of allocated chilren in child */
};

/*
 * memory block
 */
struct Memblk
{
	Ref;
	u64int	addr;		/* block address */
	Memblk	*next;		/* in hash or free list */

	/* for DBref only */
	union{
		Memblk	*rnext;		/* in list of ref blocks */
		Mfile	*mf;		/* per file mem info */
	};

	int	dirty;		/* must be written */
	int	frozen;		/* is frozen */
	int	written;		/* no need to scan this for dirties */
	Diskblk	d;
};

/*
 * Slice into a block
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
		RWLock;
		Memblk	*b;
	}	fhash[Fhashsz];

	Memblk	*blk;
	usize	nblk;
	usize	nablk;
	usize	nused;
	usize	nfree;
	Memblk	*free;
	Mfile	*mfree;

	Memblk	*refs;

	usize	limit;
	Memblk	*super;		/* locked by blklk */
	Memblk	*root;		/* only in memory */
	Memblk	*active;		/* /active */
	Memblk	*archive;	/* /archive */

	Memblk	*fzsuper;	/* frozen super */
	Memblk	*fzactive;	/* frozen active */
	Memblk	*fzarchive;	/* frozen archive */

	char	*dev;
	int	fd;
};

#pragma	varargck	type	"H"	Memblk*


extern char*tname[];

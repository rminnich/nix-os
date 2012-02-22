typedef struct Fid Fid;
typedef struct Rpc Rpc;
typedef struct Shortrpc Shortrpc;
typedef struct Largerpc Largerpc;
typedef struct Cli Cli;

enum
{
	Maxmdata = 8*KiB,
	Minmdata = 128,

	QTCACHE = 0x02,	/* experiment */
};

/*
 * One reference kept because of existence and another per req using it.
 */
struct Fid
{
	Fid	*next;		/* in hash or free list */
	Ref;
	QLock;
	void*	clino;		/* no is local to a client */
	int	no;
	Path*	p;
	int	omode;		/* -1 if closed */
	int	rclose;
	int	archived;
	int	cflags;		/* OCERR|OCEND */
	int	consopen;	/* for flush. has /cons open? */
	char	*uid;

	uvlong	loff;		/* last offset, for dir reads */
	long	lidx;		/* next dir entry index to read */
};

struct Rpc
{
	Rpc	*next;		/* in client or free list */
	Cli	*cli;
	Fid	*fid;
	union{
		Fcall	t;
		IXcall	xt;
	};
	union{
		Fcall	r;
		IXcall	xr;
	};

	/* these are for ix */
	Rpc*	rpc0;		/* where to get fid, c, closed, flushed */
	u16int	chan;		/* channel # (ix) */
	Channel* c;		/* to worker (ix) */
	int	closed;		/* got last rpc in chan */

	int	flushed;
	uchar	data[1];
};

/*
 * 9p, and ix if carrying data.
 */
struct Largerpc
{
	Rpc;
	uchar	buf[IOHDRSZ+Maxmdata];
};

/*
 * ix requests that do not carry much data.
 */
struct Shortrpc
{
	Rpc;
	uchar	buf[IOHDRSZ+Minmdata];
};

struct Cli
{
	Cli *next;		/* in list of clients or free list*/
	Ref;
	int fd;
	int cfd;
	char *addr;
	ulong msize;

	QLock wlk;	/* lock for writing replies to the client */
	uchar wdata[IOHDRSZ+Maxmdata];

	QLock rpclk;
	ulong nrpcs;	/* should we limit the max # per client? */
	Rpc *rpcs;
};

#pragma	varargck	type	"X"	Fid*
#pragma	varargck	type	"R"	Rpc*

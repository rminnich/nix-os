
typedef struct Mpool Mpool;
typedef struct Mbuf Mbuf;

struct Mbuf
{
	Msg m;
	Mpool *mp;
	Mbuf* next;
	uchar buf[];
};

struct Mpool
{
	Lock;	/* stats */
	int	nmsg;
	ulong	msize;
	int	nfree;
	ulong	nallocs;
	ulong	nfrees;
	int	nminfree;

	void	(*freepool)(Mpool*);
	Channel *bc;
};

/*	|c/f2p mpool.c	*/
extern void	poolstats(Mpool *mp);
extern void 	freepool(Mpool *mp);
extern void 	freepoolmsg(Msg *m);
extern Msg* 	newmsg(Mpool *mp);
extern Mpool* 	newpool(ulong msize, int nmsg);

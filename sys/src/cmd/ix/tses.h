typedef struct Con Con;
typedef struct Ssrv Ssrv;

struct Con
{
	Channel*rc;	/* read channel (of Msg*) */
	Channel*wc;	/* write channel (of Msg*) */
	Channel*ec;	/* error channel (of char*) */


	/* implementation */
	Mpool*pool;	/* message pool */
	Mpool*spool;	/* small message pool */
	char* err;
	Ref;		/* used by reader, writer, client */
	Con *next;	/* in list of sessions */
	char* addr;	/* debug */
	int cfd;		/* control fd */
	int dfd;		/* data fd */
	int rtid;	/* reader thread id */
};

struct Ssrv
{
	Channel*newc;	/* of Con* */

	/* implementation*/
	char*	addr;
	char	adir[40];
	int	afd;
	int	lfd;
	Channel*endc;	/* of Con* */
	Channel*listenc;	/* of ses* */
};

/*	|c/f2p tses.c	*/
extern int	closeses(Con *s);
extern Con*	dialsrv(char *addr);
extern Ssrv*	newsrv(char *addr);
extern void	startses(Con *s, Mpool *p, Mpool *sp);

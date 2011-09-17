typedef struct Ses Ses;
typedef struct Ssrv Ssrv;

struct Ses
{
	Channel*rc;	/* read channel (of Msg*) */
	Channel*wc;	/* write channel (of Msg*) */
	Channel*ec;	/* error channel (of char*) */


	/* implementation */
	Mpool*pool;	/* message pool */
	Mpool*spool;	/* small message pool */
	char* err;
	Ref;		/* used by reader, writer, client */
	Ses *next;	/* in list of sessions */
	char* addr;	/* debug */
	int cfd;	/* control fd */
	int dfd;	/* data fd */
	int rtid;	/* reader thread id */
};

struct Ssrv
{
	Channel*newc;	/* of Ses* */

	/* implementation*/
	char*	addr;
	char	adir[40];
	int	afd;
	int	lfd;
	Channel*endc;	/* of Ses* */
	Channel*listenc;	/* of ses* */
};

/*	|c/f2p tses.c	*/
extern int	closeses(Ses *s);
extern Ses*	dialsrv(char *addr);
extern Ssrv*	newsrv(char *addr);
extern void	startses(Ses *s, Mpool *p, Mpool *sp);

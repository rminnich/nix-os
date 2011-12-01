typedef struct Msg Msg;
typedef struct Io Io;

enum
{
	Nio = 1,
};

struct Io
{
	uchar *bp;	/* buffer pointer */
	uchar *rp;	/* read pointer */
	uchar *wp;	/* write pointer */
	uchar *ep;	/* end pointer */
};

struct Msg
{
	uchar	hbuf[Hbufsz];
	uchar*	hdr;	/* headers go from hdr to &hbuf[Hdrsz] */
	Io	io[Nio];
	void*	aux;
	void	(*free)(Msg*);
};

#define IOLEN(io)	((io)->wp - (io)->rp)
#define IOCAP(io)	((io)->ep - (io)->wp)

/*	|c/f2p msg.c	*/
extern void	dumpmsg(Msg *m);
extern Channel*	echancreate(int elsz, int n);
extern void	freemsg(Msg *m);
extern void	ioreset(Io *io);
extern long	msglen(Msg *m);
extern void*	msgpophdr(Msg *m, long sz);
extern void*	msgpushhdr(Msg *m, long sz);
extern int	msgput(Msg *m, void *p, long sz);
extern int	readmsg(int fd, Msg *m);
extern long	writemsg(int fd, Msg *m);

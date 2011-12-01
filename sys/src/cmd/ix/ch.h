typedef struct Ch Ch;
typedef struct Cmux Cmux;
typedef struct Chmsg Chmsg;


enum
{
	/* Ch flags */
	CFnew = 0x8000,
	CFend = 0x4000,
	CFidmask = 0x3FFF,

	Chhdrsz = BIT16SZ,

};

struct Chmsg
{
	Msg *m;
	int last;
};

struct Ch
{
	ushort id;
	Channel*rc;	/* read channel (of Chmsg) */
	Channel*wc;	/* write channel (of Chmsg) */

	/* implementation */
	Cmux *cm;
	int notnew;	/* already created because of peer */
	int rclosed;
	int wclosed;
	int flushing;

	Chmsg *chms;
	int nchms;
	int nachms;

	int dead;	/* gone channel, waiting to be reused */
	Channel *dc;
};

struct Cmux
{
	Channel*newc;	/* of Ch*; to report new Ch's created */

	/* implementation*/
	Channel*src;	/* multiplexed session read channel */
	Channel*swc;	/* multiplexed session write channel */
	Channel*sec;	/* multiplexed session error channel */
	Channel*mkc;	/* of ulong; request a new ch */
	Channel*mkrc;	/* of Ch*; reply ......with new ch */
	Channel*endc;	/* of Ch*; to release ch */
	Ch** chs;	/* array of channels in this session */
	int nuse;	/* # of channels in use */
	int nchs;	/* number of channels used */
	int nachs;	/* number of channels allocated */
	Alt *alts;	/* cmux chans + one read channel per Ch */
};

/*	|c/f2p ch.c	*/
extern void	abortch(Ch *ch);
extern Msg*	chrecv(Ch *ch, int *last);
extern int	chsend(Ch *ch, Msg *m, int last);
extern void	closech(Ch *ch);
extern void	closemux(Cmux *cm);
extern void	drainch(Ch *ch);
extern Cmux*	muxses(Channel *src, Channel *swc, Channel *sec);
extern Ch*	newch(Cmux *cm);

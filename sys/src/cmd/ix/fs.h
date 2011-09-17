enum{
	Tfid = Tversion-6,
	Rfid,
	Tclone,			/* unused numbers in fcall.h */
	Rclone,			/* following T&R conventions */
	Tcond,
	Rcond,

	OCEND = 1,		/* clunk on end of rpc */
	OCERR = 2,		/* clunk on error */

	CEQ = 0,		/* Tcond.cond */
	CGE,
	CGT,
	CLE,
	CLT,
	CNE,
	CMAX,
};

/*
   Protocol:
               Tversion msize[4] version[s]
               Rversion msize[4] version[s]
               Tauth afid[4] uname[s] aname[s]
               Rauth aqid[13]
               Rerror ename[s]
               Tattach afid[4] uname[s] aname[s]
               Rattach fid[4] qid[13]
               Tfid fid[4] cflags[1]
               Rfid
               Tclone cflags[1]
               Rclone newfid[4]
               Twalk wname[s]
               Rwalk wqid[13]
               Topen mode[1]
               Ropen qid[13] iounit[4]
               Tcreate name[s] perm[4] mode[1]
               Rcreate qid[13] iounit[4]
               Tread nmsg[4] offset[8] count[4]
               Rread count[4] data[count]	may be repeated
               Twrite offset[8] count[4] data[count]
               Rwrite count[4]
               Tclunk
               Rclunk
               Tremove
               Rremove
               Tstat
               Rstat stat[n]
               Twstat stat[n]
               Rwstat
               Tcond cond[1] stat[n]
               Rcond

	The largest packed messages, not counting strings and data, are
	Rattach, Ropen, and Rcreate, with 18 bytes.
	Plus ch header (2 bytes), plus message size (2 bytes).
	Could create pools for small messages with, say, 64 bytes,
	and use them for messages that we know that fit there.
	Probably only Rstat, Tcond, Twstat, Rread, and Twrite won't fit.
	Perhaps also some Twalk for a long name element.
 */

typedef
struct	Fscall
{
	uchar	type;
	u32int	fid;				/* Tattach, Tfid */
	union {
		struct {
			u32int	msize;		/* Tversion, Rversion */
			char	*version;	/* Tversion, Rversion */
		};
		struct {
			char	*ename;		/* Rerror */
		};
		struct {
			Qid	qid;		/* Rattach, Ropen, Rcreate */
			u32int	iounit;		/* Ropen, Rcreate */
		};
		struct {
			Qid	aqid;		/* Rauth */
		};
		struct {
			u32int	afid;		/* Tauth, Tattach */
			char	*uname;		/* Tauth, Tattach */
			char	*aname;		/* Tauth, Tattach */
		};
		struct {
			u32int	perm;		/* Tcreate */ 
			char	*name;		/* Tcreate */
			uchar	mode;		/* Tcreate, Topen */
		};
		struct {
			char	*wname;		/* Twalk */
		};
		struct {
			Qid	wqid;		/* Rwalk */
		};
		struct {
			u32int	newfid;		/* Rclone */
		};
		struct {
			u32int	nmsg;		/* Tread */
			vlong	offset;		/* Tread, Twrite */
			u32int	count;		/* Tread, Twrite, Rread */
			char	*data;		/* Twrite, Rread */
		};
		struct {
			uchar	cond;		/* Tcond */
			ushort	nstat;		/* Tcond, Twstat, Rstat */
			uchar	*stat;		/* Tcond, Twstat, Rstat */
		};
		struct {
			uchar	cflags;		/* Tfid, Tclone */
		};
	};
} Fscall;

#pragma	varargck	type	"G"	Fscall*

/*
 * |c/f2p fs.c
 */
extern void	fsinit(char *addr, char *srv);
extern void	fssrv(void);

/*
 * |c/f2p pack.c
 */
extern uint	pack(Fscall *f, uchar *ap, uint nap);
extern uint	packedsize(Fscall *f);
extern uint	unpack(uchar *ap, uint nap, Fscall *f);

/*
 * |c/f2p fmt.c
 */
extern int	fscallfmt(Fmt *fmt);




/*	|c/f2p ixreqs.c	*/
extern int	xrattach(Ch *ch, int *fidp);
extern int	xrclone(Ch *ch);
extern int	xrclunk(Ch *ch);
extern int	xrcond(Ch *ch);
extern int	xrcreate(Ch *ch);
extern int	xrfid(Ch *ch);
extern int	xropen(Ch *ch);
extern Msg*	xrread(Ch *ch);
extern int	xrremove(Ch *ch);
extern int	xrstat(Ch *ch, Dir *d, char buf[]);
extern int	xrversion(Ch *ch, ulong *mszp);
extern int	xrwalk(Ch *ch, Qid *qp);
extern long	xrwrite(Ch *ch);
extern int	xrwstat(Ch *ch);
extern int	xtattach(Ch *ch, char *uname, char *aname, int last);
extern int	xtclone(Ch *ch, int cflags, int last);
extern int	xtclunk(Ch *ch, int last);
extern int	xtcond(Ch *ch, int op, Dir *d, int last);
extern int	xtcreate(Ch *ch, char *name, int mode, int perm, int last);
extern int	xtfid(Ch *ch, int fid, int last);
extern int	xtopen(Ch *ch, int mode, int last);
extern int	xtread(Ch *ch, long count, uvlong offset, long msz, int last);
extern int	xtremove(Ch *ch, int last);
extern int	xtstat(Ch *ch, int last);
extern int	xtversion(Ch *ch, int last);
extern int	xtwalk(Ch *ch, char *elem, int last);
extern int	xtwrite(Ch *ch, Msg *m, long count, uvlong offset, int last);
extern int	xtwstat(Ch *ch, Dir *d, int last);

extern Mpool *pool, *spool;

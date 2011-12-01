

/*	|c/f2p ixreqs.c	*/
extern int	ixrattach(Ch *ch, int *fidp);
extern Msg*	ixrattr(Ch *ch);
extern int	ixrclone(Ch *ch, int *fidp);
extern int	ixrclose(Ch *ch);
extern int	ixrclunk(Ch *ch);
extern int	ixrcond(Ch *ch);
extern long	ixrcopy(Ch *ch);
extern int	ixrcreate(Ch *ch);
extern int	ixrendsession(Ch *ch);
extern int	ixrfid(Ch *ch);
extern int	ixrmove(Ch *ch);
extern int	ixropen(Ch *ch);
extern Msg*	ixrread(Ch *ch);
extern int	ixrremove(Ch *ch);
extern int	ixrsession(Ch *ch, int *ssid, int *afid, char **u);
extern int	ixrsid(Ch *ch);
extern int	ixrversion(Ch *ch, ulong *mszp);
extern int	ixrwalk(Ch *ch);
extern int	ixrwattr(Ch *ch);
extern long	ixrwrite(Ch *ch, uvlong *offsetp);
extern int	ixtattach(Ch *ch, char *aname, int last);
extern int	ixtattr(Ch *ch, char *attr, int last);
extern int	ixtclone(Ch *ch, int cflags, int last);
extern int	ixtclose(Ch *ch, int last);
extern int	ixtclunk(Ch *ch, int last);
extern int	ixtcond(Ch *ch, Msg *m, int op, char *attr, long nvalue, int last);
extern int	ixtcopy(Ch *ch, int nmsg, long count, uvlong offset, long msz,int dstfid, uvlong dstoffset, int last);
extern int	ixtcreate(Ch *ch, char *name, int mode, int perm, int last);
extern int	ixtendsession(Ch *ch, int last);
extern int	ixtfid(Ch *ch, int fid, int last);
extern int	ixtmove(Ch *ch, int dirfid, char *newname, int last);
extern int	ixtopen(Ch *ch, int mode, int last);
extern int	ixtread(Ch *ch, int nmsg, long count, uvlong offset, int last);
extern int	ixtremove(Ch *ch, int last);
extern int	ixtsession(Ch *ch, int ssid, char *u, int keep, int last);
extern int	ixtsid(Ch *ch, int ssid, int last);
extern int	ixtversion(Ch *ch, int last);
extern int	ixtwalk(Ch *ch, int nel, char **elem, int last);
extern int	ixtwattr(Ch *ch, char *attr, void *value, long nvalue, int last);
extern int	ixtwrite(Ch *ch, Msg *m, long count, uvlong offset, uvlong endoffset, int last);

extern Mpool *pool, *spool;

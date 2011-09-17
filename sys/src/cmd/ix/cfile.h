
/* |c/f2p cfile.c */
extern void	cclosefile(File *f, int fd);
extern int	ccreatefile(File **fp, char *elem, int mode, int perm);
extern void	cfileinit(Cmux *cm);
extern int	copenfile(File *f, int mode);
extern long	cpreadfile(File *f, int fd, void *a, ulong count, uvlong offset);
extern void	cputfile(File *f);
extern long	cpwritefile(File *f, int fd, void *a, ulong count, uvlong offset);
extern int	cremovefile(File *f);
extern File*	crootfile(void);
extern Dir*	cstatfile(File *f, int refresh);
extern int	cwalkfile(File **fp, char *elem);
extern int	cwstatfile(File *f, Dir *d);

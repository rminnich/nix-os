typedef struct File File;

enum
{
	None = 0,
	Meta = 1,
	Data = 2,
	Gone = 4,
};

struct File
{
	Ref;
	int nopens;
	RWLock;
	char *path;
	Dir *d;
	Dir *sd;	/* dir in server */
	File *parent;
	File *child;
	int nchild;
	File *next;	/* in child list */
	int visited;
	int cremoved;	/* file removed on client */
	int sremoved;	/* file removed on server */
	/* used by ixc */
	int fid;
};

#pragma	varargck	type	"T"	File*
#define ISDIR(f)	((f)->d->qid.type&QTDIR)


/* |c/f2p nsfile.c */
extern void	childmap(File *f, void(*fn)(File*));
extern void	closefile(File *f, int fd);
extern int	createfile(File **fp, char *elem, int mode, int perm);
extern void	dumptree(void);
extern Dir*	dupdir(Dir *d);
extern int	filechanged(File *f);
extern int	filefmt(Fmt *fmt);
extern void	fileinit(char *path, int udb);
extern Qid	fileqid(File *f);
extern int	filesync(void);
extern File*	getchild(File *parent, char *name);
extern File*	newfile(File *parent, char *path, Dir *d);
extern int	openfile(File *f, int mode);
extern int	perm(File *f, char *user, int p);
extern long	preadfile(File *, int fd, void *a, ulong count, uvlong offset);
extern void	putfile(File *f);
extern long	pwritefile(File *, int fd, void *a, ulong count, uvlong offset);
extern int	removefile(File *f);
extern File*	rootfile(void);
extern int	shortdirfmt(Fmt *fmt);
extern Dir*	statfile(File *f, int refresh);
extern char*	tmpfile(char *name);
extern int	walkfile(File **fp, char *elem);
extern int	wstatfile(File *f, Dir *d);

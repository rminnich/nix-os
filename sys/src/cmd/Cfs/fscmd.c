#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "fns.h"

enum 
{
	Nels = 64
};

static char *fsdir;
static int verb;

static char*
fsname(char *p)
{
	if(p[0] == '/')
		return strdup(p);
	if(fsdir)
		return smprint("%s/%s", fsdir, p);
	return strdup(p);
}

static Memblk*
walkto(char *a, char **lastp)
{
	char *els[Nels], *path;
	int nels;
	Memblk *f;

	path = fsname(a);
	nels = gettokens(path, els, Nels, "/");
	if(nels < 1){
		free(path);
		error("invalid path");
	}
	if(catcherror()){
		free(path);
		error("walkpath: %r");
	}
	if(lastp != nil){
		f = walkpath(fs->root, els, nels-1);
		*lastp = a + strlen(a) - strlen(els[nels-1]);
	}else
		f = walkpath(fs->root, els, nels);
	free(path);
	noerror();
	if(verb)
		print("walked to %H", f);
	return f;
}

static void
fscd(int, char *argv[])
{
	free(fsdir);
	fsdir = strdup(argv[1]);
}

static void
fsput(int, char *argv[])
{
	int fd;
	char *fn;
	Memblk *m, *f;
	Dir *d;
	char buf[4096];
	uvlong off;
	long nw, nr;

	fd = open(argv[1], OREAD);
	if(fd < 0){
		fprint(2, "%s: open: %r\n", argv[0]);
		return;
	}
	d = dirfstat(fd);
	if(d == nil){
		fprint(2, "%s: error: %r\n", argv[0]);
		goto done;
	}
	if(catcherror()){
		fprint(2, "%s: error: %r\n", argv[0]);
		goto done;
	}
	m = walkto(argv[2], &fn);
	f = dfcreate(m, fn, d->uid, d->mode&(DMDIR|0777));
	if((d->mode&DMDIR) == 0){
		off = 0;
		for(;;){
			nr = read(fd, buf, sizeof buf);
			if(nr <= 0)
				break;
			nw = dfpwrite(f, buf, nr, off);
			dDprint("wrote %ld of %ld bytes\n", nw, nr);
			off += nr;
		}
	}
	mbput(m);
	if(verb)
		print("created %H", f);
	mbput(f);
	noerror();
done:
	close(fd);
	free(d);
}

static void
fscat(int, char *argv[])
{
	Memblk *f;
	Mfile *m;
	char buf[4096];
	uvlong off;
	long nr;

	if(catcherror()){
		fprint(2, "%s: error: %r\n", argv[0]);
		return;
	}
	f = walkto(argv[2], nil);
	if(catcherror()){
		fprint(2, "%s: error: %r\n", argv[0]);
		mbput(f);
		return;
	}
	m = f->mf;
	print("cat %-30s\t%M\t%5ulld\t%s %ulld refs\n",
		m->name, (ulong)m->mode, m->length, m->uid, dbgetref(f->addr));
	if((m->mode&DMDIR) == 0){
		off = 0;
		for(;;){
			nr = dfpread(f, buf, sizeof buf, off);
			if(nr <= 0)
				break;
			write(1, buf, nr);
			off += nr;
		}
	}
	noerror();
	noerror();
	mbput(f);
}

static void
fsget(int, char *argv[])
{
	Memblk *f;
	Mfile *m;
	char buf[4096];
	uvlong off;
	long nr;
	int fd;

	fd = create(argv[1], OWRITE, 0664);
	if(fd < 0){
		fprint(2, "%s: error: %r\n", argv[0]);
		return;
	}
	if(catcherror()){
		close(fd);
		fprint(2, "%s: error: %r\n", argv[0]);
		return;
	}
	f = walkto(argv[2], nil);
	if(catcherror()){
		mbput(f);
		error(nil);
	}
	m = f->mf;
	print("get %-30s\t%M\t%5ulld\t%s %ulld refs\n",
		m->name, (ulong)m->mode, m->length, m->uid, dbgetref(f->addr));
print("%H", f);
	if((m->mode&DMDIR) == 0){
		off = 0;
		for(;;){
			nr = dfpread(f, buf, sizeof buf, off);
			if(nr <= 0)
				break;
			if(write(fd, buf, nr) != nr){
				fprint(2, "%s: error: %r\n", argv[0]);
				break;
			}
			off += nr;
		}
	}
	close(fd);
	noerror();
	noerror();
	mbput(f);
}

static void
flist(Memblk *f, char *ppath)
{
	char *path;
	Mfile *m;
	int i;

	m = f->mf;
	if(m->mode&DMDIR)
		dfloaddir(f, 0);
	rlock(m);
	if(ppath == nil){
		print("/");
		path = strdup(m->name);
	}else
		path = smprint("%s/%s", ppath, m->name);
	print("%-30s\t%M\t%5ulld\t%s %ulld refs\n",
		path, (ulong)m->mode, m->length, m->uid, dbgetref(f->addr));
	if(m->mode&DMDIR)
		for(i = 0; i < m->nchild; i++)
			flist(m->child[i].f, path);
	runlock(m);
	free(path);
}

static void
fslist(int, char**)
{
	u64int msz, fact;
	int i;

	msz = Embedsz - Dminattrsz + Ndptr*Dblkdatasz;
	fact = Dblkdatasz;
	for(i = 0; i < Niptr; i++){
		msz += Dptrperblk * fact;
		fact *= Dptrperblk;
	}
	print("fsys '%s' blksz %ulld maxdfsz %ulld:\n",
		fs->dev, fs->super->d.dblksz, msz);
	if(verb)
		fsdump();
	else
		flist(fs->root, nil);
	print("\n");
}

static void
fssnap(int, char**)
{
	fssync();
}

static void
fsrcl(int, char**)
{
	fsreclaim();
}

static void
usage(void)
{
	fprint(2, "usage: %s [-DFLAGS] [-dv] [-f disk] cmd...\n", argv0);
	exits("usage");
}

static struct
{
	char *name;
	void (*f)(int, char**);
	int nargs;
	char *usage;
} cmds[] =
{
	{"cd",	fscd,	2, "cd!where"},
	{"put",	fsput,	3, "put!src!dst"},
	{"get",	fsget,	3, "get!dst!src"},
	{"cat",	fscat,	3, "cat!what"},
	{"ls",	fslist,	1, "ls"},
	{"snap", fssnap, 1, "snap"},
	{"rcl",	fsrcl,	1, "rcl"},
};

static char xdbg[256];
static char zdbg[256];

void
main(int argc, char *argv[])
{
	char *dev;
	char *args[Nels];
	int i, j, nargs;

	dev = "disk";
	ARGBEGIN{
	case 'v':
		verb++;
		break;
	case 'f':
		dev = EARGF(usage());
		break;
	default:
		if(ARGC() >= 'A' && ARGC() <= 'Z'){
			xdbg['d'] = 1;
			xdbg[ARGC()] = 1;
		}else
			usage();
	}ARGEND;
	if(argc == 0)
		usage();
	fmtinstall('H', mbfmt);
	fmtinstall('M', dirmodefmt);
	errinit(Errstack);
	if(catcherror())
		sysfatal("error: %r");
	fsopen(dev);
	for(i = 0; i < argc; i++){
		if(catcherror())
			sysfatal("cmd %s: %r", argv[i]);
		if(verb>1)
			fsdump();
		else if(verb)
			flist(fs->root, nil);
		print("%% %s\n", argv[i]);
		nargs = gettokens(argv[i], args, Nels, "!");
		for(j = 0; j < nelem(cmds); j++){
			if(strcmp(cmds[j].name, argv[i]) != 0)
				continue;
			if(cmds[j].nargs != 0 && cmds[j].nargs != nargs)
				print("usage: %s\n", cmds[j].usage);
			else{
				memmove(dbg, xdbg, sizeof xdbg);
				cmds[j].f(nargs, args);
				memmove(dbg, zdbg, sizeof zdbg);
			}
			break;
		}
		noerror();
		if(j == nelem(cmds)){
			print("no such command\n");
			for(j = 0; j < nelem(cmds); j++)
				print("\t%s\n", cmds[j].usage);
			break;
		}
	}
	if(verb>1)
		fsdump();
	else if(verb)
		flist(fs->root, nil);
	noerror();
	exits(nil);
}


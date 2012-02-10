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
walkto(Fsys *fs, char *a, char **lastp)
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
		f = walkpath(fs, fs->root, els, nels-1);
		*lastp = a + strlen(a) - strlen(els[nels-1]);
	}else
		f = walkpath(fs, fs->root, els, nels);
	free(path);
	noerror();
	if(verb)
		print("walked to %H", f);
	return f;
}

static void
fscd(Fsys*, int, char *argv[])
{
	free(fsdir);
	fsdir = strdup(argv[1]);
}

static void
fsput(Fsys *fs, int, char *argv[])
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
	m = walkto(fs, argv[2], &fn);
	f = dfcreate(fs, m, fn, d->uid, d->mode&(DMDIR|0777));
	if((d->mode&DMDIR) == 0){
		off = 0;
		for(;;){
			nr = read(fd, buf, sizeof buf);
			if(nr <= 0)
				break;
			nw = dfpwrite(fs, f, buf, nr, off);
			dDprint("wrote %ld of %ld bytes\n", nw, nr);
			off += nr;
		}
	}
	mbput(fs, m);
	if(verb)
		print("created %H", f);
	mbput(fs, f);
	noerror();
done:
	close(fd);
	free(d);
}

static void
fscat(Fsys *fs, int, char *argv[])
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
	f = walkto(fs, argv[2], nil);
	if(catcherror()){
		fprint(2, "%s: error: %r\n", argv[0]);
		mbput(fs, f);
		return;
	}
	m = f->mf;
	print("%-30s\t%M\t%5ulld\t%s %ulld refs\n",
		m->name, (ulong)m->mode, m->length, m->uid, dbgetref(fs, f->addr));
	if((m->mode&DMDIR) == 0){
		off = 0;
		for(;;){
			nr = dfpread(fs, f, buf, sizeof buf, off);
			if(nr <= 0)
				break;
			write(1, buf, nr);
			off += nr;
		}
	}
	noerror();
	noerror();
	mbput(fs, f);
}

static void
flist(Fsys *fs, Memblk *f, char *ppath)
{
	char *path;
	Mfile *m;
	int i;

	m = f->mf;
	if(m->mode&DMDIR)
		dfloaddir(fs, f, 0);
	rlock(m);
	if(ppath == nil){
		print("/");
		path = strdup(m->name);
	}else
		path = smprint("%s/%s", ppath, m->name);
	print("%-30s\t%M\t%5ulld\t%s %ulld refs\n",
		path, (ulong)m->mode, m->length, m->uid, dbgetref(fs, f->addr));
	if(m->mode&DMDIR)
		for(i = 0; i < m->nchild; i++)
			flist(fs, m->child[i].f, path);
	runlock(m);
	free(path);
}

static void
fslist(Fsys *fs, int, char**)
{
	print("fsys '%s' blksz %ulld:\n", fs->dev, fs->super->d.dblksz);
	if(verb)
		fsdump(fs);
	else
		flist(fs, fs->root, nil);
	print("\n");
}

static void
fssnap(Fsys *fs, int, char**)
{
	fssync(fs);
}

static void
fsrcl(Fsys *fs, int, char**)
{
	fsreclaim(fs);
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
	void (*f)(Fsys*, int, char**);
	int nargs;
	char *usage;
} cmds[] =
{
	{"cd",	fscd,	2, "cd!where"},
	{"put",	fsput,	3, "put!src!dst"},
	{"cat",	fscat,	3, "cat!what"},
	{"ls",	fslist,	1, "ls"},
	{"snap", fssnap, 1, "snap"},
	{"rcl",	fsrcl,	1, "rcl"},
};

void
main(int argc, char *argv[])
{
	Fsys *fs;
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
			dbg['d'] = 1;
			dbg[ARGC()] = 1;
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
	fs = fsopen(dev);
	for(i = 0; i < argc; i++){
		if(catcherror())
			sysfatal("cmd %s: %r", argv[i]);
		if(verb>1)
			fsdump(fs);
		else if(verb)
			flist(fs, fs->root, nil);
		print("%% %s\n", argv[i]);
		nargs = gettokens(argv[i], args, Nels, "!");
		for(j = 0; j < nelem(cmds); j++){
			if(strcmp(cmds[j].name, argv[i]) != 0)
				continue;
			if(cmds[j].nargs != 0 && cmds[j].nargs != nargs)
				print("usage: %s\n", cmds[j].usage);
			else
				cmds[j].f(fs, nargs, args);
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
		fsdump(fs);
	else if(verb)
		flist(fs, fs->root, nil);
	noerror();
	exits(nil);
}


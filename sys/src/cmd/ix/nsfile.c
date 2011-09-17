/*
 * File tree kept in an external file system.
 * Metadata is kept in an index file.
 * External changes are detected.
 * Suitable for use as a cache of a remote file tree.
 */

#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include "conf.h"
#include "file.h"
#include "dbg.h"

static File *root;
static char *dir;
static int usedb;

static char*
qidtype(char *s, uchar t)
{
	char *p;

	p = s;
	if(t & QTDIR)
		*p++ = 'd';
	if(t & QTAPPEND)
		*p++ = 'a';
	if(t & QTEXCL)
		*p++ = 'l';
	if(t & QTAUTH)
		*p++ = 'A';
	*p = '\0';
	return s;
}


static void
fdirconv(char *buf, char *e, Dir *d)
{
	char tmp[16];

	seprint(buf, e, "%s '%s' "
		"(%llux %lud %s) %#luo l %lld mt %uld", d->name,
			d->uid,
			d->qid.path, d->qid.vers, qidtype(tmp, d->qid.type), d->mode,
			d->length, d->mtime);
}

int
shortdirfmt(Fmt *fmt)
{
	char buf[160];

	fdirconv(buf, buf+sizeof buf, va_arg(fmt->args, Dir*));
	return fmtstrcpy(fmt, buf);
}

Dir*
dupdir(Dir *d)
{
	int sz;
	Dir *nd;
	char *s;

	sz = strlen(d->name) + strlen(d->uid) +
		strlen(d->gid) + strlen(d->muid) + 4;
	nd = emalloc(sizeof *nd + sz);
	*nd = *d;
	nd->name = (char*)(nd+1);
	s = strecpy(nd->name, nd->name+sz, d->name);
	nd->uid = s+1;
	s = strecpy(nd->uid, nd->name+sz, d->uid);
	nd->gid = s+1;
	s = strecpy(nd->gid, nd->name+sz, d->gid);
	nd->muid = s+1;
	strecpy(nd->muid, nd->name+sz, d->muid);
	return nd;
}

int
filefmt(Fmt *fmt)
{
	File *f;
	Dir *sd;
	char tmp[16];

	f = va_arg(fmt->args, File*);
	if(f == nil)
		return fmtprint(fmt, "<nil>");
	if((sd = f->sd) != nil)
		return fmtprint(fmt, "'%s' r=%ld sq (%llux %lud %s) %D",
			f->path, f->ref,
			sd->qid.path, sd->qid.vers, qidtype(tmp, sd->qid.type),
			f->d);
	else
		return fmtprint(fmt, "'%s' r=%ld %D",
			f->path, f->ref, f->d);
}

static void
dumpfile(File *f)
{
	print("%T\n", f);
	for(f = f->child; f != nil; f = f->next)
		dumpfile(f);
}

/*
 * BEWARE: no locks.
 */
void
dumptree(void)
{
	dumpfile(root);
}

static void
delchild(File *parent, File *f)
{
	File **fp;

	for(fp = &parent->child; *fp != nil; fp = &(*fp)->next)
		if(*fp == f)
			break;
	if(*fp == nil)
		sysfatal("delchild");
	*fp = f->next;
	f->next = nil;
	parent->nchild--;
	f->parent = nil;
}

static void
addchild(File *parent, File *f)
{
	assert(f->next == nil);
	f->next = parent->child;
	parent->child = f;
	parent->nchild++;
	f->parent = parent;
}

File*
getchild(File *parent, char *name)
{
	File *f;

	for(f = parent->child; f != nil; f = f->next)
		if(strcmp(f->d->name, name) == 0){
			incref(f);
			return f;
		}
	return nil;
}

void
putfile(File *f)
{
	File *parent;

	if(decref(f) > 0)
		return;
	parent = f->parent;
	if(parent != nil){
		wlock(parent);
		delchild(parent, f);
		wunlock(parent);
	}
	while(f->child != nil)
		putfile(f->child);
	free(f->path);
	free(f->d);
	free(f);
}

/*
 * parent must be wlocked by caller.
 */
static void
unlinkfile(File *f)
{
	if(f->parent != nil)
		delchild(f->parent, f);
	putfile(f);
}

/*
 * keeps ptr to path within new file.
 */
File*
newfile(File *parent, char *path, Dir *d)
{
	File *f;

	if(d == nil){
		dnprint("dirstat %s (newfile)\n", path);
		d = dirstat(path);
	}
	if(d == nil)
		return nil;

	f = emalloc(sizeof *f);
	f->ref = 1;
	f->path = path;
	if(parent != nil)
		addchild(parent, f);
	f->d = d;
	return f;
}

static Dir*
readdir(Biobuf *bin, char *p)
{
	static uchar buf[DIRMAX];
	ulong sz;
	Dir *d;

	if(Bread(bin, buf, BIT16SZ) != BIT16SZ){
		fprint(2, "%s: %s: eof\n", argv0, p);
		return nil;
	}
	sz = GBIT16(buf);
	if(BIT16SZ + sz > sizeof buf){
		fprint(2, "%s: %s: dir too long\n", argv0, p);
		return nil;
	}
	if(Bread(bin, buf + BIT16SZ, sz) != sz){
		fprint(2, "%s: %s: read failed\n", argv0, p);
		return nil;
	}
	d = emalloc(sizeof *d + sz);
	if(convM2D(buf, sizeof buf, d, (char*)(d+1)) <= 0){
		fprint(2, "%s: %s: convM2D failed\n", argv0, p);
		free(d);
		return nil;
	}
	return d;
}

static File*
loaddir(Biobuf *bin, File *parent)
{
	File *f;
	Dir *d, *sd;
	int nchild;
	char *p;

	p = "/";
	if(parent != nil)
		p = parent->path;
	d = readdir(bin, p);
	if(d == nil)
		return nil;
	sd = readdir(bin, p);
	if(sd == nil){
		free(d);
		return nil;
	}
	f = emalloc(sizeof *f);
	f->ref = 1;
	f->d = d;
	f->sd = sd;
	if(parent == nil)
		f->path = estrdup(".");
	else{
		f->path = smprint("%s/%s", parent->path, f->d->name);
		addchild(parent, f);
	}
	if(ISDIR(f)){
		nchild = f->d->length;
		f->d->length = 0;
		while(nchild-- > 0)
			if(loaddir(bin, f) == nil)
				break;
	}
	if(dbg['n'] > 1)
		dnprint("loaded %T\n", f);
	return f;
}

static int
changed(Dir *d1, Dir *d2)
{
	if((d1->qid.type&QTDIR) != 0 && (d2->qid.type&QTDIR) != 0)
		return 0;
	return d1->length != d2->length || d1->mtime != d2->mtime ||
		d1->qid.path != d2->qid.path || d1->qid.vers != d2->qid.vers ||
		d1->qid.type != d2->qid.type;
}

static int
mchanged(Dir *d1, Dir *d2)
{
	return d1->mode != d2->mode || strcmp(d1->uid, d2->uid) != 0 ||
		strcmp(d1->gid, d2->gid) != 0;
}

char*
tmpfile(char *name)
{
	return esmprint("%s.!.", name);
}

static int
ignorename(char *name)
{
	int len;

	if(strcmp(name, ".ixd") == 0 || strcmp(name, ".ixd.new") == 0)
		return 1;
	len = strlen(name);
	if(len > 3 && strcmp(&name[len-3], ".!.") == 0)
		return 1;
	if(len > 3 && strcmp(&name[len-3], ".!!") == 0)
		return 1;
	return 0;
}


static int
scandir(File *f, Dir *ud, int recur)
{
	Dir *d, *cd;
	File *cf;
	int i, nd, fd, mustrecur;
	char *s;

	wlock(f);
	if(dbg['n']>1)
		dnprint("scan %s\n", f->path);
	d = ud;
	if(d == nil){
		dnprint("dirstat %s (scandir)\n", f->path);
		d = dirstat(f->path);
	}
	if(d == nil){
		wunlock(f);
		return 0;
	}
	if((f->d->qid.type&QTDIR) != (d->qid.type&QTDIR)){
		/* a file became a dir or a dir became a file
		 * get rid of the old data and scan it according to
		 * its new nature.
		 */
		dnprint("gone and came: %T\n", f);
		if(ISDIR(f)){
			while(f->child != nil){
				unlinkfile(f->child);
			}
			f->d->qid.type &= ~QTDIR;
		}else
			f->d->qid.type |= QTDIR;
	}
	if(mchanged(f->d, d) || changed(f->d, d)){
		free(f->d);
		if(d == ud)
			d = dupdir(ud);
		f->d = d;
		dnprint("changed: %T\n", f);
	}
	wunlock(f);
	rlock(f);
	if(ISDIR(f)){
		fd = open(f->path, OREAD);
		if(fd < 0){
			runlock(f);
			free(d);
			return -1;
		}
		dnprint("dirreadall %s\n", f->path);
		nd = dirreadall(fd, &cd);
		close(fd);
		for(i = 0; i < nd; i++){
			if(ignorename(cd[i].name))
				continue;
			cf = getchild(f, cd[i].name);
			if(cf == nil){
				mustrecur = 1;
				s = smprint("%s/%s", f->path, cd[i].name);
				cf = newfile(f, s, dupdir(&cd[i]));
				if(cf == nil){
					fprint(2, "%s: %s: %r\n", argv0, s);
					free(s);
					continue;
				}
				dnprint("came: %T\n", cf);
			}else{
				mustrecur = recur;
				putfile(cf);	/* ref won't be zero */
			}
			cf->visited = 1;
			if(mustrecur ||!ISDIR(cf)){
				runlock(f);
				scandir(cf, &cd[i], mustrecur);
				rlock(f);
			}
		}
		runlock(f);
		wlock(f);
	Again:
		for(cf = f->child; cf != nil; cf = cf->next)
			if(cf->visited == 0){
				/*
				 * If it's dirty, and has been removed,
				 * there's nothing we can do.
				 * let the client notice.
				 */
				dnprint("gone: %T\n", cf);
				incref(cf);
				unlinkfile(cf);
				putfile(cf);
				goto Again;	/* list has changed */
			}
		for(cf = f->child; cf != nil; cf = cf->next)
			cf->visited = 0;
		wunlock(f);
		free(cd);
	}else
		runlock(f);
	return 1;
}

static int
writedir(Biobuf *bout, File *f)
{
	File *cf;
	static uchar buf[DIRMAX];
	int n;
	Dir d;

	wlock(f);
	if(ISDIR(f))
		f->d->length = f->nchild;
	n = convD2M(f->d, buf, sizeof buf);
	if(ISDIR(f))
		f->d->length = 0;
	if(Bwrite(bout, buf, n) != n){
		wunlock(f);
		fprint(2, "%s: writedir: %r\n", f->path);
		return -1;
	}
	if(f->sd == nil){
		nulldir(&d);
		n = convD2M(&d, buf, sizeof buf);
	}else
		n = convD2M(f->sd, buf, sizeof buf);
	if(Bwrite(bout, buf, n) != n){
		wunlock(f);
		fprint(2, "%s: writedir2: %r\n", f->path);
		return -1;
	}
	if(dbg['n'] > 1)
		dnprint("written: %T\n", f);
	wunlock(f);
	rlock(f);
	for(cf = f->child; cf != nil; cf = cf->next)
		if(writedir(bout, cf) < 0){
			runlock(f);
			return -1;
		}
	runlock(f);
	return 0;
}

void
childmap(File *f, void(*fn)(File*))
{
	File *cf;

	rlock(f);
	for(cf = f->child; cf != nil; cf = cf->next)
		fn(cf);
	wunlock(f);
}

static int
writetree(File *f)
{
	Biobuf *bout;
	Dir d;
	int rc;

	bout = Bopen(".ixd.new", OWRITE);
	if(bout == nil){
		fprint(2, "%s: writetree: %r\n", argv0);
		return -1;
	}
	rc = writedir(bout, f);
	if(Bterm(bout) < 0)
		rc = -1;
	if(rc < 0){
		remove(".ixd.new");
		return -1;
	}
	nulldir(&d);
	d.name = ".ixd";
	if(dirwstat(".ixd.new", &d) < 0){
		fprint(2, "%s: rename .ixd: %r\n", argv0);
		remove(".ixd.new");
		return -1;
	}
	return 0;
}

static File*
loadtree(void)
{
	Biobuf *bin;
	File *f;

	dnprint("loadtree:\n");
	bin = Bopen(".ixd", OREAD);
	if(bin != nil){
		f = loaddir(bin, nil);
		Bterm(bin);
	}else
		f = newfile(nil, ".", nil);
	if(scandir(f, nil, 1) <= 0)
		sysfatal("loadtree: can't scan root");
	if(bin == nil)
		writetree(f);
	return f;
}

File*
rootfile(void)
{
	incref(root);
	return root;
}

void
fileinit(char *path, int udb)
{
	if(chdir(path) < 0)
		sysfatal("chdir %s: %r", path);
	usedb = udb;
	if(usedb)
		root = loadtree();
	else{
		root = newfile(nil, ".", nil);
		if(scandir(root, nil, 1) <= 0)
			sysfatal("can't scan %s", path);
	}
	if(root == nil)
		sysfatal("fileinit: %r");
}

int
filesync(void)
{
	dnprint("filesync\n");
	if(usedb)
		return writetree(root);
	return 0;
}

int
filechanged(File *f)
{
	Dir *nd;
	int r;

	wlock(f);
	r = None;
	dnprint("dirstat %s\n", f->path);
	nd = dirstat(f->path);
	if(nd == nil){
		wunlock(f);
		return Meta|Data|Gone;
	}
	if(changed(f->d, nd))
		r |= Data;
	if(mchanged(f->d, nd))
		r |= Meta;

	free(f->d);
	f->d = nd;
	wunlock(f);
	return r;
}

int
wstatfile(File *f, Dir *d)
{
	Dir *nd;
	Dir wd;

	dnprint("wstat: %T\n", f);
	if(d->name[0] != 0 && strcmp(d->name, f->d->name) != 0 &&
	   f->parent == nil){
		werrstr("can't rename /");
		return -1;
	}
	wlock(f);
	if(dirwstat(f->path, d) < 0){
		nulldir(&wd);
		wd.mode = d->mode;
		if(dirwstat(f->path, &wd) < 0){
			wunlock(f);
			return -1;
		}
		fprint(2, "%s: can't change uid/mtime/...: %r\n", f->path);
	}
	
	if(d->name[0] != 0 && strcmp(d->name, f->d->name) != 0){
		free(f->path);
		f->path = smprint("%s/%s", f->parent->path, d->name);
	}
	dnprint("dirstat %s\n", f->path);
	nd = dirstat(f->path);
	if(nd == nil)
		sysfatal("wstatfile: dirstat bug");
	free(f->d);
	f->d = nd;
	wunlock(f);
	return 0;
}

Qid
fileqid(File *f)
{
	Qid q;

	rlock(f);
	q = f->d->qid;
	runlock(f);
	return q;
}

Dir*
statfile(File *f, int refresh)
{
	Dir *d;

	if(refresh){
		wlock(f);
		dnprint("stat: %T\n", f);
		d = dirstat(f->path);
		if(d == nil){
			wunlock(f);
			return nil;
		}
		free(f->d);
		f->d = d;
		d = dupdir(f->d);
		wunlock(f);
	}else{
		rlock(f);
		d = dupdir(f->d);
		runlock(f);
	}
	return d;
}

int
walkfile(File **fp, char *elem)
{
	File *f, *nf;

	f = *fp;
	dnprint("walk %s %s\n", f->path, elem);
	if(utfrune(elem, '/') != nil){
		werrstr("'/' not allowed in file name");
		return -1;
	}
	if(strcmp(elem, ".") == 0)
		return 0;
	if(strcmp(elem, "..") == 0){
		if(f != root){
			incref(f->parent);
			*fp = f->parent;
			putfile(f);
		}
		return 0;
	}
	rlock(f);
	nf = getchild(f, elem);
	runlock(f);
	if(nf == nil){
		werrstr("file does not exist");
		return -1;
	}
	putfile(f);
	*fp = nf;
	return 0;
}

int
openfile(File *f, int mode)
{
	int fd;

	dnprint("open: mode %#x %T\n", mode, f);
	if((mode&~(OTRUNC|3)) != 0){
		werrstr("bad mode for openfile");
		return -1;
	}
	wlock(f);
	fd = open(f->path, mode);
	if(fd >= 0)
		f->nopens++;
	wunlock(f);
	return fd;
}

void
closefile(File *f, int fd)
{
	wlock(f);
	dnprint("close: %T\n", f);
	close(fd);
	if(--f->nopens == 0){
		dnprint("dirstat %s\n", f->path);
		free(f->d);
		f->d = dirstat(f->path);
		if(f->d == nil)
			sysfatal("closefile: dirstat bug");
	}
	wunlock(f);
}

int
createfile(File **fp, char *elem, int mode, int perm)
{
	char *s;
	File *f, *nf;
	int fd;

	f = *fp;
	dnprint("create: %s in %T\n", elem, f);
	if(utfrune(elem, '/') != nil){
		werrstr("'/' not allowed in file name");
		return -1;
	}
	if(strcmp(elem, ".") == 0 || strcmp(elem, "..") == 0){
		werrstr("'%s' can't be created", elem);
		return -1;
	}
	if(mode&~(DMDIR|3)){
		werrstr("createfile: bad mode");
		return -1;
	}
	scandir(f, nil, 0);
	wlock(f);
	nf = getchild(f, elem);
	if(nf != nil){
		/*
		 * truncate, actually.
		 */
		wlock(nf);
		fd = create(nf->path, mode, perm);
		if(fd >= 0)
			nf->nopens++;
		wunlock(nf);
		wunlock(f);
		putfile(*fp);
		*fp = nf;
		return fd;
	}
	s = smprint("%s/%s", f->path, elem);
	fd = create(s, mode, perm);
	if(fd < 0){
		free(s);
		wunlock(f);
		return -1;
	}
	nf = newfile(f, s, nil);
	nf->nopens++;
	wunlock(f);
	incref(nf);
	putfile(*fp);
	*fp = nf;
	return fd;
}

long
preadfile(File *, int fd, void *a, ulong count, uvlong offset)
{
	return pread(fd, a, count, offset);
}

long
pwritefile(File *, int fd, void *a, ulong count, uvlong offset)
{
	return pwrite(fd, a, count, offset);
}

int
removefile(File *f)
{
	File *parent;

	dnprint("remove: %T\n", f);
	parent = f->parent;
	if(parent == nil){
		werrstr("can't remove /");
		return -1;
	}
Again:
	wlock(f);
	incref(f); /* keep it here while we use it */
	parent = f->parent;

	if(!canwlock(parent)){	/* lock order: parent, then child */
		wunlock(f);
		yield();
		goto Again;
	}
	if(remove(f->path) < 0){
		wunlock(f);
		wunlock(parent);
		putfile(f);
		return -1;
	}
	unlinkfile(f);

	wunlock(f);
	wunlock(parent);
	/*
	 * We don't put the ref given by the caller.
	 * When he calls putfile() the file should go.
	 */
	return 0;
}

enum
{
	Pother = 	1,
	Pgroup = 	8,
	Powner =	64,
};

int
perm(File *f, char *user, int p)
{
	if((p*Pother) & f->d->mode)
		return 1;
	if(strcmp(user, f->d->gid)==0 && ((p*Pgroup) & f->d->mode))
		return 1;
	if(strcmp(user, f->d->uid)==0 && ((p*Powner) & f->d->mode))
		return 1;
	return 0;
}


#include <u.h>
#include <libc.h>
#include <error.h>
#include <thread.h>
#include <fcall.h>

#include "conf.h"
#include "msg.h"
#include "ch.h"
#include "mpool.h"
#include "tses.h"
#include "fs.h"
#include "file.h"
#include "cfile.h"
#include "ixreqs.h"
#include "dbg.h"

/*
 * Conflict policy:
 *	upon conflicts, we report the conflict and save the local
 *	change in the local cache. The saved file is ignored for pulls/pushes.
 *	The user may resolve by removing the saved copy or using it to
 *	update the server's file.
 *
 * Removed files:
 *	- removed files must stay with the removed flag until
 *	  any conflict is resolved:
 *		- removed in the server:
 *			the cache file is renamed and we are back in sync.
 *		- removed in the client:
 *			there's no local version to keep; just report
 *			and we are back in sync.
 */

static void	updatefile(File *f);
static int	condpull(File *f);
static int	condremove(File *f);

static Cmux *cmux;
static File *root;
static int disc;
static int rootfid;
static int twritehdrsz;

extern ulong msz;

File*
crootfile(void)
{
	incref(root);
	return root;
}

void
cputfile(File *f)
{
	putfile(f);
}

static void
disconnected(void)
{
	if(disc++)
		return;
	fprint(2, "%s: disconnected: %r\n", argv0);
}

/*
 * There's a conflict in f due to concurrent changes in server and client.
 * Copy our local version to a conflicting name and pull the server version.
 */
static void
conflict(File *f)
{
	Dir d;

	nulldir(&d);
	d.name = esmprint("%s.!!", f->d->name);
	if(dirwstat(f->path, &d) < 0)
		fprint(2, "conflict in %s. not copied.\n", f->path);
	else
		fprint(2, "conflict in %s. copied to %s.!!\n", f->path, f->path);
	free(d.name);

	free(f->sd);
	f->sd = nil;
	condpull(f);
}

static long
gotdents(File *f, uchar *a, long n)
{
	Dir d;
	char buf[512];
	int ns, fd;
	File *nf;

	while(n > 0){
		ns = convM2D(a, n, &d, buf);
		if(ns <= 0){
			dcprint("%s: gotdents: %s: convM2D\n", argv0, f->path);
			return -1;
		}
		a += ns;
		n -= ns;
		nf = getchild(f, d.name);
		if(nf == nil){
			nf = f;
			incref(f);
			fd = createfile(&nf, d.name, OREAD, d.mode);
			if(fd < 0){
				putfile(f);
				dcprint("%s: gotdents: %s/%s: %r\n",
					argv0, f->path, d.name);
			}else
				close(fd);
			/* f->sd is nil, and it will be retrieved by updatefile */
		}else{
			if(nf->sremoved){	/* it came back to life */
				free(nf->sd);
				nf->sd = nil;
			}
			if(nf->sd == nil)
				nf->sd = dupdir(&d);
		}
		nf->visited = 1;
		/* don't putfile(nf); gotdir releases all child refs */
	}
	return 0;
}

static void
gotdir(File *f)
{
	File *cf;
	int chg;

	rlock(f);
	for(cf = f->child; cf != nil; cf = cf->next){
		if(cf->sd == nil)
			updatefile(cf);
		else if(cf->visited == 0)
			cf->sremoved = 1;
		cf->visited = 0;
		putfile(cf);
	}
Again:
	for(cf = f->child; cf != nil; cf = cf->next)
		if(cf->sremoved){
			chg = filechanged(cf);
			if(chg && (chg&Gone) == 0)
				conflict(cf);
			else{
				runlock(f);
				removefile(cf);
				rlock(f);
				goto Again;
			}
		}
	runlock(f);	
}

static int
walkpath(Ch *ch, File *f)
{
	int n;

	if(f->parent == nil)
		return 0;
	n = walkpath(ch, f->parent);
	xtwalk(ch, f->d->name, 0);
	return n + 1;
}

/*
 * Get a chan and send fid, clone, and walk requests to get to the file.
 * it's ok if f is nil, and that means also /
 */
static int
walktofile(File *f, Ch **chp, int *nwalksp, char *why)
{
	Ch *ch;

	if(disc)
		return -1;
	ch = newch(cmux);
	if(ch == nil){
		disconnected();
		dcprint("%s %s: newch %r\n", why, f ? f->path : "/");
		return -1;
	}
	xtfid(ch, root->fid, 0);
	xtclone(ch, OCEND|OCERR, 0);
	if(f == nil)
		*nwalksp = 0;
	else
		*nwalksp = walkpath(ch, f);
	*chp = ch;
	return 0;
}

/*
 * Get the file, cond that it has changed.
 * Save it at a temporary place and replace the cached
 * one with the new one.
 * Return 0 if ok or disconnected. 1 if file changed. -1 upon errors.
 */
static int
condpull(File *f)
{
	Ch *ch;
	int fd, i, nwalks;
	Dir d, wd;
	char *fn, buf[512];
	uvlong offset;
	long nr;
	Msg *m;

	if(walktofile(f, &ch, &nwalks, "pull") < 0)
		return 0;
	/*
	 * If we got it from server, cond that it changed or we are done.
	 */
	if(f->sd != nil)
		xtcond(ch, CNE, f->sd, 0);
	xtstat(ch, 0);
	/*
	 * If we got it from server, cond that data changed or we are done.
	 */
	if(f->sd != nil){
		nulldir(&wd);
		wd.qid = f->sd->qid;
		wd.mtime = f->sd->mtime;
		xtcond(ch, CNE, &wd, 0);
	}
	xtopen(ch, OREAD, 0);
	xtread(ch, -1, 0ULL, msz, 1);

	fn = nil;
	fd = -1;
	if(xrfid(ch) < 0 || xrclone(ch) < 0)
		goto Fail;
	for(i = 0; i < nwalks; i++)
		if(xrwalk(ch, nil) < 0){
			rerrstr(buf, sizeof buf);
			if(strstr(buf, "not exist") != 0){
				f->sremoved = 1;
				goto Done;
			}
			goto Fail;
		}

	if(f->sd != nil)
		if(xrcond(ch) < 0)
			return 0;	/* file is up to date; we are done */
	if(xrstat(ch, &d, buf) < 0)
		goto Fail;

	if(f->sd != nil)
		if(xrcond(ch) < 0)	/* data didn't change; we are done */
			goto Done;

	if(xropen(ch) < 0)
		goto Fail;

	/*
	 * create the directory in case it does not exist, or a temporary
	 * file to retrieve the new version from the server.
	 */
	if((d.qid.type&QTDIR) != 0){
		fd = create(f->path, OREAD, d.mode); /* ignore errors */
		close(fd);
		fd = -1;
	}else{
		fn = tmpfile(f->path);
		fd = create(fn, OWRITE, d.mode);
		if(fd < 0){
			abortch(ch);
			goto Fail;
		}
	}

	/*
	 * Gather data or directory entries or file data.
	 */
	offset = 0ULL;
	do{
		m = xrread(ch);
		if(m == nil){
			dcprint("%s: read: %r\n", f->path);
			goto Fail;
		}
		nr = IOLEN(m->io);
		if(nr > 0){
			if(d.qid.type&QTDIR)
				nr = gotdents(f, m->io->rp, nr);
			else
				nr = pwrite(fd, m->io->rp, nr, offset);
			if(nr < 0){
				abortch(ch);
				freemsg(m);
				goto Fail;
			}
		}
		offset += nr;
		freemsg(m);
	}while(nr > 0);

	/*
	 * Got everything, merge dir entries or move data into place.
	 */
	if(d.qid.type&QTDIR){
		gotdir(f);
	}
	if(wstatfile(f, &d) < 0)
		goto Fail;
Done:
	free(f->sd);
	f->sd = dupdir(&d);
	if(fn != nil){
		remove(fn);	/* mail fail, if we could rename it */
		free(fn);
	}
	if(fd >= 0)
		close(fd);
	return 1;

Fail:
	if(fn != nil){
		remove(fn);
		free(fn);
	}
	if(fd >= 0)
		close(fd);
	return -1;
}

/*
 * Push the file, cond that it has not changed.
 * Return 0 if ok or disconnected, 1 if changed, -1 upon errors.
 *
 * XXX: Shouldn't we push metadata changes and then data cond that
 * data has also changed?
 */
static int
condpush(File *f)
{
	Ch *ch;
	int fd, i, nwalks;
	long nw, nr;
	Msg *m;
	uvlong offset;
	char buf[ERRMAX];

	if(f->sd != nil){
		if(walktofile(f, &ch, &nwalks, "push") < 0)
			return 0;
		xtcond(ch, CEQ, f->sd, 0);
		xtwalk(ch, "..", 0);
	}else
		if(walktofile(f->parent, &ch, &nwalks, "push") < 0)
			return 0;
	xtcreate(ch, f->d->name, OWRITE, f->d->mode, ISDIR(f));
	if(xrfid(ch) < 0 || xrclone(ch) < 0){
		if(!ISDIR(f))
			closech(ch);
		return -1;
	}
	for(i = 0; i < nwalks; i++)
		if(xrwalk(ch, nil) < 0){
			if(!ISDIR(f))
				closech(ch);
			rerrstr(buf, sizeof buf);
			return -1;
		}
	if(f->sd != nil){
		if(xrcond(ch) < 0){	/* changed in server */
			if(!ISDIR(f))
				closech(ch);
			return 1;	/* shouldn't we ignore and push? */
		}
		if(xrwalk(ch, nil) < 0){
			if(!ISDIR(f))
				closech(ch);
			return -1;
		}
	}
	if(xrcreate(ch) < 0){
		if(!ISDIR(f))
			closech(ch);
		return -1;
	}
	if(ISDIR(f))
		return 0;
	fd = open(f->path, OREAD);
	if(fd < 0){
		fprint(2, "%s: %r\n", f->path);
		xtclunk(ch, 1);
		xrclunk(ch);
		return -1;
	}
	nw = 0;
	offset = 0ULL;
	for(;;){
		m = newmsg(pool);
		nr = IOCAP(m->io) - Chhdrsz - twritehdrsz;
		m->io->wp += twritehdrsz;
		nr = read(fd, m->io->wp, nr);
		if(nr <= 0){
			if(nr < 0)
				fprint(2, "%s: read: %r", f->path);
			xtclunk(ch, 1);
			break;
		}
		m->io->wp += nr;
		if(xtwrite(ch, m, nr, offset, 0) < 0){
			fprint(2, "%s: write: %r\n", f->path);
			closech(ch);
			drainch(ch);
			close(fd);
			return -1;
		}
		nw++;
		offset += nr;	

		/* read replies so that no more than
		 * 10 outstanding writes are going.
		 */
		if(nw > Maxwritewin){
			nw--;
			if(xrwrite(ch) < 0){
				fprint(2, "%s: write: %r\n", f->path);
				closech(ch);
				close(fd);
				return -1;
			}
		}
	}
	close(fd);
	while(nw-- > 0)
		if(xrwrite(ch) < 0){
			fprint(2, "%s: write: %r\n", f->path);
			return -1;
		}

	xrclunk(ch);
	return 0;
}

/*
 * Remove the file, cond that it has not changed.
 * Return 0 if ok or disconnected, 1 if changed, -1 upon errors.
 */
static int
condremove(File *f)
{
	Ch *ch;
	int i, nwalks;
	char buf[ERRMAX];

	if(walktofile(f, &ch, &nwalks, "remove") < 0)
		return 0;

	/*
	 * If got it from server, cond that it has not changed or conflict.
	 */
	if(f->sd != nil)
		xtcond(ch, CEQ, f->sd, 0);
	xtremove(ch, 1);
	if(xrfid(ch) < 0 || xrclone(ch) < 0)
		return -1;
	if(xrfid(ch) < 0 || xrclone(ch) < 0)
		return -1;
	for(i = 0; i < nwalks; i++)
		if(xrwalk(ch, nil) < 0){
			rerrstr(buf, sizeof buf);
			if(strstr(buf, "not exist") != 0){
				f->sremoved = 1;
				return 0;
			}
			return -1;
		}
	if(f->sd != nil)
		if(xrcond(ch) < 0)
			return 1;	/* file changed in server! */
	if(xrremove(ch) < 0)
		return -1;
	return 0;
}

/*
 * Try to sync file:
 * This can be done with a cond push:
 * 1- if local changes & out of date -> conflict
 * 2- if local changes and up to date -> push
 *
 * These can be done with a cond pull:
 * 3- if no local changes & out of date -> pull
 * 4- if no local changes & up to date -> done.
 *
 * For directories, recur to check out inner files.
 */
static void
updatefile(File *f)
{
	int chg;

	chg = filechanged(f);
	if(chg){
		if(chg&Gone){
			f->cremoved = 1;
			if(condremove(f) == 1)
				conflict(f);
		}else
			if(condpush(f) == 1)
				conflict(f);
	}else
		condpull(f);
	if(ISDIR(f))
		childmap(f, updatefile);
		
}

static void
settwritehdrsz(void)
{
	Fscall t;

	t.type = Twrite;
	twritehdrsz = packedsize(&t);
}

void
cfileinit(Cmux *cm)
{
	Ch *ch;
	int fid;

	settwritehdrsz();
	cmux = cm;
	fid = -1;
	if(cm == nil){
		werrstr("no cmux");
		disconnected();
	}else{
		ch = newch(cmux);
   		xtversion(ch, 0);
		xtattach(ch, getuser(), "main", 1);
		if(xrversion(ch, &msz) < 0)
			disconnected();
		else if(xrattach(ch, &fid) < 0)
			disconnected();
	}
	root = rootfile();
	root->fid = fid;
	updatefile(root);
}

int
cwstatfile(File *f, Dir *d)
{
	return -1;
/*	send wstat
	if failed
		return -1;
	return wstatfile(f, d);
 */
}

Dir*
cstatfile(File *f, int refresh)
{
return nil;
	return statfile(f, 0);
}

int
cwalkfile(File **fp, char *elem)
{
return -1;
	return walkfile(fp, elem);
}

int
copenfile(File *f, int mode)
{
return -1;
/*	be sure mode is 3|OTRUNC at most.


	send the open, cond to our version,
	if it's for reading, we want to get the file
	and send local changes after the cond
	if it's for writing
		- truncating, nothing else
		- otherwise, we want to get the file
	and leave the chan open if its a write, to send
	delayed writes.


	return openfile(f, mode);
 */
}

void
cclosefile(File *f, int fd)
{
/*
	closefile(f, fd);
	if it was an update, send stat and update f->sd
	release the chan
*/
}


int
ccreatefile(File **fp, char *elem, int mode, int perm)
{
return -1;
/*
	change locally
	send to server

	return createfile(fp, elem, mode, perm);
*/
}

long
cpreadfile(File *f, int fd, void *a, ulong count, uvlong offset)
{
return -1;
/*
	return cpreadfile(f, fd, a, count, offset);
*/
}

long
cpwritefile(File *f, int fd, void *a, ulong count, uvlong offset)
{
return -1;
/*
	send to server
	return pwritefile(f, fd, a, count, offset);
*/
}


int
cremovefile(File *f)
{
return -1;
/*	send to server

	return removefile(f);
*/
}

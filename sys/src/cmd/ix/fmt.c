#include <u.h>
#include <libc.h>
#include <fcall.h>

#include "fs.h"

static uint dumpsome(char*, char*, char*, long);
static void fdirconv(char*, char*, Dir*);
static char *qidtype(char*, uchar);

#define	QIDFMT	"(%.16llux %lud %s)"

static char* cname[CMAX] = 
{
	[CEQ] "==",
	[CGE] ">=",
	[CGT] "> ",
	[CLT] "< ",
	[CLE] "<=",
	[CNE] "!=",
};

int
fscallfmt(Fmt *fmt)
{
	Fscall *f;
	int type;
	char buf[512], tmp[200];
	char *p, *e, *s;
	Dir *d;
	Qid *q;

	e = buf+sizeof(buf);
	f = va_arg(fmt->args, Fscall*);
	type = f->type;
	switch(type){
	case Tcond:
		if(f->cond >= CMAX)
			s = "??";
		else
			s = cname[f->cond];
		p = seprint(buf, e, "Tcond %s", s);
		if(f->nstat > sizeof tmp)
			seprint(p, e, " stat(%d bytes)", f->nstat);
		else{
			d = (Dir*)tmp;
			convM2D(f->stat, f->nstat, d, (char*)(d+1));
			seprint(p, e, " stat ");
			fdirconv(p+6, e, d);
		}
		break;

	case Rcond:
		seprint(buf, e, "Rcond");
		break;
	case Tfid:
		seprint(buf, e, "Tfid  fid %ud cflags %d", f->fid, f->cflags);
		break;
	case Rfid:
		seprint(buf, e, "Rfid");
		break;
	case Tclone:
		seprint(buf, e, "Tclone  cflags %d", f->cflags);
		break;
	case Rclone:
		seprint(buf, e, "Rclone  newfid %d", f->newfid);
		break;
	case Tversion:	/* 100 */
		seprint(buf, e, "Tversion  msize %ud version '%s'", f->msize, f->version);
		break;
	case Rversion:
		seprint(buf, e, "Rversion  msize %ud version '%s'", f->msize, f->version);
		break;
	case Tauth:	/* 102 */
		seprint(buf, e, "Tauth  afid %d uname %s aname %s",
			f->afid, f->uname, f->aname);
		break;
	case Rauth:
		seprint(buf, e, "Rauth  qid " QIDFMT,
			f->aqid.path, f->aqid.vers, qidtype(tmp, f->aqid.type));
		break;
	case Tattach:	/* 104 */
		seprint(buf, e, "Tattach  afid %d uname %s aname %s",
			f->afid, f->uname, f->aname);
		break;
	case Rattach:
		seprint(buf, e, "Rattach  fid %d qid " QIDFMT,
			f->fid, f->qid.path, f->qid.vers, qidtype(tmp, f->qid.type));
		break;
	case Rerror:	/* 107; 106 (Terror) illegal */
		seprint(buf, e, "Rerror  ename %s", f->ename);
		break;
	case Twalk:	/* 110 */
		seprint(buf, e, "Twalk wname %s", f->wname);
		break;
	case Rwalk:
		q = &f->wqid;
		seprint(buf, e, "Rwalk wqid " QIDFMT,
					q->path, q->vers, qidtype(tmp, q->type));
		break;
	case Topen:	/* 112 */
		seprint(buf, e, "Topen mode %d", f->mode);
		break;
	case Ropen:
		seprint(buf, e, "Ropen qid " QIDFMT " iounit %ud ",
			f->qid.path, f->qid.vers, qidtype(tmp, f->qid.type), f->iounit);
		break;
	case Tcreate:	/* 114 */
		seprint(buf, e, "Tcreate name %s perm %M mode %d", f->name, (ulong)f->perm, f->mode);
		break;
	case Rcreate:
		seprint(buf, e, "Rcreate qid " QIDFMT " iounit %ud ",
			f->qid.path, f->qid.vers, qidtype(tmp, f->qid.type), f->iounit);
		break;
	case Tread:	/* 116 */
		seprint(buf, e, "Tread nmsg %d offset %lld count %ud",
			f->nmsg, f->offset, f->count);
		break;
	case Rread:
		p = seprint(buf, e, "Rread count %ud ", f->count);
			dumpsome(p, e, f->data, f->count);
		break;
	case Twrite:	/* 118 */
		p = seprint(buf, e, "Twrite offset %lld count %ud ",
			f->offset, f->count);
		dumpsome(p, e, f->data, f->count);
		break;
	case Rwrite:
		seprint(buf, e, "Rwrite count %ud", f->count);
		break;
	case Tclunk:	/* 120 */
		seprint(buf, e, "Tclunk");
		break;
	case Rclunk:
		seprint(buf, e, "Rclunk");
		break;
	case Tremove:	/* 122 */
		seprint(buf, e, "Tremove");
		break;
	case Rremove:
		seprint(buf, e, "Rremove");
		break;
	case Tstat:	/* 124 */
		seprint(buf, e, "Tstat");
		break;
	case Rstat:
		p = seprint(buf, e, "Rstat  ");
		if(f->nstat > sizeof tmp)
			seprint(p, e, " stat(%d bytes)", f->nstat);
		else{
			d = (Dir*)tmp;
			convM2D(f->stat, f->nstat, d, (char*)(d+1));
			seprint(p, e, " stat ");
			fdirconv(p+6, e, d);
		}
		break;
	case Twstat:	/* 126 */
		p = seprint(buf, e, "Twstat");
		if(f->nstat > sizeof tmp)
			seprint(p, e, " stat(%d bytes)", f->nstat);
		else{
			d = (Dir*)tmp;
			convM2D(f->stat, f->nstat, d, (char*)(d+1));
			seprint(p, e, " stat ");
			fdirconv(p+6, e, d);
		}
		break;
	case Rwstat:
		seprint(buf, e, "Rwstat");
		break;
	default:
		seprint(buf, e,  "unknown type %d", type);
	}
	return fmtstrcpy(fmt, buf);
}

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

	seprint(buf, e, "'%s' '%s' '%s' '%s' "
		"q " QIDFMT " m %#luo "
		"at %ld mt %ld l %lld "
		"t %d d %d",
			d->name, d->uid, d->gid, d->muid,
			d->qid.path, d->qid.vers, qidtype(tmp, d->qid.type), d->mode,
			d->atime, d->mtime, d->length,
			d->type, d->dev);
}

/*
 * dump out count (or DUMPL, if count is bigger) bytes from
 * buf to ans, as a string if they are all printable,
 * else as a series of hex bytes
 */
#define DUMPL 64

static uint
dumpsome(char *ans, char *e, char *buf, long count)
{
	int i, printable;
	char *p;

	if(buf == nil){
		seprint(ans, e, "<no data>");
		return strlen(ans);
	}
	printable = 1;
	if(count > DUMPL)
		count = DUMPL;
	for(i=0; i<count && printable; i++)
		if((buf[i]<32 && buf[i] !='\n' && buf[i] !='\t') || (uchar)buf[i]>127)
			printable = 0;
	p = ans;
	*p++ = '\'';
	if(printable){
		if(count > e-p-2)
			count = e-p-2;
		for(; count > 0; count--, p++, buf++)
			if(*buf == '\n' || *buf == '\t')
				*p = ' ';
			else
				*p = *buf;
	}else{
		if(2*count > e-p-2)
			count = (e-p-2)/2;
		for(i=0; i<count; i++){
			if(i>0 && i%4==0)
				*p++ = ' ';
			sprint(p, "%2.2ux", (uchar)buf[i]);
			p += 2;
		}
	}
	*p++ = '\'';
	*p = 0;
	return p - ans;
}

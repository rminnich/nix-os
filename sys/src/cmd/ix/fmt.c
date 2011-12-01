#include <u.h>
#include <libc.h>
#include <fcall.h>

#include "ix.h"



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
ixcallfmt(Fmt *fmt)
{
	IXcall *f;
	int type, i;
	char buf[512];
	char *p, *e, *s;

	e = buf+sizeof(buf);
	f = va_arg(fmt->args, IXcall*);
	type = f->type;
	switch(type){
	case IXTversion:
		seprint(buf, e, "Tversion  msize %ud version '%s'", f->msize, f->version);
		break;
	case IXRversion:
		seprint(buf, e, "Rversion  msize %ud version '%s'", f->msize, f->version);
		break;

	case IXTsession:
		seprint(buf, e, "Tsession  ssid %ud uname '%s' keep %d", f->ssid, f->uname, f->keep);
		break;
	case IXRsession:
		seprint(buf, e, "Rsession  ssid %ud afid %ud uname '%s'",
			f->ssid, f->afid, f->uname);
		break;

	case IXTsid:
		seprint(buf, e, "Tsid  ssid %ud", f->ssid);
		break;
	case IXRsid:
		seprint(buf, e, "Rsid");
		break;

	case IXTendsession:
		seprint(buf, e, "Tendsession");
		break;
	case IXRendsession:
		seprint(buf, e, "Rendsession");
		break;

	case IXTfid:
		seprint(buf, e, "Tfid  fid %ud", f->fid);
		break;
	case IXRfid:
		seprint(buf, e, "Rfid");
		break;

	case IXTattach:
		seprint(buf, e, "Tattach aname '%s'", f->aname);
		break;
	case IXRattach:
		seprint(buf, e, "Rattach  fid %d", f->fid);
		break;

	case IXRerror:
		seprint(buf, e, "Rerror ename '%s'", f->ename);
		break;

	case IXTclone:
		seprint(buf, e, "Tclone  cflags %#x", f->cflags);
		break;
	case IXRclone:
		seprint(buf, e, "Rclone  fid %d", f->fid);
		break;


	case IXTwalk:
		s = seprint(buf, e, "Twalk");
		for(i = 0; i < f->nwname; i++)
			s = seprint(s, e, " '%s'", f->wname[i]);
		break;
	case IXRwalk:
		seprint(buf, e, "Rwalk");
		break;

	case IXTopen:
		seprint(buf, e, "Topen mode %d", f->mode);
		break;
	case IXRopen:
		seprint(buf, e, "Ropen");
		break;

	case IXTcreate:
		seprint(buf, e, "Tcreate name %s perm %M mode %d", f->name, (ulong)f->perm, f->mode);
		break;
	case IXRcreate:
		seprint(buf, e, "Rcreate");
		break;

	case IXTread:
		seprint(buf, e, "Tread nmsg %d offset %lld count %ud",
			f->nmsg, f->offset, f->count);
		break;
	case IXRread:
		s = seprint(buf, e, "Rread count %ud ", f->count);
		dumpsome(s, e, f->data, f->count);
		break;

	case IXTwrite:
		p = seprint(buf, e, "Twrite offset %lld endoffset %lld count %ud ",
			f->offset, f->endoffset, f->count);
		dumpsome(p, e, f->data, f->count);
		break;
	case IXRwrite:
		seprint(buf, e, "Rwrite offset %lld count %ud", f->offset, f->count);
		break;

	case IXTclunk:
		seprint(buf, e, "Tclunk");
		break;
	case IXRclunk:
		seprint(buf, e, "Rclunk");
		break;
	case IXTclose:
		seprint(buf, e, "Tclose");
		break;
	case IXRclose:
		seprint(buf, e, "Rclose");
		break;
	case IXTremove:
		seprint(buf, e, "Tremove");
		break;
	case IXRremove:
		seprint(buf, e, "Rremove");
		break;

	case IXTattr:
		seprint(buf, e, "Tattr attr '%s'", f->attr);
		break;
	case IXRattr:
		p = seprint(buf, e, "Rattr value ");
		dumpsome(p, e, f->value, f->nvalue);
		break;

	case IXTwattr:
		p = seprint(buf, e, "Twattr attr '%s' value ", f->attr);
		dumpsome(p, e, f->value, f->nvalue);
		break;
	case IXRwattr:
		seprint(buf, e, "Rwattr");
		break;

	case IXTcond:
		if(f->op >= CMAX)
			s = "??";
		else
			s = cname[f->op];
		p = seprint(buf, e, "Tcond op %s", s);
		p = seprint(p, e, "attr '%s' value ", f->attr);
		dumpsome(p, e, f->value, f->nvalue);
		break;
	case IXRcond:
		seprint(buf, e, "Rcond");
		break;

	case IXTmove:
		seprint(buf, e, "Tmove dirfid %d newname '%s'", f->dirfid, f->newname);
		break;
	case IXRmove:
		seprint(buf, e, "Rmove");
		break;

	case IXTcopy:
		seprint(buf, e, "Tcopy nmsg %d offset %lld count %ud dstfid %ud dstoffset %lld",
			f->nmsg, f->offset, f->count, f->dstfid, f->dstoffset);
		break;
	case IXRcopy:
		seprint(buf, e, "Rcopy count %ud", f->count);
		break;

	case IXTflush:
		seprint(buf, e, "Tflush");
		break;
	case IXRflush:
		seprint(buf, e, "Rflush");
		break;

	default:
		seprint(buf, e,  "unknown type %d", type);
	}
	return fmtstrcpy(fmt, buf);
}

/*
 * dump out count (or DUMPL, if count is bigger) bytes from
 * buf to ans, as a string if they are all printable,
 * else as a series of hex bytes
 */
#define DUMPL 64

uint
dumpsome(char *ans, char *e, void *b, long count)
{
	int i, printable;
	char *p;
	char *buf;

	buf = b;
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

#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <bio.h>


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
		"(%llux %lud %s) %#luo l %lld", d->name,
			d->uid,
			d->qid.path, d->qid.vers, qidtype(tmp, d->qid.type), d->mode,
			d->length);
}

static int
shortdirfmt(Fmt *fmt)
{
	char buf[160];

	fdirconv(buf, buf+sizeof buf, va_arg(fmt->args, Dir*));
	return fmtstrcpy(fmt, buf);
}

static Dir*
readdir(Biobuf *bin, char *p)
{
	static uchar buf[DIRMAX];
	ulong sz;
	Dir *d;

	if(Bread(bin, buf, BIT16SZ) != BIT16SZ)
		sysfatal("%s: eof", p);
	sz = GBIT16(buf);
	if(BIT16SZ + sz > sizeof buf)
		sysfatal("%s: dir too long", p);
	if(Bread(bin, buf + BIT16SZ, sz) != sz)
		sysfatal("%s: read failed", p);
	d = malloc(sizeof *d + sz);
	if(convM2D(buf, sizeof buf, d, (char*)(d+1)) <= 0)
		sysfatal("%s: convM2D failed", p);
	return d;
}

static int
loaddir(Biobuf *bin, char *parent)
{
	Dir *d, *sd;
	int nchild;
	char *p, *path;
	char tmp[16];

	p = "/";
	if(parent != nil)
		p = parent;
	d = readdir(bin, p);
	if(d == nil)
		return -1;
	sd = readdir(bin, p);
	if(sd == nil){
		free(d);
		return -1;
	}
	if(parent == nil)
		path = ".";
	else
		path = smprint("%s/%s", parent, d->name);
	if(sd->qid.path == ~0ULL && sd->qid.vers == ~0 && sd->qid.type == 0xFF)
		print("'%s' sq nulldir %D\n", path, d);
	else
		print("'%s' sq (%llux %lud %s) %D\n",
			path, sd->qid.path, sd->qid.vers,
			qidtype(tmp, sd->qid.type), d);
	if(d->qid.type&QTDIR){
		nchild = d->length;
		d->length = 0;
		while(nchild-- > 0)
			if(loaddir(bin, path) < 0)
				break;
	}
	free(d);
	free(sd);
	return 0;
}

static void
catixd(char *fname)
{
	Biobuf *bin;

	bin = Bopen(fname, OREAD);
	if(bin == nil)
		sysfatal("%s: %r", fname);
	loaddir(bin, nil);
	Bterm(bin);
}

static void
usage(void)
{
	fprint(2, "usage: %s [file...]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int i;

	ARGBEGIN{
	default:
		usage();
	}ARGEND;
	fmtinstall('M', dirmodefmt);
	fmtinstall('D', shortdirfmt);
	if(argc == 0)
		catixd("/fd/0");
	else
		for(i = 0; i < argc; i++){
			print("%s:\n", argv[i]);
			catixd(argv[i]);
		}
	exits(0);
}


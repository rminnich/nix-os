#ifdef PLAN9PORT
#include <u.h>
#include <signal.h>
#endif
#include "stdinc.h"
#include <bio.h>
#include "dat.h"
#include "fns.h"

#include "whack.h"
#define HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))
enum {
	GiB = 1ULL << 32
};

struct map
{
	unsigned char score[VtScoreSize];
	void *data;
	int len;
	uchar blocktype;
};

int debug;
int nofork;
int mainstacksize = 256*1024;

static struct map *maps = nil;
static int hashb, maxmap;
static u8int *mmventidata;

VtSrv *ventisrv;

static void	ventiserver(void*);

unsigned long log2(unsigned long x)
{
        unsigned long i = 1ULL << (sizeof(x)* 8 - 1ULL);
        unsigned long pow = sizeof(x) * 8 - 1ULL;

        if (! x) {
                return -1;
        }
        for(; i > x; i >>= 1, pow--)
                ;

        return pow;
}
void
usage(void)
{
	fprint(2, "usage: venti [-Ldrsw] [-a ventiaddr] [-c config] "
"[-h httpaddr] [-m %%mem] [-B blockcachesize] [-C cachesize] [-I icachesize] "
"[-W webroot]\n");
	threadexitsall("usage");
}

void
mminit(char *file)
{
	Dir *d;
	uintptr va;
	void *p, *np;
	int hashsize; /* make it a power of two -- see why later */

	d = dirstat(file);
	if (! d)
		sysfatal("Can't stat %s: %r", file);

	/* allocate: size for the file, 1/32 that size for the map, and 
	 * start it at the 1 GB boundary, please. 
	 */
	/* get top of heap */
	p = segbrk(0, 0);
	va = (uintptr)p;
	/* no non-nix systems we just usr sbrk and only have little pages */
	hashsize = d->length/32;
	maxmap = log2(hashsize / sizeof(*maps));
	hashb = log2(maxmap);
	if (va == (uintptr)-1) {
		p = sbrk(0);
		va = (uintptr)p;
		maps = (void *)va;
		va += hashsize;
		mmventidata = (void *)va;
		va += d->length;
		va = ROUNDUP((va), 4096);
		if (brk((void *)va) < 0)
			sysfatal("brk to %#p failed\n", (void *)va);
	} else {
		va = ROUNDUP((va), 1ULL*GiB);
		maps = (void *)va;
		va += hashsize;
		mmventidata = (void *)va;
		va += d->length;
		va = ROUNDUP((va), 1ULL*GiB);
		segbrk(0, (void *)va);
	}
	fprint(2, "p is %#p\n", p);

	fprint(2, "File size %lld, hashsize %d, maps %#p, data %#p\n", d->length, 
		hashsize, maps, mmventidata);
	/* morecore */
	np=(void*)va;
	segbrk(p, np);

	/* read in the file here when ready */
}

struct map *findscore(u8int *score)
{
	int ix;
	ix = hashbits(score, hashb);
fprint(2, "find for %V is %d, maps[].data %p\n", score, ix, maps[ix].data);
	while (maps[ix].data) {
		fprint(2, "Check: %d, %V\n", ix, maps[ix].score);
fprint(2, "scorecmp(%V,%V, %d\n", maps[ix].score, score,scorecmp(maps[ix].score, score) );
		if (scorecmp(maps[ix].score, score) == 0)
			return &maps[ix];
 		ix++;
	}
	return nil;
}

int
putscore(Packet *p, u8int *score, uchar blocktype)
{
	int ix, initial;
	packetsha1(p, score);
	initial = ix = hashbits(score, hashb);
	fprint(2, "putscore: ix %d, V %V, maps[].data %p\n", ix, score, maps[ix].data);
	while (maps[ix].data) {
		ix++;
		if (ix > maxmap)
			ix = 0;
		if (ix == initial)
			sysfatal("OOPS -- no more map slots");
	}
	maps[ix].data = mmventidata;
fprint(2, "set map[%d] to %p\n", ix, mmventidata);
	maps[ix].len = packetsize(p);
	scorecp(maps[ix].score, score);
	packetconsume(p, mmventidata, packetsize(p));
	maps[ix].blocktype = blocktype;
	mmventidata += maps[ix].len;
fprint(2, "mmventidata now %p\n", maps[ix].len);
	return maps[ix].len;
}

void
threadmain(int argc, char *argv[])
{
	char *haddr, *vaddr, *webroot, *file;

	traceinit();
	threadsetname("main");
	vaddr = nil;
	haddr = nil;
	webroot = nil;
	ARGBEGIN{
	case 'a':
		vaddr = EARGF(usage());
		break;
	case 'D':
		settrace(EARGF(usage()));
		break;
	case 'd':
		debug = 1;
		nofork = 1;
		break;
	case 'h':
		haddr = EARGF(usage());
		break;
	case 'L':
		ventilogging = 1;
		break;
	case 'r':
		readonly = 1;
		break;
	case 's':
		nofork = 1;
		break;
	case 'W':
		webroot = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	if(argc < 1)
		usage();

	file = argv[0];

	if(!nofork)
		rfork(RFNOTEG);

#ifdef PLAN9PORT
	{
		/* sigh - needed to avoid signals when writing to hungup networks */
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sa, nil);
	}
#endif

	ventifmtinstall();
	trace(TraceQuiet, "venti started");
	fprint(2, "%T venti: ");

	statsinit();
	mminit(file);

	/*
	 * default other configuration-file parameters
	 */
	if(vaddr == nil)
		vaddr = "tcp!*!venti";

	if(haddr){
		fprint(2, "httpd %s...", haddr);
		if(httpdinit(haddr, webroot) < 0)
			fprint(2, "warning: can't start http server: %r");
	}
	fprint(2, "init...");


	fprint(2, "announce %s...", vaddr);
	ventisrv = vtlisten(vaddr);
	if(ventisrv == nil)
		sysfatal("can't announce %s: %r", vaddr);

	fprint(2, "serving.\n");
	if(nofork)
		ventiserver(nil);
	else
		vtproc(ventiserver, nil);

	threadexits(nil);
}

static void
vtrerror(VtReq *r, char *error)
{
	r->rx.msgtype = VtRerror;
	r->rx.error = estrdup(error);
}

static void
ventiserver(void *v)
{
	Packet *p;
	VtReq *r;
	char err[ERRMAX];
	uint ms;
	int ok;
	struct map *m;

	USED(v);
	threadsetname("ventiserver");
	trace(TraceWork, "start");
	while((r = vtgetreq(ventisrv)) != nil){
		trace(TraceWork, "finish");
		trace(TraceWork, "start request %F", &r->tx);
		trace(TraceRpc, "<- %F", &r->tx);
		r->rx.msgtype = r->tx.msgtype+1;
		addstat(StatRpcTotal, 1);
		if(0) print("req (arenas[0]=%p sects[0]=%p) %F\n",
			mainindex->arenas[0], mainindex->sects[0], &r->tx);
		switch(r->tx.msgtype){
		default:
			vtrerror(r, "unknown request");
			break;
		case VtTread:
			ms = msec();
			m = findscore(r->tx.score);
fprint(2, "findscore says %p\n", m);
			if (m) {
fprint(2, "Found the block\n");
				r->rx.data = packetalloc();
				packetappend(r->rx.data, m->data, m->len);
				r->rx.blocktype = m->blocktype;
			} else {
				r->rx.data = nil;
			}
			ms = msec() - ms;
			addstat2(StatRpcRead, 1, StatRpcReadTime, ms);
			if(r->rx.data == nil){
				addstat(StatRpcReadFail, 1);
				rerrstr(err, sizeof err);
				vtrerror(r, err);
			}else{
				addstat(StatRpcReadBytes, packetsize(r->rx.data));
				addstat(StatRpcReadOk, 1);
				addstat2(StatRpcReadCached, 1, StatRpcReadCachedTime, ms);
				r->rx.msgtype = VtRread;
				r->rx.error = nil;
			}
			break;
		case VtTwrite:
			if(readonly){
				vtrerror(r, "read only");
				break;
			}
			p = r->tx.data;
			r->tx.data = nil;
			addstat(StatRpcWriteBytes, packetsize(p));
			ms = msec();
			/* todo: check for overflow of file */
			ok = putscore(p, r->rx.score, r->tx.blocktype);
			ms = msec() - ms;
			addstat2(StatRpcWrite, 1, StatRpcWriteTime, ms);

			if(ok < 0){
				addstat(StatRpcWriteFail, 1);
				rerrstr(err, sizeof err);
				vtrerror(r, err);
			} else {
				r->rx.msgtype = VtRwrite;
				r->rx.error = nil;
			}
			break;
		case VtTsync:
			/* nonsense. Write synchronously. For now. Later, have a helper thread and VtTsync will just write a Fence to it and wait for it to come back. */
			break;
		}
		trace(TraceRpc, "-> %F", &r->rx);
		vtrespond(r);
		trace(TraceWork, "start");
	}
	threadexitsall(0);
}

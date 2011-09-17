/*
 * Gather/scatter messages with protocol headers
 */
#include <u.h>
#include <libc.h>
#include <thread.h>	/* chancreate */
#include <error.h>
#include <fcall.h>
#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "dbg.h"

void
ioreset(Io *io)
{
	io->rp = io->bp;
	io->wp = io->bp;
}

static int
hdrlen(Msg *m)
{
	if(m->hdr == nil)
		return 0;
	return (m->hbuf + sizeof m->hbuf) - m->hdr;
}

Channel*
echancreate(int elsz, int n)
{
	Channel *c;

	c = chancreate(elsz, n);
	if(c == nil)
		sysfatal("chancreate");
	setmalloctag(c, getcallerpc(&elsz));
	return c;
}

long
msglen(Msg *m)
{
	Io *io;
	long tot;
	int i;

	tot = hdrlen(m);
	for(i = 0; i < nelem(m->io); i++){
		io = &m->io[i];
		if(io->bp == nil)
			break;
		tot += IOLEN(io);
	}
	return tot;
}

void*
msgpushhdr(Msg *m, long sz)
{
	if(m->hdr == nil)
		m->hdr = m->hbuf + sizeof m->hbuf;
	if(m->hdr - m->hbuf < sz){
		werrstr("no room for headers");
		return nil;
	}
	m->hdr -= sz;
	return m->hdr;
}

void*
msgpophdr(Msg *m, long sz)
{
	void *p;

	if(m->hdr != nil){
		if(hdrlen(m) < sz){
			werrstr("short header");
			return nil;
		}
		p = m->hbuf;
		m->hdr += sz;
		return p;
	}
	/*
	 * no headers in hbuf, probably they were read along with
	 * the body, take them from there.
	 */
	if(m->io[0].rp  != nil && IOLEN(&m->io[0]) >= sz){
		p = m->io[0].rp;
		m->io[0].rp += sz;
		return p;
	}
	werrstr("short header");
	return nil;
}

int
msgput(Msg *m, void *p, long sz)
{
	int i;
	Io *io;

	for(i = 0; i < nelem(m->io); i++){
		io = &m->io[i];
		if(io->bp == nil)
			break;
	}
	if(i == nelem(m->io))
		sysfatal("msgputtd: msg full");
	m->io[i].bp = p;
	m->io[i].rp = p;
	m->io[i].wp = m->io[i].rp + sz;
	m->io[i].ep = m->io[i].wp;
	return i;
}

int
readmsg(int fd, Msg *m)
{
	uchar hdr[BIT16SZ];
	long nhdr, nr;
	ulong sz;

	werrstr("eof");
	nhdr = readn(fd, hdr, sizeof hdr);
	if(nhdr < 0){
		dmprint("readmsg: %r\n");
		return -1;
	}
	if(nhdr == 0){
		dmprint("readmsg: eof\n");
		return 0;
	}
	if(nhdr < sizeof hdr){
		werrstr("short header %r");
		return -1;
	}
	sz = GBIT16(hdr);
	if(sz > IOCAP(&m->io[0])){
		/*
		 * Message too long.
		 * We could read it and skip it, but let's fail
		 * so the user knows.
		 */
		fprint(2, "readmsg: message too long (%uld > %d)\n",
			sz, Msgsz);
		werrstr("message too long");
		return -1;
	}
	nr = readn(fd, m->io->wp, sz);
	dmprint("readmsg: %ld+%ld bytes sz %ld\n", nhdr, nr, sz);
	if(nr != sz){
		werrstr("short msg data");
		return -1;
	}
	m->io->wp += nr;
	return nr;
}

long
writemsg(int fd, Msg *m)
{
	Io *io;
	long len, hlen, iol;
	uchar *hdr;
	int i;

	len = msglen(m);
	if(len > 0xFFFF){
		werrstr("message too long");
		return -1;
	}
	hdr = msgpushhdr(m, BIT16SZ);
	if(hdr == nil)
		return -1;
	PBIT16(hdr, len);
	hlen = hdrlen(m);
	if(write(fd, hdr, hlen) != hlen)
		goto Fail;
	for(i = 0; i < nelem(m->io); i++){
		io = &m->io[i];
		if(io->bp == nil)
			break;
		iol = IOLEN(io);
		if(write(fd, io->rp, iol) != iol)
			goto Fail;
		dmprint("writemsg: %ld bytes\n", iol);
	}
	msgpophdr(m, BIT16SZ);
	return len;
Fail:
	msgpophdr(m, BIT16SZ);
	return -1;
}

void
freemsg(Msg *m)
{
	Io *io;
	int i;

	if(m == nil)
		return;
	if(m->free != nil){
		dPprint("freemsg pc %#p\n", getcallerpc(&m));
		m->free(m);
		return;
	}
	for(i = 0; i < nelem(m->io); i++){
		io = &m->io[i];
		if(io->bp == nil)
			break;
		free(io->bp);
	}
	free(m);
}

void
dumpmsg(Msg *m)
{
	print("msg %#p hdr %#p ehdr %#p bp %#p rp %#p wp %#p ep %#p\n",
		m, m->hdr, m->hbuf + sizeof m->hbuf,
		m->io->bp, m->io->rp, m->io->wp, m->io->ep);
}

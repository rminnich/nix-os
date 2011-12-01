/*
 * Tcp-like transports, wrapped in a Con structure
 * to send and receive messages.
 *
 * Termination:
 *	- rproc gets and error or eof:
 *	1. rproc sends nil to client (e.g., cmux) and exits
 *	2. client is noted and writes nil to wproc
 *		it may look at s->err for the error condition.
 *	3. wproc intrs. rproc (harmless).
 *
 *	- client terminates:
 *	1. client writes nil to wproc
 *	2. wproc intrs. rproc and terminates
 *	3. rproc is noted and sends nil to client
 *
 *	Concurrent termination works ok, because rproc
 *	only sends nil once to the client.
 */
#include <u.h>
#include <libc.h>
#include <thread.h>

#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "tses.h"
#include "dbg.h"

static void
abortconn(Con *s)
{
	if(s->cfd < 0)
		return;
	dsprint("abortconn[%p]: hanging up\n", s);
//	write(s->cfd, "hangup", 6);
	close(s->cfd);
	close(s->dfd);
	s->cfd = s->dfd = -1;
}

static void
freeses(Con *s)
{
	dsprint("free session %p\n", s);
	free(s->addr);
	free(s->err);
	if(s->rc != nil)
		chanfree(s->rc);
	if(s->wc != nil)
		chanfree(s->wc);
	if(s->ec != nil)
		chanfree(s->ec);
	freepool(s->pool);
	freepool(s->spool);
	memset(s, 6, sizeof *s);	/* poison */
	free(s);
}

static void
putses(Con *s)
{
	if(decref(s) == 0)
		freeses(s);
}

static void
syserr(Con *s)
{
	/*
	 * BUG: could leak if concurrent errors.
	 */
	if(s->err == nil)
		s->err = smprint("%r");
}

static void
rproc(void *a)
{
	Con *s;
	Msg *m, *sm;
	long nr;

	s = a;
	threadsetname("rproc[%p]", a);
	dpprint("rproc[%p]: started\n", a);
	s->rtid = threadid();
	m = newmsg(s->pool);
	sm = newmsg(s->spool);
	for(;;){
		if(m == nil)
			sysfatal("rproc: newmsg");
		nr = readmsg(s->dfd, m);
		if(nr < 0){
			syserr(s);
			dmprint("rproc[%p]: readmsg: %r\n", a);
			break;
		}
		if(nr == 0){
			dmprint("rproc[%p]: eof\n", a);
			break;
		}
		if(IOLEN(m->io) <= IOCAP(sm->io)){
			memmove(sm->io->wp, m->io->rp, IOLEN(m->io));
			sm->io->wp += IOLEN(m->io);
			m->hdr = nil;
			ioreset(m->io);
			sendp(s->rc, sm);
			sm = newmsg(s->spool);
		}else{
			sendp(s->rc, m);
			m = newmsg(s->pool);
		}
	}
	sendp(s->rc, nil);
	sendp(s->ec, s->err);
	s->err = nil;
	freemsg(m);
	freemsg(sm);
	putses(s);
	dpprint("rproc[%p]: exiting\n", a);
}

static void
wproc(void *a)
{
	Con *s;
	Msg *m;

	s = a;
	threadsetname("wproc[%p]", a);
	dpprint("wproc[%p]: started\n", a);
	for(;;){
		m = recvp(s->wc);
		if(m == nil){
			dmprint("wproc[%p]: got nil msg\n", a);
			abortconn(s);
			break;
		}
		if(writemsg(s->dfd, m) < 0){
			syserr(s);
			dmprint("wproc[%p]: snd len %uld: %r\n", a, msglen(m));
			freemsg(m);
			abortconn(s);
			break;
		}
		dmprint("wproc[%p]: sent %uld bytes\n", a, msglen(m));
		freemsg(m);
	}
	threadint(s->rtid);
	while((m = nbrecvp(s->wc)) != nil)
		freemsg(m);
	putses(s);
	dpprint("wproc[%p]: exiting\n", a);
}

void
startses(Con *s, Mpool *mp, Mpool *smp)
{
	s->pool = mp;
	s->spool = smp;
	s->ec = echancreate(sizeof(char*), 0);
	s->rc = echancreate(sizeof(Msg*), 0);
	incref(s);
	proccreate(rproc, s, Stack);
	s->wc = echancreate(sizeof(Msg*), 0);
	incref(s);
	proccreate(wproc, s, Stack);
}

static void
ssrvproc(void *a)
{
	enum{New, End};

	Ssrv *ss;
	Con *sl, **nl, *s;
	Alt alts[] = {
		[New] {nil, &s, CHANRCV},
		[End] {nil, &s, CHANRCV},
		{nil, nil, CHANEND},
	};

	ss = a;
	alts[New].c = ss->listenc;
	alts[End].c = ss->endc;
	sl = nil;
	threadsetname("ssrvproc %p", a);
	dpprint("ssrvproc[%p]: started\n", a);
	for(;;){
		switch(alt(alts)){
		case New:
			dsprint("session[%p]: new %s\n", s, s->addr);
			s->next = sl;
			sl = s;
			incref(s);
			sendp(ss->newc, s);
			break;

		case End:
			dsprint("session[%p]: done %s\n", s, s->addr);
			for(nl = &sl; *nl != nil; nl = &(*nl)->next)
				if(s == *nl)
					break;
			if(*nl == nil)
				sysfatal("ssrvproc[%p]: no session", a);
			*nl = s->next;
			putses(s);
			break;

		default:
			sysfatal("alt");
		}
	}
}

static void
listenproc(void *a)
{
	Ssrv *ss;
	Con *s;
	char ldir[40];

	ss = a;
	threadsetname("listenproc %p", a);
	dpprint("listenproc[%p]: started\n", a);
	for(;;){
		s = mallocz(sizeof *s, 1);
		s->addr = strdup(ss->addr);	/* not needed in srv */
		s->cfd = s->dfd = -1;
		s->ref = 1;
		s->cfd = listen(ss->adir, ldir);
		if(s->cfd < 0){
			dprint("listen: %r");
			goto Fail;
		}
		s->dfd = accept(s->cfd, ldir);
		if(s->dfd < 0){
			dprint("accept: %r");
			goto Fail;
		}
		sendp(ss->listenc, s);
		continue;
	Fail:
		if(s->cfd >= 0)
			close(s->cfd);
		if(s->dfd >= 0)
			close(s->dfd);
		s->cfd = s->dfd = -1;
		freeses(s);
		fprint(2, "%s: listenproc: failed: %r\n", argv0);
		sleep(1000);	/* avoid retrying too fast */
	}
}

Ssrv*
newsrv(char *addr)
{
	Ssrv *ss;

	ss = mallocz(sizeof *ss, 1);
	dprint("announce %s\n", addr);
	ss->afd = announce(addr, ss->adir);
	if(ss->afd < 0){
		free(ss);
		return nil;
	}
	ss->addr = strdup(addr);
	ss->listenc = echancreate(sizeof(Con*), 0);
	ss->newc = echancreate(sizeof(Con*), 0);
	ss->endc = echancreate(sizeof(Con*), 0);
	proccreate(listenproc, ss, Stack);
	threadcreate(ssrvproc, ss, Stack);
	return ss;
}

Con*
dialsrv(char *addr)
{
	Con *s;

	s = mallocz(sizeof *s, 1);
	dprint("dial %s\n", addr);
	s->dfd = dial(addr, nil, nil, &s->cfd);
	if(s->dfd < 0){
		free(s);
		return nil;
	}
	s->ref = 1;
	s->addr = strdup(addr);
	dsprint("session[%p]: new %s\n", s, s->addr);
	return s;
}

int
closeses(Con *s)
{
	Msg *m;
	char *e;

	sendp(s->wc, nil);
	while((m = recvp(s->rc)) != nil)
		freemsg(m);
	e = recvp(s->ec);
	if(e != nil){
		werrstr(e);
		free(e);
		return -1;
	}
	return 0;
}


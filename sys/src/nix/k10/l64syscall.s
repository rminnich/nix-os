#include "mem.h"
#include "amd64l.h"

MODE $64

/*
 */
TEXT touser(SB), 1, $-4
	CLI
	SWAPGS
	MOVQ	$SSEL(SiUDS, SsRPL3), AX
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS

	MOVQ	$(UTZERO+0x28), CX		/* ip */
	MOVQ	$If, R11			/* flags */

	MOVQ	RARG, SP			/* sp */

	BYTE $0x48; SYSRET			/* SYSRETQ */

/*
 */
TEXT syscallentry(SB), 1, $-4
	SWAPGS
	/* above all, do no harm. Basically, push enough here to test the mode, then jump to linux if we're in that mode. 
	 * sadly, this adds a bit of overhead, but there's not much we can do. We also want to make this decision very early
	 * in the game, it just gets harder and harder to clean up the user stack pointer the longer we delay it. 
	 */
	PUSHQ	R15
	PUSHQ	R14
	PUSHQ	AX
	/*
	 * RMACH: R15; RUSER: r14.
	 * First thing in here trashes them both; need to be saved for Linux.
	 */
	BYTE $0x65; MOVQ 0, RMACH		/* m-> (MOVQ GS:0x0, R15) */
	MOVQ	16(RMACH), RUSER		/* m->proc */
	/* are we a linux proc?
	 * Note conservative behavior on AX here -- maybe not needed
	 */
	MOVL	0xdac(R14),AX	
	CMPL	AX,$0x0
	JEQ		plan9call
	JMP		dolinuxsyscall
plan9call: 
	ADDQ	$(3*8), SP
	MOVQ	SP, R13
	MOVQ	16(RUSER), SP			/* m->proc->kstack */
	ADDQ	$KSTACK, SP
	PUSHQ	$SSEL(SiUDS, SsRPL3)		/* old stack segment */
	PUSHQ	R13				/* old sp */
	PUSHQ	R11				/* old flags */
	PUSHQ	$SSEL(SiUCS, SsRPL3)		/* old code segment */
	PUSHQ	CX				/* old ip */

	/* registers saved at this point: R15, R14, R13, AX (user stack); SS, R13, R11, CS, CX (kernel stack) */
	SUBQ	$(18*8), SP			/* unsaved registers */

	MOVW	$SSEL(SiUDS, SsRPL3), (15*8+0)(SP)
	MOVW	ES, (15*8+2)(SP)
	MOVW	FS, (15*8+4)(SP)
	MOVW	GS, (15*8+6)(SP)

	PUSHQ	SP				/* Ureg* */
	PUSHQ	RARG				/* system call number */
	CALL	syscall(SB)

TEXT syscallreturn(SB), 1, $-4
	MOVQ	16(SP), AX			/* Ureg.ax */
	MOVQ	(16+6*8)(SP), BP		/* Ureg.bp */
_syscallreturn:
	ADDQ	$(17*8), SP			/* registers + arguments */

	CLI
	SWAPGS
	MOVW	0(SP), DS
	MOVW	2(SP), ES
	MOVW	4(SP), FS
	MOVW	6(SP), GS

	MOVQ	24(SP), CX			/* ip */
	MOVQ	40(SP), R11			/* flags */

	MOVQ	48(SP), SP			/* sp */

	BYTE $0x48; SYSRET			/* SYSRETQ */

TEXT sysrforkret(SB), 1, $-4
	MOVQ	$0, AX
	JMP	_syscallreturn

/* just push it all on the linux stack. It's what the linux code *should* have 
 * done
 */
dolinuxsyscall: 
	/* restore AX (system call #)*/
	POPQ	AX
	/* don't bother saving it, it gets trashed on returned */
	/* stack is now R15 and R14 at TOS */
	/* for linux support */
	/* arg order is di, si, dx, r10, r8, r9 */
	PUSHQ	R13
	PUSHQ	R12
	PUSHQ	R11
	PUSHQ	R10
	PUSHQ	R9
	PUSHQ	R8
	PUSHQ	BP
	PUSHQ	DI
	PUSHQ	SI
	PUSHQ	DX
	PUSHQ	CX
	PUSHQ	BX
	MOVQ	SP, R13
	MOVQ	16(RUSER), SP			/* m->proc->kstack */
	ADDQ	$KSTACK, SP
	PUSHQ	$SSEL(SiUDS, SsRPL3)		/* old stack segment */
	/* note the "user SP is not right at this point. Need to readjust it. */
	PUSHQ	R13				/* old sp */
	PUSHQ	R11				/* old flags */
	PUSHQ	$SSEL(SiUCS, SsRPL3)		/* old code segment */
	PUSHQ	CX				/* old ip */

	/* USER SP is now in R13 */
	/* we need to get a few things from it ... */
	SUBQ	$(18*8), SP			/* unsaved registers */
	MOVQ	AX, (0*8)(SP)
	MOVQ	DI,(5*8)(SP)
	MOVQ	SI, (4*8)(SP)
	MOVQ	DX, (3*8)(SP)
	MOVQ	R10, (9*8)(SP)
	MOVQ	R8, (7*8)(SP)
	MOVQ	R9, (8*8)(SP)
	MOVL	$FSbase, RARG
	CALL 	rdmsr(SB)
	MOVL	AX, (10*8+0)(SP) // use the unused R11 slot
	MOVW	DS, (15*8+0)(SP)
	MOVW	ES, (15*8+2)(SP)
	MOVW	FS, (15*8+4)(SP)
	MOVW	GS, (15*8+6)(SP)

	PUSHQ	SP				/* Ureg* */
	PUSHQ	AX				/* system call number */
	CALL	linuxsyscall(SB)

TEXT linuxsyscallreturn(SB), 1, $-4
	/* TODO: make sure syscall return is in the right place. */
	MOVQ	16(SP), AX			/* Ureg.ax */
	MOVQ	(16+6*8)(SP), BP		/* Ureg.bp */
_linuxsyscallreturn:
	MOVL	(16+10*8)(SP), R11		/* R11 for wrmsr  below */
	ADDQ	$(17*8), SP			/* registers + arguments */
	CLI
	SWAPGS
	MOVW	0(SP), DS
	MOVW	2(SP), ES
	MOVW	4(SP), FS
	MOVW	6(SP), GS
	PUSHQ AX
	MOVL	$FSbase, RARG
	XORQ	CX, CX
	MOVL	R11, CX
	PUSHQ	CX
	/* dummy */
	PUSHQ	RARG
	CALL wrmsr(SB)
	POPQ	AX
	POPQ	AX
	POPQ	AX

	MOVQ	24(SP), CX			/* ip */
	MOVQ	40(SP), R11			/* flags */

	MOVQ	48(SP), SP			/* sp */

	/* now we have to pop. */
	POPQ	BX
	POPQ	CX
	POPQ	DX
	POPQ	SI
	POPQ	DI
	POPQ	BP
	POPQ	R8
	POPQ	R9
	POPQ	R10
	POPQ	R11
	POPQ	R12
	POPQ	R13
	POPQ	R14
	POPQ	R15

	BYTE $0x48; SYSRET			/* SYSRETQ */

TEXT linuxsysrforkret(SB), 1, $-4
	MOVQ	$0, AX
	JMP	_linuxsyscallreturn

#include "amd64l.h"

MODE $64

/*
 * Port I/O.
 */
TEXT inb(SB), 1, $-4
	MOVL	RARG, DX			/* MOVL	port+0(FP), DX */
	XORL	AX, AX
	INB
	RET

TEXT insb(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), DI
	MOVL	count+16(FP), CX
	CLD
	REP;	INSB
	RET

TEXT ins(SB), 1, $-4
	MOVL	RARG, DX
	XORL	AX, AX
	INW
	RET

TEXT inss(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), DI
	MOVL	count+16(FP), CX
	CLD
	REP;	INSW
	RET

TEXT inl(SB), 1, $-4
	MOVL	RARG, DX
	INL
	RET

TEXT insl(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), DI
	MOVL	count+16(FP), CX
	CLD
	REP; INSL
	RET

TEXT outb(SB), 1, $-1
	MOVL	RARG, DX			/* MOVL	port+0(FP), DX */
	MOVL	byte+8(FP), AX
	OUTB
	RET

TEXT outsb(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), SI
	MOVL	count+16(FP), CX
	CLD
	REP; OUTSB
	RET

TEXT outs(SB), 1, $-4
	MOVL	RARG, DX
	MOVL	short+8(FP), AX
	OUTW
	RET

TEXT outss(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), SI
	MOVL	count+16(FP), CX
	CLD
	REP; OUTSW
	RET

TEXT outl(SB), 1, $-4
	MOVL	RARG, DX
	MOVL	long+8(FP), AX
	OUTL
	RET

TEXT outsl(SB), 1, $-4
	MOVL	RARG, DX
	MOVQ	address+8(FP), SI
	MOVL	count+16(FP), CX
	CLD
	REP; OUTSL
	RET

/*
 * Load/store segment descriptor tables:
 *	GDT - global descriptor table
 *	IDT - interrupt descriptor table
 *	TR - task register
 * GDTR and LDTR take an m16:m64 argument,
 * so shuffle the stack arguments to
 * get it in the right format.
 */
TEXT gdtget(SB), 1, $-4
	MOVL	GDTR, (RARG)			/* Note: 10 bytes returned */
	RET

TEXT gdtput(SB), 1, $-4
	SHLQ	$48, RARG
	MOVQ	RARG, m16+0(FP)
	LEAQ	m16+6(FP), RARG

	MOVL	(RARG), GDTR

	XORQ	AX, AX
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS
	MOVW	AX, SS

	POPQ	AX
	MOVWQZX	cs+16(FP), BX
	PUSHQ	BX
	PUSHQ	AX
	RETFQ

TEXT idtput(SB), 1, $-4
	SHLQ	$48, RARG
	MOVQ	RARG, m16+0(FP)
	LEAQ	m16+6(FP), RARG
	MOVL	(RARG), IDTR
	RET

TEXT trput(SB), 1, $-4
	MOVW	RARG, TASK
	RET

/*
 * Read/write various system registers.
 */
TEXT cr0get(SB), 1, $-4				/* CR0 - processor control */
	MOVQ	CR0, AX
	RET

TEXT cr0put(SB), 1, $-4
	MOVQ	RARG, AX
	MOVQ	AX, CR0
	RET

TEXT cr2get(SB), 1, $-4				/* CR2 - #PF virtual address */
	MOVQ	CR2, AX
	RET

TEXT cr3get(SB), 1, $-4				/* CR3 - pml4 base */
	MOVQ	CR3, AX
	RET

TEXT cr3put(SB), 1, $-4
	MOVQ	RARG, AX
	MOVQ	AX, CR3
	RET

TEXT cr4get(SB), 1, $-4				/* CR4 - processor extensions */
	MOVQ	CR4, AX
	RET

TEXT cr4put(SB), 1, $-4
	MOVQ	RARG, AX
	MOVQ	AX, CR4
	RET

TEXT rdtsc(SB), 1, $-4				/* time stamp counter */
	RDTSC
	XCHGL	DX, AX				/* swap lo/hi, zero-extend */
	SHLQ	$32, AX				/* hi<<32 */
	ORQ	DX, AX				/* (hi<<32)|lo */
	RET

TEXT rdmsr(SB), 1, $-4				/* model-specific register */
	MOVL	RARG, CX
	RDMSR
	XCHGL	DX, AX				/* swap lo/hi, zero-extend */
	SHLQ	$32, AX				/* hi<<32 */
	ORQ	DX, AX				/* (hi<<32)|lo */
	RET

TEXT wrmsr(SB), 1, $-4
	MOVL	RARG, CX
	MOVL	lo+8(FP), AX
	MOVL	hi+12(FP), DX
	WRMSR
	RET

/*
 */
TEXT invlpg(SB), 1, $-4				/* INVLPG va+0(FP) */
	MOVQ	RARG, va+0(FP)
	INVLPG	va+0(FP)
	RET

TEXT wbinvd(SB), 1, $-4
	WBINVD
	RET

TEXT lfence(SB), 1, $-4
	LFENCE
	RET

TEXT mfence(SB), 1, $-4
	MFENCE
	RET

TEXT sfence(SB), 1, $-4
	SFENCE
	RET

/*
 * Note: CLI and STI are not serialising instructions.
 * Is that assumed anywhere?
 */
TEXT splhi(SB), 1, $-4
_splhi:
	PUSHFQ
	POPQ	AX
	TESTQ	$If, AX				/* If - Interrupt Flag */

	JZ	alreadyhi
	MOVQ	(SP), BX			/* use CMOVLEQ etc. here? */
	MOVQ	BX, 8(RMACH) 			/* save PC in m->splpc */

alreadyhi:
	CLI
	RET

TEXT spllo(SB), 1, $-4
_spllo:
	PUSHFQ
	POPQ	AX
	TESTQ	$If, AX				/* If - Interrupt Flag */
	JNZ	alreadylo			/* use CMOVLEQ etc. here? */
	MOVQ	$0, 8(RMACH)			/* clear m->splpc */

alreadylo:
	STI
	RET

TEXT splx(SB), 1, $-4
	TESTQ	$If, RARG			/* If - Interrupt Flag */
	JNZ	_spllo
	JMP	_splhi

TEXT spldone(SB), 1, $-4
	RET

TEXT islo(SB), 1, $-4
	PUSHFQ
	POPQ	AX
	ANDQ	$If, AX				/* If - Interrupt Flag */
	RET

/*
 * Test-And-Set
 */
TEXT tas32(SB), 1, $-4
	MOVL	$0xdeaddead, AX
	XCHGL	AX, (RARG)			/* lock->key */
	RET

TEXT cas32(SB), 1, $-4
	MOVL	exp+8(FP), AX
	MOVL	new+16(FP), BX
	LOCK; CMPXCHGL BX, (RARG)
	MOVL	$1, AX				/* use CMOVLEQ etc. here? */
	JNZ	_cas32r0
_cas32r1:
	RET
_cas32r0:
	DECL	AX
	RET

TEXT cas64(SB), 1, $-4
	MOVQ	exp+8(FP), AX
	MOVQ	new+16(FP), BX
	LOCK; CMPXCHGQ BX, (RARG)
	MOVL	$1, AX				/* use CMOVLEQ etc. here? */
	JNZ	_cas64r0
_cas64r1:
	RET
_cas64r0:
	DECL	AX
	RET

TEXT ainc(SB), 1, $-4				/* int ainc(int*); */
	MOVL	(RARG), AX
_ainc:
	MOVL	AX, BX
	INCL	BX
	LOCK; CMPXCHGL BX, (RARG)
	JNZ	_ainc

	MOVL	BX, AX
	CMPL	AX, $0				/* overflow if -ve or 0 */
	JGT	_return
_trap:
	XORQ	BX, BX
	MOVQ	(BX), BX			/* over under sideways down */
_return:
	RET

TEXT semainc(SB), 1, $-4				/* int semainc(int*); */
	MOVL	(RARG), AX
_semainc:
	MOVL	AX, BX
	INCL	BX
	LOCK; CMPXCHGL BX, (RARG)
	JNZ	_semainc

	MOVL	BX, AX
	CMPL	AX, $0				/* overflow if -ve or 0 */
	RET

/*
 * adec would trap if the counter is negative.
 * But semaphores rely on negative values for the counter, and
 * those are ok.
 */
TEXT semadec(SB), 1, $-4				/* int semadec(int*); */
	MOVL	(RARG), AX
_semadec:
	MOVL	AX, BX
	DECL	BX
	LOCK; CMPXCHGL BX, (RARG)
	JNZ	_semadec

	MOVL	BX, AX

	RET

TEXT adec(SB), 1, $-4				/* int adec(int*); */
	MOVL	(RARG), AX
_adec:
	MOVL	AX, BX
	DECL	BX
	LOCK; CMPXCHGL BX, (RARG)
	JNZ	_adec

	MOVL	BX, AX
	CMPL	AX, $0				/* underflow if -ve */
	JLT	_trap

	RET

TEXT gotolabel(SB), 1, $-4
	MOVQ	0(RARG), SP			/* restore sp */
	MOVQ	8(RARG), AX			/* put return pc on the stack */
	MOVQ	AX, 0(SP)
	MOVL	$1, AX				/* return 1 */
	RET

TEXT setlabel(SB), 1, $-4
	MOVQ	SP, 0(RARG)			/* store sp */
	MOVQ	0(SP), BX			/* store return pc */
	MOVQ	BX, 8(RARG)
	MOVL	$0, AX				/* return 0 */
	RET

TEXT halt(SB), 1, $-4
	CLI
	CMPL	nrdy(SB), $0
	JEQ	_nothingready
	STI
	RET

_nothingready:
	STI
	HLT
	RET

TEXT hardhalt(SB), 1, $-4
	STI
	HLT
	RET

TEXT mul64fract(SB), 1, $-4
	MOVQ	a+8(FP), AX
	MULQ	b+16(FP)			/* a*b */
	SHRQ	$32, AX:DX
	MOVQ	AX, (RARG)
	RET

///*
// * Testing.
// */
//TEXT ud2(SB), $-4
//	BYTE $0x0f; BYTE $0x0b
//	RET
//

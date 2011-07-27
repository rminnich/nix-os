MODE $64

	DATA	.string<>+0(SB)/8,$"testing\n"
TEXT	_bite(SB), 1, $32
	MOVL	$1,BP
	MOVQ	$.string<>+0(SB),AX
	MOVQ	AX,8(SP)
	MOVL	$8,AX
	MOVL	AX,16(SP)
	MOVQ $20, RARG
	SYSCALL
	MOVQ RARG, a0+0(FP)
	MOVQ $8, RARG
	SYSCALL
	RET	,

/*
 * Port I/O.
 */
TEXT _main(SB), 1, $-4
		MOVQ	$0, RARG
		PUSHQ	RARG
		CALL _bite(SB)

/* 6a asys.s ; 6l -o 6.asys asys.6*/

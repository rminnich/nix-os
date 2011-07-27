MODE $64

TEXT	_bite(SB), 1, $0
	MOVQ RARG, a0+0(FP)
	MOVQ $8, RARG
	SYSCALL
	RET


/*
 * Port I/O.
 */
TEXT _main(SB), 1, $-4
		MOVQ	$0, RARG
		PUSHQ	RARG
		CALL _bite(SB)

/* 6a asys.s ; 6l -o 6.asys asys.6*/

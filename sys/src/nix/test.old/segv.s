
MODE $64

/*
 * Port I/O.
 */
TEXT _main(SB), 1, $-4
	MOVQ 0xcafebabedeadbeef, RARG
	SYSCALL
	MOVL $0, RARG
//CALL	,exits+0(SB)
	RET	,
	RET

/* 6a segv.s ; 6l -o 6.segv segv.6*/

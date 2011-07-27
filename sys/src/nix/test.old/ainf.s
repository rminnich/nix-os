
MODE $64

/*
 * Port I/O.
 */
TEXT _main(SB), 1, $-4
a:	JMP a
	MOVQ $8, RARG
	SYSCALL
	MOVL $0, RARG
//CALL	,exits+0(SB)
	RET	,
	RET

/* 6a ainf.s ; 6l -o 6.ainf ainf.6*/

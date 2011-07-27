#define	OR	57346
#define	AND	57347
#define	ADD	57348
#define	SUBT	57349
#define	MULT	57350
#define	DIV	57351
#define	REM	57352
#define	EQ	57353
#define	GT	57354
#define	GEQ	57355
#define	LT	57356
#define	LEQ	57357
#define	NEQ	57358
#define	A_STRING	57359
#define	SUBSTR	57360
#define	LENGTH	57361
#define	INDEX	57362
#define	NOARG	57363
#define	MATCH	57364
#define	MCH	57365

#line	18	"/sys/src/ape/cmd/expr/expr.y"
#define YYSTYPE charp

typedef char *charp;
extern	int	yyerrflag;
#ifndef	YYMAXDEPTH
#define	YYMAXDEPTH	150
#endif
#ifndef	YYSTYPE
#define	YYSTYPE	int
#endif
YYSTYPE	yylval;
YYSTYPE	yyval;
#define YYEOFCODE 1
#define YYERRCODE 2

#line	55	"/sys/src/ape/cmd/expr/expr.y"

/*	expression command */
#include <stdio.h>
/* get rid of yacc debug printf's */
#define printf
#define ESIZE	512
#define error(c)	errxx(c)
#define EQL(x,y) !strcmp(x,y)
long atol();
char *ltoa();
char	**Av;
int	Ac;
int	Argi;

char Mstring[1][128];
char *malloc();
extern int nbra;
int yyparse(void);

main(argc, argv) char **argv; {
	Ac = argc;
	Argi = 1;
	Av = argv;
	yyparse();
}

char *operator[] = { "|", "&", "+", "-", "*", "/", "%", ":",
	"=", "==", "<", "<=", ">", ">=", "!=",
	"match", "substr", "length", "index", "\0" };
int op[] = { OR, AND, ADD,  SUBT, MULT, DIV, REM, MCH,
	EQ, EQ, LT, LEQ, GT, GEQ, NEQ,
	MATCH, SUBSTR, LENGTH, INDEX };
yylex() {
	register char *p;
	register i;

	if(Argi >= Ac) return NOARG;

	p = Av[Argi++];

	if(*p == '(' || *p == ')')
		return (int)*p;
	for(i = 0; *operator[i]; ++i)
		if(EQL(operator[i], p))
			return op[i];

	yylval = p;
	return A_STRING;
}

char *rel(op, r1, r2) register char *r1, *r2; {
	register i;

	if(ematch(r1, "-\\{0,1\\}[0-9]*$") && ematch(r2, "-\\{0,1\\}[0-9]*$"))
		i = atol(r1) - atol(r2);
	else
		i = strcmp(r1, r2);
	switch(op) {
	case EQ: i = i==0; break;
	case GT: i = i>0; break;
	case GEQ: i = i>=0; break;
	case LT: i = i<0; break;
	case LEQ: i = i<=0; break;
	case NEQ: i = i!=0; break;
	}
	return i? "1": "0";
}

char *arith(op, r1, r2) char *r1, *r2; {
	long i1, i2;
	register char *rv;

	if(!(ematch(r1, "-\\{0,1\\}[0-9]*$") && ematch(r2, "-\\{0,1\\}[0-9]*$")))
		yyerror("non-numeric argument");
	i1 = atol(r1);
	i2 = atol(r2);

	switch(op) {
	case ADD: i1 = i1 + i2; break;
	case SUBT: i1 = i1 - i2; break;
	case MULT: i1 = i1 * i2; break;
	case DIV: i1 = i1 / i2; break;
	case REM: i1 = i1 % i2; break;
	}
	rv = malloc(16);
	strcpy(rv, ltoa(i1));
	return rv;
}
char *conj(op, r1, r2) char *r1, *r2; {
	register char *rv;

	switch(op) {

	case OR:
		if(EQL(r1, "0")
		|| EQL(r1, ""))
			if(EQL(r2, "0")
			|| EQL(r2, ""))
				rv = "0";
			else
				rv = r2;
		else
			rv = r1;
		break;
	case AND:
		if(EQL(r1, "0")
		|| EQL(r1, ""))
			rv = "0";
		else if(EQL(r2, "0")
		|| EQL(r2, ""))
			rv = "0";
		else
			rv = r1;
		break;
	}
	return rv;
}

char *substr(v, s, w) char *v, *s, *w; {
register si, wi;
register char *res;

	si = atol(s);
	wi = atol(w);
	while(--si) if(*v) ++v;

	res = v;

	while(wi--) if(*v) ++v;

	*v = '\0';
	return res;
}

char *length(s) register char *s; {
	register i = 0;
	register char *rv;

	while(*s++) ++i;

	rv = malloc(8);
	strcpy(rv, ltoa((long)i));
	return rv;
}

char *index(s, t) char *s, *t; {
	register i, j;
	register char *rv;

	for(i = 0; s[i] ; ++i)
		for(j = 0; t[j] ; ++j)
			if(s[i]==t[j]) {
				strcpy(rv=malloc(8), ltoa((long)++i));
				return rv;
			}
	return "0";
}

char *match(s, p)
{
	register char *rv;

	strcpy(rv=malloc(8), ltoa((long)ematch(s, p)));
	if(nbra) {
		rv = malloc(strlen(Mstring[0])+1);
		strcpy(rv, Mstring[0]);
	}
	return rv;
}

#define INIT	register char *sp = instring;
#define GETC()		(*sp++)
#define PEEKC()		(*sp)
#define UNGETC(c)	(--sp)
#define RETURN(c)	return
#define ERROR(c)	errxx(c)


ematch(s, p)
char *s;
register char *p;
{
	static char expbuf[ESIZE];
	char *compile();
	register num;
	extern char *braslist[], *braelist[], *loc2;

	compile(p, expbuf, &expbuf[ESIZE], 0);
	if(nbra > 1)
		yyerror("Too many '\\('s");
	if(advance(s, expbuf)) {
		if(nbra == 1) {
			p = braslist[0];
			num = braelist[0] - p;
			strncpy(Mstring[0], p, num);
			Mstring[0][num] = '\0';
		}
		return(loc2-s);
	}
	return(0);
}

errxx(c)
{
	yyerror("RE error");
}

#include  "regexp.h"
yyerror(s)

{
	write(2, "expr: ", 6);
	prt(2, s);
	exit(2);
}
prt(fd, s)
char *s;
{
	write(fd, s, strlen(s));
	write(fd, "\n", 1);
}
char *ltoa(l)
long l;
{
	static char str[20];
	register char *sp = &str[18];
	register i;
	register neg = 0;

	if(l < 0)
		++neg, l *= -1;
	str[19] = '\0';
	do {
		i = l % 10;
		*sp-- = '0' + i;
		l /= 10;
	} while(l);
	if(neg)
		*sp-- = '-';
	return ++sp;
}
short	yyexca[] =
{-1, 1,
	1, -1,
	-2, 0,
};
#define	YYNPROD	22
#define	YYPRIVATE 57344
#define	YYLAST	157
short	yyact[] =
{
   2,  23,   1,   0,  24,  25,  26,  27,  28,   0,
   0,  29,  30,  31,  32,  33,  34,  35,  36,  37,
  38,  39,  40,  41,  42,   0,  44,  45,   0,  46,
   8,   5,   6,   7,   0,   4,   0,   3,   0,   0,
   0,   0,   0,   0,   0,   0,  47,  10,  11,  18,
  19,  20,  21,  22,  12,  13,  14,  15,  16,  17,
   8,   5,   6,   7,   0,   4,  23,   3,  10,  11,
  18,  19,  20,  21,  22,  12,  13,  14,  15,  16,
  17,   0,   0,   0,   0,   0,   0,  23,   0,  43,
  10,  11,  18,  19,  20,  21,  22,  12,  13,  14,
  15,  16,  17,   0,   0,   0,   0,   9,   0,  23,
  11,  18,  19,  20,  21,  22,  12,  13,  14,  15,
  16,  17,   0,  18,  19,  20,  21,  22,  23,  18,
  19,  20,  21,  22,  12,  13,  14,  15,  16,  17,
  23,  20,  21,  22,   0,   0,  23,   0,   0,   0,
   0,   0,   0,   0,   0,   0,  23
};
short	yypact[] =
{
  13,-1000,  86,  13,  13,  13,  13,  13,-1000,-1000,
  13,  13,  13,  13,  13,  13,  13,  13,  13,  13,
  13,  13,  13,  13,  64,  43,  43,-1000,  43, 105,
 123, 117, 117, 117, 117, 117, 117, 133, 133, -22,
 -22, -22,-1000,-1000,-1000,  43,-1000,-1000
};
short	yypgo[] =
{
   0,   2,   0
};
short	yyr1[] =
{
   0,   1,   2,   2,   2,   2,   2,   2,   2,   2,
   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
   2,   2
};
short	yyr2[] =
{
   0,   2,   3,   3,   3,   3,   3,   3,   3,   3,
   3,   3,   3,   3,   3,   3,   3,   3,   4,   2,
   3,   1
};
short	yychk[] =
{
-1000,  -1,  -2,  24,  22,  18,  19,  20,  17,  21,
   4,   5,  11,  12,  13,  14,  15,  16,   6,   7,
   8,   9,  10,  23,  -2,  -2,  -2,  -2,  -2,  -2,
  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,
  -2,  -2,  -2,  25,  -2,  -2,  -2,  -2
};
short	yydef[] =
{
   0,  -2,   0,   0,   0,   0,   0,   0,  21,   1,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,  19,   0,   3,
   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
  14,  15,  16,   2,  17,   0,  20,  18
};
short	yytok1[] =
{
   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  24,  25
};
short	yytok2[] =
{
   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,
  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,
  22,  23
};
long	yytok3[] =
{
   0
};
#define YYFLAG 		-1000
#define YYERROR		goto yyerrlab
#define YYACCEPT	return(0)
#define YYABORT		return(1)
#define	yyclearin	yychar = -1
#define	yyerrok		yyerrflag = 0

#ifdef	yydebug
#include	"y.debug"
#else
#define	yydebug		0
char*	yytoknames[1];		/* for debugging */
char*	yystates[1];		/* for debugging */
#endif

/*	parser for yacc output	*/

int	yynerrs = 0;		/* number of errors */
int	yyerrflag = 0;		/* error recovery flag */

char*
yytokname(int yyc)
{
	static char x[16];

	if(yyc > 0 && yyc <= sizeof(yytoknames)/sizeof(yytoknames[0]))
	if(yytoknames[yyc-1])
		return yytoknames[yyc-1];
	sprintf(x, "<%d>", yyc);
	return x;
}

char*
yystatname(int yys)
{
	static char x[16];

	if(yys >= 0 && yys < sizeof(yystates)/sizeof(yystates[0]))
	if(yystates[yys])
		return yystates[yys];
	sprintf(x, "<%d>\n", yys);
	return x;
}

long
yylex1(void)
{
	long yychar;
	long *t3p;
	int c;

	yychar = yylex();
	if(yychar <= 0) {
		c = yytok1[0];
		goto out;
	}
	if(yychar < sizeof(yytok1)/sizeof(yytok1[0])) {
		c = yytok1[yychar];
		goto out;
	}
	if(yychar >= YYPRIVATE)
		if(yychar < YYPRIVATE+sizeof(yytok2)/sizeof(yytok2[0])) {
			c = yytok2[yychar-YYPRIVATE];
			goto out;
		}
	for(t3p=yytok3;; t3p+=2) {
		c = t3p[0];
		if(c == yychar) {
			c = t3p[1];
			goto out;
		}
		if(c == 0)
			break;
	}
	c = 0;

out:
	if(c == 0)
		c = yytok2[1];	/* unknown char */
	if(yydebug >= 3)
		printf("lex %.4lX %s\n", yychar, yytokname(c));
	return c;
}

int
yyparse(void)
{
	struct
	{
		YYSTYPE	yyv;
		int	yys;
	} yys[YYMAXDEPTH], *yyp, *yypt;
	short *yyxi;
	int yyj, yym, yystate, yyn, yyg;
	YYSTYPE save1, save2;
	int save3, save4;
	long yychar;

	save1 = yylval;
	save2 = yyval;
	save3 = yynerrs;
	save4 = yyerrflag;

	yystate = 0;
	yychar = -1;
	yynerrs = 0;
	yyerrflag = 0;
	yyp = &yys[-1];
	goto yystack;

ret0:
	yyn = 0;
	goto ret;

ret1:
	yyn = 1;
	goto ret;

ret:
	yylval = save1;
	yyval = save2;
	yynerrs = save3;
	yyerrflag = save4;
	return yyn;

yystack:
	/* put a state and value onto the stack */
	if(yydebug >= 4)
		printf("char %s in %s", yytokname(yychar), yystatname(yystate));

	yyp++;
	if(yyp >= &yys[YYMAXDEPTH]) {
		yyerror("yacc stack overflow");
		goto ret1;
	}
	yyp->yys = yystate;
	yyp->yyv = yyval;

yynewstate:
	yyn = yypact[yystate];
	if(yyn <= YYFLAG)
		goto yydefault; /* simple state */
	if(yychar < 0)
		yychar = yylex1();
	yyn += yychar;
	if(yyn < 0 || yyn >= YYLAST)
		goto yydefault;
	yyn = yyact[yyn];
	if(yychk[yyn] == yychar) { /* valid shift */
		yychar = -1;
		yyval = yylval;
		yystate = yyn;
		if(yyerrflag > 0)
			yyerrflag--;
		goto yystack;
	}

yydefault:
	/* default state action */
	yyn = yydef[yystate];
	if(yyn == -2) {
		if(yychar < 0)
			yychar = yylex1();

		/* look through exception table */
		for(yyxi=yyexca;; yyxi+=2)
			if(yyxi[0] == -1 && yyxi[1] == yystate)
				break;
		for(yyxi += 2;; yyxi += 2) {
			yyn = yyxi[0];
			if(yyn < 0 || yyn == yychar)
				break;
		}
		yyn = yyxi[1];
		if(yyn < 0)
			goto ret0;
	}
	if(yyn == 0) {
		/* error ... attempt to resume parsing */
		switch(yyerrflag) {
		case 0:   /* brand new error */
			yyerror("syntax error");
			if(yydebug >= 1) {
				printf("%s", yystatname(yystate));
				printf("saw %s\n", yytokname(yychar));
			}
yyerrlab:
			yynerrs++;

		case 1:
		case 2: /* incompletely recovered error ... try again */
			yyerrflag = 3;

			/* find a state where "error" is a legal shift action */
			while(yyp >= yys) {
				yyn = yypact[yyp->yys] + YYERRCODE;
				if(yyn >= 0 && yyn < YYLAST) {
					yystate = yyact[yyn];  /* simulate a shift of "error" */
					if(yychk[yystate] == YYERRCODE)
						goto yystack;
				}

				/* the current yyp has no shift onn "error", pop stack */
				if(yydebug >= 2)
					printf("error recovery pops state %d, uncovers %d\n",
						yyp->yys, (yyp-1)->yys );
				yyp--;
			}
			/* there is no state on the stack with an error shift ... abort */
			goto ret1;

		case 3:  /* no shift yet; clobber input char */
			if(yydebug >= YYEOFCODE)
				printf("error recovery discards %s\n", yytokname(yychar));
			if(yychar == YYEOFCODE)
				goto ret1;
			yychar = -1;
			goto yynewstate;   /* try again in the same state */
		}
	}

	/* reduction by production yyn */
	if(yydebug >= 2)
		printf("reduce %d in:\n\t%s", yyn, yystatname(yystate));

	yypt = yyp;
	yyp -= yyr2[yyn];
	yyval = (yyp+1)->yyv;
	yym = yyn;

	/* consult goto table to find next state */
	yyn = yyr1[yyn];
	yyg = yypgo[yyn];
	yyj = yyg + yyp->yys + 1;

	if(yyj >= YYLAST || yychk[yystate=yyact[yyj]] != -yyn)
		yystate = yyact[yyg];
	switch(yym) {
		
case 1:
#line	27	"/sys/src/ape/cmd/expr/expr.y"
 {
			prt(1, yypt[-1].yyv);
			exit((!strcmp(yypt[-1].yyv,"0")||!strcmp(yypt[-1].yyv,"\0"))? 1: 0);
			} break;
case 2:
#line	34	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = yypt[-1].yyv; } break;
case 3:
#line	35	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = conj(OR, yypt[-2].yyv, yypt[-0].yyv); } break;
case 4:
#line	36	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = conj(AND, yypt[-2].yyv, yypt[-0].yyv); } break;
case 5:
#line	37	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(EQ, yypt[-2].yyv, yypt[-0].yyv); } break;
case 6:
#line	38	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(GT, yypt[-2].yyv, yypt[-0].yyv); } break;
case 7:
#line	39	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(GEQ, yypt[-2].yyv, yypt[-0].yyv); } break;
case 8:
#line	40	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(LT, yypt[-2].yyv, yypt[-0].yyv); } break;
case 9:
#line	41	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(LEQ, yypt[-2].yyv, yypt[-0].yyv); } break;
case 10:
#line	42	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = rel(NEQ, yypt[-2].yyv, yypt[-0].yyv); } break;
case 11:
#line	43	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = arith(ADD, yypt[-2].yyv, yypt[-0].yyv); } break;
case 12:
#line	44	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = arith(SUBT, yypt[-2].yyv, yypt[-0].yyv); } break;
case 13:
#line	45	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = arith(MULT, yypt[-2].yyv, yypt[-0].yyv); } break;
case 14:
#line	46	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = arith(DIV, yypt[-2].yyv, yypt[-0].yyv); } break;
case 15:
#line	47	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = arith(REM, yypt[-2].yyv, yypt[-0].yyv); } break;
case 16:
#line	48	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = match(yypt[-2].yyv, yypt[-0].yyv); } break;
case 17:
#line	49	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = match(yypt[-1].yyv, yypt[-0].yyv); } break;
case 18:
#line	50	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = substr(yypt[-2].yyv, yypt[-1].yyv, yypt[-0].yyv); } break;
case 19:
#line	51	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = length(yypt[-0].yyv); } break;
case 20:
#line	52	"/sys/src/ape/cmd/expr/expr.y"
 { yyval = index(yypt[-1].yyv, yypt[-0].yyv); } break;
	}
	goto yystack;  /* stack new state and value */
}

//KRC REDUCER

#include "listhdr.h"
#include "comphdr.h"
#include "redhdr.h"

//----------------------------------------------------------------------
//The KRC system is Copyright (c) D. A. Turner 1981
//All  rights reserved.  It is distributed as free software under the
//terms in the file "COPYING", which is included in the distribution.
//----------------------------------------------------------------------

#include <string.h>	// for strlen()
#include <unistd.h>	// for sbrk()
#include <ctype.h>	// for isprint()
#include <signal.h>     // for raise(), SIGINT

// Global variables owned by reducer
LIST MEMORIES = NIL;
static WORD REDS;
WORD LISTBASE=0;   //base for list indexing
BOOL ABORTED=FALSE;

static ATOM ETC, SILLYNESS, GUARD, LISTDIFF, BADFILE, READFN, WRITEFN,
          INTERLEAVEFN;

// ARGUMENT STACK. ARGP points to the last cell allocated
static LIST *ARGSPACE=NULL;
static LIST *ARG;
static LIST *ARGMAX;
static LIST *ARGP;

void
INIT_ARGSPACE(void)
{
   IF ARGSPACE==NULL DO {
      extern int SPACE;         // Number of LIST cells, in listpack.c
      int NARGS=SPACE/5;        // Empirically, using edigits, with SPACE/6,
                                // the argstack exhausts first. with /5, it
                                // runs out of heap first.
      ARGSPACE=(LIST *)calloc(NARGS,sizeof(*ARGSPACE));
      IF ARGSPACE==NULL DO SPACE_ERROR("Cannot allocate argument stack");
      ARGMAX=ARGSPACE+NARGS-1;
   }
   ARG=ARGSPACE, ARGP=ARG-1;
}
	
// Sentinel value (impossible pointer)
#define ENDOFSTACK (-4)

LIST S;

// Local function declarations
static void SIZE(LIST E);

//PRIMITIVE FUNCTIONS
static void FUNCTIONP(LIST E);
static void LISTP(LIST E);
static void STRINGP(LIST E);
static void NUMBERP(LIST E);
static void CHAR(LIST E);
static void SIZE(LIST E);
static void CODE(LIST E);
static void DECODE(LIST E);
static void CONCAT(LIST E);
static void EXPLODE(LIST E);
static void ABORT(LIST E);
static void STARTREAD(LIST E);
static void READ(LIST E);
static void WRITEAP(LIST E);
static void SEQ(LIST E);

// LOCAL FUNCTION DELARATIONS
static void PRINTFUNCTION(LIST E);
static BOOL EQUALVAL(LIST A,LIST B);
static void BADEXP(LIST E);
static void OVERFLOW(LIST E);
static void OBEY(LIST EQNS,LIST E);
static BOOL ISFUN(LIST X);
static LIST REDUCE(LIST E);
static LIST SUBSTITUTE(LIST ACTUAL,LIST FORMAL,LIST EXP);
static BOOL BINDS(LIST FORMAL,LIST X);
static void SHOWCH(unsigned char c); //DT 2015

static void
R(char *S, void (*F)(LIST), WORD N)
   {  ATOM A=MKATOM(S);
      LIST EQN=CONS((LIST)A,CONS((LIST)CALL_C,(LIST)F));
      UNLESS F==READ DO ENTERSCRIPT(A);
      VAL(A)=CONS(CONS((LIST)N,NIL),CONS(EQN,NIL));  }

void
SETUP_PRIMFNS_ETC(void)
   {
      S=(LIST)ENDOFSTACK;  //S IS USED INSIDE REDUCE
      ETC=MKATOM("... ");  //MISCELLANEOUS INITIALISATIONS
      SILLYNESS=MKATOM("<unfounded recursion>");
      GUARD=MKATOM("<non truth-value used as guard:>");
      TRUTH=CONS((LIST)QUOTE,(LIST)MKATOM("TRUE"));
      FALSITY=CONS((LIST)QUOTE,(LIST)MKATOM("FALSE"));
      LISTDIFF=MKATOM("listdiff");
      INFINITY=CONS((LIST)QUOTE,(LIST)-3);
      R("function__",FUNCTIONP,1);  //PRIMITIVE FUNCTIONS
      R("list__",LISTP,1);
      R("string__",STRINGP,1);
      R("number__",NUMBERP,1);
      R("char__",CHAR,1);
      R("printwidth__",SIZE,1);
      R("ord__",CODE,1);
      R("chr__",DECODE,1);
      R("implode__",CONCAT,1);
      R("explode__",EXPLODE,1);
      R("abort__",ABORT,1);
      R("read__",STARTREAD,1);
      R("read ",READ,1);
      R("seq__",SEQ,2);
      R("write__",WRITEAP,3);
      BADFILE=MKATOM("<cannot open file:>");
      READFN=MKATOM("read ");
      WRITEFN=MKATOM("write");
      INTERLEAVEFN=MKATOM("interleave");
   }

// LITTLE ROUTINE TO AVOID S HAVING TO BE GLOBAL, JUST BECAUSE
// IT MAY NEED FIXING UP AFTER AN INTERRUPT. THIS ROUTINE DOES THAT.
void
FIXUP_S(void)
{
   UNLESS S==(LIST)ENDOFSTACK
   DO HD(S)=(LIST)QUOTE; //IN CASE INTERRUPT STRUCK WHILE REDUCE
                         //WAS DISSECTING A CONSTANT
}

// Return an upper-case copy of a string.
// Copy to static area of 80 chars, the same as BCPL
// also to avoid calling strdup which calls malloc() and
// contaminates the garbage collection done with Boehm GC.
char *
SCASECONV(char *S)
{  static char T[80+1];
   char *p=S, *q=T;
   while (*p) *q++ = CASECONV(*p++);
   *q = '\0';
   return T;  }

void
INITSTATS()
{
   REDS=0;
}

void
OUTSTATS()
   { WRITEF("reductions = %" W "\n",REDS);  }

// THE POSSIBLE VALUES OF A REDUCED EXPRESSION ARE:
//  VAL:= CONST | FUNCTION | LIST
//  CONST:= NUM | CONS(QUOTE,ATOM)
//  LIST:= NIL | CONS(COLON_OP,CONS(EXP,EXP))
//  FUNCTION:= NAME | CONS(E1,E2)

void
PRINTVAL(LIST E, BOOL FORMAT)
   {  E=REDUCE(E);
      if ( E==NIL
      ) { IF FORMAT DO WRITES("[]"); } else
      if ( ISNUM(E)
      ) WRITEN(GETNUM(E)); else
      if ( ISCONS(E)
      ) {  LIST H=HD(E);
              if ( H==(LIST)QUOTE
              ) PRINTATOM((ATOM)TL(E),FORMAT); else
              if ( H==(LIST)COLON_OP
              ) {  IF FORMAT DO WRCH('[');
                      E=TL(E);
                      do {
			 PRINTVAL(HD(E),FORMAT);
                         E=TL(E);
                         E=REDUCE(E);
                         UNLESS ISCONS(E) DO break;
                         if ( HD(E)==(LIST)COLON_OP
                         ) { IF FORMAT DO WRCH(','); }
                         else break;
                         E=TL(E);
                      } while(1);;
                      if ( E==NIL
                      ) { IF FORMAT DO WRCH(']'); }
                      else BADEXP(CONS((LIST)COLON_OP,CONS((LIST)ETC,E)));
                   }  else
              if ( ISCONS(H) && HD(H)==(LIST)WRITEFN
              ) {  TL(H)=REDUCE(TL(H));
                      UNLESS ISCONS(TL(H)) && HD(TL(H))==(LIST)QUOTE
                      DO BADEXP(E);
                   {  char *F=PRINTNAME((ATOM)TL(TL(H)));
                      FILE *OUT=FINDCHANNEL(F);
                      FILE *HOLD=OUTPUT();
                      UNLESS OUT!=NULL DO BADEXP(CONS((LIST)BADFILE,TL(H)));
                      SELECTOUTPUT(OUT);
                      PRINTVAL(TL(E),FORMAT);
                      SELECTOUTPUT(HOLD);
                   } }
              else PRINTFUNCTION(E); //A PARTIAL APPLICATION OR COMPOSITION
           }
      else PRINTFUNCTION(E);  //ONLY POSSIBILITY HERE SHOULD BE
                        //NAME OF FUNCTION
   }

void 
PRINTATOM(ATOM A,BOOL FORMAT)
{  if ( FORMAT
   ) 
        { int I; //DT 2015
          WRCH('"');
          for (I=1; I<=LEN(A); I++) SHOWCH(NAME(A)[I]);
          WRCH('"'); }
   else { int I;  // OUTPUT THE BCPL STRING PRESERVING nulS
        for (I=1; I<=LEN(A); I++) WRCH(NAME(A)[I]);  }
}

static void
SHOWCH(unsigned char c)
{ switch(c)
  { case '\a': WRCH('\\'); WRCH('a'); break;
    case '\b': WRCH('\\'); WRCH('b'); break;
    case '\f': WRCH('\\'); WRCH('f'); break;
    case '\n': WRCH('\\'); WRCH('n'); break;
    case '\r': WRCH('\\'); WRCH('r'); break;
    case '\t': WRCH('\\'); WRCH('t'); break;
    case '\v': WRCH('\\'); WRCH('v'); break;
    case '\\': WRCH('\\'); WRCH('\\'); break;
    case '\'': WRCH('\\'); WRCH('\''); break;
    case '\"': WRCH('\\'); WRCH('\"'); break;
    default: if ( iscntrl(c) || c>=127
             ) printf("\\%03u",c);
             else WRCH(c);
} }

static void
PRINTFUNCTION(LIST E)
   {  WRCH('<');
      PRINTEXP(E,0);
      WRCH('>'); }

static BOOL
EQUALVAL(LIST A,LIST B) //UNPREDICTABLE RESULTS IF A,B BOTH FUNCTIONS
{do{
   A=REDUCE(A);
   B=REDUCE(B);
   IF A==B DO return TRUE;
   IF ISNUM(A) && ISNUM(B)
   DO return GETNUM(A)==GETNUM(B);
   UNLESS ISCONS(A) && ISCONS(B) && (HD(A)==HD(B)) DO return FALSE;
   IF HD(A)==(LIST)QUOTE || HD(A) == (LIST)QUOTE_OP DO return TL(A)==TL(B);
   UNLESS HD(A)==(LIST)COLON_OP DO return FALSE;  //UH ?
   A=TL(A),B=TL(B);
   UNLESS EQUALVAL(HD(A),HD(B)) DO return FALSE;
   A=TL(A),B=TL(B);
} while(1); }

static void
BADEXP(LIST E) //CALLED FOR ALL EVALUATION ERRORS
   {  _WRCH=TRUEWRCH;
      CLOSECHANNELS();
      WRITES("\n**undefined expression**\n  ");
      PRINTEXP(E,0);
      //COULD INSERT MORE DETAILED DIAGNOSTICS HERE, 
      //DEPENDING ON NATURE OF HD!E, FOR EXAMPLE:
      IF ISCONS(E) && (HD(E)==(LIST)COLON_OP||HD(E)==(LIST)APPEND_OP)
      DO WRITES("\n  (non-list encountered where list expected)");
      WRITES("\n**evaluation abandoned**\n");
      ESCAPETONEXTCOMMAND();
   }

static void
OVERFLOW(LIST E) // INTEGER OVERFLOW HANDLER
   {  _WRCH=TRUEWRCH;
      CLOSECHANNELS();
      WRITES("\n**integer overflow**\n  ");
      PRINTEXP(E,0);
      WRITES("\n**evaluation abandoned**\n");
      ESCAPETONEXTCOMMAND();
   }

LIST
BUILDEXP(LIST CODE)       //A KLUDGE
{  LIST E = CONS(NIL,NIL);  //A BOGUS PIECE OF GRAPH
   OBEY(CONS(CONS(NIL,CODE),NIL),E);
   ARGP=ARG-1;  //RESET ARG STACK
   return E;
}

static void
OBEY(LIST EQNS,LIST E) //TRANSFORM A PIECE OF GRAPH, E, IN ACCORDANCE
                       //WITH EQNS - ACTUAL PARAMS ARE FOUND IN
                       // *ARG ... *ARGP
                       // (WARNING - HAS SIDE EFFECT OF RAISING ARGP)
{
   UNTIL EQNS==NIL  //EQNS LOOP
   DO {  LIST CODE=TL(HD(EQNS));
         LIST *HOLDARG=ARGP;
	 WORD I;
         do{LIST H = HD(CODE);  //DECODE LOOP
            CODE=TL(CODE);
            // First, check the only cases that increment ARGP
            switch(  (WORD)H ) {
            case LOAD_C:
            case LOADARG_C:
            case FORMLIST_C:
	       ARGP=ARGP+1;
               IF ARGP>ARGMAX DO SPACE_ERROR("Arg stack overflow");
                                  }
            switch(  (WORD)H ) {
            case LOAD_C: // ARGP=ARGP+1;
                         *ARGP=HD(CODE);
                         CODE=TL(CODE);
                         break; 
            case LOADARG_C: // ARGP=ARGP+1;
            		    IF ARGP>ARGMAX DO SPACE_ERROR("Arg stack overflow");
                            *ARGP=ARG[(WORD)(HD(CODE))];
                            CODE=TL(CODE);
                            break; 
            case APPLYINFIX_C: *ARGP=CONS(*(ARGP-1),*ARGP);
                               *(ARGP-1)=HD(CODE);
                               CODE=TL(CODE);
            case APPLY_C:      ARGP=ARGP-1;
                               IF HD(CODE)==(LIST)STOP_C
                               DO {  HD(E)=*ARGP,TL(E)=*(ARGP+1);
                                     return;  }
                               *ARGP=CONS(*ARGP,*(ARGP+1));
                               break; 
            case CONTINUE_INFIX_C: 
                       *(ARGP-1)=CONS(HD(CODE),CONS(*(ARGP-1),*ARGP));
                       CODE=TL(CODE);
                       break; 
            case IF_C: *ARGP=REDUCE(*ARGP);
                       IF *ARGP==FALSITY DO goto BREAK_DECODE_LOOP;
                       UNLESS *ARGP==TRUTH DO BADEXP(CONS((LIST)GUARD,*ARGP));
                       break; 
            case FORMLIST_C: // ARGP=ARGP+1;
                             *ARGP=NIL;
                             for (I=1; I<=(WORD)HD(CODE); I++)
                             {  ARGP=ARGP-1;
                                *ARGP=CONS((LIST)COLON_OP,
                                        CONS(*ARGP,*(ARGP+1)));
                             }
                             CODE=TL(CODE);
                             break; 
            case FORMZF_C: {  LIST X=CONS(*(ARGP-(WORD)HD(CODE)),NIL);
			      LIST *P;
                              for (P=ARGP; P>=ARGP-(WORD)HD(CODE)+1; P=P-1)
                                 X=CONS(*P,X);
                              ARGP=ARGP-(WORD)HD(CODE);
                              *ARGP=CONS((LIST)ZF_OP,X);
                              CODE=TL(CODE);
                              break;   }
            case CONT_GENERATOR_C:
                  for (I=1; I<=(WORD)HD(CODE); I++)
                     *(ARGP-I)=CONS((LIST)GENERATOR,CONS(*(ARGP-I),
                                     TL(TL(*ARGP))));
                  CODE=TL(CODE);
                  break; 
            case MATCH_C: {  WORD I=(WORD)HD(CODE);
                             CODE=TL(CODE);
                             UNLESS EQUALVAL(ARG[I],HD(CODE)) DO goto BREAK_DECODE_LOOP;
                             CODE=TL(CODE);
                             break;   }
            case MATCHARG_C: {  WORD I=(WORD)HD(CODE);
                                CODE=TL(CODE);
                                UNLESS EQUALVAL(ARG[I],ARG[(WORD)(HD(CODE))])
                                DO goto BREAK_DECODE_LOOP;
                                CODE=TL(CODE);
                                break;   }
            case MATCHPAIR_C: {  LIST *P=ARG+(WORD)(HD(CODE));
                                 *P=REDUCE(*P);
                                 UNLESS ISCONS(*P) && HD(*P)==(LIST)COLON_OP
                                 DO goto BREAK_DECODE_LOOP;
                                 ARGP=ARGP+2;
                                 *(ARGP-1)=HD(TL(*P)),*ARGP=TL(TL(*P));
                                 CODE=TL(CODE);
                                 break;   }
            case LINENO_C: CODE=TL(CODE);  //NO ACTION
                           break; 
            case STOP_C: HD(E)=(LIST)INDIR,TL(E)=*ARGP;
                         return;
            case CALL_C: (*(void (*)())CODE)(E);
                         return;
            default: WRITEF("IMPOSSIBLE INSTRUCTION <%p> IN \"OBEY\"\n", H);
         }  }  while(1);  //END OF DECODE LOOP
BREAK_DECODE_LOOP:
         EQNS=TL(EQNS);
         ARGP=HOLDARG;
      } //END OF EQNS LOOP
   BADEXP(E);
}

static void
STRINGP(LIST E)
   {  *ARG=REDUCE(*ARG);
      HD(E)=(LIST)INDIR,TL(E)=ISCONS(*ARG)&&HD(*ARG)==(LIST)QUOTE ? TRUTH:FALSITY;
   }

static void
NUMBERP(LIST E)
   {  *ARG=REDUCE(*ARG);
      HD(E)=(LIST)INDIR,TL(E)=ISNUM(*ARG)?TRUTH:FALSITY;
   }

static void
LISTP(LIST E)
   {  *ARG=REDUCE(*ARG);
      HD(E)=(LIST)INDIR;
      TL(E)=(*ARG==NIL||(ISCONS(*ARG)&&HD(*ARG)==(LIST)COLON_OP))?
                       TRUTH:FALSITY;
   }

static void
FUNCTIONP(LIST E)
   {  *ARG=REDUCE(*ARG);
      HD(E)=(LIST)INDIR;
      TL(E)=ISFUN(*ARG)?TRUTH:FALSITY;
   }

static BOOL
ISFUN(LIST X)
{ return ISATOM(X) || (ISCONS(X) && QUOTE!=HD(X) && HD(X)!=(LIST)COLON_OP); }

static void
CHAR(LIST E)
   {  *ARG=REDUCE(*ARG);
      HD(E)=(LIST)INDIR;
      TL(E)=ISCONS(*ARG) && HD(*ARG)==(LIST)QUOTE &&
                 LEN((ATOM)TL(*ARG))==1 ? TRUTH : FALSITY;
   }

static WORD COUNT;
static void
COUNTCH(WORD CH) { COUNT=COUNT+1; }

static void
SIZE(LIST E)
   {
      COUNT=0;
      _WRCH=COUNTCH;
      PRINTVAL(*ARG,FALSE);
      _WRCH=TRUEWRCH;
      HD(E)=(LIST)INDIR, TL(E)=STONUM(COUNT);
   }

static void
CODE(LIST E)
   {  *ARG = REDUCE(*ARG);
      UNLESS ISCONS(*ARG) && HD(*ARG)==QUOTE
      DO BADEXP(E);
   {  ATOM A=(ATOM)TL(*ARG);
      UNLESS LEN(A)==1 DO BADEXP(E);
      HD(E)=(LIST)INDIR, TL(E)=STONUM((WORD)NAME(A)[1] & 0xff);
   } }

static void
DECODE(LIST E)
   {  *ARG = REDUCE(*ARG);
      UNLESS ISNUM(*ARG) && 0<=(WORD)TL(*ARG) && (WORD)TL(*ARG)<=255
      DO BADEXP(E);
      BUFCH((WORD)TL(*ARG));
      HD(E)=(LIST)INDIR, TL(E)=CONS((LIST)QUOTE,(LIST)PACKBUFFER());
   }

static void
CONCAT(LIST E)
   {  *ARG = REDUCE(*ARG);
   {  LIST A = *ARG;
      WHILE ISCONS(A) && HD(A)==(LIST)COLON_OP
      DO {  LIST C=REDUCE(HD(TL(A)));
            UNLESS ISCONS(C) && HD(C)==(LIST)QUOTE
            DO BADEXP(E);
            HD(TL(A))= C;
            TL(TL(A))=REDUCE(TL(TL(A)));
            A=TL(TL(A));
         }
      UNLESS A==NIL
      DO BADEXP(E);
      A=*ARG;
      UNTIL A==NIL
      DO {  ATOM N=(ATOM)TL(HD(TL(A)));
            int I;
            for (I=1; I<=LEN(N); I++) BUFCH(NAME(N)[I]);
            A=TL(TL(A));  }
      A=(LIST)PACKBUFFER();
      HD(E) = (LIST)INDIR,
      TL(E) = A==TL(TRUTH) ? TRUTH:
              A==TL(FALSITY) ? FALSITY:
              CONS((LIST)QUOTE,A);
   } }

static void
EXPLODE(LIST E)
   {  *ARG = REDUCE(*ARG);
      UNLESS ISCONS(*ARG) && HD(*ARG)==(LIST)QUOTE
      DO BADEXP(E);
   {  ATOM A=(ATOM)TL(*ARG);
      LIST X = NIL;
      int I;
      for (I=NAME(A)[0]; I>0; I--)
         {  BUFCH(NAME(A)[I]);
            X = CONS((LIST)COLON_OP, CONS(CONS((LIST)QUOTE,(LIST)PACKBUFFER()),X)); }
      HD(E)=(LIST)INDIR, TL(E)=X;
   } }

static void
ABORT(LIST E)
   { FILE *HOLD=OUTPUT();
     SELECTOUTPUT(stderr);
     WRITES("\nprogram error: ");
     PRINTVAL(TL(E),FALSE);
     WRCH('\n');
     SELECTOUTPUT(HOLD);
     ABORTED=TRUE;
     raise(SIGINT);
   }

static void
STARTREAD(LIST E)
   {  *ARG=REDUCE(*ARG);
      UNLESS ISCONS(*ARG) && HD(*ARG)==(LIST)QUOTE
      DO BADEXP(E);
   {  FILE *IN = FINDINPUT(PRINTNAME((ATOM)TL(*ARG)));
      UNLESS IN!=NULL
      DO BADEXP(CONS((LIST)BADFILE,*ARG));
      HD(E)=(LIST)READFN,TL(E)=(LIST)IN;
   } }

static void
READ(LIST E)
   {  FILE *IN=(FILE *)TL(E);
      SELECTINPUT(IN);
      HD(E)=(LIST)INDIR,TL(E)=CONS((LIST)READFN,TL(E));
   {  LIST *X = &(TL(E)); WORD C=RDCH();
      // Read one character
      IF C!=EOF 
      DO {  char c=C;
	    *X=CONS((LIST)COLON_OP, CONS(
		         CONS((LIST)QUOTE,(LIST)MKATOMN(&c,1)), *X));
            X=&(TL(TL(*X)));
      }
      IF ferror(IN) DO {
         WRITEF("\n**File read error**\n");
         ESCAPETONEXTCOMMAND();
      }
      IF C==EOF
      DO {  ENDREAD() ; *X=NIL;  }
      SELECTINPUT(SYSIN);
   } }

static void
WRITEAP(LIST E) //CALLED IF WRITE IS APPLIED TO >2 ARGS
   { BADEXP(E); }

static void
SEQ(LIST E)  //seq a b EVALUATES a THEN RETURNS b, ADDED DT 2015
   { REDUCE(TL(HD(E)));
     HD(E)=(LIST)INDIR;
   }

//POSSIBILITIES FOR LEFTMOST FIELD OF A GRAPH ARE:
// HEAD:= NAME | NUM | NIL | OPERATOR

static LIST
REDUCE(LIST E)
{  static WORD M=0;
   static WORD N=0;
   LIST HOLD_S=S; WORD NARGS=0; LIST *HOLDARG=ARG;
   // IF &E>STACKLIMIT DO SPACE_ERROR("Arg stack overflow");
// IF ARGP>ARGMAX DO SPACE_ERROR("Arg stack overflow");
   S=(LIST)ENDOFSTACK;
   ARG=ARGP+1;
   do{  //MAIN LOOP
      WHILE ISCONS(E)  //FIND HEAD, REVERSING POINTERS EN ROUTE
      DO {  LIST HOLD=HD(E);
            NARGS=NARGS+1;
            HD(E)=S,S=E,E=HOLD;  }
      IF ISNUM(E) || E==NIL
      DO {  // UNLESS NARGS==0 DO HOLDARG=(LIST *)-1;  //FLAGS AN ERROR
            goto BREAK_MAIN_LOOP;  }
      if ( ISATOM(E)  //USER DEFINED NAME
      ) if ( VAL((ATOM)E)==NIL || TL(VAL((ATOM)E))==NIL ) BADEXP(E); else  //UNDEFINED NAME
      if ( HD(HD(VAL((ATOM)E)))==0  //VARIABLE
      ) {  LIST EQN=HD(TL(VAL((ATOM)E)));
              IF HD(EQN)==0 //MEMO NOT SET
              DO {  HD(EQN)=BUILDEXP(TL(EQN));
                    MEMORIES=CONS(E,MEMORIES);  }
              E=HD(EQN);  }  //?CAN WE GET CYCLIC EXPRESSIONS?
      else {  //FUNCTION
                 WORD N=(WORD)HD(HD(VAL((ATOM)E)));	// Hides the static N
                 IF N>NARGS DO goto BREAK_MAIN_LOOP;  //NOT ENOUGH ARGS
              {  LIST EQNS=TL(VAL((ATOM)E));
		 WORD I;
                 for (I=0; I<=N-1; I++)
                 {  LIST HOLD=HD(S);  //MOVE BACK UP GRAPH,
                    ARGP=ARGP+1;   //STACKING ARGS EN ROUTE
                    IF ARGP>ARGMAX DO SPACE_ERROR("Arg stack overflow");
                    *ARGP=TL(S);
                    HD(S)=E,E=S,S=HOLD;  }
                 NARGS=NARGS-N;
                 //E NOW HOLDS A PIECE OF GRAPH TO BE TRANSFORMED
                 // !ARG ... !ARGP  HOLD THE PARAMETERS
                 OBEY(EQNS,E);
                 ARGP=ARG-1;  //RESET ARG STACK
              } }
      else {  //OPERATORS
            switch(  (WORD)E )
         {  case QUOTE: UNLESS NARGS==1 DO HOLDARG=(LIST *)-1;
                        goto BREAK_MAIN_LOOP;
            case INDIR: {  LIST HOLD=HD(S);
                           NARGS=NARGS-1;
                           E=TL(S),HD(S)=(LIST)INDIR,S=HOLD;
                           continue;;  }
            case QUOTE_OP: UNLESS NARGS>=3 DO goto BREAK_MAIN_LOOP;
                        {  LIST OP=TL(S);
                           LIST HOLD=HD(S);
                           NARGS=NARGS-2;
                           HD(S)=E,E=S,S=HOLD;
                           HOLD=HD(S);
                           HD(S)=E,E=S,S=HOLD;
                           TL(S)=CONS(TL(E),TL(S)),E=OP;
                           continue;;  }
            case LISTDIFF_OP: E=CONS((LIST)LISTDIFF,HD(TL(S)));
                              TL(S)=TL(TL(S));
                              continue;;
            case COLON_OP: UNLESS NARGS>=2 DO goto BREAK_MAIN_LOOP;
                           //LIST INDEXING
                           NARGS=NARGS-2;
                        {  LIST HOLD=HD(S); WORD M; //Hides static M
                           HD(S)=(LIST)COLON_OP,E=S,S=HOLD;
                           TL(S)=REDUCE(TL(S));
                           UNLESS ISNUM(TL(S)) && (M=GETNUM(TL(S)))>=LISTBASE
                           DO { HOLDARG=(LIST *)-1; goto BREAK_MAIN_LOOP; }
                           WHILE M-- > LISTBASE
                           DO { E=REDUCE(TL(TL(E))); //Clobbers static M
                                UNLESS ISCONS(E) && HD(E)==(LIST)COLON_OP
                                DO BADEXP(CONS(E,STONUM(M+1))); }
                           E=HD(TL(E));
                           HOLD=HD(S);
                           HD(S)=(LIST)INDIR,TL(S)=E,S=HOLD;
                           REDS=REDS+1;
                           continue;; }
            case ZF_OP: {  LIST HOLD=HD(S);
                           NARGS=NARGS-1;
                           HD(S)=E,E=S,S=HOLD;
                           IF TL(TL(E))==NIL
                           DO {  HD(E)=(LIST)COLON_OP,TL(E)=CONS(HD(TL(E)),NIL);
                                 continue;;  }
                        {  LIST QUALIFIER=HD(TL(E));
                           LIST REST=TL(TL(E));
                           if ( ISCONS(QUALIFIER)&&HD(QUALIFIER)==(LIST)GENERATOR
                           )
                           {  LIST SOURCE=REDUCE(TL(TL(QUALIFIER)));
                              LIST FORMAL=HD(TL(QUALIFIER));
                              TL(TL(QUALIFIER))=SOURCE;
                              if ( SOURCE==NIL
                              ) HD(E)=(LIST)INDIR,TL(E)=NIL,E=NIL; else
                              if ( ISCONS(SOURCE)&&HD(SOURCE)==(LIST)COLON_OP
                              ) HD(E)=CONS((LIST)INTERLEAVEFN,
				   CONS((LIST)ZF_OP, SUBSTITUTE(HD(TL(SOURCE)),FORMAL,REST))),
      TL(E)=CONS((LIST)ZF_OP,
		 CONS(CONS((LIST)GENERATOR,CONS(FORMAL,TL(TL(SOURCE)))),
                      REST));

//                            ) HD!E,TL!E:=APPEND.OP,
//                                            CONS(
//            CONS(ZF.OP,SUBSTITUTE(HD!(TL!SOURCE),FORMAL,REST)),
//    CONS(ZF.OP,CONS(CONS(GENERATOR,CONS(FORMAL,TL!(TL!SOURCE))),REST))
//                                                )
                              else BADEXP(E);  }
                           else {  //QUALIFIER IS GUARD
                                 QUALIFIER=REDUCE(QUALIFIER);
                                 HD(TL(E))=QUALIFIER;
                                 if ( QUALIFIER==TRUTH
                                 ) TL(E)=REST; else
                                 if ( QUALIFIER==FALSITY
                                 ) HD(E)=(LIST)INDIR,TL(E)=NIL,E=NIL;
                                 else BADEXP(CONS((LIST)GUARD,QUALIFIER));  }
                           REDS=REDS+1;
                           continue;;  }  }
            case DOT_OP: UNLESS NARGS>=2
                         DO {  LIST A=REDUCE(HD(TL(S))),B=REDUCE(TL(TL(S)));
                               UNLESS ISFUN(A) && ISFUN(B)
                               DO BADEXP(CONS(E,CONS(A,B)));
                               goto BREAK_MAIN_LOOP;  }
                      {  LIST HOLD=HD(S);
                         NARGS=NARGS-1;
                         E=HD(TL(S)),TL(HOLD)=CONS(TL(TL(S)),TL(HOLD));
                         HD(S)=(LIST)DOT_OP,S=HOLD;
                         REDS=REDS+1;
                         continue;;  }
            case EQ_OP:
            case NE_OP: E=EQUALVAL(HD(TL(S)),TL(TL(S)))==(E==(LIST)EQ_OP)?
                           TRUTH:FALSITY;
              //NOTE - COULD REWRITE FOR FAST EXIT, HERE AND IN
              //OTHER CASES WHERE RESULT OF REDUCTION IS ATOMIC
                     {  LIST HOLD=HD(S);
                        NARGS=NARGS-1;
                        HD(S)=(LIST)INDIR,TL(S)=E,S=HOLD;
                        REDS=REDS+1;
                        continue;;  }
            case ENDOFSTACK: BADEXP((LIST)SILLYNESS); //OCCURS IF WE TRY TO
                                 //EVALUATE AN EXP WE ARE ALREADY INSIDE
            default: break;   }  //END OF SWITCH
         {  //STRICT OPERATORS
            LIST A=NIL,B=NIL;
	    BOOL STRINGS=FALSE;
   	    ATOM SM, SN;  // The values of M and N when STRINGS == TRUE
            if ( (WORD)E>=LENGTH_OP
            ) A=REDUCE(TL(S));  //MONADIC
            else {  A=REDUCE(HD(TL(S)));  //DIADIC
                  if ( E>=(LIST)GR_OP  //STRICT IN 2ND ARG ?
                  ) { //YES
                         B=REDUCE(E==(LIST)COMMADOTDOT_OP?HD(TL(TL(S))):TL(TL(S)));
                         if ( ISNUM(A) && ISNUM(B)
                         ) M=GETNUM(A),N=GETNUM(B); else
                         if ( E<=(LIST)LS_OP &&  //RELOPS
                              ISCONS(A) && ISCONS(B)
                              && HD(A)==(LIST)QUOTE && (LIST)QUOTE==HD(B)
                         ) STRINGS=TRUE,SM=(ATOM)TL(A),SN=(ATOM)TL(B); else
                         if ( E==(LIST)DOTDOT_OP && ISNUM(A) && B==INFINITY
                         ) M=GETNUM(A),N=M;
                         else
   BADEXP(CONS(E,CONS(A,E==(LIST)COMMADOTDOT_OP?CONS(B,TL(TL(TL(S)))):B)));
                       }
                  else B=TL(TL(S));  //NO
               }
               switch(  (WORD)E )
               {  case AND_OP: if ( A==FALSITY ) E=A; else
                               if ( A==TRUTH ) E=B; else
                               BADEXP(CONS(E,CONS(A,B)));
					break; 
                  case OR_OP:  if ( A==TRUTH ) E=A; else
                               if ( A==FALSITY ) E=B; else
                               BADEXP(CONS(E,CONS(A,B)));
					break; 
                  case APPEND_OP: IF A==NIL DO { E=B; break;  }
                                  UNLESS ISCONS(A) && HD(A)==(LIST)COLON_OP
                                  DO BADEXP(CONS(E,CONS(A,B)));
                                  E=(LIST)COLON_OP;
                                  TL(TL(S))=CONS((LIST)APPEND_OP,
                                              CONS(TL(TL(A)),B));
                                  HD(TL(S))=HD(TL(A));
                                  REDS=REDS+1;
                                  continue;
                  case DOTDOT_OP: IF M>N DO { E=NIL; break;  }
                                  E=(LIST)COLON_OP;
                                  TL(TL(S))=CONS((LIST)DOTDOT_OP,
                                             CONS(STONUM(M+1),B));
                                  REDS=REDS+1;
                                  continue;
                  case COMMADOTDOT_OP: {  WORD M1=M,N1=N;//REDUCE clobbers M,N
                                          LIST C=REDUCE(TL(TL(TL(S))));
                                          static WORD P=0;
                                          if ( ISNUM(C)
                                          ) P=GETNUM(C); else
                                          if ( C==INFINITY ) P=N1;
                                          else BADEXP(CONS(E,CONS(A,CONS(B,C))));
                                          IF (N1-M1)*(P-M1)<0 DO { E=NIL; break;  }
                                          E=(LIST)COLON_OP;
                                          HD(TL(TL(S)))=STONUM(N1+N1-M1);
                                          TL(TL(S))=CONS((LIST)COMMADOTDOT_OP,
                                                     CONS(B,TL(TL(S))));
                                          REDS=REDS+1;
                                          continue;  }
                  case NOT_OP: if ( A==TRUTH ) E=FALSITY; else
                               if ( A==FALSITY ) E=TRUTH; else
                               BADEXP(CONS(E,A));
			       break; 
                  case NEG_OP: UNLESS ISNUM(A) DO BADEXP(CONS(E,A));
                               E = STONUM(-GETNUM(A));
			       break; 
                  case LENGTH_OP: {  WORD L=0;
                                     WHILE ISCONS(A) && HD(A)==(LIST)COLON_OP
                                     DO A=REDUCE(TL(TL(A))),L=L+1;
                                     IF A==NIL DO { E = STONUM(L); break;  }
                                     BADEXP(CONS((LIST)COLON_OP,CONS((LIST)ETC,A)));
                                  }
                  case PLUS_OP: { WORD X = M+N;
				  IF (M>0 && N>0 && X <= 0) ||
				     (M<0 && N<0 && X >= 0) ||
                                     // This checks for -(2**31)
                                     (X==-X && X!=0) DO
                                        OVERFLOW(CONS((LIST)PLUS_OP,CONS(A,B)));
                                  E = STONUM(X); break;   }
                  case MINUS_OP: { WORD X = M-N;
                                   IF (M<0 && N>0 && X>0) ||
                                      (M>0 && N<0 && X<0) ||
                                      (X==-X && X!=0) DO
                                        OVERFLOW(CONS((LIST)MINUS_OP,CONS(A,B)));
                                   E = STONUM(X); break;   }
                  case TIMES_OP: { WORD X = M*N;
				   // May not catch all cases
                                   IF (M>0 && N>0 && X<=0) ||
                                      (M<0 && N<0 && X<=0) ||
                                      (M<0 && N>0 && X>=0) ||
                                      (M>0 && N<0 && X>=0) ||
                                      (X==-X && X!=0) DO
                                        OVERFLOW(CONS((LIST)TIMES_OP,CONS(A,B)));
                                   E = STONUM(X); break;   }
                  case DIV_OP: IF N==0 DO BADEXP(CONS((LIST)DIV_OP,CONS(A,B)));
                               E = STONUM(M/N); break; 
                  case REM_OP: IF N==0 DO BADEXP(CONS((LIST)REM_OP,CONS(A,B)));
                               E = STONUM(M%N); break; 
                  case EXP_OP:   IF N<0 DO BADEXP(CONS((LIST)EXP_OP,CONS(A,B)));
                              {  WORD P=1;
                                 UNTIL N==0
				 DO { WORD X=P*M;
				      // May not catch all cases
                                      IF (M>0 && P>0 && X<=0) ||
                                         (M<0 && P<0 && X<=0) ||
                                         (M<0 && P>0 && X>=0) ||
                                         (M>0 && P<0 && X>=0) ||
                                         (X==-X && X!=0) DO
                                           OVERFLOW(CONS((LIST)EXP_OP,CONS(A,B)));
				      P=X, N=N-1; }
                                 E = STONUM(P); break;   }
                  case GR_OP: E = (STRINGS?ALFA_LS(SN,SM):M>N)?
                                        TRUTH: FALSITY; break; 
                  case GE_OP: E = (STRINGS?ALFA_LS(SN,SM)||SN==SM:M>=N)?
                                        TRUTH: FALSITY; break; 
                  case LE_OP: E = (STRINGS?ALFA_LS(SM,SN)||SM==SN:M<=N)?
                                        TRUTH: FALSITY; break; 
                  case LS_OP: E = (STRINGS?ALFA_LS(SM,SN):M<N)?
                                        TRUTH: FALSITY; break; 
                  default: WRITES("IMPOSSIBLE OPERATOR IN \"REDUCE\"\n");
               } //END OF SWITCH
         {  LIST HOLD=HD(S);
            NARGS=NARGS-1;
            HD(S)=(LIST)INDIR,TL(S)=E,S=HOLD;  }
         } } //END OF OPERATORS
      REDS=REDS+1;
   } while(1); //END OF MAIN LOOP
BREAK_MAIN_LOOP:
   UNTIL S==(LIST)ENDOFSTACK   //UNREVERSE REVERSED POINTERS
   DO {  LIST HOLD=HD(S);
         HD(S)=E,E=S,S=HOLD;  }
   IF HOLDARG==(LIST *)-1 DO BADEXP(E);
   ARG=HOLDARG;  //RESET ARG STACKFRAME
   S=HOLD_S;
   return E;
}

static LIST
SUBSTITUTE(LIST ACTUAL,LIST FORMAL,LIST EXP)
{    if ( EXP==FORMAL ) return ACTUAL;
     else if ( !ISCONS(EXP) || HD(EXP)==(LIST)QUOTE || BINDS(FORMAL,HD(EXP))
     ) return EXP; else
     {  LIST H=SUBSTITUTE(ACTUAL,FORMAL,HD(EXP));
        LIST T=SUBSTITUTE(ACTUAL,FORMAL,TL(EXP));
        return H==HD(EXP) && T==TL(EXP) ? EXP : CONS(H,T);  }
}

static BOOL
BINDS(LIST FORMAL,LIST X)
{  return ISCONS(X) && HD(X)==(LIST)GENERATOR && HD(TL(X))==FORMAL;  }

// Mark elements in the argument stack for preservation by the GC.
// This routine should be called by your BASES() function.
void
REDUCER_BASES(void (*F)(LIST *))
{  LIST *AP;

   for (AP=ARGSPACE; AP<=ARGP; AP++)
      F(AP);
}


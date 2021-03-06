#include "listhdr.h"
#include "comphdr.h"
#include "redhdr.h"
#include "emas.h"
#include "revision"
#ifdef LINENOISE
# include "linenoise.h"
#endif

//----------------------------------------------------------------------
//The KRC system is Copyright (c) D. A. Turner 1981
//All  rights reserved.  It is distributed as free software under the
//terms in the file "COPYING", which is included in the distribution.
//----------------------------------------------------------------------

//#include <ctype.h>	// for toupper()
#include <setjmp.h>
#include <string.h>	// for strcmp()
#include <unistd.h>	// for fork(), stat()
#include <sys/types.h>	// for sys/wait.h, stat()
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

// Local function declarations
static void DIRCOM();
static void DISPLAYCOM();
static void QUITCOM();
static void OBJECTCOM();
static void RESETCOM();
static void GCCOM();
static void COUNTCOM();
static void SAVECOM();
static void FILECOM();
static void GETCOM();
static void LISTCOM();
static void NAMESCOM();
static void LIBCOM();
static void CLEARCOM();
static void OPENLIBCOM();
static void HELPCOM();
static void RENAMECOM();
static void ABORDERCOM();
static void REORDERCOM();
static void DELETECOM();
static BOOL STARTDISPLAYCOM();

static void PARSELINE(char *line);
static void INITIALISE();
static void ENTERARGV(int USERARGC, LIST USERARGV);
static void SETUP_COMMANDS();
static void COMMAND();
static void DISPLAYALL(BOOL DOUBLESPACING);
static BOOL MAKESURE();
static void FILENAME();
static BOOL OKFILE(FILE *STR, char *FILENAME);
static void CHECK_HITS();
static BOOL GETFILE(char *FILENAME);
static void FIND_UNDEFS();
static BOOL ISDEFINED(ATOM X);
static void SCRIPTLIST(LIST S);
static LIST SUBST(LIST Z,LIST A);
static void NEWEQUATION();
static void CLEARMEMORY();
static void COMMENT();
static void EVALUATION();
static LIST SORT(LIST X);
static void SCRIPTREORDER();
static WORD NO_OF_EQNS(ATOM A);
static BOOL PROTECTED(ATOM A);
static BOOL PRIMITIVE(ATOM A);
static void REMOVE(ATOM A);
static LIST EXTRACT(ATOM A, ATOM B);

static LIST COMMANDS=NIL, SCRIPT=NIL, OUTFILES=NIL;	//BASES
static ATOM LASTFILE=0;					//BASES

static LIST LIBSCRIPT=NIL, HOLDSCRIPT=NIL, GET_HITS=NIL; //BASES
static BOOL SIGNOFF=FALSE, SAVED=TRUE, EVALUATING=FALSE;
static BOOL ATOBJECT=FALSE, ATCOUNT=FALSE; //FLAGS USED IN DEBUGGING SYSTEM

// Global variables owned by main.c
WORD LEGACY=FALSE; //set by -z option
LIST FILECOMMANDS = NIL;
char *USERLIB=NULL; //SET BY -l OPTION

// Local variables
static BOOL FORMATTING;		// Are we evaluating with '?' ?

static BOOL QUIET = FALSE; 	// Suppress greetings, prompts etc.?
static char *EVALUATE = NULL;	// Expression to execute in batch mode

// INITIALISATION AND STEERING

void ESCAPETONEXTCOMMAND(void);

// Are we ignoring interrupts?
static BOOL INTERRUPTS_ARE_HELD = FALSE;
// Was an interrupt delivered while we were ignoring them?
static BOOL INTERRUPT_OCCURRED = FALSE;

static void
CATCHINTERRUPT(int signum)
{  IF INTERRUPTS_ARE_HELD DO {
      INTERRUPT_OCCURRED = signum;	// Can't be 0
      return;
   }
   FIXUP_S();	   //IN CASE INTERRUPT STRUCK WHILE REDUCE
                   //WAS DISSECTING A CONSTANT
   _WRCH=TRUEWRCH;
   CLOSECHANNELS();
   UNLESS QUIET || ABORTED  // die quietly if running as script or ABORT() called
   DO //WRITES("\n**break in - return to KRC command level**\n");
      WRITES("<interrupt>\n");
   ABORTED=FALSE;
   ESCAPETONEXTCOMMAND();  }


void
HOLD_INTERRUPTS() {  INTERRUPTS_ARE_HELD = TRUE;  }

void
RELEASE_INTERRUPTS()
   {  INTERRUPTS_ARE_HELD = FALSE;
      IF INTERRUPT_OCCURRED DO {
         INTERRUPT_OCCURRED=FALSE;
         CATCHINTERRUPT(INTERRUPT_OCCURRED);
   }  }

         //ESSENTIAL THAT DEFINITIONS OF THE ABOVE SHOULD BE PROVIDED IF
         //THE PACKAGE IS TO BE USED IN AN INTERACTIVE PROGRAM

// Where to jump back to on runtime errors or keyboard interrupts
static jmp_buf nextcommand;

void ESCAPETONEXTCOMMAND()
   {  _WRCH=TRUEWRCH;
      IF INPUT()!=SYSIN DO {  ENDREAD() ; SELECTINPUT(SYSIN); }
      CLOSECHANNELS();
      IF EVALUATING
      DO {  IF ATCOUNT DO OUTSTATS();
	    CLEARMEMORY(); //IN CASE SOME POINTERS HAVE BEEN LEFT REVERSED
	    EVALUATING=FALSE;  }
      IF HOLDSCRIPT!=NIL 
      DO {  SCRIPT=HOLDSCRIPT, HOLDSCRIPT=NIL;
	    CHECK_HITS(); }
      INIT_CODEV();
      INIT_ARGSPACE();
      longjmp(nextcommand, 1);  }

// Buffer for signal handling
static struct sigaction act;	// All initialised to 0/NULL is fine.

void
GO()
   {  // STACKLIMIT:= @V4 + 30000  //IMPLEMENTATION DEPENDENT,TO TEST FOR RUNAWAY RECURSION
      IF setjmp(nextcommand) == 0 DO
      {  // First-time initialization
	 INIT_CODEV();
	 INIT_ARGSPACE();
	 INITIALISE();
	 // Set up the interrupt handler
         act.sa_handler = CATCHINTERRUPT;
	 act.sa_flags = SA_NODEFER; // Bcos the interrupt handler never returns
         sigaction(SIGINT, &act, NULL);
      } else {
	 // When the GC is called from CONS() from the depths of an
	 // evaluation, it is more likely that stale pointers left in
	 // registers, either still in them or saved on the stack,
	 // will cause now-unused areas of the heap to be preserved.
	 // We mitigate this by calling the GC here, after an interrupt
	 // or an out-of-space condition, when the stack is shallow and
	 // the registers are less likely to contain values pointing
	 // inside the CONS space.
	 BOOL HOLDATGC=ATGC; ATGC=FALSE;
	 FORCE_GC();
	 ATGC=HOLDATGC;
      }
      // Both initially and on longjump, continue here.
      IF EVALUATE && !SIGNOFF DO {
	 SIGNOFF=TRUE;  // Quit on errors or interrupts
	 PARSELINE(EVALUATE);
	 if ( EXPFLAG ) EVALUATION(); else
	   WRITES("-e takes an expression followed by ? or !\n");
	 IF ERRORFLAG
         DO SYNTAX_ERROR("malformed expression after -e\n");
      }
      UNTIL SIGNOFF DO COMMAND();
      QUITCOM();
//    FINISH //moved inside QUITCOM()
   }


// PARSELINE: A version of READLINE that gets its input from a string

static char *input_line;

// Alternative version of RDCH that gets its chars from a string
static int
str_RDCH(void)
{
   IF input_line==NULL DO return EOF;
   IF *input_line=='\0' DO {  input_line=NULL;
				return '\n';  }
   return *input_line++;
}

static int
str_UNRDCH(int c)
{
   if ( input_line==NULL && c=='\n'
   ) input_line="\n";
   else *(--input_line)=c;
   return c;
}

// SAME AS READLINE, BUT GETS ITS INPUT FROM A C STRING
static void
PARSELINE(char *line)
{  input_line=line;
   _RDCH=str_RDCH, _UNRDCH=str_UNRDCH;
   READLINE();
   _RDCH=bcpl_RDCH, _UNRDCH=bcpl_UNRDCH;
}

// ----- END OF PARSELINE

static char TITLE[] = "Kent Recursive Calculator 1.0";

// Where to look for "prelude" and other files KRC needs
#ifndef LIBDIR
#define LIBDIR "/usr/lib/krc"
#endif
//but use krclib in current directory if present, see below

static void
INITIALISE()
   {  BOOL LOADPRELUDE=TRUE;	// Do we need to read the prelude?
      BOOL OLDLIB=FALSE;        // Use legacy prelude?
      char *USERSCRIPT=NULL;	// Script given on command line
      LIST USERARGV=NIL;        // Reversed list of args after script name
      int  USERARGC=0;	        // How many items in USERARGV?
//    BOOL  LISTSCRIPT=FALSE;	// List the script as we read it?
      int  I;

      IF !isatty(0) DO QUIET=TRUE;

      SETUP_PRIMFNS_ETC();
      for (I=1; I<ARGC; I++) {
         if ( ARGV[I][0]=='-' )
            switch( ARGV[I][1] ) {
	    case 'n': LOADPRELUDE=FALSE;
		      break; 
            case 's': SKIPCOMMENTS=TRUE;
                      break; 
            case 'c': ATCOUNT=TRUE; break; 
            case 'o': ATOBJECT=TRUE; break; 
            case 'd':		// Handled in listpack.c
            case 'l':		// Handled in listpack.c
            case 'h': ++I;	// Handled in listpack.c
            case 'g':		// Handled in listpack.c
		      break; 
            case 'e': IF ++I>=ARGC || ARGV[I][0] == '-'
		      DO {  WRITES("krc: -e What?\n"); FINISH  }
		      IF EVALUATE
		      DO {  WRITES("krc: Only one -e flag allowed\n"); FINISH  }
		      EVALUATE=ARGV[I];
                      QUIET=TRUE;
                      break; 
            case 'z': LISTBASE=1;
                      LEGACY=TRUE;
                      WRITES("LISTBASE=1\n");
                      break; 
            case 'L': OLDLIB=1; break; 
//          case 'v': LISTSCRIPT=TRUE; break; 
            // Other parameters may be detected using HAVEPARAM()
            case 'C': case 'N': case 'O': //used only by testcomp, disabled
	    default:  WRITEF("krc: invalid option -%c\n",ARGV[I][1]);
                      FINISH
		      break; 
         } else {
	    // Filename of script to load, or arguments for script
	    IF USERSCRIPT==NULL DO USERSCRIPT=ARGV[I]; //was if (...OR
	    USERARGV=CONS((LIST)MKATOM(ARGV[I]), USERARGV), USERARGC++;
      }  }
      if ( EVALUATE ) ENTERARGV(USERARGC, USERARGV);
      else IF USERARGC>1 DO { WRITES("krc: too many arguments\n"); FINISH }
      if ( LOADPRELUDE )
           if ( USERLIB ) GETFILE(USERLIB); //-l option was used
           else { struct stat buf;
                if ( stat("krclib",&buf)==0
                ) GETFILE(OLDLIB?"krclib/lib1981":"krclib/prelude");
                else GETFILE(OLDLIB?LIBDIR "/lib1981":LIBDIR "/prelude"); }
      else // if ( USERLIB || OLDLIB )
         // { WRITES("krc: invalid combination -n and -l or -L\n"); FINISH } else
         WRITES("\"PRELUDE\" suppressed\n");
      SKIPCOMMENTS=FALSE;  //effective only for prelude
      LIBSCRIPT=SORT(SCRIPT),SCRIPT=NIL;
      IF USERSCRIPT DO {
//      IF LISTSCRIPT DO _RDCH=echo_RDCH;
	GETFILE(USERSCRIPT);
        SAVED=TRUE;
//      IF LISTSCRIPT DO _RDCH=bcpl_RDCH;
	LASTFILE=MKATOM(USERSCRIPT);
      }
      SETUP_COMMANDS();
      RELEASE_INTERRUPTS();
      IF !QUIET DO WRITEF("%s\nrevised %s\n%s\n",TITLE,revision,
//                        "http://krc-lang.org",
                          "/h for help");
   }

// Given the (reverse-order) list of atoms made from command-line arguments
// supplied after the name of the script file, create their an entry in the
// script called "argv" for the krc program to access them.
// We create it as a list of strings (i.e. a list of atoms) for which
// the code for a three-element list of string is:
// ( (0x0.NIL).  :- 0 parameters, no comment
//   ( 0.      :- memo field unset
//     LOAD.(QUOTE."one").LOAD.(QUOTE."two").LOAD.(QUOTE."three").
//     FORMLIST.0x03.STOP.NIL ).
//   NIL )
static void
ENTERARGV(int USERARGC, LIST USERARGV)
{  
   ATOM A=MKATOM("argv");
   LIST CODE=CONS((LIST)FORMLIST_C,
		  CONS((LIST)USERARGC,
                       CONS((LIST)STOP_C, NIL)));
   for ( ;USERARGV != NIL; USERARGV=TL(USERARGV))
      CODE=CONS((LIST)LOAD_C,
                CONS(CONS((LIST)QUOTE, HD(USERARGV)),CODE));
   VAL(A) = CONS(CONS((LIST)0, NIL),
                 CONS(CONS((LIST)0,CODE),
                      NIL));
   ENTERSCRIPT(A);
}

void
SPACE_ERROR(char *MESSAGE)
{  _WRCH=TRUEWRCH;
   CLOSECHANNELS();
   if ( EVALUATING
   ) {  WRITEF("\n**%s**\n**evaluation abandoned**\n",MESSAGE);
           ESCAPETONEXTCOMMAND();  } else
   if ( MEMORIES==NIL
   )
   {  WRITEF("\n%s - recovery impossible\n", MESSAGE);
      FINISH  }
   else CLEARMEMORY();  //LET GO OF MEMOS AND TRY TO CARRY ON
}

void
BASES(void (*F)(LIST *)) {
extern LIST S;	// In reducer.c
      F(&COMMANDS);
      F(&FILECOMMANDS);
      F(&SCRIPT);
      F(&LIBSCRIPT);
      F(&HOLDSCRIPT);
      F(&GET_HITS);
      F((LIST *)&LASTFILE);
      F(&OUTFILES);
      F(&MEMORIES);
      F(&S);
      F(&TOKENS);
      F((LIST *)&THE_ID);
      F(&THE_CONST);
      F(&LASTLHS);
      F(&TRUTH);
      F(&FALSITY);
      F(&INFINITY);
      COMPILER_BASES(F);
      REDUCER_BASES(F);
}

static void
SETUP_COMMANDS()
   {
#define F(S,R) { COMMANDS=CONS(CONS((LIST)MKATOM(S),(LIST)R),COMMANDS); }
#define FF(S,R) { FILECOMMANDS=CONS((LIST)MKATOM(S),FILECOMMANDS); F(S,R); }
      F("delete",DELETECOM);
      F("d",DELETECOM); //SYNONYM
      F("reorder",REORDERCOM);
      FF("save",SAVECOM);
      FF("get",GETCOM);
      FF("list",LISTCOM);
      FF("file",FILECOM);
      FF("f",FILECOM);
      F("dir",DIRCOM);
      F("quit",QUITCOM);
      F("q",QUITCOM); //SYNONYM
      F("names",NAMESCOM);
      F("lib",LIBCOM);
      F("aborder",ABORDERCOM);
      F("rename",RENAMECOM);
      F("openlib",OPENLIBCOM);
      F("clear",CLEARCOM);
      F("help",HELPCOM);
      F("h",HELPCOM); //SYNONYM
      F("object",OBJECTCOM);  //THESE LAST COMMANDS ARE FOR USE IN
      F("reset",RESETCOM);    //DEBUGGING THE SYSTEM
      F("gc",GCCOM);
      F("dic",REPORTDIC);
      F("count",COUNTCOM);
      F("lpm",LISTPM);
#undef FF
#undef F
   }

static void
DIRCOM()
   {  int status;
      switch (fork()) {
      case 0: execlp("ls", "ls", NULL); break;
      case -1: break;
      default: wait(&status);
   }  }

void
CLOSECHANNELS()
   {  IF !EVALUATING && OUTPUT()!=SYSOUT DO ENDWRITE();
      UNTIL OUTFILES==NIL
      DO {  SELECTOUTPUT((FILE *)TL(HD(OUTFILES)));
            IF FORMATTING DO NEWLINE();
            ENDWRITE();
            OUTFILES=TL(OUTFILES); }
      SELECTOUTPUT(SYSOUT);
   }

FILE *
FINDCHANNEL(char *F)
{  LIST P=OUTFILES;
   UNTIL P==NIL || strcmp((char *)HD(HD(P)),F) == 0
   DO P=TL(P);
   if ( P==NIL
   ) {  FILE *OUT = FINDOUTPUT(F);
           IF OUT != NULL
           DO OUTFILES=CONS(CONS((LIST)F,(LIST)OUT),OUTFILES);
           return OUT; }
   else return (FILE *)TL(HD(P));
}

// COMMAND INTERPRETER
// EACH COMMAND IS TERMINATED BY A NEWLINE
// <COMMAND>::= /<EMPTY> |    (DISPLAYS WHOLE SCRIPT)
//              /DELETE <THINGY>* |   
//                  (IF NO <THINGY>'S ARE SPECIFIED IT DELETES WHOLE SCRIPT)
//              /DELETE <NAME> <PART>* |
//              /REORDER <THINGY>* |
//              /REORDER <NAME> <PART>* |
//              /ABORDER |
//              /SAVE "<FILENAME>" |
//              /GET "<FILENAME>" |
//              /LIST "<FILENAME>" |
//              /FILE  |
//              /QUIT  |
//              /NAMES |
//              /OPEN|
//              /CLEAR |
//              /LIB   |
//              <NAME> |     (DISPLAYS EQNS FOR THIS NAME)
//              <NAME> .. <NAME> |    (DISPLAYS A SECTION OF THE SCRIPT)
//              <EXP>? |     (EVALUATE AND PRINT)
//              <EXP>! |     (SAME BUT WITH UNFORMATTED PRINTING)
//              <EQUATION>    (ADD TO SCRIPT)
// <THINGY> ::= <NAME> | <NAME> .. <NAME> | <NAME> ..
// <PART> ::= <INT> | <INT>..<INT> | <INT>..

//static char *HELP[] = { //replaced by HELPCOM() see below
//"/                  Displays the whole script",
//"/delete NAMES      Deletes the named functions. /d deletes everything",
//"/delete NAME PARTS Deletes the numbered equations from function NAME",
//"/reorder NAME NAMES Moves the equations for NAMES after those for NAME",
//"/reorder NAME PARTS Redefines the order of NAME's equations",
//"/aborder           Sorts the script into alphabetical order",
//"/rename FROMs,TOs  Changes the names of one or more functions",
//"/save FILENAME     Saves the script in the named file",
//"/get FILENAME      Adds the contents of a file to the script",
//"/list FILENAME     Displays the contents of a disk file",
//"/file (or /f)      Shows the current default filename",
//"/file FILENAME     Changes the default filename",
//"/dir               List filenames in current directory/folder",
//"/quit (or /q)      Ends this KRC session",
//"/names             Displays the names defined in your script",
//"/openlib           Allows you to modify equations in the prelude/library",
//"/clear             Clears the memo fields for all variables",
//"/lib               Displays the names defined in the prelude/library",
//"NAME               Displays the equations defined for the function NAME",
//"NAME..NAME         Displays a section of the script",
//"EXP?               Evaluates an expression and pretty-print the result",
//"EXP!               The same but with unformatted output",
//"EQUATION           Adds an equation to the script",
//"   NAMES ::= NAME | NAME..NAME | NAME..   PARTS ::= INT | INT..INT | INT..",
//NULL,
//};
//
//static void
//SHOWHELP()
//{
//	char **h;
//	for (h=HELP; *h; h++) printf("%s\n", *h);
//}

#define KRCPAGER "less -F -X -P'%F (press q to quit)' "
#define HELPLOCAL KRCPAGER "krclib/help/"
#define HELP KRCPAGER LIBDIR "/help/"
#define BUFLEN 80

static void
HELPCOM()
{ struct stat buf;
  char strbuf[BUFLEN+1],*topic;
  int local=stat("krclib",&buf)==0,r;
  if ( HAVE(EOL)
  ) { if ( local
         ) r=system(HELPLOCAL "menu");
         else r=system(HELP "menu");
         return; }
  topic = HAVEID()?PRINTNAME(THE_ID):NULL;
  UNLESS topic && HAVE(EOL)
  DO { WRITES("/h What? `/h' for options\n");
       return; }
  snprintf(strbuf, sizeof(strbuf), "%s%s", local?HELPLOCAL:HELP, topic);
  r=system(strbuf); }

static void
COMMAND()
   {
      static char prompt[]="krc> ";
#ifdef LINENOISE
      char *line=linenoise(QUIET ? "" : prompt);
      if (line && line[0] == '\0') return;      // Otherwise the interpreter exits
      PARSELINE(line);                          // Handles NULL->EOF OK
      IF HAVE(EOL) DO { free(line); return; }   //IGNORE BLANK LINES
      if (line) {
         linenoiseHistoryAdd(line);
         free(line);
      }
#else
      IF !QUIET DO PROMPT(prompt); // ON EMAS PROMPTS REMAIN IN EFFECT UNTIL CANCELLED
      READLINE();
      IF HAVE(EOL) DO return; //IGNORE BLANK LINES
      SUPPRESSPROMPTS();  // CANCEL PROMPT (IN CASE COMMAND READS DATA)
#endif
      if ( HAVE((TOKEN)EOF)
      ) SIGNOFF=TRUE; else
      if ( HAVE((TOKEN)'/')
      ) if ( HAVE(EOL)
           ) DISPLAYALL(FALSE); else
           // if ( HAVE((TOKEN)'@') && HAVE(EOL)
           // ) LISTPM(); else  //FOR DEBUGGING THE SYSTEM
           {  LIST P=COMMANDS;
              if ( HAVEID()
              ) THE_ID=MKATOM(SCASECONV(PRINTNAME(THE_ID)));
                 //ALWAYS ACCEPT COMMANDS IN EITHER CASE
              else P=NIL;
              UNTIL P==NIL || THE_ID==(ATOM)HD(HD(P)) DO P=TL(P);
              if ( P==NIL
              ) //SHOWHELP();
                   WRITES("command not recognised\nfor help type /h\n");
              else ((void (*)())TL(HD(P)))();    // SEE "SETUP_COMMANDS()"
           } else
      if ( STARTDISPLAYCOM() ) DISPLAYCOM(); else
      if ( COMMENTFLAG>0 ) COMMENT(); else
      if ( EQNFLAG ) NEWEQUATION();
      else EVALUATION();
      IF ERRORFLAG DO SYNTAX_ERROR("**syntax error**\n");
   }

static BOOL
STARTDISPLAYCOM()
{ LIST HOLD=TOKENS;
  WORD  R=HAVEID() && (HAVE(EOL) || HAVE((TOKEN)DOTDOT_SY));
  TOKENS=HOLD;
  return R;
}

static void
DISPLAYCOM()
{  if ( HAVEID()
   ) if ( HAVE(EOL)
        ) DISPLAY(THE_ID,TRUE,FALSE); else
        if ( HAVE((TOKEN)DOTDOT_SY)
        ) {  ATOM A = THE_ID; LIST X=NIL;
                ATOM B = HAVE(EOL) ? (ATOM)EOL :	// BUG?
                        HAVEID() && HAVE(EOL) ? THE_ID :
                        0;
                if ( B==0 ) SYNTAX();
                else X=EXTRACT(A,B);
                UNTIL X==NIL
                DO {  DISPLAY((ATOM)HD(X),FALSE,FALSE);
                      X=TL(X);  }  } //could insert extra line here between groups
        else SYNTAX();
   else SYNTAX();
}

static void
DISPLAYALL(BOOL DOUBLESPACING)  // "SCRIPT" IS A LIST OF ALL USER DEFINED
                                // NAMES IN ALPHABETICAL ORDER
   {  LIST P=SCRIPT;
      IF P==NIL DO WRITES("Script=empty\n");
      UNTIL P==NIL DO { UNLESS PRIMITIVE((ATOM)HD(P))
                        //don't display builtin fns (relevant only in /openlib)
                        DO DISPLAY((ATOM)HD(P),FALSE,FALSE);
                        P=TL(P);  
                        IF DOUBLESPACING && P != NIL
                        //extra line between groups
                        DO NEWLINE(); }
   }

static BOOL
PRIMITIVE(ATOM A)
{ IF TL(VAL(A))==NIL DO return FALSE; //A has comment but no eqns
  return HD(TL(HD(TL(VAL(A)))))==(LIST)CALL_C; }

static void
QUITCOM()
   {  IF TOKENS!=NIL DO CHECK(EOL);
      IF ERRORFLAG DO return;
      IF MAKESURE()
      DO { WRITES("krc logout\n");
           FINISH  }
   }

static BOOL
MAKESURE()
{  IF SAVED || SCRIPT==NIL DO return TRUE;
   WRITES("Are you sure? ");
{  WORD CH=RDCH(), C;
   UNRDCH(CH);
   UNTIL (C=RDCH())=='\n' || C == EOF DO continue;
   IF CH=='y' || CH=='Y' DO return TRUE;
   WRITES("Command ignored\n");
   return FALSE;
}  }

static void
OBJECTCOM()
{  ATOBJECT=TRUE;  }

static void
RESETCOM()
{  ATOBJECT=FALSE,ATCOUNT=FALSE,ATGC=FALSE;  }

static void
GCCOM()
   {  ATGC=TRUE;
      FORCE_GC();  }

static void
COUNTCOM()
{  ATCOUNT=TRUE;  }

static void
SAVECOM()
   {  FILENAME();
      IF ERRORFLAG DO return;
      IF SCRIPT==NIL
      DO {  WRITES("Cannot save empty script\n");
            return;  }
   {  
      FILE *OUT = FINDOUTPUT("T#SCRIPT");
      SELECTOUTPUT(OUT);
      DISPLAYALL(TRUE);
      ENDWRITE();
      SELECTOUTPUT(SYSOUT);
      // Copy T#SCRIPT back to the save file.
      {  int status;
         switch (fork()) {
         case 0:  execlp("mv", "mv", "T#SCRIPT", PRINTNAME(THE_ID), (char *)0);
         default: wait(&status);
		  if (status == 0) SAVED=TRUE;
		  else /* Drop into... */
         case -1:    WRITES("File saved in T#SCRIPT.\n"); break;
		  break;
}  }  }  }

static void
FILENAME()
{  if ( HAVE(EOL)
   ) if ( LASTFILE==0
        ) {  WRITES("(No file set)\n") ; SYNTAX();  }
        else THE_ID=LASTFILE;
   else if ( HAVEID() && HAVE(EOL)
      ) LASTFILE=THE_ID;
      else {  IF HAVECONST() && HAVE(EOL) && !ISNUM(THE_CONST)
            DO WRITES("(Warning - quotation marks no longer expected around filenames in file commands - DT, Nov 81)\n");
            SYNTAX(); }
}

static void
FILECOM()
{  if ( HAVE(EOL)
   ) if ( LASTFILE==0
        ) WRITES("No files used\n");
        else WRITEF("File = %s\n",PRINTNAME(LASTFILE));
   else FILENAME();
}

static BOOL
OKFILE(FILE *STR, char *FILENAME)
{  IF STR!=NULL DO return TRUE;
   WRITEF("Cannot open \"%s\"\n",FILENAME);
   return FALSE; }

static void
GETCOM()
   {  BOOL CLEAN = SCRIPT==NIL;
      FILENAME();
      IF ERRORFLAG DO return;
      HOLDSCRIPT=SCRIPT,SCRIPT=NIL,GET_HITS=NIL;
      GETFILE(PRINTNAME(THE_ID));
      CHECK_HITS();
      SCRIPT=APPEND(HOLDSCRIPT,SCRIPT),SAVED=CLEAN,HOLDSCRIPT=NIL;
   }

static void
CHECK_HITS()
{  UNLESS GET_HITS==NIL
   DO {  WRITES("Warning - /get has overwritten or modified:\n");
         SCRIPTLIST(REVERSE(GET_HITS));
         GET_HITS=NIL;  }
}

static BOOL
GETFILE(char *FILENAME)
   {  FILE *IN = FINDINPUT(FILENAME);
      UNLESS OKFILE(IN,FILENAME) DO return FALSE;
      SELECTINPUT(IN);
   {  int line=0; //to locate line number of error in file
      do{line++;
         READLINE();
	 IF ferror(IN) DO {
	    ERRORFLAG=TRUE;
	    break;;
         }
         IF HAVE(EOL) DO continue;;  
         IF HD(TOKENS)==ENDSTREAMCH
         DO break;
         if ( COMMENTFLAG
         ) { line+=(COMMENTFLAG-1);
                COMMENT(); }
         else NEWEQUATION();
         IF ERRORFLAG
         DO { SYNTAX_ERROR("**syntax error in file ");
              WRITEF("%s at line %d\n",FILENAME,line); }
      } while(1);
      ENDREAD();
      SELECTINPUT(SYSIN);
      LASTLHS=NIL;
      return TRUE;  }}

static void
LISTCOM()
   {  FILENAME();
      IF ERRORFLAG DO return;
   {  char *FNAME=PRINTNAME(THE_ID);
      FILE *IN=FINDINPUT(FNAME);
      UNLESS OKFILE(IN,FNAME) DO return;
      SELECTINPUT(IN);
   {  WORD CH=RDCH();
      UNTIL CH==EOF
      DO  {  WRCH(CH); CH=RDCH();  }
      ENDREAD();
      SELECTINPUT(SYSIN);
}  }  }

static void
NAMESCOM()
   {  CHECK(EOL);
      IF ERRORFLAG DO return;
      if ( SCRIPT==NIL
      ) DISPLAYALL(FALSE);
      else  {  SCRIPTLIST(SCRIPT); FIND_UNDEFS();  }
   }

static void
FIND_UNDEFS()  //SEARCHES THE SCRIPT FOR NAMES USED BUT NOT DEFINED
   {  LIST S=SCRIPT, UNDEFS=NIL;
      UNTIL S==NIL
      DO {  LIST EQNS = TL(VAL((ATOM)HD(S)));
            UNTIL EQNS==NIL
            DO {  LIST CODE = TL(HD(EQNS));
                  WHILE ISCONS(CODE)
                  DO {  LIST A = HD(CODE);
                        IF ISATOM(A) && !ISDEFINED((ATOM)A) && !MEMBER(UNDEFS,A)
                        DO UNDEFS=CONS(A,UNDEFS);
                        CODE=TL(CODE);  }
                  EQNS=TL(EQNS);  }
            S=TL(S);  }
      UNLESS UNDEFS==NIL
      DO {  WRITES("\nNames used but not defined:\n");
            SCRIPTLIST(REVERSE(UNDEFS));  }
   }

static BOOL
ISDEFINED(ATOM X)
{  return VAL(X)==NIL||TL(VAL(X))==NIL ? FALSE : TRUE;  }

static void
LIBCOM()
   {  CHECK(EOL);
      IF ERRORFLAG DO return;
      if ( LIBSCRIPT==NIL
      ) WRITES("library = empty\n");
      else SCRIPTLIST(LIBSCRIPT);  }
 
static void
CLEARCOM()
   {  CHECK(EOL);
      IF ERRORFLAG DO return;
      CLEARMEMORY();  }

static void
SCRIPTLIST(LIST S)
   {  WORD COL=0,I=0;
#define LINEWIDTH 68  //THE MINIMUM OF VARIOUS DEVICES
      UNTIL S==NIL
      DO {  char *N = PRINTNAME((ATOM)HD(S));
            IF PRIMITIVE((ATOM)HD(S)) DO {S=TL(S); continue;}
            COL=COL+strlen(N)+1;
            IF COL>LINEWIDTH
            DO  {  COL=0 ; NEWLINE();  }
            WRITES(N);
            WRCH(' ');
            I=I+1,S=TL(S);  }
      IF COL+6>LINEWIDTH DO NEWLINE();
      WRITEF(" (%" W ")\n",I);
   }

static void
OPENLIBCOM()
   {  CHECK(EOL);
      IF ERRORFLAG DO return;
      SAVED=SCRIPT==NIL;
      SCRIPT=APPEND(SCRIPT,LIBSCRIPT);
      LIBSCRIPT=NIL;
   }

static void
RENAMECOM()
   {  LIST X=NIL,Y=NIL,Z=NIL;
      WHILE HAVEID() DO X=CONS((LIST)THE_ID,X);
      CHECK((TOKEN)',');
      WHILE HAVEID() DO Y=CONS((LIST)THE_ID,Y);
      CHECK(EOL);
      IF ERRORFLAG DO return;
      {  //FIRST CHECK LISTS ARE OF SAME LENGTH
         LIST X1=X,Y1=Y;
         UNTIL X1==NIL||Y1==NIL DO Z=CONS(CONS(HD(X1),HD(Y1)),Z),X1=TL(X1),Y1=TL(Y1);
         UNLESS X1==NIL && Y1==NIL && Z!=NIL DO { SYNTAX(); return;  }  }
      {  // NOW CHECK LEGALITY OF RENAME
         LIST Z1=Z,POSTDEFS=NIL,DUPS=NIL;
         UNTIL Z1==NIL
         DO {  IF MEMBER(SCRIPT,HD(HD(Z1)))
               DO POSTDEFS=CONS(TL(HD(Z1)),POSTDEFS);
               IF ISDEFINED((ATOM)TL(HD(Z1))) && (!MEMBER(X,TL(HD(Z1))) || !MEMBER(SCRIPT,TL(HD(Z1))) )
               DO POSTDEFS=CONS(TL(HD(Z1)),POSTDEFS);
               Z1=TL(Z1);  }
         UNTIL POSTDEFS==NIL
         DO {  IF MEMBER(TL(POSTDEFS),HD(POSTDEFS)) &&
                 !MEMBER(DUPS,HD(POSTDEFS)) DO DUPS=CONS(HD(POSTDEFS),DUPS);
               POSTDEFS=TL(POSTDEFS); }
         UNLESS DUPS==NIL
         DO {  WRITES("/rename illegal because of conflicting uses of ");
               UNTIL DUPS==NIL
               DO  {  WRITES(PRINTNAME((ATOM)HD(DUPS)));
                      WRCH(' ');
                      DUPS=TL(DUPS);  }
               NEWLINE();
               return;  }  }
      HOLD_INTERRUPTS();
      CLEARMEMORY();
    //PREPARE FOR ASSIGNMENT TO VAL FIELDS
   {  LIST X1=X,XVALS=NIL,TARGETS=NIL;
      UNTIL X1==NIL
      DO {  IF MEMBER(SCRIPT,HD(X1))
            DO XVALS=CONS(VAL((ATOM)HD(X1)),XVALS),TARGETS=CONS(HD(Y),TARGETS);
            X1=TL(X1),Y=TL(Y);  }
      //NOW CONVERT ALL OCCURRENCES IN THE SCRIPT
   {  LIST S=SCRIPT;
      UNTIL S==NIL
      DO {  LIST EQNS=TL(VAL((ATOM)HD(S)));
            WORD NARGS=(WORD)HD(HD(VAL((ATOM)HD(S))));
            UNTIL EQNS==NIL
            DO {  LIST CODE=TL(HD(EQNS));
                  IF NARGS>0
                  DO {  LIST LHS=HD(HD(EQNS));
			WORD I;
                        for (I=2; I<=NARGS; I++)
                           LHS=HD(LHS);
                        HD(LHS)=SUBST(Z,HD(LHS)); }
                  WHILE ISCONS(CODE)
                  DO HD(CODE)=SUBST(Z,HD(CODE)),CODE=TL(CODE);
                  EQNS=TL(EQNS);  }
            IF MEMBER(X,HD(S)) DO VAL((ATOM)HD(S))=NIL;
            HD(S)=SUBST(Z,HD(S));
            S=TL(S);  }
      //NOW REASSIGN VAL FIELDS
      UNTIL TARGETS==NIL
      DO {  VAL((ATOM)HD(TARGETS))=HD(XVALS);
            TARGETS=TL(TARGETS),XVALS=TL(XVALS);  }
      RELEASE_INTERRUPTS();
   }  }  }

static LIST
SUBST(LIST Z,LIST A)
{  UNTIL Z==NIL
   DO {  IF A==HD(HD(Z))
         DO  {  SAVED=FALSE; return TL(HD(Z));  }
         Z=TL(Z); }
   return A;  }

static void
NEWEQUATION()
   {  WORD EQNO = -1;
      IF HAVENUM()
      DO {  EQNO=100*THE_NUM+THE_DECIMALS;
            CHECK((TOKEN)')');  }
   {  LIST X=EQUATION();
      IF ERRORFLAG DO return;
   {  ATOM SUBJECT=(ATOM)HD(X);
      WORD NARGS=(WORD)HD(TL(X));
      LIST EQN=TL(TL(X));
      IF ATOBJECT DO  {  PRINTOB(EQN) ; NEWLINE();  }
      if ( VAL(SUBJECT)==NIL
      ) {  VAL(SUBJECT)=CONS(CONS((LIST)NARGS,NIL),CONS(EQN,NIL));
              ENTERSCRIPT(SUBJECT);  } else
      if ( PROTECTED(SUBJECT)
      ) return;  else
      if ( TL(VAL(SUBJECT))==NIL  //SUBJECT CURRENTLY DEFINED ONLY BY A COMMENT
      ) {  HD(HD(VAL(SUBJECT)))=(LIST)NARGS;
              TL(VAL(SUBJECT))=CONS(EQN,NIL);  } else
//    if ( NARGS==0 //SIMPLE DEF SILENTLY OVERWRITING EXISTING EQNS - REMOVED DT 2015
//    ) {  VAL(SUBJECT)=CONS(CONS(0,TL(HD(VAL(SUBJECT)))),CONS(EQN,NIL));
//            CLEARMEMORY(); } else
      if ( NARGS!=(WORD)HD(HD(VAL(SUBJECT)))
      ) {  WRITEF("Wrong no of args for \"%s\"\n",PRINTNAME(SUBJECT));
              WRITES("Equation rejected\n");
              return;  } else
      if ( EQNO==-1  //UNNUMBERED EQN
      ) {  LIST EQNS=TL(VAL(SUBJECT));
              LIST P=PROFILE(EQN);
              do{IF EQUAL(P,PROFILE(HD(EQNS)))
                 DO {  LIST CODE=TL(HD(EQNS));
                       if ( HD(CODE)==(LIST)LINENO_C //IF OLD EQN HAS LINE NO,
                       ) {  TL(TL(CODE))=TL(EQN);  //NEW EQN INHERITS
                               HD(HD(EQNS))=HD(EQN);  }
                       else HD(EQNS)=EQN;
                       CLEARMEMORY();
                       break;  }
                 IF TL(EQNS)==NIL
                 DO {  TL(EQNS)=CONS(EQN,NIL);
                       break;  }
                 EQNS=TL(EQNS);
              } while(1);
           } 
      else {  LIST EQNS = TL(VAL(SUBJECT));  //NUMBERED EQN
            WORD N = 0;
            IF EQNO % 100!=0 || EQNO==0 //IF EQN HAS NON STANDARD LINENO
            DO TL(EQN)=CONS((LIST)LINENO_C,CONS((LIST)EQNO,TL(EQN))); //MARK WITH NO.
            do{N=HD(TL(HD(EQNS)))==(LIST)LINENO_C ? (WORD)HD(TL(TL(HD(EQNS)))) :
                   (N/100+1)*100;
               IF EQNO==N
               DO {  HD(EQNS)=EQN;
                     CLEARMEMORY();
                     break;  }
               IF EQNO<N
               DO {  LIST HOLD=HD(EQNS);
                     HD(EQNS)=EQN;
                     TL(EQNS)=CONS(HOLD,TL(EQNS));
                     CLEARMEMORY();
                     break;  }
               IF TL(EQNS)==NIL
               DO {  TL(EQNS)=CONS(EQN,NIL);
                     break;  }
               EQNS=TL(EQNS);
            } while(1);
         } 
      SAVED=FALSE;
   }  }  }

static void
CLEARMEMORY() //CALLED WHENEVER EQNS ARE DESTROYED,REORDERED OR
                  //INSERTED (OTHER THAN AT THE END OF A DEFINITION)
{  UNTIL MEMORIES==NIL //MEMORIES HOLDS A LIST OF ALL VARS WHOSE MEMO
   DO {  LIST X=VAL((ATOM)HD(MEMORIES));  //FIELDS HAVE BEEN SET
         UNLESS X==NIL DO HD(HD(TL(X)))=0; //UNSET MEMO FIELD
         MEMORIES=TL(MEMORIES);  }  }

void
ENTERSCRIPT(ATOM A)    //ENTERS "A" IN THE SCRIPT
{  if ( SCRIPT==NIL
   ) SCRIPT=CONS((LIST)A,NIL);
   else {  LIST S=SCRIPT;
         UNTIL TL(S)==NIL
         DO S=TL(S);
         TL(S) = CONS((LIST)A,NIL);  }
}

static void
COMMENT()
   {  ATOM SUBJECT=(ATOM)TL(HD(TOKENS));
      LIST COMMENT=HD(TL(TOKENS));
      IF VAL(SUBJECT)==NIL
      DO {  VAL(SUBJECT)=CONS(CONS(0,NIL),NIL);
            ENTERSCRIPT(SUBJECT); }
      IF PROTECTED(SUBJECT) DO return;
      TL(HD(VAL(SUBJECT)))=COMMENT;
      IF COMMENT==NIL && TL(VAL(SUBJECT))==NIL
      DO REMOVE(SUBJECT);
      SAVED=FALSE;
   }

static void
EVALUATION()
   {  LIST CODE=EXP();
      WORD CH=(WORD)HD(TOKENS);
      LIST E=0;  //STATIC SO INVISIBLE TO GARBAGE COLLECTOR
      UNLESS HAVE((TOKEN)'!') DO CHECK((TOKEN)'?');
      IF ERRORFLAG DO return;;
      CHECK(EOL);
      IF ATOBJECT DO {  PRINTOB(CODE) ; NEWLINE();  }
      E=BUILDEXP(CODE);
      IF ATCOUNT DO RESETGCSTATS();
      INITSTATS();
      EVALUATING=TRUE;
      FORMATTING=CH=='?';
      PRINTVAL(E,FORMATTING);
      IF FORMATTING DO NEWLINE();
      CLOSECHANNELS();
      EVALUATING=FALSE;
      IF ATCOUNT DO OUTSTATS();
   }

static void
ABORDERCOM()
{  SCRIPT=SORT(SCRIPT),SAVED=FALSE;  }

static LIST
SORT(LIST X)
{  IF X==NIL || TL(X)==NIL DO return X;
   {  LIST A=NIL, B=NIL, HOLD=NIL;  //FIRST SPLIT X
      UNTIL X==NIL DO HOLD=A, A=CONS(HD(X),B), B=HOLD, X=TL(X);
      A=SORT(A),B=SORT(B);
      UNTIL A==NIL||B==NIL  //NOW MERGE THE TWO HALVES BACK TOGETHER
      DO if ( ALFA_LS((ATOM)HD(A),(ATOM)HD(B))
	 ) X=CONS(HD(A),X), A=TL(A);
	 else   X=CONS(HD(B),X), B=TL(B);
      IF A==NIL DO A=B;
      UNTIL A==NIL DO X=CONS(HD(A),X), A=TL(A);
      return REVERSE(X);  }
}

static void
REORDERCOM()
{  if ( ISID(HD(TOKENS)) && (ISID(HD(TL(TOKENS))) || HD(TL(TOKENS))==(LIST)DOTDOT_SY)
   ) SCRIPTREORDER(); else
   if ( HAVEID() && HD(TOKENS)!=EOL
   ) {  LIST NOS = NIL;
           WORD MAX = NO_OF_EQNS(THE_ID);
           WHILE HAVENUM()
           DO {  WORD A=THE_NUM;
                 WORD B = HAVE(DOTDOT_SY) ?
                         HAVENUM()? THE_NUM : MAX :  A;
		 WORD I;
                 for (I=A; I<=B; I++)
                    IF !MEMBER(NOS,(LIST)I) && 1<=I && I<=MAX
                    DO NOS=CONS((LIST)I,NOS);
                    //NOS OUT OF RANGE ARE SILENTLY IGNORED
              }
           CHECK(EOL);
           IF ERRORFLAG DO return;
           IF VAL(THE_ID)==NIL
           DO {  DISPLAY(THE_ID,FALSE,FALSE);
                 return;  }
           IF PROTECTED(THE_ID) DO return;
           {  WORD I;
	      for (I=1; I<= MAX; I++)
              UNLESS MEMBER(NOS,(LIST)I)
              DO NOS=CONS((LIST)I,NOS);
              // ANY EQNS LEFT OUT ARE TACKED ON AT THE END
	   }
           // NOTE THAT "NOS" ARE IN REVERSE ORDER
        {  LIST NEW = NIL;
           LIST EQNS = TL(VAL(THE_ID));
           UNTIL NOS==NIL
           DO {  LIST EQN=ELEM(EQNS,(WORD)HD(NOS));
                 REMOVELINENO(EQN);
                 NEW=CONS(EQN,NEW);
                 NOS=TL(NOS);  }
           //  NOTE THAT THE EQNS IN "NEW" ARE NOW IN THE CORRECT ORDER
           TL(VAL(THE_ID))=NEW;
           DISPLAY(THE_ID,TRUE,FALSE);
           SAVED=FALSE;
           CLEARMEMORY();
        }  } 
   else SYNTAX();
}

static void
SCRIPTREORDER()
   {  LIST R=NIL;
      WHILE HAVEID()
      DO if ( HAVE(DOTDOT_SY)
         ) {  ATOM A=THE_ID, B=0; LIST X=NIL;
                 if ( HAVEID() ) B=THE_ID; else
                 IF HD(TOKENS)==EOL DO B=(ATOM)EOL;
                 if ( B==0 ) SYNTAX(); else X=EXTRACT(A,B);
                 IF X==NIL DO SYNTAX();
                 R=SHUNT(X,R);  }
         else if ( MEMBER(SCRIPT,(LIST)THE_ID)
            ) R=CONS((LIST)THE_ID,R);
            else {  WRITEF("\"%s\" not in script\n",PRINTNAME(THE_ID));
                  SYNTAX();  }
      CHECK(EOL);
      IF ERRORFLAG DO return;
   {  LIST R1 = NIL;
      UNTIL TL(R)==NIL
      DO {  UNLESS MEMBER(TL(R),HD(R)) DO SCRIPT=SUB1(SCRIPT,(ATOM)HD(R)), R1=CONS(HD(R),R1);
            R=TL(R);  }
      SCRIPT=APPEND(EXTRACT((ATOM)HD(SCRIPT),(ATOM)HD(R)),APPEND(R1,TL(EXTRACT((ATOM)HD(R),(ATOM)EOL))));
      SAVED=FALSE;
   }  }

static WORD
NO_OF_EQNS(ATOM A)
{  return VAL(A)==NIL ? 0 : LENGTH(TL(VAL(A)));  }

static BOOL
PROTECTED(ATOM A)
  //LIBRARY FUNCTIONS ARE RECOGNISABLE BY NOT BEING PART OF THE SCRIPT
{  IF MEMBER(SCRIPT,(LIST)A) DO return FALSE;
   IF MEMBER(HOLDSCRIPT,(LIST)A)
   DO {  UNLESS MEMBER(GET_HITS,(LIST)A) DO GET_HITS=CONS((LIST)A,GET_HITS);
         return FALSE;  }
   WRITEF("\"%s\" is predefined and cannot be altered\n",PRINTNAME(A));
   return TRUE;  }

static void
REMOVE(ATOM A)   // REMOVES "A" FROM THE SCRIPT
   {  SCRIPT=SUB1(SCRIPT,A);
      VAL(A)=NIL;
   }

static LIST
EXTRACT(ATOM A, ATOM B)         //RETURNS A SEGMENT OF THE SCRIPT
{  LIST S=SCRIPT, X=NIL;
   UNTIL S==NIL || HD(S)==(LIST)A DO S=TL(S);
   UNTIL S==NIL || HD(S)==(LIST)B DO X=CONS(HD(S),X),S=TL(S);
   UNLESS S==NIL DO X=CONS(HD(S),X);
   IF S==NIL && B!=(ATOM)EOL DO X=NIL;
   IF X==NIL DO WRITEF("\"%s..%s\" not in script\n",
                      PRINTNAME(A),B==(ATOM)EOL?"":PRINTNAME(B));
   return REVERSE(X);  }

static void
DELETECOM()
   {  LIST DLIST = NIL;
      WHILE HAVEID()
      DO if ( HAVE(DOTDOT_SY)
         ) {  ATOM A=THE_ID, B=(ATOM)EOL;
                 if ( HAVEID()
                 ) B=THE_ID; else
                 UNLESS HD(TOKENS)==EOL DO SYNTAX();
                 DLIST=CONS(CONS((LIST)A,(LIST)B),DLIST);  } else
         {  WORD MAX = NO_OF_EQNS(THE_ID);
            LIST NLIST = NIL;
            WHILE HAVENUM()
            DO {  WORD A = THE_NUM;
                  WORD B = HAVE(DOTDOT_SY) ?
                          HAVENUM()?THE_NUM:MAX : A;
		  WORD I;
                  for (I=A; I<=B; I++)
                     NLIST=CONS((LIST)I,NLIST);
               }
            DLIST=CONS(CONS((LIST)THE_ID,NLIST),DLIST);
         }
      CHECK(EOL);
      IF ERRORFLAG DO return;
   {  WORD DELS = 0;
      IF DLIST==NIL   //DELETE ALL
      DO {
	 if ( SCRIPT==NIL ) DISPLAYALL(FALSE); else
         {  UNLESS MAKESURE() DO return;
            UNTIL SCRIPT==NIL
            DO {  DELS=DELS + NO_OF_EQNS((ATOM)HD(SCRIPT));
                  VAL((ATOM)HD(SCRIPT))=NIL;
                  SCRIPT=TL(SCRIPT);  }  }  }
      UNTIL DLIST == NIL
      DO if ( ISATOM(TL(HD(DLIST))) || TL(HD(DLIST))==EOL //"NAME..NAME"
         ) {  LIST X=EXTRACT((ATOM)HD(HD(DLIST)),(ATOM)TL(HD(DLIST)));
                 DLIST=TL(DLIST);
                 UNTIL X==NIL
                 DO DLIST=CONS(CONS(HD(X),NIL),DLIST), X=TL(X);  } else
         {  ATOM NAME = (ATOM)HD(HD(DLIST));
            LIST NOS = TL(HD(DLIST));
            LIST NEW = NIL;
            DLIST=TL(DLIST);
            IF VAL(NAME) == NIL
            DO {  DISPLAY(NAME,FALSE,FALSE);
                  continue; }
             IF PROTECTED(NAME) DO continue;
            if ( NOS==NIL
            ) {  DELS=DELS+NO_OF_EQNS(NAME);
                    REMOVE(NAME);
                    continue;  }
            else {
		WORD I;
		for (I=NO_OF_EQNS(NAME); I>=1; I=I-1)
                  if ( MEMBER(NOS,(LIST)I)
                  ) DELS=DELS+1;
                  else {  LIST EQN=ELEM(TL(VAL(NAME)),I);
                        REMOVELINENO(EQN);
                        NEW=CONS(EQN,NEW);  }  }
            TL(VAL(NAME))=NEW;
            IF NEW==NIL &&
               TL(HD(VAL(NAME)))==NIL   //COMMENT FIELD
            DO REMOVE(NAME);  } 
      WRITEF("%" W " equations deleted\n",DELS);
      IF DELS>0 DO {  SAVED=FALSE; CLEARMEMORY();  }
   }  }

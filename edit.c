/*
  edit.c - Enhanced command line editing for CMD.EXE.

  Jason Hood, 24 October to 21 November, 2005 and 19 to 23 December, 2006.

  API hooking derived from ANSI.xs by Jean-Louis Morel, from his Perl package
  Win32::Console::ANSI.  Keyboard hooking based on the hook tutorial sample
  code by RattleSnake: Systemwide Windows Hooks without external DLL.

  v1.02, 23 July, 2010:
  - better handling of control characters;
  - fixed alternative directory association in the root;
  - LSTK was listing Shift+Control for Control keys;
  - tweaks to completion;
  + read options from HKLM if they don't exist in HKCU.
*/

#include <stdio.h>
#include <windows.h>
#include <ImageHlp.h>
#include <tchar.h>
#include <string.h>
#include "cmdkey.h"


// ========== Auxiliary debug function

#define MYDEBUG 0

#if (MYDEBUG > 0)
void DEBUGSTR( char* szFormat, ... )	// sort of OutputDebugStringf
{
  char szBuffer[1024];
  va_list pArgList;
  va_start( pArgList, szFormat );
  _vsnprintf( szBuffer, sizeof(szBuffer), szFormat, pArgList );
  va_end( pArgList );
  OutputDebugString( szBuffer );
}
#else
#define DEBUGSTR(...)
#endif


// Macro for adding pointers/DWORDs together without C arithmetic interfering
#define MakePtr( cast, ptr, addValue ) (cast)( (DWORD)(ptr)+(DWORD)(addValue))


// ========== Global variables and constants

BOOL installed	__attribute__((dllexport, shared, section(".share"))) = FALSE;
BOOL is_enabled __attribute__((dllexport, shared, section(".share"))) = TRUE;

Option option __attribute__((dllexport, shared, section(".share"))) = {
  { 25, 50 },			// insert & overwrite cursor sizes
  0,				// default insert mode
  0,				// beep on errors
  0,				// don't auto-recall commands
  0,				// enable macro & symbol translation
  0,				// enable CMDkey
  0,				// append backslash on completed dirs
  0,				// empty match remains at start
  ' ',				// prefix character to disable translation
  1,				// minimum line length to store in history
  50,				// number of commands to store in history
  0,				// enable colouring
  31,				// command line,	bright white  on blue
  27,				// recording,		bright cyan   on blue
  27,				// drive and colon,	bright cyan   on blue
  30,				// directory separator, bright yellow on blue
  26,				// directory,		bright green  on blue
  30,				// greater than sign,	bright yellow on blue
};

char cfgname[MAX_PATH] __attribute__((dllexport, shared, section(".share")))
		       = { 0 }; // default configuration file
char cmdname[MAX_PATH] __attribute__((dllexport, shared, section(".share")))
		       = { 0 }; // runtime configuration file


// Structure to hold a line.
typedef struct
{
  PWSTR txt;			// text of the line
  DWORD len;			// length of the line
} Line, *PLine;


// Structure to search for functions, keys & internal commands.
typedef struct
{
  PCWSTR name;			// function/key/internal command name
  int	 func;			// function/VK/function ptr
} Cfg;


// Structure to store the character and function assigned to a key.
typedef struct
{
  WCHAR ch;			// character
  char	fn;			// function
  char	filler; 		// ensure alignment as a single DWORD
} Key, *PKey;


// Structure for keyboard macros.
typedef struct macro_s
{
  struct macro_s* next;
  char*  key;			// pointer into appropriate key array
  int	 len;
  union
  {
    PWSTR line; 		// for len <= 0
    PKey  func; 		// for len > 0
    Key   chfn; 		// for abs(len) == 1
  };
} Macro, *PMacro;


// Structure to hold lines for macros, symbols and associations.
typedef struct linelist_s
{
  struct linelist_s* next;
  DWORD  len;
  WCHAR  line[0];
} LineList, *PLineList;


// Structure for a definition (macro, symbol or association).
typedef struct define_s
{
  struct define_s* next;
  PLineList line;
  DWORD     len;
  WCHAR     name[0];
} Define, *PDefine;


// Structure for the history and completed filenames.
typedef struct history_s
{
  struct history_s* prev;
  struct history_s* next;
  DWORD  len;
  WCHAR  line[0];
} History, *PHistory;


// Function prototype for an internal command.
typedef void (*IntFunc( DWORD ));


BOOL	enabled = TRUE; 	// are we active?

HANDLE	hConIn, hConOut;	// handles to keyboard input and screen output
CONSOLE_SCREEN_BUFFER_INFO screen; // current screen info
Line	prompt = { L"", 0 };    // pointer to the prompt
WORD	p_attr[MAX_PATH+2];	// buffer to store prompt's attributes
BOOL	show_prompt;		// should we redisplay the prompt?

Line	line;			// line being edited
DWORD	max;			// maximum size of above
DWORD	dispbeg, dispend;	// beginning and ending position to display

BOOL	kbd;			// is input from the keyboard?
BOOL	def_macro;		// defining a macro?
Line	mcmd;			// buffer for multiple commands
int	lastm;			// previous macro listed was multi-line

BOOL	found_quote;		// true if get_string found a quote

FILE*	file;			// file containing commands

int	check_break;		// Control+Break received?
BOOL	trap_break;		// pass it on?

Line	envvar; 		// buffer for environment variable


#define ESCAPE		'^'     // character to treat next character literally
#define CMDSEP		19	// character used to separate multiple commands
#define VARIABLE	'%'     // character used for symbol and variable subst

#define FEXEC		L".exe.com.bat.cmd"         // default executable list
#define FIGNORE 	L".exe.com.dll.obj.o.bak"   // default ignore list

#define INVALID_FNAME	L"=,;+<|>&@"            // "invalid" filename characters
#define QUOTE_FNAME	L" &()[]{}^=;!%'+,`~"   // quoted filename characters
						//  (same as CMD.EXE)
#define BRACE_TERM	L" \t,;+"               // brace expansion separators
#define BRACE_STOP	L"<|>&"                 // brace expansion terminators
#define BRACE_ESCAPE	L"{},^"                 // escaped brace exp. characters

#define DEF_TERM	L" \t<|>/"              // definition delimiter

#define VAR_ESCAPE	L"%^"                   // escaped variable characters
#define ARG_ESCAPE	L"%*^"                  // escaped argument characters

#define WSZ( len )	((len) * sizeof(WCHAR)) // byte size of a wide string


// Map control characters using the CP437 glyphs as Unicode codepoints.
const WCHAR ControlChar[] = {  ' ',           //  0
			      L'\x263A',      //  1 White Smiling Face
			      L'\x263B',      //  2 Black Smiling Face
			      L'\x2665',      //  3 Black Heart Suit
			      L'\x2666',      //  4 Black Diamond Suit
			      L'\x2663',      //  5 Black Club Suit
			      L'\x2660',      //  6 Black Spade Suit
			      L'\x2022',      //  7 Bullet
			      L'\x25D8',      //  8 Inverse Bullet
			      L'\x25CB',      //  9 White Circle
			      L'\x25D9',      // 10 Inverse White Circle
			      L'\x2642',      // 11 Male Sign
			      L'\x2640',      // 12 Female Sign
			      L'\x266A',      // 13 Eigth Note
			      L'\x266B',      // 14 Beamed Eigth Notes
			      L'\x263C',      // 15 White Sun With Rays
			      L'\x25BA',      // 16 Black Right-Pointing Pointer
			      L'\x25C4',      // 17 Black Left-Pointing Pointer
			      L'\x2195',      // 18 Up Down Arrow
			      L'\x203C',      // 19 Double Exclamation Mark
			      L'\x00B6',      // 20 Pilcrow Sign
			      L'\x00A7',      // 21 Section Sign
			      L'\x25AC',      // 22 Black Rectangle
			      L'\x21A8',      // 23 Up Down Arrow With Base
			      L'\x2191',      // 24 Upwards Arrow
			      L'\x2193',      // 25 Downwards Arrow
			      L'\x2192',      // 26 Rightwards Arrow
			      L'\x2190',      // 27 Leftwards Arrow
			      L'\x221F',      // 28 Right Angle
			      L'\x2194',      // 29 Left Right Arrow
			      L'\x25B2',      // 30 Black Up-Pointing Pointer
			      L'\x25BC'       // 31 Black Down-Pointing Pointer
			    };

// Functions
enum
{
  Default,		// just place the character in the buffer
  Ignore,		// totally ignore the key
  Quote,		// the next key will not be interpreted as a command
  CharLeft,		// cursor one character to the left
  CharRight,		// cursor one character to the right
  WordLeft,		// cursor to the start of the current/previous word
  WordRight,		// cursor to the start of the next word
  StringLeft,		// cursor to the start of the current/previous string
  StringRight,		// cursor to the start of the next string
  BegLine,		// cursor to the beginning of the line
  EndLine,		// cursor to the end of the line
  PrevLine,		// previous command in history buffer
  NextLine,		// next command in history buffer
  SearchBack,		// search the history backwards
  SearchForw,		// search the history forwards
  FirstLine,		// first command in history
  LastLine,		// last command in history
  List, 		// filename completion list
  ListDir,		// directory completion list
  Cycle,		// filename completion cycle
  CycleBack,		// filename completion cycle backwards
  CycleDir,		// directory completion cycle
  CycleDirBack, 	// directory completion cycle backwards
  SelectFiles,		// select files from the Open dialog
  DelLeft,		// delete character to the left of the cursor
  DelRight,		// delete character under the cursor
  DelWordLeft,		// delete word at left of cursor
  DelWordRight, 	// delete word at right of cursor
  DelArg,		// delete argument at or left of cursor
  DelBegLine,		// delete to the beginning of the line
  DelEndLine,		// delete to the end of the line
  DelEndExec,		// delete to the end of the line and execute the line
  Erase,		// erase line
  StoreErase,		// erase the line but put it in the history
  CmdSep,		// command separator
  Transpose,		// transpose (swap characters)
  AutoRecall,		// toggle auto-recall
  MacroToggle,		// macro/symbol/brace toggling
  VarSubst,		// inline var. substitution/brace expansion/association
  Enter,		// accept line
  Wipe, 		// execute the line but don't put it in the history
  InsOvr,		// insert/overwrite toggle
  Play, 		// play back a series of keys
  Record,		// record a series of keys
  LastFunc
};


#define f( name ) { L###name, name },	// simplify function config definitions

// Function strings
const Cfg cfg[] = {
  f( AutoRecall   )
  f( BegLine	  )
  f( CharLeft	  )
  f( CharRight	  )
  f( CmdSep	  )
  f( Cycle	  )
  f( CycleBack	  )
  f( CycleDir	  )
  f( CycleDirBack )
  f( Default	  )
  f( DelArg	  )
  f( DelBegLine   )
  f( DelEndExec   )
  f( DelEndLine   )
  f( DelLeft	  )
  f( DelRight	  )
  f( DelWordLeft  )
  f( DelWordRight )
  f( EndLine	  )
  f( Enter	  )
  f( Erase	  )
  f( FirstLine	  )
  f( Ignore	  )
  f( InsOvr	  )
  f( LastLine	  )
  f( List	  )
  f( ListDir	  )
  f( MacroToggle  )
  f( NextLine	  )
  f( Play	  )
  f( PrevLine	  )
  f( Quote	  )
  f( Record	  )
  f( SearchBack   )
  f( SearchForw   )
  f( SelectFiles  )
  f( StoreErase   )
  f( StringLeft   )
  f( StringRight  )
  f( Transpose	  )
  f( VarSubst	  )
  f( Wipe	  )
  f( WordLeft	  )
  f( WordRight	  )
};


// Function names for lstk.
const char const * func_str[] =
{
  "Default",    "Ignore",      "Quote",        "CharLeft",    "CharRight",
  "WordLeft",   "WordRight",   "StringLeft",   "StringRight", "BegLine",
  "EndLine",    "PrevLine",    "NextLine",     "SearchBack",  "SearchForw",
  "FirstLine",  "LastLine",    "List",         "ListDir",     "Cycle",
  "CycleBack",  "CycleDir",    "CycleDirBack", "SelectFiles", "DelLeft",
  "DelRight",   "DelWordLeft", "DelWordRight", "DelArg",      "DelBegLine",
  "DelEndLine", "DelEndExec",  "Erase",        "StoreErase",  "CmdSep",
  "Transpose",  "AutoRecall",  "MacroToggle",  "VarSubst",    "Enter",
  "Wipe",       "InsOvr",      "Play",         "Record",
};


// Key strings
const Cfg cfgkey[] = {
  { L"Bksp",    VK_DOWN+1 },
  { L"Del",     VK_DELETE },
  { L"Down",    VK_DOWN   },
  { L"End",     VK_END    },
  { L"Enter",   VK_DOWN+3 },
  { L"Esc",     VK_DOWN+4 },
  { L"Home",    VK_HOME   },
  { L"Ins",     VK_INSERT },
  { L"Left",    VK_LEFT   },
  { L"PgDn",    VK_NEXT   },
  { L"PgUp",    VK_PRIOR  },
  { L"Right",   VK_RIGHT  },
  { L"Tab",     VK_DOWN+2 },
  { L"Up",      VK_UP     },
};

#define CFGKEYS (sizeof(cfgkey) / sizeof(*cfgkey))


// Key names for lstk.

const char const * key_str[] =
{
  "PgUp", "PgDn", "End", "Home",  "Left", "Up",  "Right",
  "Down", "Bksp", "Tab", "Enter", "Esc",  "Ins", "Del",
};


// Keymaps

char key_table[][4] = { 	// VK_PRIOR to VK_DELETE
  // plain	shift		control 	alt
  { FirstLine,	Ignore, 	Ignore, 	Ignore, 	},// PageUp
  { LastLine,	Ignore, 	Ignore, 	Ignore, 	},// PageDn
  { EndLine,	Ignore, 	DelEndLine,	Ignore, 	},// End
  { BegLine,	Ignore, 	DelBegLine,	Ignore, 	},// Home
  { CharLeft,	Ignore, 	WordLeft,	StringLeft,	},// Left
  { PrevLine,	Ignore, 	Ignore, 	Ignore, 	},// Up
  { CharRight,	Ignore, 	WordRight,	StringRight,	},// Right
  { NextLine,	Ignore, 	Ignore, 	Ignore, 	},// Down

  // plain	shift		control 	shift+control
  { DelLeft,	DelLeft,	DelWordLeft,	DelArg, 	},// Backspace
  { Cycle,	CycleBack,	List,		ListDir,	},// Tab
  { Enter,	Enter,		Ignore, 	Ignore, 	},// Enter
  { Erase,	Erase,		Ignore, 	Ignore, 	},// Escape

  // plain	shift		control 	alt
  { InsOvr,	Ignore, 	Ignore, 	Ignore, 	},// Insert
  { DelRight,	Ignore, 	Ignore, 	Ignore, 	},// Delete
};

char fkey_table[][4] = {	// VK_F1 to VK_F12
  // plain	shift		control 	alt
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F1
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F2
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F3
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F4
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F5
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F6
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F7
  { SearchBack, SearchForw,	Ignore, 	Ignore, 	},// F8
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F9
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F10
  { Ignore,	Ignore, 	Ignore, 	Ignore, 	},// F11
  { Record,	Ignore, 	Ignore, 	Ignore, 	},// F12
};

char ctrl_key_table[][2] = {
  // plain		// shift
  { Ignore,		Ignore, 	}, // ^@
  { BegLine,		Ignore, 	}, // ^A
  { CharLeft,		Ignore, 	}, // ^B
  { Ignore,		Ignore, 	}, // ^C
  { DelRight,		ListDir,	}, // ^D
  { EndLine,		Ignore, 	}, // ^E
  { CharRight,		List,		}, // ^F
  { StoreErase, 	Ignore, 	}, // ^G
  { DelLeft,		Ignore, 	}, // ^H
  { Cycle,		CycleBack,	}, // ^I
  { VarSubst,		Ignore, 	}, // ^J
  { DelEndLine, 	Ignore, 	}, // ^K
  { DelWordLeft,	Ignore, 	}, // ^L
  { Enter,		Ignore, 	}, // ^M
  { NextLine,		Ignore, 	}, // ^N
  { DelEndExec, 	Ignore, 	}, // ^O
  { PrevLine,		Ignore, 	}, // ^P
  { Quote,		Ignore, 	}, // ^Q
  { SearchBack, 	Ignore, 	}, // ^R
  { CmdSep,		SelectFiles,	}, // ^S
  { Transpose,		Ignore, 	}, // ^T
  { PrevLine,		Ignore, 	}, // ^U
  { SearchForw, 	Ignore, 	}, // ^V
  { DelWordRight,	Ignore, 	}, // ^W
  { DelBegLine, 	Ignore, 	}, // ^X
  { AutoRecall, 	Ignore, 	}, // ^Y
  { Default,		Ignore, 	}, // ^Z
  { Erase,		Ignore, 	}, // ^[
  { CycleDir,		CycleDirBack,	}, // ^\ (dumb GCC)
  { CmdSep,		Ignore, 	}, // ^]
  { Wipe,		Ignore, 	}, // ^^
  { MacroToggle,	Ignore, 	}, // ^_
};


// Keyboard macros

PMacro macro_head, macro_prev;		// head of the list, previous macro

PMacro find_macro( char* );		// search macro list for key
PMacro add_macro(  char* );		// add or clear macro for key
void   end_macro(  PMacro );		// finish off a macro
void   del_macro(  char* );		// delete the macro for key


// Internal commands

FILE* lstout;				// list output file
BOOL  lstpipe;				// outputting to a pipe?
BOOL  redirect( DWORD );		// check for redirection
void  end_redirect( void );		// close the file
void  list_key( char* );		// show the assignment of a key

void execute_defa( DWORD );
void execute_defk( DWORD );
void execute_defm( DWORD );
void execute_defs( DWORD );
void execute_dela( DWORD );
void execute_delh( DWORD );
void execute_delk( DWORD );
void execute_delm( DWORD );
void execute_dels( DWORD );
void execute_lsta( DWORD );
void execute_lsth( DWORD );
void execute_lstk( DWORD );
void execute_lstm( DWORD );
void execute_lsts( DWORD );
void execute_rsta( DWORD );
void execute_rsth( DWORD );
void execute_rstm( DWORD );
void execute_rsts( DWORD );

const Cfg internal[] = {
  { L"defa",    (int)execute_defa },    // define association
  { L"defk",    (int)execute_defk },    // define key
  { L"defm",    (int)execute_defm },    // define macro
  { L"defs",    (int)execute_defs },    // define symbol
  { L"dela",    (int)execute_dela },    // delete association(s)
  { L"delh",    (int)execute_delh },    // delete history line(s)
  { L"delk",    (int)execute_delk },    // delete key(s)
  { L"delm",    (int)execute_delm },    // delete macro(s)
  { L"dels",    (int)execute_dels },    // delete symbol(s)
  { L"lsta",    (int)execute_lsta },    // list associations
  { L"lsth",    (int)execute_lsth },    // list history
  { L"lstk",    (int)execute_lstk },    // list keys
  { L"lstm",    (int)execute_lstm },    // list macros
  { L"lsts",    (int)execute_lsts },    // list symbols
  { L"rsta",    (int)execute_rsta },    // reset associations
  { L"rsth",    (int)execute_rsth },    // reset history
  { L"rstm",    (int)execute_rstm },    // reset macros
  { L"rsts",    (int)execute_rsts },    // reset symbols
};

//#define MIN_CMD_LEN 4
//#define MAX_CMD_LEN 4
#define CMD_LEN 4
#define INTERNAL_CMDS (sizeof(internal) / sizeof(*internal))

#define DEFM	 L"DEFM> "              // prompt for defining a macro
#define DEFM_LEN 6
const WCHAR ENDM[] = L"endm";           // string used to end macro definition
#define ENDM_LEN 4


// Line manipulation

COORD line_to_scr( DWORD );		// convert line position to screen
void  set_display_marks( DWORD, DWORD ); // indicate positions to display
void  copy_chars( PCWSTR, DWORD );	// set line to string
void  remove_chars( DWORD, DWORD );	// remove characters from line
void  insert_chars( DWORD, PCWSTR, DWORD );	    // add string to line
void  replace_chars( DWORD, DWORD, PCWSTR, DWORD ); // replace string w/ another
char* get_key( PKey );			// read a key
WCHAR process_keypad( WORD );		// translate Alt+Keypad to character
void  edit_line( void );		// read and edit line from the keyboard
void  display_prompt( void );		// re-display the original prompt


// Definitions (macro, symbol and association)

PDefine sym_head, mac_head, assoc_head; 	// heads of the various lists
PDefine macro_stk;				// stack of executing macros

PDefine add_define( PDefine*, DWORD, DWORD );	// add definition to list
PDefine find_define( PDefine*, DWORD, DWORD );	// find definition in list
PDefine find_assoc( PCWSTR, DWORD );		// find extension in list
void	del_define( PDefine* ); 		// delete definition from list
void	delete_define( PDefine*, DWORD );	// delete list of definitions
void	reset_define( PDefine* );		// wipe all definitions
void	list_define( PDefine, char );		// display a definition
void	list_defines( PDefine*, DWORD );	// display a list of definitions
PLineList add_line( DWORD );			// add a line to the line list
void	free_linelist( PLineList );		// free memory used by line list


// Line input

BOOL read_cmdfile( PCSTR );		// read the file
BOOL get_file_line( void );		// read the line from file
void get_next_line( void );		// get next line of input
void get_macro_line( void );		// input coming from a macro
void pop_macro( void ); 		// macro finished, remove it from stack


// Line output

void multi_cmd( void ); 		// separate multiple commands
void dosify( void );			// convert UNIX-style command to Windows
BOOL brace_expansion( void );		// expand the first set of braces
void expand_braces( void );		// perform brace expansion
BOOL associate( void ); 		// perform filename association
BOOL expand_symbol( void );		// expand a symbol to its definition
BOOL expand_macro( void );		// expand a macro to its definition
BOOL internal_cmd( void );		// process internal command
void expand_vars( BOOL );		// expand environment vars and symbols


// History

History  history = { &history, &history, 0 };	// constant empty line
int	 histsize;				// number of lines in history

PHistory new_history( PCWSTR, DWORD );		// allocate new history item
void	 remove_from_history( PHistory );	// remove item from history
void	 add_to_history( void );		// add current line to history
PHistory search_history( PHistory, DWORD, BOOL ); // search history for match


// Filename completion

PHistory fname; 		// list of found filenames and initial pattern
DWORD	 fname_pos, path_pos;	// position in line of start of filename/path
WCHAR	 dirchar = '\\';        // character to use for directory indicator
int	 fname_max, fname_cnt;	// longest name and number of names found
DWORD	 assoc_pos;		// position of the association within the list
PWCHAR	 flist; 		// open dialog filenames
#define  FLIST_LEN 2048 	// size of flist

BOOL match_file( PCWSTR, PCWSTR, DWORD, BOOL, BOOL, PHANDLE, PWIN32_FIND_DATAW);
				// find a file, possibly matching its extension
int   find_files( int*, int );	// find matching files and common prefix
void  list_files( void );	// list all files
BOOL  check_name_count( int );	// determine action to take for many files
BOOL  quote_needed( PCWSTR, int ); // should filename be quoted?
PWSTR make_filter( BOOL );	// make the open dialog filter string
void  make_relative( PWSTR, PWSTR ); // make an absolute path relative


// Utility

#define isword(  ch ) (IsCharAlphaNumericW( ch ) || ch == '_')
#define isblank( ch ) (ch == ' ' || ch == '\t')

int   search_cfg( PCWSTR, DWORD, const Cfg [], int ); // find config string
char* find_key( DWORD, DWORD ); 	// find the position of a key
PWSTR new_txt( PCWSTR, DWORD ); 	// make a copy of a string
void  bell( void );			// audible alert
DWORD skip_blank( DWORD );		// skip over spaces and tabs
DWORD skip_nonblank( DWORD );		// skip over everything not space/tab
DWORD skip_nondelim( DWORD );		// skip over everything not a delimiter
BOOL  is_quote( int );			// quote character?
DWORD get_string( DWORD, LPDWORD, BOOL ); // retrieve argument
void  un_escape( PCWSTR );		// remove the escape character
BOOL  match_ext( PCWSTR, DWORD, PCWSTR, DWORD ); // match extension in list
DWORD get_env_var( PCWSTR, PCWSTR );	// get environment variable


// -------------------------   Line Manipulation   ---------------------------


// Convert line position to cursor position.
COORD line_to_scr( DWORD pos )
{
  COORD c;

  pos += screen.dwCursorPosition.X;
  c.X  = pos % screen.dwSize.X;
  c.Y  = pos / screen.dwSize.X + screen.dwCursorPosition.Y;

  return c;
}


// Set the beginning and ending positions to be displayed; end is after the
// position (ie. end - beg == number of characters).
void set_display_marks( DWORD beg, DWORD end )
{
  if (beg < dispbeg)
    dispbeg = beg;
  if (end > dispend)
    dispend = end;
}


// Set the line to str, which is cnt characters.
void copy_chars( PCWSTR str, DWORD cnt )
{
  set_display_marks( 0, line.len );
  if (cnt > max)
  {
    bell();
    cnt = max;
  }
  memcpy( line.txt, str, WSZ(cnt) );
  line.len = cnt;
  set_display_marks( 0, line.len );
}


// Remove cnt characters starting from pos.
void remove_chars( DWORD pos, DWORD cnt )
{
  set_display_marks( pos, line.len );
  memcpy( line.txt + pos, line.txt + pos + cnt, WSZ(line.len - pos - cnt) );
  line.len -= cnt;
}


// Insert string of cnt characters at pos.
void insert_chars( DWORD pos, PCWSTR str, DWORD cnt )
{
  if (line.len + cnt > max)
  {
    bell();
    cnt = max - line.len;
  }
  memmove( line.txt + pos + cnt, line.txt + pos, WSZ(line.len - pos) );
  memcpy( line.txt + pos, str, WSZ(cnt) );
  line.len += cnt;
  set_display_marks( pos, line.len );
}


// Replace old characters at pos with string of cnt characters.
void replace_chars( DWORD pos, DWORD old, PCWSTR str, DWORD cnt )
{
  if (old >= cnt)
  {
    set_display_marks( pos, pos + cnt );
    memcpy( line.txt + pos, str, WSZ(cnt) );
    if (old != cnt)
      remove_chars( pos + cnt, old - cnt );
  }
  else
  {
    set_display_marks( pos, pos + old );
    memcpy( line.txt + pos, str, WSZ(old) );
    insert_chars( pos + old, str + old, cnt - old );
  }
}


#define VK rec.Event.KeyEvent.wVirtualKeyCode	// damn long identifiers


// Read a key from the keyboard, returning its keymap position and setting chfn
// to its character and function.
char* get_key( PKey chfn )
{
  static INPUT_RECORD rec;
  DWORD  read;
  int	 shift, ctrl, alt;
  char*  key = NULL;

  if (rec.Event.KeyEvent.wRepeatCount == 0)
  {
    do
    {
      ReadConsoleInputW( hConIn, &rec, 1, &read );
      if (check_break > 1)
      {
	check_break = 1;
	chfn->fn = Erase;
	return key;
      }
    } while (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown ||
	     VK == VK_SHIFT || VK == VK_CONTROL || VK == VK_MENU);
  }
  --rec.Event.KeyEvent.wRepeatCount;

  *chfn = (Key){ rec.Event.KeyEvent.uChar.UnicodeChar, Default };
  shift = !!(rec.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED);
  ctrl	= (rec.Event.KeyEvent.dwControlKeyState &
	   (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
  alt	= (rec.Event.KeyEvent.dwControlKeyState &
	   (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));

  // Remap these keys to a more convenient location and use Shift+Ctrl instead
  // of Alt, due to Windows itself using that combination.
  switch (VK)
  {
    case VK_BACK:   VK = VK_DOWN+1; alt = (shift && ctrl); break;
    case VK_TAB:    VK = VK_DOWN+2; alt = (shift && ctrl); break;
    case VK_RETURN: VK = VK_DOWN+3; alt = (shift && ctrl); break;
    case VK_ESCAPE: VK = VK_DOWN+4; alt = (shift && ctrl); break;
  }

  if (VK >= VK_PRIOR && VK <= VK_DELETE)
  {
    alt = (alt) ? 3 : (ctrl) ? 2 : shift;
    key = &key_table[VK - VK_PRIOR][alt];
  }
  else if (VK >= VK_F1 && VK <= VK_F12)
  {
    alt = (alt) ? 3 : (ctrl) ? 2 : shift;
    key = &fkey_table[VK - VK_F1][alt];
  }
  else if (alt)
  {
    if (VK >= VK_NUMPAD0 && VK <= VK_NUMPAD9)
    {
      chfn->ch = process_keypad( VK );
    }
    else
    {
      WPARAM id = 0;
      switch (VK)
      {
	case 'C': id = 0xFFF2; break;  // Mark
	case 'F': id = 0xFFF4; break;  // Find...
	case 'S': id = 0xFFF3; break;  // Scroll
	case 'V': id = 0xFFF1; break;  // Paste
      }
      if (id)
      {
	SendMessage( GetForegroundWindow(), WM_COMMAND, id, 0 );
	chfn->fn = Ignore;
      }
    }
  }
  else if (chfn->ch == 0)
  {
    if (ctrl)
    {
      switch (VK)
      {
	case '2':                    break;     //       Ctrl+2 = ^@
	case 219: chfn->ch = 27;     break;	// Shift+Ctrl+[ = #^[
	case 220: chfn->ch = 28;     break;	// Shift+Ctrl+\ = #^\�
	case 221: chfn->ch = 29;     break;	// Shift+Ctrl+] = #^]
	case '6': chfn->ch = 30;     break;     //       Ctrl+6 = ^^
	case 189: chfn->ch = 31;     break;	//	 Ctrl+- = ^_
	default:  chfn->fn = Ignore; break;
      }
      if (chfn->fn == Default)
	key = &ctrl_key_table[chfn->ch][shift];
    }
    else
      chfn->fn = Ignore;
  }
  else if (chfn->ch < 32)
  {
    key = &ctrl_key_table[chfn->ch][shift];
  }

  if (key)
    chfn->fn = *key;

  return key;
}


// Hook the keyboard to bypass Windows handling of Alt+Enter.

HHOOK hKeyHook;
HANDLE event;

LRESULT CALLBACK KeyEvent( int nCode, WPARAM wParam, LPARAM lParam )
{
  if (nCode == HC_ACTION && wParam == WM_SYSKEYDOWN &&
      *(DWORD*)lParam == VK_RETURN)
  {
    // Signal the input loop we've found Alt+Enter and discard it.
    SetEvent( event );
    return 1;
  }

  return CallNextHookEx( hKeyHook, nCode, wParam, lParam );
}

DWORD msgloop( LPVOID param )
{
  MSG message;

  hKeyHook = SetWindowsHookEx( WH_KEYBOARD_LL, (HOOKPROC)KeyEvent,
			       GetModuleHandle( NULL ), 0 );
  GetMessage( &message, NULL, 0, 0 );
  UnhookWindowsHookEx( hKeyHook );

  return 0;
}


// Translate an Alt+Keypad sequence to a character.  If first is 0 treat the
// number as hexadecimal, using Divide, Multiply, Minus, Plus, Enter and Point
// as the hex digits A to F.  A number below 256 will be taken as an OEM char,
// otherwise treat it as Unicode.
WCHAR process_keypad( WORD first )
{
  INPUT_RECORD rec;
  CHAR	 ch;
  WCHAR  wch;
  DWORD  read;
  int	 num, base;
  HANDLE thread;
  DWORD  id;
  HANDLE objs[2];
  static const int vkdigit[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
				 0xB, 0xD, 0xE, 0xC, 0xF, 0xA };

  num  = first - VK_NUMPAD0;
  base = (num == 0) ? 16 : 10;
  if (base == 16)
  {
    thread = CreateThread( NULL, 4096, (LPTHREAD_START_ROUTINE)msgloop,
			   0, 0, &id );
    event = CreateEvent( NULL, FALSE, FALSE, "jmhConsoleEvent" );
    objs[0] = hConIn;
    objs[1] = event;
  }
  else
    thread = 0; // just to quieten gcc

  for (;;)
  {
    if (base == 16)
    {
      DWORD obj = WaitForMultipleObjects( 2, objs, FALSE, INFINITE);
      if (obj == 1)
      {
	rec.EventType = KEY_EVENT;
	rec.Event.KeyEvent.bKeyDown = TRUE;
	VK = VK_SEPARATOR;
      }
      else
	ReadConsoleInputW( hConIn, &rec, 1, &read );
    }
    else
      ReadConsoleInputW( hConIn, &rec, 1, &read );
    if (rec.EventType == KEY_EVENT)
    {
      if (rec.Event.KeyEvent.bKeyDown == FALSE)
      {
	if (VK == VK_MENU)
	  break;
      }
      else if (VK >= VK_NUMPAD0 && VK <= VK_DIVIDE)
	num = num * base + vkdigit[VK - VK_NUMPAD0];
    }
  }
  if (base == 16)
  {
    PostThreadMessage( id, WM_USER, 0, 0 );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
    CloseHandle( event );
  }

  if (num < 256)
  {
    ch = num;
    MultiByteToWideChar( GetConsoleOutputCP(), 0, &ch, 1, &wch, 1 );
  }
  else
    wch = num;

  return wch;
}


// Read and edit a line of input from the keyboard.
void edit_line( void )
{
  DWORD  imode, omode;			// original input & output modes
  CONSOLE_CURSOR_INFO cci, org_cci;	// current and original cursor size
  Key	 chfn;				// character and function read
  char*  key;				// key read
  BOOL	 recording = FALSE;		// recording a keyboard macro?
  PMacro mac = NULL,  rec_mac = NULL;	// macro playing and recording
  DWORD  mac_pos = 0, rec_len = 0;	// position of playback, recording
  BOOL	 done = FALSE;			// done editing?
  int	 ovr = option.overwrite;	// insert/overwrite flag
  int	 pos = 0;			// position within the line
  int	 empty = 0;			// searching an empty line?
  int	 recall = option.auto_recall;	// is auto-recall active?
  int	 cont_recall = 1;		// should auto-recall remain active?
  PHistory hist, shist; 		// position within history, of match
  PHistory fnp = NULL;			// list of completed filenames
  PWSTR  fnm;				// current filename
  int	 compl = 0, name = 0;		// filename completion flags
  BOOL	 quote, fnoq = FALSE, pq = FALSE; // filename quoting flags
  BOOL	 dir;				// filename is a directory
  DWORD  start, end, cnt;		// scratch variables
  DWORD  read;				// dummy variable for API functions

  GetConsoleScreenBufferInfo( hConOut, &screen );
  if (show_prompt)
    display_prompt();
  else
    show_prompt = TRUE;

  GetConsoleMode( hConIn,  &imode );
  GetConsoleMode( hConOut, &omode );
  SetConsoleMode( hConIn,  imode & ~0x1F );	// just keep the extended flags
  SetConsoleMode( hConOut, ENABLE_PROCESSED_OUTPUT|ENABLE_WRAP_AT_EOL_OUTPUT );

  GetConsoleCursorInfo( hConOut, &org_cci );
  cci.bVisible = TRUE;
  cci.dwSize   = option.cursor_size[ovr];
  SetConsoleCursorInfo( hConOut, &cci );

  hist = &history;
  done = FALSE;
  while (!done)
  {
    if (mac)
    {
      chfn = (mac->len == 1) ? mac->chfn : mac->func[mac_pos];
      if (++mac_pos == mac->len)
      {
	mac = NULL;
	mac_pos = 0;
      }
      key = NULL;
    }
    else
      key = get_key( &chfn );

    dispbeg = dispend = 0;		// nothing to display
    compl >>= 1;			// update state of completion
    name  >>= 1;
    empty >>= 1;			// update state of empty search
    recall &= cont_recall;		// update state of auto-recall
    cont_recall = 0;			//  and assume it shouldn't continue

    switch (chfn.fn)
    {
      case Ignore:
      break;

      case Quote:
	get_key( &chfn );
	chfn.fn = Default;

      case Default:
	if (chfn.ch == 0)		// disallow NULs
	{
	  bell();
	  chfn.fn = Ignore;
	}
      break;

      case CharLeft:
	if (pos > 0)
	  --pos;
      break;

      case CharRight:
	if (pos < line.len)
	  ++pos;
      break;

      case WordLeft:
      case DelWordLeft:
	if (pos > 0)
	{
	  start = pos;
	  while (--pos >= 0 && !isword( line.txt[pos] )) ;
	  while (pos >= 0 && isword( line.txt[pos] ))
	    --pos;
	  ++pos;
	  if (chfn.fn == DelWordLeft)
	    remove_chars( pos, start - pos );
	}
      break;

      case WordRight:
      case DelWordRight:
	if (pos < line.len)
	{
	  start = pos;
	  while (pos < line.len && isword( line.txt[pos] ))
	    ++pos;
	  while (pos < line.len && !isword( line.txt[pos] ))
	    ++pos;
	  if (chfn.fn == DelWordRight)
	  {
	    remove_chars( start, pos - start );
	    pos = start;
	  }
	}
      break;

      case StringLeft:
	if (pos > 0)
	{
	  while (--pos >= 0 && isblank( line.txt[pos] )) ;
	  while (pos >= 0 && !isblank( line.txt[pos] ))
	    --pos;
	  ++pos;
	}
      break;

      case StringRight:
	pos = skip_nonblank( pos );
	pos = skip_blank( pos );
      break;

      case BegLine:
	pos = 0;
      break;

      case EndLine:
	pos = line.len;
      break;

      case DelLeft:
	if (pos > 0)
	  remove_chars( --pos, 1 );
	cont_recall = 1;
      break;

      case DelRight:
	if (pos < line.len)
	  remove_chars( pos, 1 );
	cont_recall = 1;
      break;

      case DelArg:
	end = 0;
	do
	{
	  start = get_string( end, &cnt, TRUE );
	  end	= skip_blank( start + cnt );
	} while (end <= pos && end < line.len);
	remove_chars( pos = start, end - start );
      break;

      case DelBegLine:
	remove_chars( 0, pos );
	pos = 0;
      break;

      case DelEndLine:
	set_display_marks( pos, line.len );
	line.len = pos;
      break;

      case StoreErase:
	add_to_history();
	hist = &history;

      case Erase:
	set_display_marks( 0, line.len );
	pos = line.len = 0;
	cont_recall = 1;
	hist = &history;
      break;

      case DelEndExec:
	set_display_marks( pos, line.len );
	line.len = pos;

      case Enter:
	add_to_history();
	done = TRUE;
      break;

      case Wipe:
	FillConsoleOutputCharacterW( hConOut, ' ', line.len,
				     screen.dwCursorPosition, &read );
	SetConsoleCursorPosition( hConOut, screen.dwCursorPosition );
	done = TRUE;
      break;

      case Transpose:
	if (line.len >= 2)
	{
	  start = (pos == 0) ? 0 : (pos == line.len) ? pos - 2 : pos - 1;
	  chfn.ch = line.txt[start];
	  line.txt[start]   = line.txt[start+1];
	  line.txt[start+1] = chfn.ch;
	  set_display_marks( start, start + 2 );
	}
      break;

      case FirstLine:
	hist = history.next;
	goto hist_line;

      case LastLine:
	hist = history.prev;
	goto hist_line;

      case PrevLine:
	hist = hist->prev;
	goto hist_line;

      case NextLine:
	hist = hist->next;
	goto hist_line;

      case SearchBack:
      case SearchForw:
	if (option.empty_hist)
	{
	  empty |= 2;
	  if (empty == 2 && line.len != 0)
	    empty = 0;
	  else
	    pos = 0;
	}
	shist = search_history( hist, pos, (chfn.fn == SearchBack) );
	if (shist == NULL)
	{
	  bell();
	  break;
	}
	hist = shist;
	recall = option.auto_recall;
	cont_recall = 1;

      hist_line:
	copy_chars( hist->line, hist->len );
	if ((chfn.fn != SearchBack && chfn.fn != SearchForw) || (empty & 2))
	  pos = line.len;
      break;

      case List:		// name is  2 for new name completion
      case Cycle:		//	    3 for continuing name
      case CycleBack:		//	    0 for new & continuing directory
	name |= 2;		//	    1 for directory after name
				//	    2 for name after directory
      case ListDir:
      case CycleDir:		// compl is 2 for for new completion
      case CycleDirBack:	//	    3 for continuing it
	compl |= 2;
	if (compl == 2 || name == 1 || name == 2)
	{
	  if ((end = find_files( &pos, !(name & 2) )) == -1)
	  {
	    compl = 0;
	    bell();
	    break;
	  }
	  fnoq = pq = FALSE;
	  if (found_quote)
	    fnoq = pq = TRUE, --path_pos;
	  else
	  {
	    // Check if one of the characters after the prefix needs quoting.
	    for (fnp = fname->next; fnp != fname; fnp = fnp->next)
	    {
	      if (fnp->len > end && quote_needed( fnp->line + end, 1 ))
	      {
		pq = TRUE;
		break;
	      }
	    }
	  }
	  fnp = fname;
	  fnm = fname->next->line;
	  if (fname_cnt == 1 || end == -2 ||
	      chfn.fn == CycleBack || chfn.fn == CycleDirBack)
	  {
	    if (fname_cnt == 1)
	      compl = 0;
	    else if (chfn.fn == List || chfn.fn == ListDir)
	    {
	      list_files();
	      break;
	    }
	    if (chfn.fn == CycleBack || chfn.fn == CycleDirBack)
	    {
	      fnp = fname->prev;
	      fnm = fnp->line;
	    }
	    else
	      fnp = fname->next;
	    end = fnp->len;
	  }
	}
	else if (chfn.fn == List || chfn.fn == ListDir)
	{
	  list_files();
	  break;
	}
	else
	{
	  if (chfn.fn == Cycle || chfn.fn == CycleDir)
	    fnp = fnp->next;
	  else // (chfn.fn == CycleBack || chfn.fn == CycleDirBack)
	    fnp = fnp->prev;
	  if (fnp == fname)
	    bell();
	  fnm = fnp->line;
	  end = fnp->len;
	}
	quote = (pq || quote_needed( fnm, end ));
	if (quote && !fnoq)
	{
	  insert_chars( path_pos, L"\"", 1 );
	  ++fname_pos;
	  ++pos;
	  fnoq = TRUE;
	}
	else if (!quote && fnoq)
	{
	  remove_chars( path_pos, 1 );
	  --fname_pos;
	  --pos;
	  fnoq = FALSE;
	}
	dir = (fnm[end-1] == dirchar);
	if (dir && option.no_slash)
	  --end;
	replace_chars( fname_pos, pos - fname_pos, fnm, end );
	pos = fname_pos + end;
	if (!dir && fnp != fname)
	{
	  insert_chars( pos, L"\" " + 1 - quote, quote + 1 );
	  pos += quote + 1;
	}
      break;

      case SelectFiles:
	flist = malloc( FLIST_LEN*2 );
	if (flist == NULL)
	  break;
	if (find_files( &pos, -1 ))
	{
	  PWCHAR p = flist + fname_pos;
	  WCHAR  path[MAX_PATH];
	  if (found_quote)
	    --path_pos;
	  remove_chars( path_pos, pos - path_pos );
	  pos = path_pos;
	  make_relative( flist, path );
	  int d = wcslen( path ), dq = quote_needed( path, d ), f, q = dq;
	  while (*p)
	  {
	    f = wcslen( p );
	    if (dq || (q = quote_needed( p, f )))
	      insert_chars( pos++, L"\"", 1 );
	    insert_chars( pos, path, d );
	    pos += d;
	    insert_chars( pos, p, f );
	    pos += f;
	    insert_chars( pos, L"\" " + 1 - q, q + 1 );
	    pos += q + 1;
	    p += f + 1;
	  }
	}
	free( flist );
      break;

      case CmdSep:
	chfn = (Key){ CMDSEP, Default };
      break;

      case AutoRecall:
	option.auto_recall ^= 1;
	recall = option.auto_recall;
	cont_recall = 1;
      break;

      case MacroToggle:
	option.disable_macro ^= 1;
      break;

      case InsOvr:
	ovr ^= 1;
	cci.dwSize = option.cursor_size[ovr];
	SetConsoleCursorInfo( hConOut, &cci );
      break;

      case Play:
	mac = find_macro( key );
	if (mac && mac->len <= 0)
	{
	  copy_chars( (mac->len == -1) ? &mac->chfn.ch : mac->line, -mac->len );
	  done = TRUE;
	}
      break;

      case Record:
	if (recording)
	  break;
	if (!option.nocolour)
	  SetConsoleTextAttribute( hConOut, option.rec_col );
	dispbeg = pos;
	dispend = pos + printf( " * Press key for recording * " );
	if (!option.nocolour)
	  SetConsoleTextAttribute( hConOut, screen.wAttributes );
	key = get_key( &chfn );
	chfn.fn = Ignore;
	if (key != NULL && *key != Erase && *key != Enter && *key != Record)
	{
	  rec_mac = add_macro( key );
	  if (rec_mac)
	  {
	    recording = TRUE;
	    rec_len = 0;
	    *key = Record;
	    set_display_marks( 0, line.len );
	  }
	}
      break;

      case VarSubst:
	expand_braces();
	expand_vars( TRUE );
	associate();
	expand_macro();
	expand_symbol();
	pop_macro();
	pos = line.len;
      break;
    }

    if (recording)
    {
      if (chfn.fn == Record)
      {
	recording = FALSE;
      }
      else if (chfn.fn == DelLeft && rec_mac->len > 0 &&
	       rec_mac->func[rec_mac->len-1].fn == Default)
      {
	--rec_mac->len;
      }
      else if (chfn.fn != Ignore)
      {
	if (rec_mac->len == rec_len)
	{
	  PKey k = realloc( rec_mac->func, (rec_len + 10) * sizeof(Key) );
	  if (k == NULL)
	    recording = FALSE;
	  else
	  {
	    rec_mac->func = k;
	    rec_len += 10;
	  }
	}
	if (recording)
	{
	  rec_mac->func[rec_mac->len++] = chfn;
	  if (done)
	    recording = FALSE;
	}
      }
      if (!recording)
      {
	end_macro( rec_mac );
	set_display_marks( 0, line.len );
      }
    }

    if (chfn.fn == Default)
    {
      if (ovr || recall)
      {
	if (pos != max)
	{
	  set_display_marks( pos, pos + 1 );
	  if (pos == line.len)
	    ++line.len;
	}
      }
      else if (line.len < max)
      {
	memmove( line.txt + pos + 1, line.txt + pos, WSZ(line.len - pos) );
	set_display_marks( pos, ++line.len );
      }
      if (dispend)
      {
	line.txt[pos++] = chfn.ch;
	if (recall)
	{
	  shist = search_history( hist->next, pos, TRUE );
	  if (shist == NULL)
	  {
	    set_display_marks( pos, line.len );
	    line.len = pos;
	  }
	  else
	  {
	    hist = shist;
	    copy_chars( hist->line, hist->len );
	    cont_recall = 1;
	  }
	}
      }
      else
	bell();
    }

    cnt = dispend - dispbeg;
    if (cnt)
    {
      COORD c;
      int len = line.len - dispbeg;
      if (len > 0)
      {
	// Force a scroll if the line extends beyond the console buffer.
	c = line_to_scr( dispbeg + len );
	if (c.Y >= screen.dwSize.Y)
	{
	  SMALL_RECT src;
	  COORD dst;
	  CHAR_INFO fill;
	  src.Top = c.Y - screen.dwSize.Y + 1;
	  src.Bottom = screen.dwSize.Y - 1;
	  src.Left = 0;
	  src.Right = screen.dwSize.X - 1;
	  dst.X = dst.Y = 0;
	  fill.Char.AsciiChar = ' ';
	  fill.Attributes = screen.wAttributes;
	  ScrollConsoleScreenBuffer( hConOut, &src, NULL, dst, &fill );
	  screen.dwCursorPosition.Y -= src.Top;
	}
	else if (c.Y > screen.srWindow.Bottom)
	{
	  screen.srWindow.Top += c.Y - screen.srWindow.Bottom;
	  screen.srWindow.Bottom += c.Y - screen.srWindow.Bottom;
	  SetConsoleWindowInfo( hConOut, TRUE, &screen.srWindow );
	}

	c = line_to_scr( dispbeg );
	if (!option.nocolour)
	  FillConsoleOutputAttribute( hConOut, (recording) ? option.rec_col
							   : option.cmd_col,
				      len, c, &read );
	// The Unicode version will not write control characters using a
	// TrueType font, so remap them to their Unicode code point.
	for (start = end = 0; end < len; ++end)
	{
	  if (line.txt[dispbeg+end] < 32)
	  {
	    if (end > start)
	    {
	      WriteConsoleOutputCharacterW( hConOut, line.txt+dispbeg+start,
					    end - start, c, &read);
	      c.X += read;
	      if (c.X >= screen.dwSize.X)
	      {
		c.Y += c.X / screen.dwSize.X;
		c.X %= screen.dwSize.X;
	      }
	    }
	    start = end + 1;
	    WriteConsoleOutputCharacterW( hConOut,
					  ControlChar + line.txt[dispbeg+end],
					  1, c, &read );
	    if (++c.X == screen.dwSize.X)
	    {
	      c.X = 0;
	      ++c.Y;
	    }
	  }
	}
	WriteConsoleOutputCharacterW( hConOut, line.txt+dispbeg+start,
				      end - start, c, &read);
	cnt -= len;
      }
      if (cnt)
      {
	c = line_to_scr( line.len );
	FillConsoleOutputCharacterW( hConOut, ' ', cnt, c, &read );
	if (!option.nocolour)
	  FillConsoleOutputAttribute(hConOut, screen.wAttributes, cnt,c, &read);
      }
    }

    SetConsoleCursorPosition( hConOut, line_to_scr( (done) ? line.len : pos ) );
  }

  SetConsoleCursorInfo( hConOut, &org_cci );
  SetConsoleMode( hConOut, omode );
  // See if the user has changed QuickEdit or Insert modes.
  GetConsoleMode( hConIn,  &read );
  if ((read & ENABLE_QUICK_EDIT_MODE) ^ (imode & ENABLE_QUICK_EDIT_MODE))
    imode ^= ENABLE_QUICK_EDIT_MODE;
  if ((read & ENABLE_INSERT_MODE) ^ (imode & ENABLE_INSERT_MODE))
    imode ^= ENABLE_INSERT_MODE;
  SetConsoleMode( hConIn,  imode );

  if ((line.len + screen.dwCursorPosition.X) % screen.dwSize.X)
    WriteConsoleW( hConOut, L"\n", 1, &read, NULL );
}


// Display the user's prompt.
void display_prompt( void )
{
  DWORD read;

  if (kbd)
  {
    WriteConsoleW( hConOut, L"\n", 1, &read, NULL );
    GetConsoleScreenBufferInfo( hConOut, &screen );
    WriteConsoleW( hConOut, prompt.txt, prompt.len, &read, NULL );
    if (*p_attr)
      WriteConsoleOutputAttribute( hConOut, p_attr, prompt.len,
				   screen.dwCursorPosition, &read );
    GetConsoleScreenBufferInfo( hConOut, &screen );
  }
}


// ------------------------------   History   --------------------------------


// Allocate a new history line (does not set the previous and next pointers).
PHistory new_history( PCWSTR txt, DWORD len )
{
  PHistory h;

  h = malloc( sizeof(History) + WSZ(len) );
  if (h)
  {
    memcpy( h->line, txt, WSZ(len) );
    h->len = len;
  }

  return h;
}


// Remove the item from the history.  Should not be the history itself.
void remove_from_history( PHistory h )
{
  h->prev->next = h->next;
  h->next->prev = h->prev;
  free( h );
  --histsize;
}


// Add the line to the history.  If the line already exists (matched case
// sensitively), just move it to the front.
void add_to_history( void )
{
  PHistory h;

  if (line.len < option.min_length)	// line is too small to be remembered
    return;

  for (h = history.prev; h != &history; h = h->prev)
    if (h->len == line.len && memcmp( h->line, line.txt, WSZ(line.len) ) == 0)
      break;

  if (h == &history)			// not already present
  {
    if (option.histsize && histsize == option.histsize)
      remove_from_history( history.next ); // too many lines, remove the first
    h = new_history( line.txt, line.len );
    if (!h)
      return;
    ++histsize;
  }
  else
  {
    h->prev->next = h->next;		// found it, so relocate it
    h->next->prev = h->prev;
  }
  h->prev = history.prev;		// make it the last line
  h->next = &history;
  history.prev->next = h;
  history.prev = h;
}


// Find the first len characters of the line in the history (ignoring case).
// back is TRUE to search backwards (most recent lines first).	Search starts
// from the line /after/ hist.	Return a pointer to the matching history, or
// NULL if not found.
PHistory search_history( PHistory hist, DWORD len, BOOL back )
{
  PHistory h;
  BOOL	 fnd;

  h = hist;
  do
  {
    h	= (back) ? h->prev : h->next;
    fnd = (h->len >= len && _wcsnicmp( h->line, line.txt, len ) == 0);
  } while (!fnd && h != hist);

  return (fnd) ? h : NULL;
}


// ------------------------   Filename Completion   --------------------------


// Find the files matching name (using NULL to continue), testing against the
// extensions in extlist (if extlen is not zero; if exe is TRUE the extension
// must be in the list, otherwise it must NOT be in the list).	If dirs is TRUE
// only directories will be matched; if exe is TRUE only executables and assoc-
// iated files.  The find handle is returned in fh, with fd containing the file
// information.  Returns FALSE if no (more) names matched, TRUE otherwise.
BOOL match_file( PCWSTR name, PCWSTR extlist, DWORD extlen, BOOL dirs, BOOL exe,
		 PHANDLE fh, PWIN32_FIND_DATAW fd )
{
  static const WCHAR DOT[] = L".";
  PCWSTR dot;
  WCHAR  path[MAX_PATH], buf[MAX_PATH];

  if (name)
  {
    *fh = FindFirstFileW( name, fd );
    if (*fh == INVALID_HANDLE_VALUE)
      return FALSE;
  }
  else if (!FindNextFileW( *fh, fd ))
  {
    FindClose( *fh );
    return FALSE;
  }

  do
  {
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      // Directory always succeeds, but ignore "." and "..".
      if (fd->cFileName[0] == '.' &&
	  (fd->cFileName[1] == '\0' ||
	   (fd->cFileName[1] == '.' && fd->cFileName[2] == '\0')))
	continue;
      return TRUE;
    }
    else if (dirs)			// not a directory, but only dirs wanted
      continue;
    if (extlen == 0)			// not matching extension
      return TRUE;
    for (dot = NULL, name = fd->cFileName; *name; ++name)
      if (*name == '.')
	dot = name;
    if (!dot)				// there's no extension, but pretend
    {					//  there is so extensionless matching
      dot  = DOT;			//  can work
      name = dot + 1;
    }
    if (exe)
    {
      if (match_ext( dot, name - dot, extlist, extlen ) ||
	  find_assoc( dot, name - dot ))
      {
	if (dot == DOT) 		// the dot is needed for association
	  wcscat( fd->cFileName, dot );
	return TRUE;
      }
      // Didn't find the extension in our lists, so try Windows'.
      if (dot != DOT)
      {
	memcpy( path, line.txt + path_pos, WSZ(fname_pos - path_pos) );
	wcscpy( path + fname_pos - path_pos, fd->cFileName );
	if (FindExecutableW( path, NULL, buf ) > (HINSTANCE)32)
	  return TRUE;
      }
    }
    else if (!match_ext( dot, name - dot, extlist, extlen ))
      return TRUE;
  } while (FindNextFileW( *fh, fd ));

  FindClose( *fh );
  return FALSE;
}


// The first time the open dialog is used it puts the window behind all other
// console windows (seems to be something to do with ReadConsoleInput).  Use
// the hook to move it back to the front.
UINT_PTR CALLBACK OpenHook( HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam )
{
  if (msg == WM_INITDIALOG)
    SetForegroundWindow( dlg );
  return 0;
}


// Find the files matching the path up to pos.	If dirs is TRUE only find
// directories; if -1, use the open dialog.  Returns -1 if no files matched;
// -2 if wildcards were explicitly given; otherwise the length of the
// portion common to all files; the dialog returns TRUE if any files were
// selected, FALSE otherwise.  path_pos is set to the start of the path (if
// found_quote is TRUE there is a quote before it); fname_pos to the start
// of the filename; fname_cnt has the number of matching files; and
// fname_max is the length of the longest.
int find_files( int* pos, int dirs )
{
  HANDLE   fh;
  WIN32_FIND_DATAW fd;
  OPENFILENAMEW ofn;
  PHistory f, p;
  int	   beg;
  DWORD    end;
  BOOL	   wild, exe;
  WCHAR    wch[2];
  int	   prefix;
  DWORD    extlen;
  BOOL	   match;
  DWORD    quote;
  WCHAR    dir[MAX_PATH];
  static int openinit = FALSE;

  // Free the names from the previous completion.
  if (fname)
  {
    f = fname;
    do
    {
      p = f->next;
      free( f );
      f = p;
    } while (f != fname);
    fname = NULL;
  }

  // Find the start of the path - either the first non-terminated quote or
  // an appropriate delimiter.
  found_quote = quote = FALSE;
  for (path_pos = beg = 0; beg < *pos; ++beg)
  {
    if (quote)
    {
      if (is_quote( beg ))
	quote = FALSE;
    }
    else if (is_quote( beg ))
      quote = TRUE;
    else if (line.txt[beg] <= ' ' || wcschr( INVALID_FNAME, line.txt[beg] ))
      path_pos = beg + 1;
  }
  // Remove all quotes...
  for (beg = path_pos; beg < *pos; ++beg)
  {
    if (is_quote( beg ))
    {
      found_quote = TRUE;
      remove_chars( beg--, 1 );
      --*pos;
    }
  }
  // ...but still keep the opening quote.
  if (found_quote)
  {
    insert_chars( path_pos++, L"\"", 1 );
    ++*pos;
  }

  // If the path starts at the very beginning of the line, only complete
  // executable files.
  exe = (path_pos == found_quote);

  // Now find the filename position within the path and check for wildcards.
  wild = FALSE;
  for (fname_pos = beg = path_pos; beg < *pos; ++beg)
  {
    switch (line.txt[beg])
    {
      case '*' :                                // assume they're in the name
      case '?' : wild = TRUE; break;
      case '/' :
      case '\\': dirchar   = line.txt[beg];     // use the same separator
      case ':' : fname_pos = beg + 1;
    }
  }

  // Add the wildcard if not already present and NUL-terminate the line for the
  // API call (there's always room for it due to max being two less).
  wch[0] = line.txt[*pos];
  wch[1] = line.txt[*pos+1];
  if (wild)
    line.txt[*pos] = '\0';
  else
  {
    line.txt[*pos]   = '*';
    line.txt[*pos+1] = '\0';
  }

  if (dirs < 0)
  {
    ZeroMemory( &ofn, sizeof(ofn) );
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetForegroundWindow();
    ofn.lpstrFile   = flist;
    ofn.nMaxFile    = FLIST_LEN;
    ofn.lpstrFilter = make_filter( exe );
    if (path_pos == fname_pos)
      ofn.lpstrInitialDir = L".";
    else
    {
      wcsncpy( fd.cFileName, line.txt + path_pos, fname_pos - path_pos );
      fd.cFileName[fname_pos-path_pos] = '\0';
      // Open dialog doesn't like using other drives.
      GetFullPathNameW( fd.cFileName, MAX_PATH, dir, NULL );
      ofn.lpstrInitialDir = dir;
    }
    ofn.lpstrTitle  = (exe) ? L"Select Executable" : L"Select Files";
    ofn.Flags	    = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_HIDEREADONLY
			| OFN_NOCHANGEDIR | OFN_NODEREFERENCELINKS;
    if (!openinit)
    {
      openinit = TRUE;
      ofn.lpfnHook = OpenHook;
      ofn.Flags |= OFN_ENABLEHOOK;
    }
    *flist = '\0';
    ofn.nFileExtension = ~0;
    prefix = GetOpenFileNameW( &ofn );
    fname_pos = ofn.nFileOffset;
    if (ofn.nFileExtension != (WORD)~0)
      flist[fname_pos-1] = '\0';
    if (ofn.lpstrFilter)
      free( (char*)ofn.lpstrFilter );
  }
  else
  {
    // Store the original completion name as the first entry.
    fname = new_history( line.txt + fname_pos, *pos - fname_pos );
    if (!fname)
      return -1;
    fname->next = fname->prev = fname;

    if (exe)
    {
      extlen = get_env_var( L"FEXEC", NULL );
      if (extlen == 0)
	extlen = get_env_var( L"PATHEXT", FEXEC );
    }
    else
      extlen = get_env_var( L"FIGNORE", FIGNORE );

    match = match_file(line.txt+path_pos, envvar.txt,extlen, dirs,exe, &fh, &fd);
    // If nothing was found try again without the ignore list.
    if (!match && !exe && !dirs)
      match = match_file(line.txt+path_pos, NULL,extlen=0, FALSE,FALSE, &fh, &fd);
    prefix = (!match || !wild) ? -1 : -2;
    fname_max = fname_cnt = 0;
    while (match)
    {
      ++fname_cnt;
      end = wcslen( fd.cFileName );
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	fd.cFileName[end++] = dirchar;
      if (end > fname_max)
	fname_max = end;
      f = new_history( fd.cFileName, end );

      if (!f)
	return -1;

      if (!wild)
      {
	// Find the portion common to all the names.
	if (prefix < 0)
	  prefix = end;
	else
	{
	  for (beg = 0; beg < prefix; ++beg)
	  {
	    if ((WCHAR)(DWORD)CharLowerW( (PWCHAR)(DWORD)f->line[beg] ) !=
		(WCHAR)(DWORD)CharLowerW( (PWCHAR)(DWORD)fname->next->line[beg] ))
	      break;
	  }
	  // Don't use less than original name (preserve trailing dot).
	  if (beg >= *pos - fname_pos)
	    prefix = beg;
	}
      }

      // Find where to insert the new name to sort the list.  Work backwards,
      // since NTFS maintains a sorted list, anyway.
      for (p = fname->prev; p != fname; p = p->prev)
      {
	if (CompareStringW( LOCALE_USER_DEFAULT, NORM_IGNORECASE,
			    f->line, f->len, p->line, p->len ) == 3)
	  break;
      }
      f->prev = p;
      f->next = p->next;
      p->next->prev = f;
      p->next = f;

      match = match_file( NULL, envvar.txt, extlen, dirs, exe, &fh, &fd );
    }
  }

  line.txt[*pos]   = wch[0];
  line.txt[*pos+1] = wch[1];

  return prefix;
}


// List all the files found by the completion.	If there are too many lines for
// the buffer, refuse to list any; prompt if there are too many for the window.
// Assumes more than one name.
void list_files( void )
{
  PHistory f;
  int	   lines, cols;
  DWORD    read;

  WriteConsoleW( hConOut, L"\n", 1, &read, NULL );

  // Are the names too long for column output?
  if (fname_max + 2 + fname_max > screen.dwSize.X)
  {
    if (check_name_count( fname_cnt ))
    {
      for (f = fname->next; f != fname; f = f->next)
      {
	WriteConsoleW( hConOut, f->line, f->len, &read, NULL );
	if (f->len % screen.dwSize.X)
	  WriteConsoleW( hConOut, L"\n", 1, &read, NULL );
      }
    }
  }
  else
  {
    cols = screen.dwSize.X / fname_max;
    if ((cols - 1) * 2 > screen.dwSize.X % fname_max)
      --cols;
    lines = fname_cnt / cols;
    if (fname_cnt % cols)
      ++lines;
    if (check_name_count( lines ))
    {
      SetConsoleMode( hConOut, ENABLE_PROCESSED_OUTPUT ); // don't wrap at EOL
      GetConsoleScreenBufferInfo( hConOut, &screen );
      cols = 0;
      for (f = fname->next; f != fname; f = f->next)
      {
	SetConsoleCursorPosition( hConOut, screen.dwCursorPosition );
	WriteConsoleW( hConOut, f->line, f->len, &read, NULL );
	if (++cols == lines)
	{
	  screen.dwCursorPosition.X += fname_max + 2;
	  screen.dwCursorPosition.Y -= lines - 1;
	  cols = 0;
	}
	else if (++screen.dwCursorPosition.Y == screen.dwSize.Y)
	{
	  WriteConsoleW( hConOut, L"\n", 1, &read, NULL );
	  --screen.dwCursorPosition.Y;
	}
      }
      if (cols)
      {
	screen.dwCursorPosition.Y += lines - cols - 1;
	SetConsoleCursorPosition( hConOut, screen.dwCursorPosition );
      }
      WriteConsoleW( hConOut, L"\n", 1, &read, NULL );
      SetConsoleMode(hConOut,ENABLE_PROCESSED_OUTPUT|ENABLE_WRAP_AT_EOL_OUTPUT);
    }
  }

  display_prompt();
  set_display_marks( 0, line.len );
}


// Return TRUE if the names should be displayed, FALSE if not.
BOOL check_name_count( int lines )
{
  Key yn;

  if (lines > screen.dwSize.Y - 2)
  {
    printf( "Too many names to display (%d)!\n", fname_cnt );
    return FALSE;
  }

  if (lines > screen.srWindow.Bottom - screen.srWindow.Top - 1)
  {
    printf( "Display all %d possibilities? ", fname_cnt );
    get_key( &yn );
    if (yn.ch == 'y' || yn.ch == 'Y')
    {
      puts( "Yes" );
      return TRUE;
    }
    puts( "No" );
    return FALSE;
  }

  return TRUE;
}


// Return TRUE if str should be quoted.
BOOL quote_needed( PCWSTR str, int cnt )
{
  while (--cnt >= 0)
    if (wcschr( QUOTE_FNAME, str[cnt] ))
      return TRUE;

  return FALSE;
}


// Make a filter string for the open dialog.
PWSTR make_filter( BOOL exe )
{
  PWSTR f;
  WCHAR buf[2048];
  int	len, extlen;

  *buf = '\0';
  len = wcslen( line.txt+fname_pos );
  if (exe && line.txt[fname_pos+len-1] == '*')
  {
    extlen = get_env_var( L"FEXEC", NULL );
    if (extlen == 0)
      extlen = get_env_var( L"PATHEXT", FEXEC );
    if (extlen)
    {
      PWSTR e = envvar.txt;
      int   d = 0;
      f = buf;
      while (*e)
      {
	while (e[++d] != '.' && e[d] != ';' && e[d] != ':' && e[d] != '\0') ;
	f += swprintf( f, L"%s%.*s;", line.txt+fname_pos, d, e );
	if (e[d] == ';' || e[d] == ':')
	  ++d;
	e += d;
	d = 0;
      }
      f[-1] = '\0';
    }
  }
  if (!*buf)
    wcscpy( buf, line.txt+fname_pos );

  len += 1 + wcslen( buf ) + 1 + 13;
  f = malloc( len * 2 );
  if (f)
  {
    wsprintfW( f, L"%s%c%s%cAll files%c*%c",
		  line.txt+fname_pos, 0, buf, 0, 0, 0 );
  }
  return f;
}


// Make an absolute path a relative path and ensure it ends with a backslash.
// If path starts with the current directory remove that portion.  If the cwd
// is one or two levels below path, use the shortcuts.	Otherwise keep the
// complete path.
void make_relative( PWSTR path, PWSTR rel )
{
  WCHAR buf[MAX_PATH];
  PWSTR cwd = buf, dir, root;

  GetCurrentDirectoryW( MAX_PATH, cwd );
  if (*cwd == *path)
  {
    *rel = '\0';
  }
  else
  {
    rel[0] = *path;
    rel[1] = ':';
    rel[2] = '\0';
    GetFullPathNameW( rel, MAX_PATH, cwd, NULL );
    rel += 2;
  }
  cwd  += 2;
  path += 2;
  root	= path;

  // Root current directory is always relative.
  if (cwd[1] == '\0')
  {
    if (*path != '\0')
    {
      wcscpy( rel, path+1 );
      wcscat( rel, L"\\" );
    }
    return;
  }

  // Root path is always absolute.
  if (*path == '\0')
  {
    wcscat( rel, L"\\" );
    return;
  }

  // Skip over the common directories.
  dir = cwd;
  while (*++path == *++cwd && *path)
  {
    if (*cwd == '\\')
      dir = cwd;
  }

  // In the current directory.
  if (*cwd == '\0' && *path == '\0')
    return;

  // Below the current directory.
  if (*cwd == '\0' && *path == '\\')
  {
    root = path + 1;
  }
  else
  {
    if (*cwd == '\\' && *path == '\0')
      dir = cwd;
    else
      path -= cwd - dir;

    // One level above, use parent shortcut.
    dir = wcschr( dir+1, '\\' );
    if (dir == NULL)
    {
      wcscpy( rel, L".." );
      rel += 2;
      root = path;
    }
    else
    {
      // Two levels above, use two parent shortcuts.
      dir = wcschr( dir+1, '\\' );
      if (dir == NULL)
      {
	wcscpy( rel, L"..\\.." );
	rel += 5;
	root = path;
      }
    }
  }

  wcscpy( rel, root );
  wcscat( rel, L"\\" );
}


// ------------------------------   Line Input	 -----------------------------


// Read the file.  Internal commands will be processed as usual; anything else
// will be added to the history.
BOOL read_cmdfile( PCSTR name )
{
  BOOL rc;

  kbd = FALSE;
  file = fopen( name, "r" );
  if (file != NULL)
  {
    line.txt = NULL;
    max = 0;
    while (get_file_line())
    {
      if (!internal_cmd())
	add_to_history();
    }
    fclose( file );
    file = NULL;
    rc = TRUE;
  }
  else
  {
    printf( "CMDkey: could not open \"%s\".\n", name );
    rc = FALSE;
  }

  return rc;
}


// Read a line from the file.  Blank lines and lines starting with '-' are
// ignored.
BOOL get_file_line( void )
{
  char buf[1024];

  do
  {
    if (fgets( buf, sizeof(buf), file ) == NULL)
    {
      if (line.txt && line.txt != ENDM)
	free( line.txt );
      if (def_macro)
      {
	line.txt = (LPWSTR)ENDM;
	line.len = ENDM_LEN;
	return TRUE;
      }
      line.txt = NULL;
      line.len = 0;
      return FALSE;
    }
  } while (*buf == '\n' || *buf == '-');

  line.len = MultiByteToWideChar( CP_OEMCP, 0, buf, -1, NULL, 0 );
  if (line.len > max)
  {
    line.txt = realloc( line.txt, WSZ(line.len) );
    if (line.txt == NULL)
      return FALSE;
    max = line.len;
  }
  MultiByteToWideChar( CP_OEMCP, 0, buf, -1, line.txt, line.len );

  line.len -= 2;			// discount LF and NUL
  return TRUE;
}


// Read the next line from the appropriate source.
void get_next_line( void )
{
  line.len = 0;
  kbd = FALSE;
  if (file)
    get_file_line();
  else if (macro_stk)
    get_macro_line();
  else if (mcmd.txt)
  {
    if (mcmd.len)
    {
      copy_chars( mcmd.txt, mcmd.len );
      free( mcmd.txt );
    }
    mcmd.txt = NULL;
  }
  else
  {
    kbd = TRUE;
    edit_line();
  }
}


// Retrieve a line from a macro, replacing "%n" with the nth argument (0 to 9),
// "%*" with all arguments, or "%n*" with all arguments from the nth.  Use the
// escape character to treat the '%' or '*' literally.
void get_macro_line( void )
{
  DWORD pos, arg, cnt;
  int	argnum, var;
  Line	temp;

  copy_chars( macro_stk->line->line, macro_stk->line->len );
  for (pos = 0; pos < line.len; ++pos)
  {
    if (line.txt[pos] == ESCAPE)
      ++pos;
    else if (line.txt[pos] == VARIABLE && pos+1 < line.len &&
	     (line.txt[pos+1] == '*' ||
	      (line.txt[pos+1] >= '0' && line.txt[pos+1] <= '9')))
    {
      ++pos;
      argnum   = (line.txt[pos] == '*') ? 2 : line.txt[pos] - '0' + 1;
      temp     = line;
      line.txt = macro_stk->name;
      line.len = macro_stk->len;
      arg = cnt = 0;
      do
      {
	arg = get_string( arg + cnt, &cnt, TRUE );
      } while (--argnum);
      line = temp;
      var  = 2;
      if (line.txt[pos] != '*' && pos+1 < line.len && line.txt[pos+1] == '*')
      {
	++pos;
	var = 3;
      }
      if (line.txt[pos] == '*')
	cnt = macro_stk->len - arg;
      replace_chars( pos - var + 1, var, macro_stk->name + arg, cnt );
    }
  }
  un_escape( ARG_ESCAPE );

  macro_stk->line = macro_stk->line->next;
  if (macro_stk->line == NULL)
    pop_macro();
}


// Finished with a macro, remove its line from the stack.
void pop_macro( void )
{
  if (macro_stk)
  {
    PDefine m = macro_stk;
    macro_stk = macro_stk->next;
    free( m );
  }
}


// ----------------------------   Line Output	------------------------------


// If the line contains an unquoted/unescaped CMDSEP, make a copy of everything
// after it and chop the line at it.
void multi_cmd( void )
{
  DWORD pos;
  BOOL	quote = FALSE;

  // Do a quick scan to see if it's worthwhile continuing.
  if (!memchr( line.txt, CMDSEP, WSZ(line.len) ))
    return;

  for (pos = 0; pos < line.len; ++pos)
  {
    if (quote)
    {
      if (line.txt[pos] == '"')
	quote = FALSE;
    }
    else if (line.txt[pos] == '"')
      quote = TRUE;
    else if (line.txt[pos] == ESCAPE)
      ++pos;
    else if (line.txt[pos] == CMDSEP)
    {
      ++pos;
      mcmd.len = line.len - pos;
      if (mcmd.len == 0)
	mcmd.txt = (PWSTR)1;
      else
      {
	mcmd.txt = new_txt( line.txt + pos, mcmd.len );
	if (!mcmd.txt)
	  return;
      }
      line.len = pos - 1;
      return;
    }
  }
}


// Convert '/' to '\', leading '-' to '/' and strip trailing '\'.
void dosify( void )
{
  DWORD pos;

  for (pos = 0; pos < line.len; ++pos)
  {
    if (line.txt[pos] == '/' || line.txt[pos] == '\\')
    {
      line.txt[pos] = '\\';
      if ((pos+1 == line.len || isblank( line.txt[pos+1] )) &&
	  pos > 0 && line.txt[pos-1] != ':' && !isblank( line.txt[pos-1] ))
	line.txt[pos] = ' ';
    }
    else if (line.txt[pos] == '-' && pos > 0 && isblank( line.txt[pos-1] ))
      line.txt[pos] = '/';
  }
}


// Perform brace expansion.  Brace expansion will duplicate what comes before
// (prepend) and after (postpend) the braces, with a comma-separated list of
// items inside the braces.  Use the escape character to treat braces and commas
// literally.  If the prepend has an unterminated quote, there must be no
// quotes inside the braces and the postpend must have the closing quote.  The
// pends will be terminated and separated by the CMD.EXE delimiter list; if the
// prepend terminator is a space, the postpend terminator will be used.
// eg: a;b{1,2}c,d ==> a;b1c;b2c,d
//     a b{1,2}c,d ==> a b1c,b2c,d
BOOL brace_expansion( void )
{
  int	count;				// keep track of nested braces
  int	pos;
  int	prepos,  prelen;		// position and length of the prepend
  int	postpos, postlen;		// position and length of the postpend
  WCHAR term;				// character used to separate items
  PWSTR pend;				// copy of the postpend
  BOOL	quote, q1;			// keep track of quotes
  BOOL	comma;				// must be at least two items

  prepos = pos = 0;
  term	 = ' ';
  quote  = FALSE;
  comma  = FALSE;
  while (!comma)
  {
    // Do a quick scan to see if it's worthwhile continuing.
    if (!memchr( line.txt + pos, '{', WSZ(line.len) )) // } (maintain balance)
      return FALSE;

    // Find the opening brace and the start of the prepend.
    for (; pos < line.len; ++pos)
    {
      if (is_quote( pos ))
	quote ^= TRUE;
      if (line.txt[pos] == ESCAPE)
	++pos;
      else if (line.txt[pos] == '{')    // }
	break;
      else if (!quote)
      {
	if (wcschr( BRACE_TERM, line.txt[pos] ))
	{
	  term	 = line.txt[pos];
	  prepos = pos + 1;
	}
	else if (wcschr( BRACE_STOP, line.txt[pos] ))
	  prepos = pos + 1;
      }
    }
    if (pos >= line.len)		// no opening brace
      return FALSE;
    prelen = pos - prepos;

    // Find the closing brace, checking for at least two items.
    q1 = FALSE;
    count = 1;
    for (postpos = ++pos; postpos < line.len; ++postpos)
    {
      if (q1)
      {
	if (is_quote( postpos ))
	  q1 = FALSE;
      }
      else if (is_quote( postpos ))
      {
	if (quote)
	  return FALSE; 		// quoted item with quoted prepend
	q1 = TRUE;
      }
      else if (line.txt[postpos] == ESCAPE)
	++postpos;
      else if (line.txt[postpos] == '{')
	++count;
      else if (line.txt[postpos] == '}')
      {
	if (--count == 0)
	  break;
      }
      else if (line.txt[postpos] == ',' && count == 1)
	comma = TRUE;
    }
    if (count)				// unbalanced braces
      return FALSE;
  }

  // Find the end of the postpend.  Made awkward by the use of quotes and
  // braces being used as part of the postpend.
  for (postlen = ++postpos; postlen < line.len; ++postlen)
  {
    if (count)				// skip over quotes inside braces
    {
      if (q1)
      {
	if (is_quote( postlen ))
	  q1 = FALSE;
      }
      else if (is_quote( postlen ))
	q1 = TRUE;
      else if (line.txt[postlen] == ESCAPE)
	++postlen;
      else if (line.txt[postlen] == '{')
	++count;
      else if (line.txt[postlen] == '}')
	--count;
    }
    else
    {
      if (is_quote( postlen ))
	quote ^= TRUE;
      if (line.txt[postlen] == ESCAPE)
	++postlen;
      else if (line.txt[postlen] == '{') // }
	++count;
      else if (!quote)
      {
	if (wcschr( BRACE_TERM, line.txt[postlen] ))
	{
	  if (term == ' ')
	    term = line.txt[postlen];
	  break;
	}
	if (wcschr( BRACE_STOP, line.txt[postlen] ))
	  break;
      }
    }
  }
  if (quote || count)			// unbalanced quotes, braces
    return FALSE;
  postlen -= postpos;
  if (postlen < 0)
    postlen = 0;

  // Make a copy of the postpend, including the terminator.
  pend = new_txt( line.txt + postpos, postlen + 1 );
  if (!pend)
    return FALSE;
  pend[postlen++] = term;

  // Remove the opening brace, thus generating the first prepend.
  pos = prepos + prelen;
  remove_chars( pos, 1 );
  // Now add the postpend and prepend to all subsequent items.
  for (; count >= 0; ++pos)
  {
    if (quote)
    {
      if (is_quote( pos ))
	quote = FALSE;
    }
    else if (is_quote( pos ))
      quote = TRUE;
    else if (line.txt[pos] == ESCAPE)
      ++pos;
    else if (line.txt[pos] == '{')
      ++count;
    else if (line.txt[pos] == '}')
      --count;
    else if (line.txt[pos] == ',' && count == 0)
    {
      replace_chars( pos, 1, pend, postlen );
      insert_chars( pos + postlen, line.txt + prepos, prelen );
      pos += postlen + prelen - 1;
    }
  }
  // Remove the closing brace, thus generating the last postpend.
  remove_chars( pos - 1, 1 );

  free( pend );

  return TRUE;
}


// Keep expanding braces until no more, then replace escaped characters.
void expand_braces( void )
{
  while (brace_expansion()) ;
  un_escape( BRACE_ESCAPE );
}


// If the first word has an extension in the association list, insert its
// definition at the start of the line.
BOOL associate( void )
{
  PDefine a;
  DWORD   ext, beg, cnt;
  int	  alt = 0;

  beg = get_string( 0, &cnt, FALSE );
  if (cnt == 0)
    return FALSE;

  ext = beg + cnt - 1;
  if (cnt > 1 && line.txt[ext] == '=')
  {
    alt = 1;
    --ext;
    --cnt;
  }

  if (line.txt[ext] == '/' || line.txt[ext] == '\\')
  {
    line.txt[ext] = '\\';
    a = find_define( &assoc_head, ext, 1 + alt );
    if (!a)
      return FALSE;
    if (cnt > 1 && line.txt[ext-1] != ':')
    {
      remove_chars( ext, 1 + alt );	// include the alternative indicator
      alt = 0;				//  and don't remove it again
    }
    else
      cnt = 1;
  }
  // Don't match a dot by itself, or consecutive dots.
  else if (line.txt[ext] == '.' && (ext == beg || line.txt[ext-1] == '.'))
    return FALSE;
  else
  {
    for (cnt = 1; line.txt[ext] != '.'; ++cnt, --ext)
      if (ext == beg || line.txt[ext] == '/' || line.txt[ext] == '\\'
		     || line.txt[ext] == ':')
	return FALSE;
    a = find_assoc( line.txt + ext, cnt + alt );
    if (!a)
      return FALSE;
  }

  if (alt)
    remove_chars( ext + cnt, 1 );
  insert_chars( 0, a->line->line, a->line->len );
  insert_chars( a->line->len, L" ", 1 );

  return TRUE;
}


// If the first word is a symbol, replace it with its definition.
BOOL expand_symbol( void )
{
  PDefine s;
  DWORD   sym, end;
  BOOL	  sp;

  sym = skip_blank( 0 );
  end = skip_nondelim( sym );

  s = find_define( &sym_head, sym, end - sym );
  if (!s)
    return FALSE;

  sp = (end < line.len && !isblank( line.txt[end] ));

  replace_chars( sym, end - sym, s->line->line, s->line->len );
  if (sp)
    insert_chars( sym + s->line->len, L" ", 1 );

  return TRUE;
}


// If the first word is a macro, replace the line with its definition.
BOOL expand_macro( void )
{
  PDefine m;
  DWORD   mac, end;

  mac = skip_blank( 0 );
  end = skip_nondelim( mac );

  m = find_define( &mac_head, mac, end - mac );
  if (!m)
    return FALSE;

  // Make a copy of the line in order to remember the arguments.
  if (!add_define( &macro_stk, 0, line.len ))
    return FALSE;

  macro_stk->line = m->line;
  get_macro_line();

  return TRUE;
}


// Determine if the first word is meant for CMDkey and execute it if so.
BOOL internal_cmd( void )
{
  DWORD pos, cnt;
  int	func;

  // Treat Control+Break as internal.
  if (check_break > 1)
  {
    check_break = 1;
    while (macro_stk)
      pop_macro();
    return TRUE;
  }

  pos = skip_blank( 0 );
  cnt = skip_nonblank( pos ) - pos;
  //if (cnt < MIN_CMD_LEN || cnt > MAX_CMD_LEN)
  if (cnt != CMD_LEN)
    return FALSE;

  func = search_cfg( line.txt + pos, cnt, internal, INTERNAL_CMDS - 1 );
  if (func == -1)
    return FALSE;

  if (kbd)
    un_escape( NULL );

  pos = skip_blank( pos + cnt );
  ((IntFunc*)func)( pos );

  return TRUE;
}


// Search for a word between two VARIABLE characters.  If the word exists in
// the environment (and env is true) or it's a symbol, replace it.  Use the
// escape character to treat VARIABLE as a normal character.
//
// Eg: defs sym -expansion-
//     %a%sym%	  --> %a-expansion- [if a is not defined]
//     %sym%sym%  --> -expansion-sym%
//     ^%sym%sym% --> ^%sym-expansion-
//     %sym^%sym% --> %sym^%sym%

void expand_vars( BOOL env )
{
  Line	  var = { NULL, 0 };
  PDefine d;
  DWORD   pos, cnt, start;

  start = ~0;
  for (pos = 0; pos < line.len; ++pos)
  {
    if (line.txt[pos] == ESCAPE)
      ++pos;
    else if (line.txt[pos] == VARIABLE)
    {
      if (start == ~0)
	start = pos + 1;
      else
      {
	cnt = pos - start;
	if (env)
	{
	  line.txt[pos] = '\0';
	  var.len	= get_env_var( line.txt + start, NULL );
	  var.txt	= envvar.txt;
	  line.txt[pos] = VARIABLE;
	}
	else
	  var.len = 0;
	if (!var.len)
	{
	  d = find_define( &sym_head, start, cnt );
	  if (d)
	  {
	    var.txt = d->line->line;
	    var.len = d->line->len;
	  }
	}
	if (var.len)
	{
	  replace_chars( start - 1, cnt + 2, var.txt, var.len );
	  start = ~0;
	}
	else
	  start = pos + 1;
      }
    }
  }
  un_escape( VAR_ESCAPE );
}


// --------------------------	Internal Commands   --------------------------


// Define an association.
void execute_defa( DWORD pos )
{
  PDefine a;
  DWORD   end, def;

  if (pos == line.len)
    return;

  execute_dela( pos );			// remove any previous association

  end = skip_nonblank( pos );
  def = skip_blank( end );
  if (def == line.len)
    return;

  a = add_define( &assoc_head, pos, end - pos );
  if (a)
    a->line = add_line( def );
}


// Define a key.  This can be a function, a command or a macro.
void execute_defk( DWORD pos )
{
  DWORD  end, cnt;
  char*  key;
  int	 func;
  PMacro m;

  if (pos == line.len)
    return;

  end = skip_nonblank( pos );
  cnt = end - pos;
  key = find_key( pos, cnt );
  if (key == NULL)
  {
    printf( "CMDkey: unrecognised key: %.*S\n", (int)cnt, line.txt + pos );
    return;
  }

  pos = skip_blank( end );
  if (pos == line.len)			// not defining it to anything
  {					//  so wipe it
    if (*key == Play)
      del_macro( key );
    *key = Ignore;
    return;
  }

  if (line.txt[pos] == '=')             // a command - replace the line with
  {					//  the definition and execute it
    ++pos;
    m = add_macro( key );
    if (!m)
      return;
    m->len = pos - line.len;
    if (m->len == -1)
      m->chfn.ch = line.txt[pos];
    else if (m->len != 0)
    {
      m->line = new_txt( line.txt + pos, -m->len );
      if (!m->line)
      {
	del_macro( key );
	return;
      }
    }
    *key = Play;
    return;
  }

  pos = get_string( pos, &cnt, FALSE );
  end = skip_blank( pos + cnt );
  if (end == line.len && !found_quote)
  {
    // A single word - change the keymap.
    func = search_cfg( line.txt + pos, cnt, cfg, LastFunc - 1 );
    if (func == -1)
    {
      printf( "CMDkey: unrecognised function: %.*S\n", (int)cnt, line.txt+pos );
      return;
    }
    if (*key == Play)
      del_macro( key );
    *key = func;
  }
  else					// a macro - a series of functions
  {					//  and/or characters
    m = add_macro( key );
    if (!m)
      return;
    m->func = malloc( (line.len - pos) * sizeof(Key) );
    if (!m->func)
    {
      del_macro( key );
      return;
    }
    do
    {
      if (found_quote)			// add each character in the string
      {
	while (cnt--)
	{
	  if (line.txt[pos] == '"')
	  {
	    for (end = pos - 1; line.txt[end] == '\\'; --end) ;
	    m->len -= (pos - end) / 2;
	  }
	  m->func[m->len++] = (Key){ line.txt[pos++], Default };
	}
	if (pos < line.len) // && line.txt[pos] == '"')
	{
	  for (end = pos - 1; line.txt[end] == '\\'; --end) ;
	  m->len -= (pos - end) / 2;
	  ++pos;			// jump over the closing quote
	}
      }
      else				// add a function
      {
	func = search_cfg( line.txt + pos, cnt, cfg, LastFunc - 1 );
	if (func == -1)
	{
	  printf("CMDkey: unrecognised function: %.*S\n",(int)cnt,line.txt+pos);
	  del_macro( key );
	  return;
	}
	m->func[m->len++] = (Key){ 0, func };
	pos += cnt;
      }
      pos = get_string( pos, &cnt, FALSE );
    } while (cnt);
    end_macro( m );
  }
}


// Define a macro.  If the macro already exists as a symbol, delete it; if it's
// already a macro, redefine it.
void execute_defm( DWORD pos )
{
  PDefine   mac;
  PLineList ll;
  DWORD     end, def, cnt;

  if (def_macro)			// can't define a macro within a macro
    return;
  def_macro = TRUE;

  end = skip_nondelim( pos );
  if (end < line.len && !isblank( line.txt[end] ))
  {
    end = skip_nonblank( end );
    printf( "CMDkey: invalid macro name: \"%.*S\".\n",
	    (int)(end - pos), line.txt + pos );
    goto ret;
  }
  cnt = end - pos;

  mac = find_define( &sym_head, pos, cnt );
  if (mac)
    del_define( &sym_head );

  mac = find_define( &mac_head, pos, cnt );
  if (mac)
  {
    free_linelist( mac->line );
    mac->line = NULL;
  }
  else
  {
    mac = add_define( &mac_head, pos, cnt );
    if (!mac)
      goto ret;
  }

  def = skip_blank( end );
  if (def != line.len)			// single-line definition
  {
    mac->line = add_line( def );
    if (!mac->line)
      del_define( &mac_head );
    goto ret;
  }

  for (ll = NULL;;)			// multi-line definition
  {
    if (kbd)
    {
      WriteConsoleW( hConOut, DEFM, DEFM_LEN, &end, NULL );
      show_prompt = FALSE;
    }
    get_next_line();
    pos = skip_blank( 0 );
    end = skip_nonblank( pos );
    if (end - pos == ENDM_LEN && _wcsnicmp( line.txt+pos, ENDM, ENDM_LEN ) == 0)
      break;
    if (!ll)
      ll = mac->line = add_line( 0 );
    else
      ll = ll->next = add_line( 0 );
    if (!ll)
      break;
  }
  if (!ll)
    del_define( &mac_head );

ret:
  def_macro = FALSE;
}


// Define a symbol.  If the name already exists as a macro, delete it; if it's
// already a symbol, redefine it.
void execute_defs( DWORD pos )
{
  PDefine sym;
  DWORD   end, def, cnt;

  if (pos == line.len)
    return;

  end = skip_nondelim( pos );
  if (end < line.len && !isblank( line.txt[end] ))
  {
    end = skip_nonblank( end );
    printf( "CMDkey: invalid symbol name: \"%.*S\".\n",
	    (int)(end - pos), line.txt + pos );
    return;
  }
  cnt = end - pos;

  sym = find_define( &mac_head, pos, cnt );
  if (sym)
    del_define( &mac_head );

  sym = find_define( &sym_head, pos, cnt );
  if (sym && sym->line)
  {
    free( sym->line );
    sym->line = NULL;
  }

  def = skip_blank( end );
  if (def == line.len)			// defining to nothing
  {					//  so wipe it
    if (sym)
      del_define( &sym_head );
    return;
  }

  if (!sym)
  {
    sym = add_define( &sym_head, pos, cnt );
    if (!sym)
      return;
  }
  sym->line = add_line( def );
  if (!sym->line)
    del_define( &sym_head );
}


// Delete one or more associations.  This should be exactly as defined, or
// separated by spaces.  Eg: "dela .c.h" will delete exactly ".c.h", whereas
// "dela .c .h" will delete them even if they're part of a list.
void execute_dela( DWORD pos )
{
  PDefine a;
  DWORD   end, cnt;

  while (pos < line.len)
  {
    end = skip_nonblank( pos );
    cnt = end - pos;
    a	= find_define( &assoc_head, pos, cnt );
    if (a)
      del_define( &assoc_head );
    else
    {
      a = find_assoc( line.txt + pos, cnt );
      if (a)
      {
	if (a->len == cnt)
	  del_define( &assoc_head );
	else
	{
	  if (assoc_pos + cnt < a->len &&
	      (a->name[assoc_pos+cnt] == ';' ||
	       a->name[assoc_pos+cnt] == ':'))
	    ++cnt;
	  memmove( a->name + assoc_pos, a->name + assoc_pos + cnt,
		   WSZ(a->len - assoc_pos - cnt) );
	  a->len -= cnt;
	}
      }
    }
    pos = skip_blank( end );
  }
}


// Delete history lines containing text, including the DELH line itself.
void execute_delh( DWORD pos )
{
  PHistory h, n;
  int end;

  remove_from_history( history.prev );	// the DELH line

  // Use RSTH to delete everything, not empty text.
  if (pos == line.len)
    return;

  for (h = history.next; h != &history; h = n)
  {
    n = h->next;
    for (end = h->len - (line.len - pos); end >= 0; --end)
    {
      if (_wcsnicmp( h->line + end, line.txt + pos, line.len - pos ) == 0)
      {
	remove_from_history( h );
	break;
      }
    }
  }
}


// Delete (ignore) one or more keys.
void execute_delk( DWORD pos )
{
  DWORD end;
  char* key;

  while (pos < line.len)
  {
    end = skip_nonblank( pos );
    key = find_key( pos, end - pos );
    if (key)
    {
      if (*key == Play)
	del_macro( key );
      *key = Ignore;
    }
    pos = skip_blank( end );
  }
}


// Delete one or more macros.
void execute_delm( DWORD pos )
{
  delete_define( &mac_head, pos );
}


// Delete one or more symbols.
void execute_dels( DWORD pos )
{
  delete_define( &sym_head, pos );
}


// List the associations - either all of them or just those specified (as
// individual extensions, not as its list).
void execute_lsta( DWORD pos )
{
  PDefine a;
  DWORD   end, cnt;

  if (!redirect( pos ))
    return;

  if (pos == line.len)
  {
    for (a = assoc_head; a; a = a->next)
      list_define( a, 'a' );
  }
  else
  {
    while (pos < line.len)
    {
      end = skip_nonblank( pos );
      cnt = end - pos;
      a   = find_assoc( line.txt + pos, cnt );
      if (a)
	fprintf( lstout, "defa %-3.*S\t%.*S\n", (int)cnt, line.txt + pos,
		 (int)a->line->len, a->line->line );
      pos = skip_blank( end );
    }
  }

  end_redirect();
}


// List all the lines in the history, or the last N lines, or the first N lines
// if N is negative, or the lines containing the text.	The most recent line is
// always displayed last.  If the text starts with '"', skip over it - this
// allows numbers to be matched.
// Eg: lsth 5	- show the last five commands;
//     lsth "5  - show the commands containing 5;
//     lsth "5" - show the commands containing "5".
void execute_lsth( DWORD pos )
{
  PHistory h;
  DWORD    cnt;
  BOOL	   back, q;
  int	   end;

  if (!redirect( pos ))
    return;

  if (pos == line.len)
  {
    for (h = history.next; h != &history; h = h->next)
      fprintf( lstout, "%.*S\n", (int)h->len, h->line );
  }
  else
  {
    cnt  = 0;
    q	 = (line.txt[pos] == '"' && pos+1 < line.len);
    end  = pos + q;
    back = (line.txt[end] == '-') ? ++end, FALSE : TRUE;
    while (end < line.len && '0' <= line.txt[end] && line.txt[end] <= '9')
    {
      cnt = cnt * 10 + line.txt[end] - '0';
      ++end;
    }
    if (end != line.len)		// there's more after the number
      cnt = 0;				//  so treat it as text
    else if (q) 			// if it started with a quote
      cnt = 0, ++pos;			//  treat the number as text

    if (cnt)
    {
      if (back)
      {
	// List the last cnt commands, excluding this one (assuming it's
	// not a keyboard macro).
	for (end = cnt, h = history.prev; end > 0 && h != &history;)
	  --end, h = h->prev;
	if (h == &history)
	  h = history.next;
      }
      else
	h = history.next;
      for (; h != &history && cnt; --cnt, h = h->next)
	fprintf( lstout, "%.*S\n", (int)h->len, h->line );
    }
    else				// list commands containing text,
    {					//  excluding this command
      for (h = history.next; h != history.prev; h = h->next)
      {
	for (end = h->len - (line.len - pos); end >= 0; --end)
	{
	  if (_wcsnicmp( h->line + end, line.txt + pos, line.len - pos ) == 0)
	  {
	    fprintf( lstout, "%.*S\n", (int)h->len, h->line );
	    break;
	  }
	}
      }
    }
  }

  end_redirect();
}


// List all keys or just those specified.  When listing all keys, list all the
// plain keys, but exclude modified keys that are ignored.
void execute_lstk( DWORD pos )
{
  int	end, cnt;
  char* key;
  static const char * const state[5] = { "  ", " #", " ^", " @", "#^" };

  if (!redirect( pos ))
    return;

  if (pos == line.len)
  {
    // Control keys.
    for (cnt = 0; cnt < 32; ++cnt)
    {
      for (end = 0; end < 2; ++end)
      {
	if (end == 0 || ctrl_key_table[cnt][end] != Ignore)
	{
	  fprintf( lstout, "defk %s%c\t", state[end*2+2], cnt + '@' );
	  list_key( &ctrl_key_table[cnt][end] );
	}
      }
    }
    fputc( '\n', lstout );

    // Editing keys.
    for (cnt = 0; cnt < CFGKEYS; ++cnt)
    {
      for (end = 0; end < 4; ++end)
      {
	if (end == 0 || key_table[cnt][end] != Ignore)
	{
	  fprintf( lstout, "defk %s%s\t",
		   (end == 3 && 8 <= cnt && cnt <= 11) ? state[4] : state[end],
		   key_str[cnt] );
	  list_key( &key_table[cnt][end] );
	}
      }
    }
    fputc( '\n', lstout );

    // Function keys.
    for (cnt = 0; cnt < 12; ++cnt)
    {
      for (end = 0; end < 4; ++end)
      {
	if (end == 0 || fkey_table[cnt][end] != Ignore)
	{
	  fprintf( lstout, "defk %sF%d\t", state[end], cnt + 1 );
	  list_key( &fkey_table[cnt][end] );
	}
      }
    }
  }
  else
  {
    while (pos < line.len)
    {
      end = skip_nonblank( pos );
      cnt = end - pos;
      key = find_key( pos, cnt );
      if (key)
      {
	fprintf( lstout, "defk %-3.*S\t", (int)cnt, line.txt + pos );
	list_key( key );
      }
      pos = skip_blank( end );
    }
  }

  end_redirect();
}


// List macros.
void execute_lstm( DWORD pos )
{
  list_defines( &mac_head, pos );
}


// List symbols.
void execute_lsts( DWORD pos )
{
  list_defines( &sym_head, pos );
}


// Delete every association.
void execute_rsta( DWORD pos )
{
  reset_define( &assoc_head );
}


// Delete the history.
void execute_rsth( DWORD pos )
{
  PHistory h, p;

  for (h = history.prev; h != &history; h = p)
  {
    p = h->prev;
    free( h );
  }
  history.prev = history.next = &history;
  histsize = 0;
}


// Delete every macro.
void execute_rstm( DWORD pos )
{
  reset_define( &mac_head );
}


// Delete every symbol.
void execute_rsts( DWORD pos )
{
  reset_define( &sym_head );
}


// Handle redirection for the list commands.
BOOL redirect( DWORD pos )
{
  DWORD  beg = 0, end;
  PCWSTR mode;
  PCSTR  err = NULL;
  DWORD  append;

  lstout = stdout;
  append = 0;
  for (; pos < line.len; ++pos)
  {
    // No real need to test the quoting of these.
    if (line.txt[pos] == '>' || line.txt[pos] == '|')
    {
      mode = L"w";
      if (line.txt[pos] == '>' && pos+1 < line.len && line.txt[pos+1] == '>')
	append = 1, mode = L"a";
      beg = get_string( pos + 1 + append, &end, FALSE );
      if (end == 0)
      {
	puts( "CMDkey: syntax error." );
	return FALSE;
      }
      line.txt[beg+end] = '\0';
      if (line.txt[pos] == '|')
      {
	lstpipe = TRUE;
	lstout	= _wpopen( line.txt + beg, mode );
	err	= "execute";
      }
      else
      {
	lstpipe = FALSE;
	lstout	= _wfopen( line.txt + beg, mode );
	err	= (append) ? "open" : "create";
      }
      break;
    }
  }
  if (!lstout)
  {
    printf( "CMDkey: unable to %s \"%S\".\n", err, line.txt + beg );
    return FALSE;
  }

  end = skip_blank( beg + end + 1 );
  if (end > line.len)
    end = line.len;
  remove_chars( pos, end - pos );

  if (append)
    lastm = TRUE;
  else if (lstout != stdout || kbd)
    lastm = EOF;

  return TRUE;
}


// Close the file used by redirection.
void end_redirect( void )
{
  if (lstpipe)
    pclose( lstout );
  else if (lstout != stdout)
    fclose( lstout );
}


// Show the assignment of a key.
void list_key( char* key )
{
  PMacro m;
  BOOL	 quote;
  int	 pos, bs;

  if (*key != Play)
  {
    fprintf( lstout, "%s\n", func_str[(int)*key] );
    return;
  }

  m = find_macro( key );
  if (!m)
  {
    fprintf( lstout, "%s\n", func_str[Ignore] );
    return;
  }

  if (m->len <= 0)
  {
    fprintf( lstout, "=%.*S\n", -m->len, m->line );
    return;
  }

  if (m->len == 1)
  {
    if (m->chfn.ch == '"')              // why would you bother?
      fputs( "\"\\\"\"\n", lstout );
    else
      fprintf( lstout, "\"%C\"\n", m->chfn.ch );
    return;
  }

  quote = FALSE;
  for (pos = 0; pos < m->len; ++pos)
  {
    if (m->func[pos].ch == 0)
    {
      if (quote)
      {
	fputs( "\" ", lstout );
	quote = FALSE;
      }
      fprintf( lstout, "%s ", func_str[(int)m->func[pos].fn] );
    }
    else
    {
      if (!quote)
      {
	fputc( '"', lstout );
	quote = TRUE;
      }
      if (m->func[pos].ch == '"')
      {
	for (bs = pos; --bs >= 0 && m->func[bs].ch == '\\';)
	  fputc( '\\', lstout );
	fputc( '\\', lstout );
      }
      fputwc( m->func[pos].ch, lstout );
    }
  }
  if (quote)
  {
    while (--pos >= 0 && m->func[pos].ch == '\\')
      fputc( '\\', lstout );
    fputc( '"', lstout );
  }
  fputc( '\n', lstout );
}


// --------------------------	Keyboard Macros   ----------------------------


// Find the keyboard macro given a keymap location, or NULL if it is not
// defined.  Sets macro_prev to the macro before the one found.
PMacro find_macro( char* key )
{
  PMacro m;

  for (macro_prev = NULL, m = macro_head; m; macro_prev = m, m = m->next)
  {
    if (m->key == key)
      return m;
    if (m->key > key)
      break;
  }

  return NULL;
}


// Add key to the keyboard macro list.	If it is already defined, reset it.
PMacro add_macro( char* key )
{
  PMacro m;

  m = find_macro( key );
  if (!m)
  {
    m = malloc( sizeof(Macro) );
    if (!m)
      return m;
    m->key = key;
    if (macro_prev)
    {
      m->next = macro_prev->next;
      macro_prev->next = m;
    }
    else
    {
      m->next = macro_head;
      macro_head = m;
    }
  }
  else if (line.len < -1 || line.len > 1)
    free( m->line );

  m->len  = 0;
  m->line = NULL;

  return m;
}


// Finish a keyboard macro definition.
void end_macro( PMacro m )
{
  if (m->len == 0)			// nothing was defined
  {					//  so remove it
    if (m->func)
      free( m->func );
    del_macro( m->key );
    return;
  }
  else if (m->len == 1) 		// a single key is stored directly
  {
    Key k = m->func[0];
    free( m->func );
    if (k.ch == 0)
    {
      char* key = m->key;
      del_macro( m->key );
      *key = k.fn;
      return;
    }
    m->chfn = k;
  }
  else					// reduce memory to actual length
    m->func = realloc( m->func, m->len * sizeof(Key) );

  *m->key = Play;			// successfully assigned
}


// Remove the keyboard macro assigned to key.
void del_macro( char* key )
{
  PMacro m;

  m = find_macro( key );
  if (m)
  {
    if (m->len < -1 || m->len > 1)
      free( m->line );
    if (macro_prev)
      macro_prev->next = m->next;
    else
      macro_head = m->next;
    free( m );
    *key = Ignore;
  }
}


// ----------------------------   Definitions	------------------------------


// Add a definition to the list.  The new definition becomes the head.
PDefine add_define( PDefine* head, DWORD pos, DWORD cnt )
{
  PDefine d;

  d = malloc( sizeof(Define) + WSZ(cnt) );
  if (d)
  {
    memcpy( d->name, line.txt + pos, WSZ(cnt) );
    d->len  = cnt;
    d->line = NULL;
    d->next = *head;
    *head   = d;
  }

  return d;
}


// Find the definition in the list.  Returns NULL if not found; otherwise moves
// the definition to the head and returns it.
PDefine find_define( PDefine* head, DWORD pos, DWORD cnt )
{
  PDefine d, p;

  for (p = NULL, d = *head; d; p = d, d = d->next)
  {
    if (d->len == cnt && _wcsnicmp( d->name, line.txt + pos, cnt ) == 0)
    {
      if (p)
      {
	p->next = d->next;
	d->next = *head;
	*head	= d;
      }
      break;
    }
  }

  return d;
}


// Search the list of associations for ext.  Return pointer to definition if
// found and move it to the head; otherwise NULL.
PDefine find_assoc( PCWSTR ext, DWORD cnt )
{
  PDefine a, p;

  for (p = NULL, a = assoc_head; a; p = a, a = a->next)
  {
    if (match_ext( ext, cnt, a->name, a->len ))
    {
      if (p)
      {
	p->next = a->next;
	a->next = assoc_head;
	assoc_head = a;
      }
      break;
    }
  }

  return a;
}


// Remove the head of the list.
void del_define( PDefine* head )
{
  PDefine d;

  d = *head;
  *head = d->next;
  free_linelist( d->line );
  free( d );
}


// Delete a list of definitions.
void delete_define( PDefine* head, DWORD pos )
{
  DWORD   end;
  PDefine d;

  while (pos < line.len)
  {
    end = skip_nonblank( pos );
    d = find_define( head, pos, end - pos );
    if (d)
      del_define( head );
    pos = skip_blank( end );
  }
}


// Delete every definition in the list.
void reset_define( PDefine* head )
{
  PDefine p;

  while (*head)
  {
    p = (*head)->next;
    free_linelist( (*head)->line );
    free( *head );
    *head = p;
  }
}


// List a definition.
void list_define( PDefine d, char t )
{
  PLineList ll;

  // If the last definition was a multi-line macro, or the next one is,
  // add a blank line.
  if (lastm == TRUE || (lastm == FALSE && d->line->next))
    fputc( '\n', lstout );

  fprintf( lstout, "def%c %-3.*S%c", t, (int)d->len, d->name,
				     (d->line->next) ? '\n' : '\t' );
  for (ll = d->line; ll; ll = ll->next)
    fprintf( lstout, "%.*S\n", (int)ll->len, ll->line );

  if (d->line->next)
  {
    fprintf( lstout, "%S\n", ENDM );
    lastm = TRUE;
  }
  else
    lastm = FALSE;
}


// List all macros or symbols, or just those specified.
void list_defines( PDefine* head, DWORD pos )
{
  PDefine d;
  DWORD   end, cnt;
  char	  t;

  if (!redirect( pos ))
    return;

  t = (head == &mac_head) ? 'm' : 's';

  if (pos == line.len)
  {
    for (d = *head; d; d = d->next)
      list_define( d, t );
  }
  else
  {
    while (pos < line.len)
    {
      end = skip_nonblank( pos );
      cnt = end - pos;
      d = find_define( head, pos, cnt );
      if (d)
	list_define( d, t );
      pos = skip_blank( end );
    }
  }

  end_redirect();
}


// Take a copy of the line from pos.
PLineList add_line( DWORD pos )
{
  PLineList ll;

  ll = malloc( sizeof(LineList) + WSZ(line.len - pos) );
  if (ll)
  {
    memcpy( ll->line, line.txt + pos, WSZ(line.len - pos) );
    ll->len  = line.len - pos;
    ll->next = NULL;
  }

  return ll;
}


// Free the memory used by a line list.
void free_linelist( PLineList ll )
{
  PLineList nxt;

  while (ll)
  {
    nxt = ll->next;
    free( ll );
    ll = nxt;
  }
}


// ------------------------------   Utility   --------------------------------


// Perform a binary search of cfg[] for name.  end is the index of the last
// item, NOT the number of items.  Return -1 if not found, otherwise the value
// appropriate to name.
int search_cfg( PCWSTR name, DWORD cnt, const Cfg cfg[], int end )
{
  int rc;
  int beg = 0, mid;

  while (end >= beg)
  {
    mid = (beg + end) / 2;
    rc	= _wcsnicmp( cfg[mid].name, name, cnt );
    if (rc == 0 && cfg[mid].name[cnt] == '\0')
      return cfg[mid].func;
    if (rc >= 0)
      end = mid - 1;
    else
      beg = mid + 1;
  }
  return -1;
}


// Return the keymap position of a key, or NULL if it is not recognised.
char* find_key( DWORD pos, DWORD cnt )
{
  PWSTR name = line.txt + pos;
  char* key  = NULL;
  int	state;
  int	ch;

  if (cnt < 2)
    return NULL;

  if ((cnt == 2 && *name == '^') ||
      (cnt == 3 && *name == '#' && name[1] == '^'))
  {
    state = (*name == '^') ? 0 : 1;
    ch = (name[state+1] | 0x20) - 0x60;
    if ((unsigned)ch < 32)
      key = &ctrl_key_table[ch][state];
  }
  else
  {
    state = 0;
    switch (*name)
    {
      case '@': ++state;        // alt
      case '^': ++state;        // ctrl
      case '#': ++state;        // shift
		++name;
		--cnt;
    }
    if (*name == 'f' || *name == 'F')
    {
      if (cnt == 2 && name[1] >= '1' && name[1] <= '9')
	key = &fkey_table[name[1] - '1'][state];
      else if (cnt == 3 && name[1] == '1' && name[2] >= '0' && name[2] <= '2')
	key = &fkey_table[9 + name[2] - '0'][state];
    }
    else  // no key happens to start with 'F'
    {
      if (*name == '^' && state == 1)
      {
	state = 3;		// shift+ctrl
	++name;
	--cnt;
      }
      ch = search_cfg( name, cnt, cfgkey, CFGKEYS - 1 );
      if (ch != -1)
	key = &key_table[ch - VK_PRIOR][state];
    }
  }

  return key;
}


// Make a copy of txt, returning a pointer to the copy.
PWSTR new_txt( PCWSTR txt, DWORD len )
{
  PWSTR t;

  t = malloc( WSZ(len) );
  if (t)
    memcpy( t, txt, WSZ(len) );

  return t;
}


// Make a sound if it's not in silent mode.
void bell( void )
{
  if (!option.silent)
    MessageBeep( ~0 );			// standard beep
}


// Return the position of the first non-blank from pos.
DWORD skip_blank( DWORD pos )
{
  while (pos < line.len && isblank( line.txt[pos] ))
    ++pos;

  return pos;
}


// Return the position of the first blank from pos.
DWORD skip_nonblank( DWORD pos )
{
  while (pos < line.len && !isblank( line.txt[pos] ))
    ++pos;

  return pos;
}


// Return the position of the first delimiter from pos.
DWORD skip_nondelim( DWORD pos )
{
  while (pos < line.len && !wcschr( DEF_TERM, line.txt[pos] ))
    ++pos;

  return pos;
}


// Determine if a character is the beginning or end of a quoted string.
// An odd number of backslashes before the quote will treat it literally.
BOOL is_quote( int pos )
{
  BOOL lit;

  lit = FALSE;
  if (pos < line.len && line.txt[pos] == '"')
  {
    lit = TRUE;
    while (--pos >= 0 && line.txt[pos] == '\\')
      lit ^= TRUE;
  }

  return lit;
}


// Get the string starting from pos.  Returns the start position of it and sets
// cnt to its length.  If keep is true quotes are preserved; otherwise embedded
// quotes are made surrounding and the quotes are excluded from the string.
DWORD get_string( DWORD pos, LPDWORD cnt, BOOL keep )
{
  DWORD start;
  BOOL	quote, oq;		// has the open quote been added?
  DWORD cq = ~0;		// position of the close quote

  found_quote = quote = oq = FALSE;
  for (start = pos = skip_blank( pos ); pos < line.len; ++pos)
  {
    if (quote)
    {
      if (is_quote( pos ))
      {
	quote = FALSE;
	if (!keep)
	{
	  if (cq != ~0)
	  {
	    remove_chars( cq, 1 );
	    --pos;
	  }
	  cq = pos;
	}
      }
    }
    else if (is_quote( pos ))
    {
      found_quote = quote = TRUE;
      if (!keep)
      {
	if (oq)
	  remove_chars( pos--, 1 );
	else
	{
	  oq = TRUE;
	  if (pos != start)
	  {
	    memmove( line.txt + start + 1, line.txt + start, WSZ(pos - start) );
	    line.txt[start] = '"';
	    set_display_marks( start, pos + 1 );
	  }
	}
      }
    }
    else if (isblank( line.txt[pos] ))
      break;
  }
  if (!keep)
  {
    if (oq)
      ++start;
    if (cq != ~0)
    {
      if (cq != --pos)
      {
	memcpy( line.txt + cq, line.txt + cq + 1, WSZ(pos - cq) );
	line.txt[pos] = '"';
	set_display_marks( cq, pos + 1 );
      }
    }
  }

  *cnt = pos - start;
  return start;
}


// If unq is NULL, remove all non-quoted escape characters; otherwise only
// remove the escaped characters present in unq, inside quotes (let CMD.EXE
// handle those outside quotes).
void un_escape( PCWSTR unq )
{
  DWORD pos;
  BOOL	quote;

  if (!memchr( line.txt, ESCAPE, WSZ( line.len) ))
    return;

  quote = FALSE;
  for (pos = 0; pos < line.len; ++pos)
  {
    if (quote)
    {
      if (is_quote( pos ))
	quote = FALSE;
      else if (unq && line.txt[pos] == ESCAPE && pos+1 < line.len &&
	       wcschr( unq, line.txt[pos+1] ))
	remove_chars( pos, 1 );
    }
    else if (is_quote( pos ))
      quote = TRUE;
    else if (!unq && line.txt[pos] == ESCAPE && pos+1 < line.len)
      remove_chars( pos, 1 );
  }
}


// Determine if ext, of cnt characters, is a substring of extlist, which is a
// list of extensions, including dot, possibly separated by (semi)colon (eg:
// ".exe.com", ".exe;.com" or ".exe:.com").
BOOL match_ext( PCWSTR ext, DWORD cnt, PCWSTR extlist, DWORD extlen )
{
  DWORD pos, end;

  for (pos = 0; pos < extlen; pos = end)
  {
    for (end = pos; ++end < extlen && extlist[end] != '.' &&
				      extlist[end] != ';' &&
				      extlist[end] != ':';) ;
    if (end - pos == cnt && _wcsnicmp( extlist + pos, ext, cnt ) == 0)
    {
      assoc_pos = pos;
      return TRUE;
    }
    if (end == extlen)
      break;
    if (extlist[end] != '.')
      ++end;
  }

  return FALSE;
}


// Get an environment variable; if it doesn't exist use def, if it exists.
// The variable is stored in the global envvar buffer; its length is returned.
DWORD get_env_var( PCWSTR var, PCWSTR def )
{
  DWORD varlen;
  PWSTR v;
  BOOL	exist;

  varlen = GetEnvironmentVariableW( var, envvar.txt, envvar.len );
  if (varlen == 0 && def && *def)
  {
    exist  = FALSE;
    varlen = wcslen( def );
    if (varlen <= envvar.len)
      memcpy( envvar.txt, def, WSZ(varlen) );
  }
  else
    exist = TRUE;

  if (varlen && varlen > envvar.len)
  {
    v = realloc( envvar.txt, WSZ(varlen) );
    if (!v)
      return 0;
    envvar.txt = v;
    envvar.len = varlen;
    if (exist)
      varlen = GetEnvironmentVariableW( var, envvar.txt, envvar.len );
    else
      memcpy( envvar.txt, def, WSZ(varlen) );
  }

  return varlen;
}


//-----------------------------------------------------------------------------
//   MyReadConsoleW
// It is the new function that must replace the original ReadConsoleW function.
// This function have exactly the same signature as the original one.
//-----------------------------------------------------------------------------

BOOL
WINAPI MyReadConsoleW( HANDLE hConsoleInput, LPVOID lpBuffer,
		       DWORD nNumberOfCharsToRead,
		       LPDWORD lpNumberOfCharsRead, LPVOID lpReserved )
{
  COORD c;
  int	j;

  if (option.disable_cmdkey)
  {
    enabled ^= TRUE;			// only disable this instance
    option.disable_cmdkey = 0;
  }

  hConIn  = hConsoleInput;
  hConOut = GetStdHandle( STD_OUTPUT_HANDLE );
  if (enabled && nNumberOfCharsToRead > 1 &&
      GetConsoleScreenBufferInfo( hConOut, &screen ))
  {
    trap_break = TRUE;
    if (check_break)
    {
      while (macro_stk)
	pop_macro();
      if (mcmd.txt)
      {
	if (mcmd.len)
	  free( mcmd.txt );
	mcmd.txt = NULL;
      }
    }
    check_break = 1;

    if (macro_stk || mcmd.txt || *cmdname)
    {
      // Remove the prompt.
      c.X = 0;
      c.Y = screen.dwCursorPosition.Y - prompt.len / screen.dwSize.X;
      FillConsoleOutputCharacterW( hConOut, ' ', prompt.len, c,
				   lpNumberOfCharsRead );
      // Reclaim the blank line.
      --c.Y;
      SetConsoleCursorPosition( hConOut, c );
    }
    else
    {
      if (!option.nocolour &&
	  prompt.txt[1] == ':' && prompt.txt[prompt.len-1] == '>')
      {
	// Assume $P$G and colour it appropriately.
	p_attr[0] = p_attr[1] = option.drv_col;
	p_attr[2] = (prompt.txt[3] == '>') ? option.dir_col : option.sep_col;
	j = prompt.len - 1;
	p_attr[j] = option.gt_col;
	while (--j > 2)
	  p_attr[j] = (prompt.txt[j] == '\\') ? option.sep_col : option.dir_col;
	c.X = 0;
	c.Y = screen.dwCursorPosition.Y - prompt.len / screen.dwSize.X;
	WriteConsoleOutputAttribute( hConOut, p_attr, prompt.len, c,
				     lpNumberOfCharsRead );
      }
      else
	*p_attr = 0;
    }

    if (*cmdname)
    {
      read_cmdfile( cmdname );
      *cmdname = 0;
    }

    line.txt = lpBuffer;
    max = nNumberOfCharsToRead - 2;	// leave room for CRLF
    show_prompt = FALSE;
    if (option.disable_macro)
      get_next_line();
    else
    {
      do
      {
	get_next_line();
	multi_cmd();
	do
	{
	  if (check_break > 1)
	    break;
	  if (line.len && line.txt[0] == '@')
	  {
	    remove_chars( 0, 1 );
	    dosify();
	  }
	  if (line.len && line.txt[0] == option.ignore_char)
	  {
	    remove_chars( 0, 1 );
	    break;
	  }
	  expand_braces();
	} while (associate() || expand_symbol() || expand_macro());
      } while (internal_cmd());
      expand_vars( FALSE );
    }

    line.txt[line.len++] = '\r';
    line.txt[line.len++] = '\n';
    *lpNumberOfCharsRead = line.len;

    trap_break	= FALSE;
    check_break = 0;
    return TRUE;
  }

  return ReadConsoleW( hConsoleInput, lpBuffer, nNumberOfCharsToRead,
		       lpNumberOfCharsRead, lpReserved );
}


// Indicate Control+Break was received.
BOOL ctrl_break( DWORD type )
{
  if (type == CTRL_BREAK_EVENT)
  {
    ++check_break;
    return trap_break;
  }
  return FALSE;
}


// Assume the output prior to input is the prompt.
BOOL
WINAPI MyWriteConsoleW( HANDLE hConsoleOutput, CONST VOID* lpBuffer,
			DWORD nNumberOfCharsToWrite,
			LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved )
{
  prompt.txt = (PWSTR)lpBuffer;
  prompt.len = nNumberOfCharsToWrite;
  return WriteConsoleW( hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
			lpNumberOfCharsWritten, lpReserved );
}


// ========== Hooking API functions
//
// References about API hooking (and dll injection):
// - Matt Pietrek ~ Windows 95 System Programming Secrets.
// - Jeffrey Richter ~ Programming Applications for Microsoft Windows 4th ed.

typedef struct
{
  PSTR name;
  PROC newfunc;
  PROC oldfunc;
} HookFn, *PHookFn;


//-----------------------------------------------------------------------------
//   HookAPIOneMod
// Substitute a new function in the Import Address Table (IAT) of the
// specified module.
// Return FALSE on error and TRUE on success.
//-----------------------------------------------------------------------------

BOOL HookAPIOneMod(
    HMODULE hFromModule,	// Handle of the module to intercept calls from
    PHookFn Hooks		// Functions to replace
    )
{
  PIMAGE_DOS_HEADER	   pDosHeader;
  PIMAGE_NT_HEADERS	   pNTHeader;
  PIMAGE_IMPORT_DESCRIPTOR pImportDesc;
  PIMAGE_THUNK_DATA	   pThunk;
  PHookFn		   hook;
  HMODULE		   kernel;

  // Tests to make sure we're looking at a module image (the 'MZ' header)
  pDosHeader = (PIMAGE_DOS_HEADER)hFromModule;
  if (IsBadReadPtr( pDosHeader, sizeof(IMAGE_DOS_HEADER) ))
  {
    DEBUGSTR( "error: %s(%d)", __FILE__, __LINE__ );
    return FALSE;
  }
  if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
  {
    DEBUGSTR( "error: %s(%d)", __FILE__, __LINE__ );
    return FALSE;
  }

  // The MZ header has a pointer to the PE header
  pNTHeader = MakePtr( PIMAGE_NT_HEADERS, pDosHeader, pDosHeader->e_lfanew );

  // More tests to make sure we're looking at a "PE" image
  if (IsBadReadPtr( pNTHeader, sizeof(IMAGE_NT_HEADERS) ))
  {
    DEBUGSTR( "error: %s(%d)", __FILE__, __LINE__ );
    return FALSE;
  }
  if (pNTHeader->Signature != IMAGE_NT_SIGNATURE)
  {
    DEBUGSTR( "error: %s(%d)", __FILE__, __LINE__ );
    return FALSE;
  }

  // We now have a valid pointer to the module's PE header.
  // Get a pointer to its imports section
  pImportDesc = MakePtr( PIMAGE_IMPORT_DESCRIPTOR,
			 pDosHeader,
			 pNTHeader->OptionalHeader.
			  DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].
			   VirtualAddress );

  // Bail out if the RVA of the imports section is 0 (it doesn't exist)
  if (pImportDesc == (PIMAGE_IMPORT_DESCRIPTOR)pNTHeader)
  {
    return TRUE;
  }

  // Iterate through the array of imported module descriptors, looking
  // for the module whose name matches the pszFunctionModule parameter
  while (pImportDesc->Name)
  {
    PSTR pszModName = MakePtr( PSTR, pDosHeader, pImportDesc->Name );
    if (stricmp( pszModName, "kernel32.dll" ) == 0)
      break;
    pImportDesc++; // Advance to next imported module descriptor
  }

  // Bail out if we didn't find the import module descriptor for the
  // specified module.  pImportDesc->Name will be non-zero if we found it.
  if (pImportDesc->Name == 0)
    return TRUE;

  // Get a pointer to the found module's import address table (IAT)
  pThunk = MakePtr( PIMAGE_THUNK_DATA, pDosHeader, pImportDesc->FirstThunk );

  // Get the entry points to the original functions.
  kernel = GetModuleHandle( "kernel32.dll" );
  for (hook = Hooks; hook->name; ++hook)
    hook->oldfunc = GetProcAddress( kernel, hook->name );

  // Blast through the table of import addresses, looking for the ones
  // that match the address we got back from GetProcAddress above.
  while (pThunk->u1.Function)
  {
    for (hook = Hooks; hook->name; ++hook)
      // double cast avoid warning with VC6 and VC7 :-)
      if ((DWORD)pThunk->u1.Function == (DWORD)hook->oldfunc) // We found it!
      {
	DWORD flOldProtect, flNewProtect, flDummy;
	MEMORY_BASIC_INFORMATION mbi;

	// Get the current protection attributes
	VirtualQuery( &pThunk->u1.Function, &mbi, sizeof(mbi) );
	// Take the access protection flags
	flNewProtect = mbi.Protect;
	// Remove ReadOnly and ExecuteRead flags
	flNewProtect &= ~(PAGE_READONLY | PAGE_EXECUTE_READ);
	// Add on ReadWrite flag
	flNewProtect |= (PAGE_READWRITE);
	// Change the access protection on the region of committed pages in the
	// virtual address space of the current process
	VirtualProtect( &pThunk->u1.Function, sizeof(PVOID),
			flNewProtect, &flOldProtect );

	// Overwrite the original address with the address of the new function
	if (!WriteProcessMemory( GetCurrentProcess(),
				 &pThunk->u1.Function,
				 &hook->newfunc,
				 sizeof(hook->newfunc), NULL ))
	{
	  DEBUGSTR( "error: %s(%d)", __FILE__, __LINE__ );
	  return FALSE;
	}

	// Put the page attributes back the way they were.
	VirtualProtect( &pThunk->u1.Function, sizeof(PVOID),
			flOldProtect, &flDummy );
      }
    pThunk++;	// Advance to next imported function address
  }
  return TRUE;	// Function not found
}


// ========== Initialisation

HookFn Hooks[] = {
  { "ReadConsoleW",  (PROC)MyReadConsoleW,  NULL },
  { "WriteConsoleW", (PROC)MyWriteConsoleW, NULL },
  { NULL, NULL, NULL }
};


BOOL ReadOptions( HKEY root )
{
  HKEY	key;
  DWORD exist;

  if (RegOpenKeyEx( root, REGKEY, 0, KEY_QUERY_VALUE, &key ) == ERROR_SUCCESS)
  {
    exist = sizeof(option);
    RegQueryValueEx( key, "Options", NULL, NULL, (LPBYTE)&option, &exist );
    exist = sizeof(cfgname);
    RegQueryValueEx( key, "Cmdfile", NULL, NULL, (LPBYTE)cfgname, &exist );
    RegCloseKey( key );
    return TRUE;
  }
  return FALSE;
}


//-----------------------------------------------------------------------------
//   DllMain()
// Function called by the system when processes and threads are initialized
// and terminated.
//-----------------------------------------------------------------------------

BOOL WINAPI DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved )
{
  BOOL bResult;

  switch (dwReason)
  {
    case DLL_PROCESS_ATTACH:
      // Don't bother hooking into CMDkey.
      if (lpReserved)			// static initialisation
      {
	is_enabled = enabled;		// let status know the state
	break;
      }
      bResult = HookAPIOneMod( GetModuleHandle( NULL ), Hooks );
      if (bResult && !installed)
      {
	if (!ReadOptions( HKEY_CURRENT_USER ))
	  ReadOptions( HKEY_LOCAL_MACHINE );
	installed = bResult;
      }
      if (installed)
      {
	if (*cfgname)
	  read_cmdfile( cfgname );
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE)ctrl_break, TRUE );
      }
    break;

    case DLL_PROCESS_DETACH:
    break;
  }

  return TRUE;
}
#ifndef VIS_CORE_H
#define VIS_CORE_H

#include <setjmp.h>
#include "vis.h"
#include "text.h"
#include "text-regex.h"
#include "map.h"
#include "ring-buffer.h"

/* a mode contains a set of key bindings which are currently valid.
 *
 * each mode can specify one parent mode which is consultated if a given key
 * is not found in the current mode. hence the modes form a tree which is
 * searched from the current mode up towards the root mode until a valid binding
 * is found.
 *
 * if no binding is found, mode->input(...) is called and the user entered
 * keys are passed as argument. this is used to change the document content.
 */
typedef struct Mode Mode;
struct Mode {
	Mode *parent;                       /* if no match is found in this mode, search will continue there */
	Map *bindings;
	const char *name;                   /* descriptive, user facing name of the mode */
	const char *status;                 /* name displayed in the window status bar */
	const char *help;                   /* short description used by :help */
	bool isuser;                        /* whether this is a user or internal mode */
	void (*enter)(Vis*, Mode *old);           /* called right before the mode becomes active */
	void (*leave)(Vis*, Mode *new);           /* called right before the mode becomes inactive */
	void (*input)(Vis*, const char*, size_t); /* called whenever a key is not found in this mode and all its parent modes */
	void (*idle)(Vis*);                 /* called whenever a certain idle time i.e. without any user input elapsed */
	time_t idle_timeout;                /* idle time in seconds after which the registered function will be called */
	bool visual;                        /* whether text selection is possible in this mode */
};

typedef struct {
	int count;        /* how many times should the command be executed? */
	Register *reg;    /* always non-NULL, set to a default register */
	Filerange range;  /* which part of the file should be affected by the operator */
	size_t pos;       /* at which byte from the start of the file should the operation start? */
	size_t newpos;    /* new position after motion or EPOS if none given */
	bool linewise;    /* should the changes always affect whole lines? */
	const Arg *arg;   /* arbitrary arguments */
} OperatorContext;

typedef struct {
	/* operator logic, returns new cursor position, if EPOS is
	 * the cursor is disposed (except if it is the primary one) */
	size_t (*func)(Vis*, Text*, OperatorContext*);
} Operator;

typedef struct { /* Motion implementation, takes a cursor postion and returns a new one */
	/* TODO: merge types / use union to save space */
	size_t (*cur)(Cursor*);
	size_t (*txt)(Text*, size_t pos);
	size_t (*file)(Vis*, File*, size_t pos);
	size_t (*vis)(Vis*, Text*, size_t pos);
	size_t (*view)(Vis*, View*);
	size_t (*win)(Vis*, Win*, size_t pos);
	enum {
		LINEWISE  = 1 << 0,  /* should the covered range be extended to whole lines? */
		CHARWISE  = 1 << 1,  /* scrolls window content until position is visible */
		INCLUSIVE = 1 << 2,  /* should new position be included in operator range? */
		IDEMPOTENT = 1 << 3, /* does the returned postion remain the same if called multiple times? */
		JUMP = 1 << 4,
	} type;
} Movement;

typedef struct {
	/* gets a cursor position and returns a file range (or text_range_empty())
	 * representing the text object containing the position. */
	Filerange (*range)(Text*, size_t pos);
	enum {  /* whether the object should include the delimiting symbols or not */
		INNER,
		OUTER,
	} type;
} TextObject;

/* a macro is just a sequence of symbolic keys as received from ui->getkey */
typedef Buffer Macro;
#define macro_release buffer_release
#define macro_reset buffer_truncate
#define macro_append buffer_append0

typedef struct {             /** collects all information until an operator is executed */
	int count;
	enum VisMotionType type;
	const Operator *op;
	const Movement *movement;
	const TextObject *textobj;
	const Macro *macro;
	Register *reg;
	enum VisMark mark;
	Arg arg;
} Action;

struct File { /* shared state among windows displaying the same file */
	Text *text;                      /* data structure holding the file content */
	const char *name;                /* file name used when loading/saving */
	volatile sig_atomic_t truncated; /* whether the underlying memory mapped region became invalid (SIGBUS) */
	bool is_stdin;                   /* whether file content was read from stdin */
	struct stat stat;                /* filesystem information when loaded/saved, used to detect changes outside the editor */
	int refcount;                    /* how many windows are displaying this file? (always >= 1) */
	Mark marks[VIS_MARK_INVALID];    /* marks which are shared across windows */
	File *next, *prev;
};

typedef struct {
	time_t state;           /* state of the text, used to invalidate change list */
	size_t index;           /* #number of changes */
	size_t pos;             /* where the current change occured */
} ChangeList;

struct Win {
	Vis *vis;               /* editor instance to which this window belongs to */
	UiWin *ui;              /* ui object handling visual appearance of this window */
	File *file;             /* file being displayed in this window */
	View *view;             /* currently displayed part of underlying text */
	ViewEvent events;
	RingBuffer *jumplist;   /* LRU jump management */
	ChangeList changelist;  /* state for iterating through least recently changes */
	Win *prev, *next;       /* neighbouring windows */
};

struct Vis {
	Ui *ui;                              /* user interface repsonsible for visual appearance */
	File *files;                         /* all files currently managed by this editor instance */
	Win *windows;                        /* all windows currently managed by this editor instance */
	Win *win;                            /* currently active/focused window */
	Register registers[VIS_REG_INVALID]; /* registers used for yank and put */
	Macro macros[VIS_MACRO_INVALID];     /* recorded macros */
	Macro *recording, *last_recording;   /* currently (if non NULL) and least recently recorded macro */
	Macro *macro_operator;               /* special macro used to repeat certain operators */
	Win *prompt;                         /* 1-line height window to get user input */
	Win *prompt_window;                  /* window which was focused before prompt was shown */
	char prompt_type;                    /* command ':' or search '/','?' prompt */
	Mode *mode_before_prompt;            /* user mode which was active before entering prompt */
	Regex *search_pattern;               /* last used search pattern */
	char search_char[8];                 /* last used character to search for via 'f', 'F', 't', 'T' */
	int last_totill;                     /* last to/till movement used for ';' and ',' */
	int tabwidth;                        /* how many spaces should be used to display a tab */
	bool expandtab;                      /* whether typed tabs should be converted to spaces */
	bool autoindent;                     /* whether indentation should be copied from previous line on newline */
	Map *cmds;                           /* ":"-commands, used for unique prefix queries */
	Map *options;                        /* ":set"-options */
	Buffer input_queue;                  /* holds pending input keys */
	Buffer *keys;                        /* currently active keys buffer (either the input_queue or a macro) */
	Action action;                       /* current action which is in progress */
	Action action_prev;                  /* last operator action used by the repeat (dot) command */
	Mode *mode;                          /* currently active mode, used to search for keybindings */
	Mode *mode_prev;                     /* previsouly active user mode */
	volatile bool running;               /* exit main loop once this becomes false */
	int exit_status;                     /* exit status when terminating main loop */
	volatile sig_atomic_t cancel_filter; /* abort external command/filter (SIGINT occured) */
	volatile sig_atomic_t sigbus;        /* one of the memory mapped region became unavailable (SIGBUS) */
	sigjmp_buf sigbus_jmpbuf;            /* used to jump back to a known good state in the mainloop after (SIGBUS) */
	Map *actions;                        /* registered editor actions / special keys commands */
	lua_State *lua;                      /* lua context used for syntax highligthing */
};

/** stuff used by multiple of the vis-* files */

/* TODO: make part of Vis struct? enable dynamic modes? */
extern Mode vis_modes[VIS_MODE_LAST];

extern Movement moves[VIS_MOVE_INVALID];

extern Operator ops[VIS_OP_INVALID];

const char *expandtab(Vis *vis);

void macro_operator_stop(Vis *vis);
void macro_operator_record(Vis *vis);

void action_reset(Action*);

void mode_set(Vis *vis, Mode *new_mode);
Mode *mode_get(Vis *vis, enum VisMode mode);

#endif

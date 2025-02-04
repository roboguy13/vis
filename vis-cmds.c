#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vis-core.h"
#include "text-util.h"
#include "text-motions.h"
#include "util.h"

enum CmdOpt {          /* option flags for command definitions */
	CMD_OPT_NONE,  /* no option (default value) */
	CMD_OPT_FORCE, /* whether the command can be forced by appending '!' */
	CMD_OPT_ARGS,  /* whether the command line should be parsed in to space
	                * separated arguments to placed into argv, otherwise argv[1]
	                * will contain the  remaining command line unmodified */
};

typedef struct {             /* command definitions for the ':'-prompt */
	const char *name[3]; /* name and optional alias for the command */
	/* command logic called with a NULL terminated array of arguments.
	 * argv[0] will be the command name */
	bool (*cmd)(Vis*, Filerange*, enum CmdOpt opt, const char *argv[]);
	enum CmdOpt opt;     /* command option flags */
} Command;

/** ':'-command implementations */
/* set various runtime options */
static bool cmd_set(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* for each argument create a new window and open the corresponding file */
static bool cmd_open(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* close current window (discard modifications if forced ) and open argv[1],
 * if no argv[1] is given re-read to current file from disk */
static bool cmd_edit(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* close the current window, discard modifications if forced */
static bool cmd_quit(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* close all windows which show current file, discard modifications if forced  */
static bool cmd_bdelete(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* close all windows, exit editor, discard modifications if forced */
static bool cmd_qall(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* for each argument try to insert the file content at current cursor postion */
static bool cmd_read(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
static bool cmd_substitute(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* if no argument are given, split the current window horizontally,
 * otherwise open the file */
static bool cmd_split(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* if no argument are given, split the current window vertically,
 * otherwise open the file */
static bool cmd_vsplit(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* create a new empty window and arrange all windows either horizontally or vertically */
static bool cmd_new(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
static bool cmd_vnew(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* save the file displayed in the current window and close it */
static bool cmd_wq(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* save the file displayed in the current window if it was changvis, then close the window */
static bool cmd_xit(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* save the file displayed in the current window to the name given.
 * do not change internal filname association. further :w commands
 * without arguments will still write to the old filename */
static bool cmd_write(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* save the file displayed in the current window to the name given,
 * associate the new name with the buffer. further :w commands
 * without arguments will write to the new filename */
static bool cmd_saveas(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* filter range through external program argv[1] */
static bool cmd_filter(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* switch to the previous/next saved state of the text, chronologically */
static bool cmd_earlier_later(Vis*, Filerange*, enum CmdOpt, const char *argv[]);
/* dump current key bindings */
static bool cmd_help(Vis*, Filerange*, enum CmdOpt, const char *argv[]);

/* command recognized at the ':'-prompt. commands are found using a unique
 * prefix match. that is if a command should be available under an abbreviation
 * which is a prefix for another command it has to be added as an alias. the
 * long human readable name should always come first */
static Command cmds[] = {
	/* command name / optional alias, function,       options */
	{ { "bdelete"                  }, cmd_bdelete,    CMD_OPT_FORCE },
	{ { "edit"                     }, cmd_edit,       CMD_OPT_FORCE },
	{ { "help"                     }, cmd_help,       CMD_OPT_NONE  },
	{ { "new"                      }, cmd_new,        CMD_OPT_NONE  },
	{ { "open"                     }, cmd_open,       CMD_OPT_NONE  },
	{ { "qall"                     }, cmd_qall,       CMD_OPT_FORCE },
	{ { "quit", "q"                }, cmd_quit,       CMD_OPT_FORCE },
	{ { "read",                    }, cmd_read,       CMD_OPT_FORCE },
	{ { "saveas"                   }, cmd_saveas,     CMD_OPT_FORCE },
	{ { "set",                     }, cmd_set,        CMD_OPT_ARGS  },
	{ { "split"                    }, cmd_split,      CMD_OPT_NONE  },
	{ { "substitute", "s"          }, cmd_substitute, CMD_OPT_NONE  },
	{ { "vnew"                     }, cmd_vnew,       CMD_OPT_NONE  },
	{ { "vsplit",                  }, cmd_vsplit,     CMD_OPT_NONE  },
	{ { "wq",                      }, cmd_wq,         CMD_OPT_FORCE },
	{ { "write", "w"               }, cmd_write,      CMD_OPT_FORCE },
	{ { "xit",                     }, cmd_xit,        CMD_OPT_FORCE },
	{ { "earlier"                  }, cmd_earlier_later, CMD_OPT_NONE },
	{ { "later"                    }, cmd_earlier_later, CMD_OPT_NONE },
	{ { "!",                       }, cmd_filter,     CMD_OPT_NONE  },
	{ /* array terminator */                                        },
};


static void windows_arrange(Vis *vis, enum UiLayout layout) {
	vis->ui->arrange(vis->ui, layout);
}

static void tabwidth_set(Vis *vis, int tabwidth) {
	if (tabwidth < 1 || tabwidth > 8)
		return;
	for (Win *win = vis->windows; win; win = win->next)
		view_tabwidth_set(win->view, tabwidth);
	vis->tabwidth = tabwidth;
}

/* parse human-readable boolean value in s. If successful, store the result in
 * outval and return true. Else return false and leave outval alone. */
static bool parse_bool(const char *s, bool *outval) {
	for (const char **t = (const char*[]){"1", "true", "yes", "on", NULL}; *t; t++) {
		if (!strcasecmp(s, *t)) {
			*outval = true;
			return true;
		}
	}
	for (const char **f = (const char*[]){"0", "false", "no", "off", NULL}; *f; f++) {
		if (!strcasecmp(s, *f)) {
			*outval = false;
			return true;
		}
	}
	return false;
}

static bool cmd_set(Vis *vis, Filerange *range, enum CmdOpt cmdopt, const char *argv[]) {

	typedef struct {
		const char *names[3];
		enum {
			OPTION_TYPE_STRING,
			OPTION_TYPE_BOOL,
			OPTION_TYPE_NUMBER,
		} type;
		bool optional;
		int index;
	} OptionDef;

	enum {
		OPTION_AUTOINDENT,
		OPTION_EXPANDTAB,
		OPTION_TABWIDTH,
		OPTION_SYNTAX,
		OPTION_SHOW,
		OPTION_NUMBER,
		OPTION_NUMBER_RELATIVE,
		OPTION_CURSOR_LINE,
		OPTION_THEME,
		OPTION_COLOR_COLUMN,
	};

	/* definitions have to be in the same order as the enum above */
	static OptionDef options[] = {
		[OPTION_AUTOINDENT]      = { { "autoindent", "ai"       }, OPTION_TYPE_BOOL   },
		[OPTION_EXPANDTAB]       = { { "expandtab", "et"        }, OPTION_TYPE_BOOL   },
		[OPTION_TABWIDTH]        = { { "tabwidth", "tw"         }, OPTION_TYPE_NUMBER },
		[OPTION_SYNTAX]          = { { "syntax"                 }, OPTION_TYPE_STRING, true },
		[OPTION_SHOW]            = { { "show"                   }, OPTION_TYPE_STRING },
		[OPTION_NUMBER]          = { { "numbers", "nu"          }, OPTION_TYPE_BOOL   },
		[OPTION_NUMBER_RELATIVE] = { { "relativenumbers", "rnu" }, OPTION_TYPE_BOOL   },
		[OPTION_CURSOR_LINE]     = { { "cursorline", "cul"      }, OPTION_TYPE_BOOL   },
		[OPTION_THEME]           = { { "theme"                  }, OPTION_TYPE_STRING },
		[OPTION_COLOR_COLUMN]    = { { "colorcolumn", "cc"      }, OPTION_TYPE_NUMBER },
	};

	if (!vis->options) {
		if (!(vis->options = map_new()))
			return false;
		for (int i = 0; i < LENGTH(options); i++) {
			options[i].index = i;
			for (const char **name = options[i].names; *name; name++) {
				if (!map_put(vis->options, *name, &options[i]))
					return false;
			}
		}
	}

	if (!argv[1]) {
		vis_info_show(vis, "Expecting: set option [value]");
		return false;
	}

	Arg arg;
	bool invert = false;
	OptionDef *opt = NULL;

	if (!strncasecmp(argv[1], "no", 2)) {
		opt = map_closest(vis->options, argv[1]+2);
		if (opt && opt->type == OPTION_TYPE_BOOL)
			invert = true;
		else
			opt = NULL;
	}

	if (!opt)
		opt = map_closest(vis->options, argv[1]);
	if (!opt) {
		vis_info_show(vis, "Unknown option: `%s'", argv[1]);
		return false;
	}

	switch (opt->type) {
	case OPTION_TYPE_STRING:
		if (!opt->optional && !argv[2]) {
			vis_info_show(vis, "Expecting string option value");
			return false;
		}
		arg.s = argv[2];
		break;
	case OPTION_TYPE_BOOL:
		if (!argv[2]) {
			arg.b = true;
		} else if (!parse_bool(argv[2], &arg.b)) {
			vis_info_show(vis, "Expecting boolean option value not: `%s'", argv[2]);
			return false;
		}
		if (invert)
			arg.b = !arg.b;
		break;
	case OPTION_TYPE_NUMBER:
		if (!argv[2]) {
			vis_info_show(vis, "Expecting number");
			return false;
		}
		/* TODO: error checking? long type */
		arg.i = strtoul(argv[2], NULL, 10);
		break;
	}

	switch (opt->index) {
	case OPTION_EXPANDTAB:
		vis->expandtab = arg.b;
		break;
	case OPTION_AUTOINDENT:
		vis->autoindent = arg.b;
		break;
	case OPTION_TABWIDTH:
		tabwidth_set(vis, arg.i);
		break;
	case OPTION_SYNTAX:
		if (!argv[2]) {
			const char *syntax = view_syntax_get(vis->win->view);
			if (syntax)
				vis_info_show(vis, "Syntax definition in use: `%s'", syntax);
			else
				vis_info_show(vis, "No syntax definition in use");
			return true;
		}

		if (parse_bool(argv[2], &arg.b) && !arg.b)
			return view_syntax_set(vis->win->view, NULL);
		if (!view_syntax_set(vis->win->view, argv[2])) {
			vis_info_show(vis, "Unknown syntax definition: `%s'", argv[2]);
			return false;
		}
		break;
	case OPTION_SHOW:
		if (!argv[2]) {
			vis_info_show(vis, "Expecting: spaces, tabs, newlines");
			return false;
		}
		char *keys[] = { "spaces", "tabs", "newlines" };
		int values[] = {
			UI_OPTION_SYMBOL_SPACE,
			UI_OPTION_SYMBOL_TAB|UI_OPTION_SYMBOL_TAB_FILL,
			UI_OPTION_SYMBOL_EOL,
		};
		int flags = view_options_get(vis->win->view);
		for (const char **args = &argv[2]; *args; args++) {
			for (int i = 0; i < LENGTH(keys); i++) {
				if (strcmp(*args, keys[i]) == 0) {
					flags |= values[i];
				} else if (strstr(*args, keys[i]) == *args) {
					bool show;
					const char *v = *args + strlen(keys[i]);
					if (*v == '=' && parse_bool(v+1, &show)) {
						if (show)
							flags |= values[i];
						else
							flags &= ~values[i];
					}
				}
			}
		}
		view_options_set(vis->win->view, flags);
		break;
	case OPTION_NUMBER: {
		enum UiOption opt = view_options_get(vis->win->view);
		if (arg.b) {
			opt &= ~UI_OPTION_LINE_NUMBERS_RELATIVE;
			opt |=  UI_OPTION_LINE_NUMBERS_ABSOLUTE;
		} else {
			opt &= ~UI_OPTION_LINE_NUMBERS_ABSOLUTE;
		}
		view_options_set(vis->win->view, opt);
		break;
	}
	case OPTION_NUMBER_RELATIVE: {
		enum UiOption opt = view_options_get(vis->win->view);
		if (arg.b) {
			opt &= ~UI_OPTION_LINE_NUMBERS_ABSOLUTE;
			opt |=  UI_OPTION_LINE_NUMBERS_RELATIVE;
		} else {
			opt &= ~UI_OPTION_LINE_NUMBERS_RELATIVE;
		}
		view_options_set(vis->win->view, opt);
		break;
	}
	case OPTION_CURSOR_LINE: {
		enum UiOption opt = view_options_get(vis->win->view);
		if (arg.b)
			opt |= UI_OPTION_CURSOR_LINE;
		else
			opt &= ~UI_OPTION_CURSOR_LINE;
		view_options_set(vis->win->view, opt);
		break;
	}
	case OPTION_THEME:
		if (!vis_theme_load(vis, arg.s)) {
			vis_info_show(vis, "Failed to load theme: `%s'", arg.s);
			return false;
		}
		break;
	case OPTION_COLOR_COLUMN:
		view_colorcolumn_set(vis->win->view, arg.i);
		break;
	}

	return true;
}

static bool is_file_pattern(const char *pattern) {
	return pattern && (strcmp(pattern, ".") == 0 || strchr(pattern, '*') ||
	       strchr(pattern, '[') || strchr(pattern, '{'));
}

static const char *file_open_dialog(Vis *vis, const char *pattern) {
	if (!is_file_pattern(pattern))
		return pattern;
	/* this is a bit of a hack, we temporarily replace the text/view of the active
	 * window such that we can use cmd_filter as is */
	char vis_open[512];
	static char filename[PATH_MAX];
	Filerange range = text_range_empty();
	Win *win = vis->win;
	File *file = win->file;
	Text *txt_orig = file->text;
	View *view_orig = win->view;
	Text *txt = text_load(NULL);
	View *view = view_new(txt, NULL, NULL);
	filename[0] = '\0';
	snprintf(vis_open, sizeof(vis_open)-1, "vis-open %s", pattern ? pattern : "");

	if (!txt || !view)
		goto out;
	win->view = view;
	file->text = txt;

	if (cmd_filter(vis, &range, CMD_OPT_NONE, (const char *[]){ "open", vis_open, NULL })) {
		size_t len = text_size(txt);
		if (len >= sizeof(filename))
			len = 0;
		if (len > 0)
			text_bytes_get(txt, 0, --len, filename);
		filename[len] = '\0';
	}

out:
	view_free(view);
	text_free(txt);
	win->view = view_orig;
	file->text = txt_orig;
	return filename[0] ? filename : NULL;
}

static bool openfiles(Vis *vis, const char **files) {
	for (; *files; files++) {
		const char *file = file_open_dialog(vis, *files);
		if (!file)
			return false;
		errno = 0;
		if (!vis_window_new(vis, file)) {
			vis_info_show(vis, "Could not open `%s' %s", file,
			                 errno ? strerror(errno) : "");
			return false;
		}
	}
	return true;
}

static bool cmd_open(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (!argv[1])
		return vis_window_new(vis, NULL);
	return openfiles(vis, &argv[1]);
}

static bool is_view_closeable(Win *win) {
	if (!text_modified(win->file->text))
		return true;
	return win->file->refcount > 1;
}

static void info_unsaved_changes(Vis *vis) {
	vis_info_show(vis, "No write since last change (add ! to override)");
}

static bool cmd_edit(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	Win *oldwin = vis->win;
	if (!(opt & CMD_OPT_FORCE) && !is_view_closeable(oldwin)) {
		info_unsaved_changes(vis);
		return false;
	}
	if (!argv[1])
		return vis_window_reload(oldwin);
	if (!openfiles(vis, &argv[1]))
		return false;
	if (vis->win != oldwin)
		vis_window_close(oldwin);
	return vis->win != oldwin;
}

static bool cmd_quit(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (!(opt & CMD_OPT_FORCE) && !is_view_closeable(vis->win)) {
		info_unsaved_changes(vis);
		return false;
	}
	vis_window_close(vis->win);
	if (!vis->windows)
		vis_exit(vis, EXIT_SUCCESS);
	return true;
}

static bool cmd_xit(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (text_modified(vis->win->file->text) && !cmd_write(vis, range, opt, argv)) {
		if (!(opt & CMD_OPT_FORCE))
			return false;
	}
	return cmd_quit(vis, range, opt, argv);
}

static bool cmd_bdelete(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	Text *txt = vis->win->file->text;
	if (text_modified(txt) && !(opt & CMD_OPT_FORCE)) {
		info_unsaved_changes(vis);
		return false;
	}
	for (Win *next, *win = vis->windows; win; win = next) {
		next = win->next;
		if (win->file->text == txt)
			vis_window_close(win);
	}
	if (!vis->windows)
		vis_exit(vis, EXIT_SUCCESS);
	return true;
}

static bool cmd_qall(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	for (Win *next, *win = vis->windows; win; win = next) {
		next = win->next;
		if (!text_modified(vis->win->file->text) || (opt & CMD_OPT_FORCE))
			vis_window_close(win);
	}
	if (!vis->windows)
		vis_exit(vis, EXIT_SUCCESS);
	else
		info_unsaved_changes(vis);
	return vis->windows == NULL;
}

static bool cmd_read(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	char cmd[255];

	if (!argv[1]) {
		vis_info_show(vis, "Filename or command expected");
		return false;
	}

	bool iscmd = (opt & CMD_OPT_FORCE) || argv[1][0] == '!';
	const char *arg = argv[1]+(argv[1][0] == '!');
	snprintf(cmd, sizeof cmd, "%s%s", iscmd ? "" : "cat ", arg);

	size_t pos = view_cursor_get(vis->win->view);
	if (!text_range_valid(range))
		*range = (Filerange){ .start = pos, .end = pos };
	Filerange delete = *range;
	range->start = range->end;

	bool ret = cmd_filter(vis, range, opt, (const char*[]){ argv[0], "sh", "-c", cmd, NULL});
	if (ret)
		text_delete_range(vis->win->file->text, &delete);
	return ret;
}

static bool cmd_substitute(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	char pattern[255];
	if (!text_range_valid(range))
		*range = (Filerange){ .start = 0, .end = text_size(vis->win->file->text) };
	snprintf(pattern, sizeof pattern, "s%s", argv[1]);
	return cmd_filter(vis, range, opt, (const char*[]){ argv[0], "sed", pattern, NULL});
}

static bool cmd_split(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	enum UiOption options = view_options_get(vis->win->view);
	windows_arrange(vis, UI_LAYOUT_HORIZONTAL);
	if (!argv[1])
		return vis_window_split(vis->win);
	bool ret = openfiles(vis, &argv[1]);
	view_options_set(vis->win->view, options);
	return ret;
}

static bool cmd_vsplit(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	enum UiOption options = view_options_get(vis->win->view);
	windows_arrange(vis, UI_LAYOUT_VERTICAL);
	if (!argv[1])
		return vis_window_split(vis->win);
	bool ret = openfiles(vis, &argv[1]);
	view_options_set(vis->win->view, options);
	return ret;
}

static bool cmd_new(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	windows_arrange(vis, UI_LAYOUT_HORIZONTAL);
	return vis_window_new(vis, NULL);
}

static bool cmd_vnew(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	windows_arrange(vis, UI_LAYOUT_VERTICAL);
	return vis_window_new(vis, NULL);
}

static bool cmd_wq(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (cmd_write(vis, range, opt, argv))
		return cmd_quit(vis, range, opt, argv);
	return false;
}

static bool cmd_write(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	File *file = vis->win->file;
	Text *text = file->text;
	if (!text_range_valid(range))
		*range = (Filerange){ .start = 0, .end = text_size(text) };
	if (!argv[1])
		argv[1] = file->name;
	if (!argv[1]) {
		if (file->is_stdin) {
			if (strchr(argv[0], 'q')) {
				ssize_t written = text_write_range(text, range, STDOUT_FILENO);
				if (written == -1 || (size_t)written != text_range_size(range)) {
					vis_info_show(vis, "Can not write to stdout");
					return false;
				}
				/* make sure the file is marked as saved i.e. not modified */
				text_save_range(text, range, NULL);
				return true;
			}
			vis_info_show(vis, "No filename given, use 'wq' to write to stdout");
			return false;
		}
		vis_info_show(vis, "Filename expected");
		return false;
	}
	for (const char **name = &argv[1]; *name; name++) {
		struct stat meta;
		if (!(opt & CMD_OPT_FORCE) && file->stat.st_mtime && stat(*name, &meta) == 0 &&
		    file->stat.st_mtime < meta.st_mtime) {
			vis_info_show(vis, "WARNING: file has been changed since reading it");
			return false;
		}
		if (!text_save_range(text, range, *name)) {
			vis_info_show(vis, "Can't write `%s'", *name);
			return false;
		}
		if (!file->name) {
			vis_window_name(vis->win, *name);
			file->name = vis->win->file->name;
		}
		if (strcmp(file->name, *name) == 0)
			file->stat = text_stat(text);
	}
	return true;
}

static bool cmd_saveas(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (cmd_write(vis, range, opt, argv)) {
		vis_window_name(vis->win, argv[1]);
		vis->win->file->stat = text_stat(vis->win->file->text);
		return true;
	}
	return false;
}

static bool cmd_filter(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	/* if an invalid range was given, stdin (i.e. key board input) is passed
	 * through the external command. */
	Text *text = vis->win->file->text;
	View *view = vis->win->view;
	int pin[2], pout[2], perr[2], status = -1;
	bool interactive = !text_range_valid(range);
	size_t pos = view_cursor_get(view);

	if (pipe(pin) == -1)
		return false;
	if (pipe(pout) == -1) {
		close(pin[0]);
		close(pin[1]);
		return false;
	}

	if (pipe(perr) == -1) {
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return false;
	}

	vis->ui->terminal_save(vis->ui);
	pid_t pid = fork();

	if (pid == -1) {
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		close(perr[0]);
		close(perr[1]);
		vis_info_show(vis, "fork failure: %s", strerror(errno));
		return false;
	} else if (pid == 0) { /* child i.e filter */
		if (!interactive)
			dup2(pin[0], STDIN_FILENO);
		close(pin[0]);
		close(pin[1]);
		dup2(pout[1], STDOUT_FILENO);
		close(pout[1]);
		close(pout[0]);
		if (!interactive)
			dup2(perr[1], STDERR_FILENO);
		close(perr[0]);
		close(perr[1]);
		if (!argv[2])
			execl("/bin/sh", "sh", "-c", argv[1], NULL);
		else
			execvp(argv[1], (char**)argv+1);
		vis_info_show(vis, "exec failure: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	vis->cancel_filter = false;

	close(pin[0]);
	close(pout[1]);
	close(perr[1]);

	fcntl(pout[0], F_SETFL, O_NONBLOCK);
	fcntl(perr[0], F_SETFL, O_NONBLOCK);

	if (interactive)
		*range = (Filerange){ .start = pos, .end = pos };

	/* ranges which are written to the filter and read back in */
	Filerange rout = *range;
	Filerange rin = (Filerange){ .start = range->end, .end = range->end };

	/* The general idea is the following:
	 *
	 *  1) take a snapshot
	 *  2) write [range.start, range.end] to exteneral command
	 *  3) read the output of the external command and insert it after the range
	 *  4) depending on the exit status of the external command
	 *     - on success: delete original range
	 *     - on failure: revert to previous snapshot
	 *
	 *  2) and 3) happend in small junks
	 */

	text_snapshot(text);

	fd_set rfds, wfds;
	Buffer errmsg;
	buffer_init(&errmsg);

	do {
		if (vis->cancel_filter) {
			kill(-pid, SIGTERM);
			vis_info_show(vis, "Command cancelled");
			break;
		}

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		if (pin[1] != -1)
			FD_SET(pin[1], &wfds);
		if (pout[0] != -1)
			FD_SET(pout[0], &rfds);
		if (perr[0] != -1)
			FD_SET(perr[0], &rfds);

		if (select(FD_SETSIZE, &rfds, &wfds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			vis_info_show(vis, "Select failure");
			break;
		}

		if (pin[1] != -1 && FD_ISSET(pin[1], &wfds)) {
			Filerange junk = *range;
			if (junk.end > junk.start + PIPE_BUF)
				junk.end = junk.start + PIPE_BUF;
			ssize_t len = text_write_range(text, &junk, pin[1]);
			if (len > 0) {
				range->start += len;
				if (text_range_size(range) == 0) {
					close(pout[1]);
					pout[1] = -1;
				}
			} else {
				close(pin[1]);
				pin[1] = -1;
				if (len == -1)
					vis_info_show(vis, "Error writing to external command");
			}
		}

		if (pout[0] != -1 && FD_ISSET(pout[0], &rfds)) {
			char buf[BUFSIZ];
			ssize_t len = read(pout[0], buf, sizeof buf);
			if (len > 0) {
				text_insert(text, rin.end, buf, len);
				rin.end += len;
			} else if (len == 0) {
				close(pout[0]);
				pout[0] = -1;
			} else if (errno != EINTR && errno != EWOULDBLOCK) {
				vis_info_show(vis, "Error reading from filter stdout");
				close(pout[0]);
				pout[0] = -1;
			}
		}

		if (perr[0] != -1 && FD_ISSET(perr[0], &rfds)) {
			char buf[BUFSIZ];
			ssize_t len = read(perr[0], buf, sizeof buf);
			if (len > 0) {
				buffer_append(&errmsg, buf, len);
			} else if (len == 0) {
				close(perr[0]);
				perr[0] = -1;
			} else if (errno != EINTR && errno != EWOULDBLOCK) {
				vis_info_show(vis, "Error reading from filter stderr");
				close(pout[0]);
				pout[0] = -1;
			}
		}

	} while (pin[1] != -1 || pout[0] != -1 || perr[0] != -1);

	if (pin[1] != -1)
		close(pin[1]);
	if (pout[0] != -1)
		close(pout[0]);
	if (perr[0] != -1)
		close(perr[0]);

	if (waitpid(pid, &status, 0) == pid && status == 0) {
		text_delete_range(text, &rout);
		text_snapshot(text);
	} else {
		/* make sure we have somehting to undo */
		text_insert(text, pos, " ", 1);
		text_undo(text);
	}

	view_cursor_to(view, rout.start);

	if (!vis->cancel_filter) {
		if (status == 0)
			vis_info_show(vis, "Command succeded");
		else if (errmsg.len > 0)
			vis_info_show(vis, "Command failed: %s", errmsg.data);
		else
			vis_info_show(vis, "Command failed");
	}

	vis->ui->terminal_restore(vis->ui);
	return status == 0;
}

static bool cmd_earlier_later(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	Text *txt = vis->win->file->text;
	char *unit = "";
	long count = 1;
	size_t pos = EPOS;
	if (argv[1]) {
		errno = 0;
		count = strtol(argv[1], &unit, 10);
		if (errno || unit == argv[1] || count < 0) {
			vis_info_show(vis, "Invalid number");
			return false;
		}

		if (*unit) {
			while (*unit && isspace((unsigned char)*unit))
				unit++;
			switch (*unit) {
			case 'd': count *= 24; /* fall through */
			case 'h': count *= 60; /* fall through */
			case 'm': count *= 60; /* fall through */
			case 's': break;
			default:
				vis_info_show(vis, "Unknown time specifier (use: s,m,h or d)");
				return false;
			}

			if (argv[0][0] == 'e')
				count = -count; /* earlier, move back in time */

			pos = text_restore(txt, text_state(txt) + count);
		}
	}

	if (!*unit) {
		if (argv[0][0] == 'e')
			pos = text_earlier(txt, count);
		else
			pos = text_later(txt, count);
	}

	time_t state = text_state(txt);
	char buf[32];
	strftime(buf, sizeof buf, "State from %H:%M", localtime(&state));
	vis_info_show(vis, "%s", buf);

	return pos != EPOS;
}

bool print_keybinding(const char *key, void *value, void *data) {
	Text *txt = (Text*)data;
	KeyBinding *binding = (KeyBinding*)value;
	const char *desc = binding->alias;
	if (!desc && binding->action)
		desc = binding->action->help;
	return text_appendf(txt, "  %-15s\t%s\n", key, desc ? desc : "");
}

static void print_mode(Mode *mode, Text *txt, bool recursive) {
	if (recursive && mode->parent)
		print_mode(mode->parent, txt, recursive);
	map_iterate(mode->bindings, print_keybinding, txt);
}

static bool cmd_help(Vis *vis, Filerange *range, enum CmdOpt opt, const char *argv[]) {
	if (!vis_window_new(vis, NULL))
		return false;

	Text *txt = vis->win->file->text;

	text_appendf(txt, "vis %s, compiled " __DATE__ " " __TIME__ "\n\n", VERSION);

	text_appendf(txt, " Modes\n\n");
	for (int i = 0; i < LENGTH(vis_modes); i++) {
		Mode *mode = &vis_modes[i];
		if (mode->help)
			text_appendf(txt, "  %-15s\t%s\n", mode->name, mode->help);
	}

	for (int i = 0; i < LENGTH(vis_modes); i++) {
		Mode *mode = &vis_modes[i];
		if (mode->isuser && !map_empty(mode->bindings)) {
			text_appendf(txt, "\n %s\n\n", mode->name);
			print_mode(mode, txt, i == VIS_MODE_NORMAL ||
				i == VIS_MODE_INSERT);
		}
	}

	text_appendf(txt, "\n Text objects\n\n");
	print_mode(&vis_modes[VIS_MODE_TEXTOBJ], txt, false);

	text_appendf(txt, "\n Motions\n\n");
	print_mode(&vis_modes[VIS_MODE_MOVE], txt, false);

	text_appendf(txt, "\n :-Commands\n\n");
	for (Command *cmd = cmds; cmd && cmd->name[0]; cmd++)
		text_appendf(txt, "  %s\n", cmd->name[0]);

	text_save(txt, NULL);
	return true;
}

static Filepos parse_pos(Win *win, char **cmd) {
	size_t pos = EPOS;
	View *view = win->view;
	Text *txt = win->file->text;
	Mark *marks = win->file->marks;
	switch (**cmd) {
	case '.':
		pos = text_line_begin(txt, view_cursor_get(view));
		(*cmd)++;
		break;
	case '$':
		pos = text_size(txt);
		(*cmd)++;
		break;
	case '\'':
		(*cmd)++;
		if ('a' <= **cmd && **cmd <= 'z')
			pos = text_mark_get(txt, marks[**cmd - 'a']);
		else if (**cmd == '<')
			pos = text_mark_get(txt, marks[MARK_SELECTION_START]);
		else if (**cmd == '>')
			pos = text_mark_get(txt, marks[MARK_SELECTION_END]);
		(*cmd)++;
		break;
	case '/':
		(*cmd)++;
		char *pattern_end = strchr(*cmd, '/');
		if (!pattern_end)
			return EPOS;
		*pattern_end++ = '\0';
		Regex *regex = text_regex_new();
		if (!regex)
			return EPOS;
		if (!text_regex_compile(regex, *cmd, 0)) {
			*cmd = pattern_end;
			pos = text_search_forward(txt, view_cursor_get(view), regex);
		}
		text_regex_free(regex);
		break;
	case '+':
	case '-':
	{
		CursorPos curspos = view_cursor_getpos(view);
		long long line = curspos.line + strtoll(*cmd, cmd, 10);
		if (line < 0)
			line = 0;
		pos = text_pos_by_lineno(txt, line);
		break;
	}
	default:
		if ('0' <= **cmd && **cmd <= '9')
			pos = text_pos_by_lineno(txt, strtoul(*cmd, cmd, 10));
		break;
	}

	return pos;
}

static Filerange parse_range(Win *win, char **cmd) {
	Text *txt = win->file->text;
	Filerange r = text_range_empty();
	Mark *marks = win->file->marks;
	char start = **cmd;
	switch (**cmd) {
	case '%':
		r.start = 0;
		r.end = text_size(txt);
		(*cmd)++;
		break;
	case '*':
		r.start = text_mark_get(txt, marks[MARK_SELECTION_START]);
		r.end = text_mark_get(txt, marks[MARK_SELECTION_END]);
		(*cmd)++;
		break;
	default:
		r.start = parse_pos(win, cmd);
		if (**cmd != ',') {
			if (start == '.')
				r.end = text_line_next(txt, r.start);
			return r;
		}
		(*cmd)++;
		r.end = parse_pos(win, cmd);
		break;
	}
	return r;
}

static Command *lookup_cmd(Vis *vis, const char *name) {
	if (!vis->cmds) {
		if (!(vis->cmds = map_new()))
			return NULL;

		for (Command *cmd = cmds; cmd && cmd->name[0]; cmd++) {
			for (const char **name = cmd->name; *name; name++)
				map_put(vis->cmds, *name, cmd);
		}
	}
	return map_closest(vis->cmds, name);
}

bool vis_cmd(Vis *vis, const char *cmdline) {
	enum CmdOpt opt = CMD_OPT_NONE;
	size_t len = strlen(cmdline);
	char *line = malloc(len+2);
	if (!line)
		return false;
	line = strncpy(line, cmdline, len+1);
	char *name = line;

	Filerange range = parse_range(vis->win, &name);
	if (!text_range_valid(&range)) {
		/* if only one position was given, jump to it */
		if (range.start != EPOS && !*name) {
			view_cursor_to(vis->win->view, range.start);
			free(line);
			return true;
		}

		if (name != line) {
			vis_info_show(vis, "Invalid range\n");
			free(line);
			return false;
		}
	}
	/* skip leading white space */
	while (*name == ' ')
		name++;
	char *param = name;
	while (*param && isalpha(*param))
		param++;

	if (*param == '!') {
		if (param != name) {
			opt |= CMD_OPT_FORCE;
			*param = ' ';
		} else {
			param++;
		}
	}

	memmove(param+1, param, strlen(param)+1);
	*param++ = '\0'; /* separate command name from parameters */

	Command *cmd = lookup_cmd(vis, name);
	if (!cmd) {
		vis_info_show(vis, "Not an editor command");
		free(line);
		return false;
	}

	char *s = param;
	const char *argv[32] = { name };
	for (int i = 1; i < LENGTH(argv); i++) {
		while (s && *s && *s == ' ')
			s++;
		if (s && !*s)
			s = NULL;
		argv[i] = s;
		if (!(cmd->opt & CMD_OPT_ARGS)) {
			/* remove trailing spaces */
			if (s) {
				while (*s) s++;
				while (*(--s) == ' ') *s = '\0';
			}
			s = NULL;
		}
		if (s && (s = strchr(s, ' ')))
			*s++ = '\0';
		/* strip out a single '!' argument to make ":q !" work */
		if (argv[i] && !strcmp(argv[i], "!")) {
			opt |= CMD_OPT_FORCE;
			i--;
		}
	}

	cmd->cmd(vis, &range, opt, argv);
	free(line);
	return true;
}

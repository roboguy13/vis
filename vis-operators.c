#include <string.h>
#include <ctype.h>
#include "vis-core.h"
#include "text-motions.h"
#include "text-objects.h"
#include "text-util.h"
#include "util.h"

static size_t op_delete(Vis *vis, Text *txt, OperatorContext *c) {
	c->reg->linewise = c->linewise;
	register_put(c->reg, txt, &c->range);
	text_delete_range(txt, &c->range);
	size_t pos = c->range.start;
	if (c->linewise && pos == text_size(txt))
		pos = text_line_begin(txt, text_line_prev(txt, pos));
	return pos;
}

static size_t op_change(Vis *vis, Text *txt, OperatorContext *c) {
	op_delete(vis, txt, c);
	macro_operator_record(vis);
	return c->range.start;
}

static size_t op_yank(Vis *vis, Text *txt, OperatorContext *c) {
	c->reg->linewise = c->linewise;
	register_put(c->reg, txt, &c->range);
	return c->pos;
}

static size_t op_put(Vis *vis, Text *txt, OperatorContext *c) {
	size_t pos = c->pos;
	bool sel = text_range_size(&c->range) > 0;
	bool sel_linewise = sel && text_range_is_linewise(txt, &c->range);
	if (sel) {
		text_delete_range(txt, &c->range);
		pos = c->pos = c->range.start;
	}
	switch (c->arg->i) {
	case VIS_OP_PUT_AFTER:
	case VIS_OP_PUT_AFTER_END:
		if (c->reg->linewise && !sel_linewise)
			pos = text_line_next(txt, pos);
		else if (!sel)
			pos = text_char_next(txt, pos);
		break;
	case VIS_OP_PUT_BEFORE:
	case VIS_OP_PUT_BEFORE_END:
		if (c->reg->linewise)
			pos = text_line_begin(txt, pos);
		break;
	}

	for (int i = 0; i < c->count; i++) {
		text_insert(txt, pos, c->reg->data, c->reg->len);
		pos += c->reg->len;
	}

	if (c->reg->linewise) {
		switch (c->arg->i) {
		case VIS_OP_PUT_BEFORE_END:
		case VIS_OP_PUT_AFTER_END:
			pos = text_line_start(txt, pos);
			break;
		case VIS_OP_PUT_AFTER:
			pos = text_line_start(txt, text_line_next(txt, c->pos));
			break;
		case VIS_OP_PUT_BEFORE:
			pos = text_line_start(txt, c->pos);
			break;
		}
	} else {
		switch (c->arg->i) {
		case VIS_OP_PUT_AFTER:
		case VIS_OP_PUT_BEFORE:
			pos = text_char_prev(txt, pos);
			break;
		}
	}

	return pos;
}

static size_t op_shift_right(Vis *vis, Text *txt, OperatorContext *c) {
	size_t pos = text_line_begin(txt, c->range.end), prev_pos;
	const char *tab = expandtab(vis);
	size_t tablen = strlen(tab);

	/* if range ends at the begin of a line, skip line break */
	if (pos == c->range.end)
		pos = text_line_prev(txt, pos);

	do {
		prev_pos = pos = text_line_begin(txt, pos);
		text_insert(txt, pos, tab, tablen);
		pos = text_line_prev(txt, pos);
	}  while (pos >= c->range.start && pos != prev_pos);

	return c->pos + tablen;
}

static size_t op_shift_left(Vis *vis, Text *txt, OperatorContext *c) {
	size_t pos = text_line_begin(txt, c->range.end), prev_pos;
	size_t tabwidth = vis->tabwidth, tablen;

	/* if range ends at the begin of a line, skip line break */
	if (pos == c->range.end)
		pos = text_line_prev(txt, pos);

	do {
		char c;
		size_t len = 0;
		prev_pos = pos = text_line_begin(txt, pos);
		Iterator it = text_iterator_get(txt, pos);
		if (text_iterator_byte_get(&it, &c) && c == '\t') {
			len = 1;
		} else {
			for (len = 0; text_iterator_byte_get(&it, &c) && c == ' '; len++)
				text_iterator_byte_next(&it, NULL);
		}
		tablen = MIN(len, tabwidth);
		text_delete(txt, pos, tablen);
		pos = text_line_prev(txt, pos);
	}  while (pos >= c->range.start && pos != prev_pos);

	return c->pos - tablen;
}

static size_t op_case_change(Vis *vis, Text *txt, OperatorContext *c) {
	size_t len = text_range_size(&c->range);
	char *buf = malloc(len);
	if (!buf)
		return c->pos;
	len = text_bytes_get(txt, c->range.start, len, buf);
	size_t rem = len;
	for (char *cur = buf; rem > 0; cur++, rem--) {
		int ch = (unsigned char)*cur;
		if (isascii(ch)) {
			if (c->arg->i == VIS_OP_CASE_SWAP)
				*cur = islower(ch) ? toupper(ch) : tolower(ch);
			else if (c->arg->i == VIS_OP_CASE_UPPER)
				*cur = toupper(ch);
			else
				*cur = tolower(ch);
		}
	}

	text_delete(txt, c->range.start, len);
	text_insert(txt, c->range.start, buf, len);
	free(buf);
	return c->pos;
}

static size_t op_cursor(Vis *vis, Text *txt, OperatorContext *c) {
	View *view = vis->win->view;
	Filerange r = text_range_linewise(txt, &c->range);
	for (size_t line = text_range_line_first(txt, &r); line != EPOS; line = text_range_line_next(txt, &r, line)) {
		Cursor *cursor = view_cursors_new(view);
		if (cursor) {
			size_t pos;
			if (c->arg->i == VIS_OP_CURSOR_EOL)
				pos = text_line_finish(txt, line);
			else
				pos = text_line_start(txt, line);
			view_cursors_to(cursor, pos);
		}
	}
	return EPOS;
}

static size_t op_join(Vis *vis, Text *txt, OperatorContext *c) {
	size_t pos = text_line_begin(txt, c->range.end), prev_pos;

	/* if operator and range are both linewise, skip last line break */
	if (c->linewise && text_range_is_linewise(txt, &c->range)) {
		size_t line_prev = text_line_prev(txt, pos);
		size_t line_prev_prev = text_line_prev(txt, line_prev);
		if (line_prev_prev >= c->range.start)
			pos = line_prev;
	}

	do {
		prev_pos = pos;
		size_t end = text_line_start(txt, pos);
		pos = text_char_next(txt, text_line_finish(txt, text_line_prev(txt, end)));
		if (pos >= c->range.start && end > pos) {
			text_delete(txt, pos, end - pos);
			text_insert(txt, pos, " ", 1);
		} else {
			break;
		}
	} while (pos != prev_pos);

	return c->range.start;
}

static size_t op_insert(Vis *vis, Text *txt, OperatorContext *c) {
	macro_operator_record(vis);
	return c->newpos != EPOS ? c->newpos : c->pos;
}

static size_t op_replace(Vis *vis, Text *txt, OperatorContext *c) {
	macro_operator_record(vis);
	return c->newpos != EPOS ? c->newpos : c->pos;
}

Operator ops[] = {
	[VIS_OP_DELETE]      = { op_delete      },
	[VIS_OP_CHANGE]      = { op_change      },
	[VIS_OP_YANK]        = { op_yank        },
	[VIS_OP_PUT_AFTER]   = { op_put         },
	[VIS_OP_SHIFT_RIGHT] = { op_shift_right },
	[VIS_OP_SHIFT_LEFT]  = { op_shift_left  },
	[VIS_OP_CASE_SWAP]   = { op_case_change },
	[VIS_OP_JOIN]        = { op_join        },
	[VIS_OP_INSERT]      = { op_insert      },
	[VIS_OP_REPLACE]     = { op_replace     },
	[VIS_OP_CURSOR_SOL]  = { op_cursor      },
};

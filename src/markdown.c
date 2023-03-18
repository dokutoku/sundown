/* markdown.c - generic markdown parser */

/*
 * Copyright (c) 2009, Natacha Porté
 * Copyright (c) 2011, Vicent Marti
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "markdown.h"
#include "stack.h"

#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#if defined(_WIN32)
#define strncasecmp _strnicmp
#endif

#define REF_TABLE_SIZE 8

#define BUFFER_BLOCK 0
#define BUFFER_SPAN 1

/* internal list flag */
#define MKD_LI_END 8

#define gperf_case_strncmp(s1, s2, n) strncasecmp(s1, s2, n)
#define GPERF_DOWNCASE 1
#define GPERF_CASE_STRNCMP 1
#include "html_blocks.h"

/* **************
 * LOCAL TYPES *
 ***************/

/**
 * reference to a link
 */
struct link_ref
{
	unsigned int id;

	struct buf *link;
	struct buf *title;

	struct link_ref *next;
};

/**
 * reference to a footnote
 */
struct footnote_ref
{
	unsigned int id;

	int is_used;
	unsigned int num;

	struct buf *contents;
};

/**
 * an item in a footnote_list
 */
struct footnote_item
{
	struct footnote_ref *ref_;
	struct footnote_item *next;
};

/**
 * linked list of footnote_item
 */
struct footnote_list
{
	unsigned int count;
	struct footnote_item *head;
	struct footnote_item *tail;
};

/**
 * function pointer to render active chars
 */
/*   returns the number of chars taken care of */
/*   data is the pointer of the beginning of the span */
/*   offset is the number of valid chars before data */
struct sd_markdown;
typedef size_t (*char_trigger)(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);

static size_t char_emphasis(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_linebreak(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_codespan(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_escape(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_entity(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_langle_tag(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_autolink_url(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_autolink_email(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_autolink_www(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_link(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);
static size_t char_superscript(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size);

enum markdown_char_t
{
	MD_CHAR_NONE = 0,
	MD_CHAR_EMPHASIS,
	MD_CHAR_CODESPAN,
	MD_CHAR_LINEBREAK,
	MD_CHAR_LINK,
	MD_CHAR_LANGLE,
	MD_CHAR_ESCAPE,
	MD_CHAR_ENTITITY,
	MD_CHAR_AUTOLINK_URL,
	MD_CHAR_AUTOLINK_EMAIL,
	MD_CHAR_AUTOLINK_WWW,
	MD_CHAR_SUPERSCRIPT,
};

static char_trigger markdown_char_ptrs[] =
{
	NULL,
	&char_emphasis,
	&char_codespan,
	&char_linebreak,
	&char_link,
	&char_langle_tag,
	&char_escape,
	&char_entity,
	&char_autolink_url,
	&char_autolink_email,
	&char_autolink_www,
	&char_superscript,
};

/**
 * structure containing one particular render
 */
struct sd_markdown
{
	struct sd_callbacks cb;
	void *opaque;

	struct link_ref *refs[REF_TABLE_SIZE];
	struct footnote_list footnotes_found;
	struct footnote_list footnotes_used;
	uint8_t active_char[256];
	struct stack work_bufs[2];
	unsigned int ext_flags;
	size_t max_nesting;
	int in_link_body;
};

/* **************************
 * HELPER FUNCTIONS *
 ***************************/

static inline struct buf *rndr_newbuf(struct sd_markdown *rndr, int type)
{
	static const size_t buf_size[2] = {256, 64};
	struct buf *work = NULL;
	struct stack *pool = &rndr->work_bufs[type];

	if (pool->size < pool->asize && pool->item[pool->size] != NULL) {
		work = pool->item[pool->size++];
		work->size = 0;
	} else {
		work = bufnew(buf_size[type]);

		if (work == NULL) {
			return NULL;
		}

		if (stack_push(pool, work) < 0) {
			stack_free(pool);

			return NULL;
		}
	}

	return work;
}

static inline void rndr_popbuf(struct sd_markdown *rndr, int type)
{
	rndr->work_bufs[type].size--;
}

static void unscape_text(struct buf *ob, struct buf *src)
{
	size_t i = 0;

	while (i < src->size) {
		size_t org = i;

		while (i < src->size && src->data[i] != '\\') {
			i++;
		}

		if (i > org) {
			bufput(ob, src->data + org, i - org);
		}

		if (i + 1 >= src->size) {
			break;
		}

		bufputc(ob, src->data[i + 1]);
		i += 2;
	}
}

static unsigned int hash_link_ref(const uint8_t *link_ref, size_t length_)
{
	unsigned int hash = 0;

	for (size_t i = 0; i < length_; ++i) {
		hash = tolower(link_ref[i]) + (hash << 6) + (hash << 16) - hash;
	}

	return hash;
}

static struct link_ref *add_link_ref(struct link_ref **references, const uint8_t *name, size_t name_size)
{
	struct link_ref *ref_ = calloc(1, sizeof(struct link_ref));

	if (!ref_) {
		return NULL;
	}

	ref_->id = hash_link_ref(name, name_size);
	ref_->next = references[ref_->id % REF_TABLE_SIZE];

	references[ref_->id % REF_TABLE_SIZE] = ref_;

	return ref_;
}

static struct link_ref *find_link_ref(struct link_ref **references, uint8_t *name, size_t length_)
{
	unsigned int hash = hash_link_ref(name, length_);
	struct link_ref *ref_ = references[hash % REF_TABLE_SIZE];

	while (ref_ != NULL) {
		if (ref_->id == hash) {
			return ref_;
		}

		ref_ = ref_->next;
	}

	return NULL;
}

static void free_link_refs(struct link_ref **references)
{
	for (size_t i = 0; i < REF_TABLE_SIZE; ++i) {
		struct link_ref *r = references[i];

		while (r) {
			struct link_ref *next = r->next;
			bufrelease(r->link);
			bufrelease(r->title);
			free(r);
			r = next;
		}
	}
}

static struct footnote_ref *create_footnote_ref(struct footnote_list *list, const uint8_t *name, size_t name_size)
{
	struct footnote_ref *ref_ = calloc(1, sizeof(struct footnote_ref));

	if (!ref_) {
		return NULL;
	}

	ref_->id = hash_link_ref(name, name_size);

	return ref_;
}

static int add_footnote_ref(struct footnote_list *list, struct footnote_ref *ref_)
{
	struct footnote_item *item = calloc(1, sizeof(struct footnote_item));

	if (!item) {
		return 0;
	}

	item->ref_ = ref_;

	if (list->head == NULL) {
		list->tail = item;
		list->head = item;
	} else {
		list->tail->next = item;
		list->tail = item;
	}

	list->count++;

	return 1;
}

static struct footnote_ref *find_footnote_ref(struct footnote_list *list, uint8_t *name, size_t length_)
{
	unsigned int hash = hash_link_ref(name, length_);
	struct footnote_item *item = list->head;

	while (item != NULL) {
		if (item->ref_->id == hash) {
			return item->ref_;
		}

		item = item->next;
	}

	return NULL;
}

static void free_footnote_ref(struct footnote_ref *ref_)
{
	bufrelease(ref_->contents);
	free(ref_);
}

static void free_footnote_list(struct footnote_list *list, int free_refs)
{
	struct footnote_item *item = list->head;

	while (item) {
		struct footnote_item *next = item->next;

		if (free_refs) {
			free_footnote_ref(item->ref_);
		}

		free(item);
		item = next;
	}
}

/**
 * Check whether a char is a Markdown space.
 *
 * Right now we only consider spaces the actual
 * space and a newline: tabs and carriage returns
 * are filtered out during the preprocessing phase.
 *
 * If we wanted to actually be UTF-8 compliant, we
 * should instead extract an Unicode codepoint from
 * this character and check for space properties.
 */
static inline int _isspace(int c)
{
	return c == ' ' || c == '\n';
}

/* ***************************
 * INLINE PARSING FUNCTIONS *
 ****************************/

/**
 * looks for the address part of a mail autolink and '>'
 */
/* this is less strict than the original markdown e-mail address matching */
static size_t is_mail_autolink(uint8_t *data, size_t size)
{
	/* address is assumed to be: [-@._a-zA-Z0-9]+ with exactly one '@' */
	for (size_t i = 0, nb = 0; i < size; ++i) {
		if (isalnum(data[i])) {
			continue;
		}

		switch (data[i]) {
			case '@':
				nb++;

				/* ToDo: FALLTHROUGH? */

			case '-':
			case '.':
			case '_':
				break;

			case '>':
				return (nb == 1) ? (i + 1) : (0);

			default:
				return 0;
		}
	}

	return 0;
}

/**
 * returns the length of the given tag, or 0 is it's not valid
 */
static size_t tag_length(uint8_t *data, size_t size, enum mkd_autolink *autolink)
{
	/* a valid tag can't be shorter than 3 chars */
	if (size < 3) {
		return 0;
	}

	/* begins with a '<' optionally followed by '/', followed by letter or number */
	if (data[0] != '<') {
		return 0;
	}

	size_t i = (data[1] == '/') ? (2) : (1);

	if (!isalnum(data[i])) {
		return 0;
	}

	/* scheme test */
	*autolink = MKDA_NOT_AUTOLINK;

	/* try to find the beginning of an URI */
	while (i < size && (isalnum(data[i]) || data[i] == '.' || data[i] == '+' || data[i] == '-')) {
		i++;
	}

	size_t j;

	if (i > 1 && data[i] == '@') {
		if ((j = is_mail_autolink(data + i, size - i)) != 0) {
			*autolink = MKDA_EMAIL;

			return i + j;
		}
	}

	if (i > 2 && data[i] == ':') {
		*autolink = MKDA_NORMAL;
		i++;
	}

	/* completing autolink test: no whitespace or ' or " */
	if (i >= size) {
		*autolink = MKDA_NOT_AUTOLINK;
	} else if (*autolink != MKDA_NOT_AUTOLINK) {
		j = i;

		while (i < size) {
			if (data[i] == '\\') {
				i += 2;
			} else if (data[i] == '>' || data[i] == '\'' || data[i] == '"' || data[i] == ' ' || data[i] == '\n') {
				break;
			} else {
				i++;
			}
		}

		if (i >= size) {
			return 0;
		}

		if (i > j && data[i] == '>') {
			return i + 1;
		}

		/* one of the forbidden chars has been found */
		*autolink = MKDA_NOT_AUTOLINK;
	}

	/* looking for sometinhg looking like a tag end */
	while (i < size && data[i] != '>') {
		i++;
	}

	if (i >= size) {
		return 0;
	}

	return i + 1;
}

/**
 * parses inline markdown elements
 */
static void parse_inline(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	if (rndr->work_bufs[BUFFER_SPAN].size + rndr->work_bufs[BUFFER_BLOCK].size > rndr->max_nesting) {
		return;
	}

	size_t i = 0;
	size_t end = 0;
	uint8_t action = 0;
	struct buf work = {NULL, 0, 0, 0};

	while (i < size) {
		/* copying inactive chars into the output */
		while (end < size && (action = rndr->active_char[data[end]]) == 0) {
			end++;
		}

		if (rndr->cb.normal_text) {
			work.data = data + i;
			work.size = end - i;
			rndr->cb.normal_text(ob, &work, rndr->opaque);
		} else {
			bufput(ob, data + i, end - i);
		}

		if (end >= size) {
			break;
		}

		i = end;

		end = markdown_char_ptrs[(int)action](ob, rndr, data + i, i, size - i);

		if (!end) { /* no action from the callback */
			end = i + 1;
		} else {
			i += end;
			end = i;
		}
	}
}

/**
 * looks for the next emph uint8_t, skipping other constructs
 */
static size_t find_emph_char(uint8_t *data, size_t size, uint8_t c)
{
	size_t i = 1;

	while (i < size) {
		while (i < size && data[i] != c && data[i] != '`' && data[i] != '[') {
			i++;
		}

		if (i == size) {
			return 0;
		}

		if (data[i] == c) {
			return i;
		}

		/* not counting escaped chars */
		if (i && data[i - 1] == '\\') {
			i++;

			continue;
		}

		if (data[i] == '`') {
			size_t span_nb = 0;

			/* counting the number of opening backticks */
			while (i < size && data[i] == '`') {
				i++;
				span_nb++;
			}

			if (i >= size) {
				return 0;
			}

			/* finding the matching closing sequence */
			size_t bt = 0;
			size_t tmp_i = 0;

			while (i < size && bt < span_nb) {
				if (!tmp_i && data[i] == c) {
					tmp_i = i;
				}

				if (data[i] == '`') {
					bt++;
				} else {
					bt = 0;
				}

				i++;
			}

			if (i >= size) {
				return tmp_i;
			}
		} else if (data[i] == '[') {
			/* skipping a link */

			size_t tmp_i = 0;

			i++;

			while (i < size && data[i] != ']') {
				if (!tmp_i && data[i] == c) {
					tmp_i = i;
				}

				i++;
			}

			i++;

			while (i < size && (data[i] == ' ' || data[i] == '\n')) {
				i++;
			}

			if (i >= size) {
				return tmp_i;
			}

			uint8_t cc;

			switch (data[i]) {
				case '[':
					cc = ']';

					break;

				case '(':
					cc = ')';

					break;

				default:
					if (tmp_i) {
						return tmp_i;
					} else {
						continue;
					}
			}

			i++;

			while (i < size && data[i] != cc) {
				if (!tmp_i && data[i] == c) {
					tmp_i = i;
				}

				i++;
			}

			if (i >= size) {
				return tmp_i;
			}

			i++;
		}
	}

	return 0;
}

/**
 * parsing single emphase
 */
/* closed by a symbol not preceded by whitespace and not followed by symbol */
static size_t parse_emph1(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, uint8_t c)
{
	if (!rndr->cb.emphasis) {
		return 0;
	}

	size_t i = 0;

	/* skipping one symbol if coming from emph3 */
	if (size > 1 && data[0] == c && data[1] == c) {
		i = 1;
	}

	while (i < size) {
		size_t len = find_emph_char(data + i, size - i, c);

		if (!len) {
			return 0;
		}

		i += len;

		if (i >= size) {
			return 0;
		}

		if (data[i] == c && !_isspace(data[i - 1])) {
			if (rndr->ext_flags & MKDEXT_NO_INTRA_EMPHASIS) {
				if (i + 1 < size && isalnum(data[i + 1])) {
					continue;
				}
			}

			struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

			if (work == NULL) {
				return 0;
			}

			parse_inline(work, rndr, data, i);
			int r = rndr->cb.emphasis(ob, work, rndr->opaque);
			rndr_popbuf(rndr, BUFFER_SPAN);

			return (r) ? (i + 1) : (0);
		}
	}

	return 0;
}

/**
 * parsing single emphase
 */
static size_t parse_emph2(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, uint8_t c)
{
	int (*render_method)(struct buf * ob, const struct buf * text, void *opaque);

	render_method = (c == '~') ? (rndr->cb.strikethrough) : (rndr->cb.double_emphasis);
	render_method = (c == '+') ? (rndr->cb.ins) : (render_method);

	if (!render_method) {
		return 0;
	}

	size_t i = 0;

	while (i < size) {
		size_t len = find_emph_char(data + i, size - i, c);

		if (!len) {
			return 0;
		}

		i += len;

		if (i + 1 < size && data[i] == c && data[i + 1] == c && i && !_isspace(data[i - 1])) {
			struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

			if (work == NULL) {
				return 0;
			}

			parse_inline(work, rndr, data, i);
			int r = render_method(ob, work, rndr->opaque);
			rndr_popbuf(rndr, BUFFER_SPAN);

			return (r) ? (i + 2) : (0);
		}

		i++;
	}

	return 0;
}

/**
 * parsing single emphase
 */
/* finds the first closing tag, and delegates to the other emph */
static size_t parse_emph3(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, uint8_t c)
{
	size_t i = 0;

	while (i < size) {
		size_t len = find_emph_char(data + i, size - i, c);

		if (!len) {
			return 0;
		}

		i += len;

		/* skip whitespace preceded symbols */
		if (data[i] != c || _isspace(data[i - 1])) {
			continue;
		}

		if (i + 2 < size && data[i + 1] == c && data[i + 2] == c && rndr->cb.triple_emphasis) {
			/* triple symbol found */
			struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

			if (work == NULL) {
				return 0;
			}

			parse_inline(work, rndr, data, i);
			int r = rndr->cb.triple_emphasis(ob, work, rndr->opaque);
			rndr_popbuf(rndr, BUFFER_SPAN);

			return (r) ? (i + 3) : (0);
		} else if (i + 1 < size && data[i + 1] == c) {
			/* double symbol found, handing over to emph1 */
			len = parse_emph1(ob, rndr, data - 2, size + 2, c);

			if (!len) {
				return 0;
			} else {
				return len - 2;
			}

		} else {
			/* single symbol found, handing over to emph2 */
			len = parse_emph2(ob, rndr, data - 1, size + 1, c);

			if (!len) {
				return 0;
			} else {
				return len - 1;
			}
		}
	}

	return 0;
}

/**
 * single and double emphasis parsing
 */
static size_t char_emphasis(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (rndr->ext_flags & MKDEXT_NO_INTRA_EMPHASIS) {
		if (offset > 0 && !_isspace(data[-1]) && data[-1] != '>') {
			return 0;
		}
	}

	uint8_t c = data[0];
	size_t ret;

	if (size > 2 && data[1] != c) {
		/*
		 * whitespace cannot follow an opening emphasis;
		 * ins only takes two characters '++'
		 * strikethrough only takes two characters '~~'
		 */
		if (c == '+' || c == '~' || _isspace(data[1]) || (ret = parse_emph1(ob, rndr, data + 1, size - 1, c)) == 0) {
			return 0;
		}

		return ret + 1;
	}

	if (size > 3 && data[1] == c && data[2] != c) {
		if (_isspace(data[2]) || (ret = parse_emph2(ob, rndr, data + 2, size - 2, c)) == 0) {
			return 0;
		}

		return ret + 2;
	}

	if (size > 4 && data[1] == c && data[2] == c && data[3] != c) {
		if (c == '+' || c == '~' || _isspace(data[3]) || (ret = parse_emph3(ob, rndr, data + 3, size - 3, c)) == 0) {
			return 0;
		}

		return ret + 3;
	}

	return 0;
}

/**
 * '\n' preceded by two spaces (assuming linebreak != 0)
 */
static size_t char_linebreak(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (offset < 2 || data[-1] != ' ' || data[-2] != ' ') {
		return 0;
	}

	/* removing the last space from ob and rendering */
	while (ob->size && ob->data[ob->size - 1] == ' ') {
		ob->size--;
	}

	return (rndr->cb.linebreak(ob, rndr->opaque)) ? (1) : (0);
}

/**
 * '`' parsing a code span (assuming codespan != 0)
 */
static size_t char_codespan(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	size_t nb = 0;

	/* counting the number of backticks in the delimiter */
	while (nb < size && data[nb] == '`') {
		nb++;
	}

	/* finding the next delimiter */
	size_t i = 0;
	size_t end;

	for (end = nb; end < size && i < nb; end++) {
		if (data[end] == '`') {
			i++;
		} else {
			i = 0;
		}
	}

	if (i < nb && end >= size) {
		/* no matching delimiter */
		return 0;
	}

	/* trimming outside whitespaces */
	size_t f_begin = nb;

	while (f_begin < end && data[f_begin] == ' ') {
		f_begin++;
	}

	size_t f_end = end - nb;

	while (f_end > nb && data[f_end - 1] == ' ') {
		f_end--;
	}

	/* real code span */
	if (f_begin < f_end) {
		struct buf work = {data + f_begin, f_end - f_begin, 0, 0};

		if (!rndr->cb.codespan(ob, &work, rndr->opaque)) {
			end = 0;
		}
	} else {
		if (!rndr->cb.codespan(ob, 0, rndr->opaque)) {
			end = 0;
		}
	}

	return end;
}

/**
 * '\\' backslash escape
 */
static size_t char_escape(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	static const char *escape_chars = "\\`*_{}[]()#+-.!:|&<>^~$";
	struct buf work = {NULL, 0, 0, 0};

	if (size > 1) {
		if (strchr(escape_chars, data[1]) == NULL) {
			return 0;
		}

		if (rndr->cb.normal_text) {
			work.data = data + 1;
			work.size = 1;
			rndr->cb.normal_text(ob, &work, rndr->opaque);
		} else {
			bufputc(ob, data[1]);
		}
	} else if (size == 1) {
		bufputc(ob, data[0]);
	}

	return 2;
}

/**
 * '&' escaped when it doesn't belong to an entity
 */
/* valid entities are assumed to be anything matching &#?[A-Za-z0-9]+; */
static size_t char_entity(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	size_t end = 1;
	struct buf work = {NULL, 0, 0, 0};

	if (end < size && data[end] == '#') {
		end++;
	}

	while (end < size && isalnum(data[end])) {
		end++;
	}

	if (end < size && data[end] == ';') {
		/* real entity */
		end++;
	} else {
		/* lone '&' */
		return 0;
	}

	if (rndr->cb.entity) {
		work.data = data;
		work.size = end;
		rndr->cb.entity(ob, &work, rndr->opaque);
	} else {
		bufput(ob, data, end);
	}

	return end;
}

/**
 * '<' when tags or autolinks are allowed
 */
static size_t char_langle_tag(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	enum mkd_autolink altype = MKDA_NOT_AUTOLINK;
	size_t end = tag_length(data, size, &altype);
	struct buf work = {data, end, 0, 0};
	int ret = 0;

	if (end > 2) {
		if (rndr->cb.autolink && altype != MKDA_NOT_AUTOLINK) {
			struct buf *u_link = rndr_newbuf(rndr, BUFFER_SPAN);

			if (u_link == NULL) {
				return 0;
			}

			work.data = data + 1;
			work.size = end - 2;
			unscape_text(u_link, &work);
			ret = rndr->cb.autolink(ob, u_link, altype, rndr->opaque);
			rndr_popbuf(rndr, BUFFER_SPAN);
		} else if (rndr->cb.raw_html_tag) {
			ret = rndr->cb.raw_html_tag(ob, &work, rndr->opaque);
		}
	}

	if (!ret) {
		return 0;
	} else {
		return end;
	}
}

static size_t char_autolink_www(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (!rndr->cb.link || rndr->in_link_body) {
		return 0;
	}

	struct buf *link = rndr_newbuf(rndr, BUFFER_SPAN);

	if (link == NULL) {
		return 0;
	}

	size_t link_len;
	size_t rewind;

	if ((link_len = sd_autolink__www(&rewind, link, data, offset, size, 0)) > 0) {
		struct buf *link_url = rndr_newbuf(rndr, BUFFER_SPAN);

		if (link_url == NULL) {
			return 0;
		}

		BUFPUTSL(link_url, "http://");
		bufput(link_url, link->data, link->size);

		ob->size -= rewind;

		if (rndr->cb.normal_text) {
			struct buf *link_text = rndr_newbuf(rndr, BUFFER_SPAN);

			if (link_text == NULL) {
				return 0;
			}

			rndr->cb.normal_text(link_text, link, rndr->opaque);
			rndr->cb.link(ob, link_url, NULL, link_text, rndr->opaque);
			rndr_popbuf(rndr, BUFFER_SPAN);
		} else {
			rndr->cb.link(ob, link_url, NULL, link, rndr->opaque);
		}

		rndr_popbuf(rndr, BUFFER_SPAN);
	}

	rndr_popbuf(rndr, BUFFER_SPAN);

	return link_len;
}

static size_t char_autolink_email(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (!rndr->cb.autolink || rndr->in_link_body) {
		return 0;
	}

	struct buf *link = rndr_newbuf(rndr, BUFFER_SPAN);

	if (link == NULL) {
		return 0;
	}

	size_t link_len;
	size_t rewind;

	if ((link_len = sd_autolink__email(&rewind, link, data, offset, size, 0)) > 0) {
		ob->size -= rewind;
		rndr->cb.autolink(ob, link, MKDA_EMAIL, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_SPAN);

	return link_len;
}

static size_t char_autolink_url(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (!rndr->cb.autolink || rndr->in_link_body) {
		return 0;
	}

	struct buf *link = rndr_newbuf(rndr, BUFFER_SPAN);

	if (link == NULL) {
		return 0;
	}

	size_t link_len;
	size_t rewind;

	if ((link_len = sd_autolink__url(&rewind, link, data, offset, size, 0)) > 0) {
		ob->size -= rewind;
		rndr->cb.autolink(ob, link, MKDA_NORMAL, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_SPAN);

	return link_len;
}

/**
 * '[': parsing a link or an image
 */
static size_t char_link(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	int is_img = (offset && data[-1] == '!');
	size_t i = 1;
	size_t org_work_size = rndr->work_bufs[BUFFER_SPAN].size;
	int ret = 0;

	/* checking whether the correct renderer exists */
	if ((is_img && !rndr->cb.image) || (!is_img && !rndr->cb.link)) {
		goto cleanup;
	}

	int text_has_nl = 0;

	/* looking for the matching closing bracket */
	for (int level = 1; i < size; i++) {
		if (data[i] == '\n') {
			text_has_nl = 1;
		} else if (data[i - 1] == '\\') {
			continue;
		} else if (data[i] == '[') {
			level++;
		} else if (data[i] == ']') {
			level--;

			if (level <= 0) {
				break;
			}
		}
	}

	if (i >= size) {
		goto cleanup;
	}

	size_t txt_e = i;
	i++;

	/* footnote link */
	if (rndr->ext_flags & MKDEXT_FOOTNOTES && data[1] == '^') {
		if (txt_e < 3) {
			goto cleanup;
		}

		struct buf id = {NULL, 0, 0, 0};

		id.data = data + 2;

		id.size = txt_e - 2;

		struct footnote_ref *fr = find_footnote_ref(&rndr->footnotes_found, id.data, id.size);

		/* mark footnote used */
		if (fr && !fr->is_used) {
			if (!add_footnote_ref(&rndr->footnotes_used, fr)) {
				goto cleanup;
			}

			fr->is_used = 1;
			fr->num = rndr->footnotes_used.count;
		}

		/* render */
		if (fr && rndr->cb.footnote_ref) {
			ret = rndr->cb.footnote_ref(ob, fr->num, rndr->opaque);
		}

		goto cleanup;
	}

	/* skip any amount of whitespace or newline */
	/* (this is much more laxist than original markdown syntax) */
	while (i < size && _isspace(data[i])) {
		i++;
	}

	size_t title_b = 0;
	size_t title_e = 0;
	struct buf *link = NULL;
	struct buf *title = NULL;

	/* inline style link */
	if (i < size && data[i] == '(') {
		/* skipping initial whitespace */
		i++;

		while (i < size && _isspace(data[i])) {
			i++;
		}

		size_t link_b = i;

		/* looking for link end: ' " ) */
		while (i < size) {
			if (data[i] == '\\') {
				i += 2;
			} else if (data[i] == ')') {
				break;
			} else if (i >= 1 && _isspace(data[i - 1]) && (data[i] == '\'' || data[i] == '"')) {
				break;
			} else {
				i++;
			}
		}

		if (i >= size) {
			goto cleanup;
		}

		size_t link_e = i;

		/* looking for title end if present */
		if (data[i] == '\'' || data[i] == '"') {
			int qtype = data[i];
			int in_title = 1;
			i++;
			title_b = i;

			while (i < size) {
				if (data[i] == '\\') {
					i += 2;
				} else if (data[i] == qtype) {
					in_title = 0;
					i++;
				} else if ((data[i] == ')') && !in_title) {
					break;
				} else {
					i++;
				}
			}

			if (i >= size) {
				goto cleanup;
			}

			/* skipping whitespaces after title */
			title_e = i - 1;

			while (title_e > title_b && _isspace(data[title_e])) {
				title_e--;
			}

			/* checking for closing quote presence */
			if (data[title_e] != '\'' && data[title_e] != '"') {
				title_e = 0;
				title_b = 0;
				link_e = i;
			}
		}

		/* remove whitespace at the end of the link */
		while (link_e > link_b && _isspace(data[link_e - 1])) {
			link_e--;
		}

		/* remove optional angle brackets around the link */
		if (data[link_b] == '<') {
			link_b++;
		}

		if (data[link_e - 1] == '>') {
			link_e--;
		}

		/* building escaped link and title */
		if (link_e > link_b) {
			link = rndr_newbuf(rndr, BUFFER_SPAN);

			if (link == NULL) {
				goto cleanup;
			}

			bufput(link, data + link_b, link_e - link_b);
		}

		if (title_e > title_b) {
			title = rndr_newbuf(rndr, BUFFER_SPAN);

			if (link == NULL) {
				goto cleanup;
			}

			bufput(title, data + title_b, title_e - title_b);
		}

		i++;
	} else if (i < size && data[i] == '[') {
		/* reference style link */

		/* looking for the id */
		i++;
		size_t link_b = i;

		while (i < size && data[i] != ']') {
			i++;
		}

		if (i >= size) {
			goto cleanup;
		}

		struct buf id = {NULL, 0, 0, 0};

		size_t link_e = i;

		/* finding the link_ref */
		if (link_b == link_e) {
			if (text_has_nl) {
				struct buf *b = rndr_newbuf(rndr, BUFFER_SPAN);

				if (b == NULL) {
					goto cleanup;
				}

				for (size_t j = 1; j < txt_e; j++) {
					if (data[j] != '\n') {
						bufputc(b, data[j]);
					} else if (data[j - 1] != ' ') {
						bufputc(b, ' ');
					}
				}

				id.data = b->data;
				id.size = b->size;
			} else {
				id.data = data + 1;
				id.size = txt_e - 1;
			}
		} else {
			id.data = data + link_b;
			id.size = link_e - link_b;
		}

		struct link_ref *lr = find_link_ref(rndr->refs, id.data, id.size);

		if (!lr) {
			goto cleanup;
		}

		/* keeping link and title from link_ref */
		link = lr->link;
		title = lr->title;
		i++;
	} else {
		/* shortcut reference style link */

		struct buf id = {NULL, 0, 0, 0};

		/* crafting the id */
		if (text_has_nl) {
			struct buf *b = rndr_newbuf(rndr, BUFFER_SPAN);

			if (b == NULL) {
				goto cleanup;
			}

			for (size_t j = 1; j < txt_e; j++) {
				if (data[j] != '\n') {
					bufputc(b, data[j]);
				} else if (data[j - 1] != ' ') {
					bufputc(b, ' ');
				}
			}

			id.data = b->data;
			id.size = b->size;
		} else {
			id.data = data + 1;
			id.size = txt_e - 1;
		}

		/* finding the link_ref */
		struct link_ref *lr = find_link_ref(rndr->refs, id.data, id.size);

		if (!lr) {
			goto cleanup;
		}

		/* keeping link and title from link_ref */
		link = lr->link;
		title = lr->title;

		/* rewinding the whitespace */
		i = txt_e + 1;
	}

	struct buf *content = NULL;

	/* building content: img alt is escaped, link content is parsed */
	if (txt_e > 1) {
		content = rndr_newbuf(rndr, BUFFER_SPAN);

		if (content == NULL) {
			goto cleanup;
		}

		if (is_img) {
			bufput(content, data + 1, txt_e - 1);
		} else {
			/*
			 * disable autolinking when parsing inline the
			 * content of a link
			 */
			rndr->in_link_body = 1;
			parse_inline(content, rndr, data + 1, txt_e - 1);
			rndr->in_link_body = 0;
		}
	}

	struct buf *u_link = NULL;

	if (link) {
		u_link = rndr_newbuf(rndr, BUFFER_SPAN);

		if (u_link == NULL) {
			goto cleanup;
		}

		unscape_text(u_link, link);
	}

	/* calling the relevant rendering function */
	if (is_img) {
		if (ob->size && ob->data[ob->size - 1] == '!') {
			ob->size -= 1;
		}

		ret = rndr->cb.image(ob, u_link, title, content, rndr->opaque);
	} else {
		ret = rndr->cb.link(ob, u_link, title, content, rndr->opaque);
	}

	/* cleanup */
cleanup:
	rndr->work_bufs[BUFFER_SPAN].size = (int)org_work_size;

	return (ret) ? (i) : (0);
}

static size_t char_superscript(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t offset, size_t size)
{
	if (!rndr->cb.superscript) {
		return 0;
	}

	if (size < 2) {
		return 0;
	}

	size_t sup_start;
	size_t sup_len;

	if (data[1] == '(') {
		sup_len = 2;
		sup_start = 2;

		while (sup_len < size && data[sup_len] != ')' && data[sup_len - 1] != '\\') {
			sup_len++;
		}

		if (sup_len == size) {
			return 0;
		}
	} else {
		sup_len = 1;
		sup_start = 1;

		while (sup_len < size && !_isspace(data[sup_len])) {
			sup_len++;
		}
	}

	if (sup_len - sup_start == 0) {
		return (sup_start == 2) ? (3) : (0);
	}

	struct buf *sup = rndr_newbuf(rndr, BUFFER_SPAN);

	if (sup == NULL) {
		return 0;
	}

	parse_inline(sup, rndr, data + sup_start, sup_len - sup_start);
	rndr->cb.superscript(ob, sup, rndr->opaque);
	rndr_popbuf(rndr, BUFFER_SPAN);

	return (sup_start == 2) ? (sup_len + 1) : (sup_len);
}

/* ********************************
 * BLOCK-LEVEL PARSING FUNCTIONS *
 *********************************/

/**
 * returns the line length when it is empty, 0 otherwise
 */
static size_t is_empty(const uint8_t *data, size_t size)
{
	size_t i;

	for (i = 0; i < size && data[i] != '\n'; i++) {
		if (data[i] != ' ') {
			return 0;
		}
	}

	return i + 1;
}

/**
 * returns whether a line is a horizontal rule
 */
static int is_hrule(uint8_t *data, size_t size)
{
	/* skipping initial spaces */
	if (size < 3) {
		return 0;
	}

	size_t i = 0;

	if (data[0] == ' ') {
		i++;

		if (data[1] == ' ') {
			i++;

			if (data[2] == ' ') {
				i++;
			}
		}
	}

	/* looking at the hrule uint8_t */
	if (i + 2 >= size || (data[i] != '*' && data[i] != '-' && data[i] != '_')) {
		return 0;
	}

	uint8_t c = data[i];

	size_t n = 0;

	/* the whole line must be the char or whitespace */
	while (i < size && data[i] != '\n') {
		if (data[i] == c) {
			n++;
		} else if (data[i] != ' ') {
			return 0;
		}

		i++;
	}

	return n >= 3;
}

/**
 * check if a line begins with a code fence; return the
 * width of the code fence
 */
static size_t prefix_codefence(uint8_t *data, size_t size)
{
	/* skipping initial spaces */
	if (size < 3) {
		return 0;
	}

	size_t i = 0;

	if (data[0] == ' ') {
		i++;

		if (data[1] == ' ') {
			i++;

			if (data[2] == ' ') {
				i++;
			}
		}
	}

	/* looking at the hrule uint8_t */
	if (i + 2 >= size || !(data[i] == '~' || data[i] == '`')) {
		return 0;
	}

	uint8_t c = data[i];

	size_t n = 0;

	/* the whole line must be the uint8_t or whitespace */
	while (i < size && data[i] == c) {
		n++;
		i++;
	}

	if (n < 3) {
		return 0;
	}

	return i;
}

/**
 * check if a line is a code fence; return its size if it is
 */
static size_t is_codefence(uint8_t *data, size_t size, struct buf *syntax)
{
	size_t i = prefix_codefence(data, size);

	if (i == 0) {
		return 0;
	}

	while (i < size && data[i] == ' ') {
		i++;
	}

	uint8_t *syn_start = data + i;

	size_t syn_len = 0;

	if (i < size && data[i] == '{') {
		i++;
		syn_start++;

		while (i < size && data[i] != '}' && data[i] != '\n') {
			syn_len++;
			i++;
		}

		if (i == size || data[i] != '}') {
			return 0;
		}

		/*
		 * strip all whitespace at the beginning and the end
		 * of the {} block
		 */
		while (syn_len > 0 && _isspace(syn_start[0])) {
			syn_start++;
			syn_len--;
		}

		while (syn_len > 0 && _isspace(syn_start[syn_len - 1])) {
			syn_len--;
		}

		i++;
	} else {
		while (i < size && !_isspace(data[i])) {
			syn_len++;
			i++;
		}
	}

	if (syntax) {
		syntax->data = syn_start;
		syntax->size = syn_len;
	}

	while (i < size && data[i] != '\n') {
		if (!_isspace(data[i])) {
			return 0;
		}

		i++;
	}

	return i + 1;
}

/**
 * returns whether the line is a hash-prefixed header
 */
static int is_atxheader(struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	if (data[0] != '#') {
		return 0;
	}

	if (rndr->ext_flags & MKDEXT_SPACE_HEADERS) {
		size_t level = 0;

		while (level < size && level < 6 && data[level] == '#') {
			level++;
		}

		if (level < size && data[level] != ' ') {
			return 0;
		}
	}

	return 1;
}

/**
 * returns whether the line is a setext-style hdr underline
 */
static int is_headerline(uint8_t *data, size_t size)
{
	size_t i = 0;

	/* test of level 1 header */
	if (data[i] == '=') {
		for (i = 1; i < size && data[i] == '='; i++) {
			;
		}

		while (i < size && data[i] == ' ') {
			i++;
		}

		return (i >= size || data[i] == '\n') ? (1) : (0);
	}

	/* test of level 2 header */
	if (data[i] == '-') {
		for (i = 1; i < size && data[i] == '-'; i++) {
			;
		}

		while (i < size && data[i] == ' ') {
			i++;
		}

		return (i >= size || data[i] == '\n') ? (2) : (0);
	}

	return 0;
}

static int is_next_headerline(uint8_t *data, size_t size)
{
	size_t i = 0;

	while (i < size && data[i] != '\n') {
		i++;
	}

	if (++i >= size) {
		return 0;
	}

	return is_headerline(data + i, size - i);
}

/**
 * returns blockquote prefix length
 */
static size_t prefix_quote(uint8_t *data, size_t size)
{
	size_t i = 0;

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == '>') {
		if (i + 1 < size && data[i + 1] == ' ') {
			return i + 2;
		}

		return i + 1;
	}

	return 0;
}

/**
 * returns prefix length for block code
 */
static size_t prefix_code(uint8_t *data, size_t size)
{
	if (size > 3 && data[0] == ' ' && data[1] == ' ' && data[2] == ' ' && data[3] == ' ') {
		return 4;
	}

	return 0;
}

/**
 * returns ordered list item prefix
 */
static size_t prefix_oli(uint8_t *data, size_t size)
{
	size_t i = 0;

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i >= size || data[i] < '0' || data[i] > '9') {
		return 0;
	}

	while (i < size && data[i] >= '0' && data[i] <= '9') {
		i++;
	}

	if (i + 1 >= size || data[i] != '.' || data[i + 1] != ' ') {
		return 0;
	}

	if (is_next_headerline(data + i, size - i)) {
		return 0;
	}

	return i + 2;
}

/**
 * returns ordered list item prefix
 */
static size_t prefix_uli(uint8_t *data, size_t size)
{
	size_t i = 0;

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i < size && data[i] == ' ') {
		i++;
	}

	if (i + 1 >= size || (data[i] != '*' && data[i] != '+' && data[i] != '-') || data[i + 1] != ' ') {
		return 0;
	}

	if (is_next_headerline(data + i, size - i)) {
		return 0;
	}

	return i + 2;
}

/**
 * parsing of one block, returning next uint8_t to parse
 */
static void parse_block(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size);

/**
 * handles parsing of a blockquote fragment
 */
static size_t parse_blockquote(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	struct buf *out_ = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (out_ == NULL) {
		return 0;
	}

	size_t beg = 0;
	size_t end = 0;
	size_t work_size = 0;
	uint8_t *work_data = NULL;

	while (beg < size) {
		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++) {
			;
		}

		size_t pre = prefix_quote(data + beg, end - beg);

		if (pre) {
			/* skipping prefix */
			beg += pre;
		} else if (is_empty(data + beg, end - beg) && (end >= size || (prefix_quote(data + end, size - end) == 0 && !is_empty(data + end, size - end)))) {
			/* empty line followed by non-quote line */
			break;
		}

		if (beg < end) { /* copy into the in-place working buffer */
			/* bufput(work, data + beg, end - beg); */
			if (!work_data) {
				work_data = data + beg;
			} else if (data + beg != work_data + work_size) {
				memmove(work_data + work_size, data + beg, end - beg);
			}

			work_size += end - beg;
		}

		beg = end;
	}

	parse_block(out_, rndr, work_data, work_size);

	if (rndr->cb.blockquote) {
		rndr->cb.blockquote(ob, out_, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_BLOCK);

	return end;
}

static size_t parse_htmlblock(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, int do_render);

/**
 * handles parsing of a regular paragraph
 */
static size_t parse_paragraph(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	size_t i = 0;
	size_t end = 0;
	int level = 0;

	while (i < size) {
		for (end = i + 1; end < size && data[end - 1] != '\n'; end++) {
			/* empty */
			;
		}

		if (is_empty(data + i, size - i)) {
			break;
		}

		if ((level = is_headerline(data + i, size - i)) != 0) {
			break;
		}

		if (is_atxheader(rndr, data + i, size - i) || is_hrule(data + i, size - i) || prefix_quote(data + i, size - i)) {
			end = i;

			break;
		}

		/*
		 * Early termination of a paragraph with the same logic
		 * as Markdown 1.0.0. If this logic is applied, the
		 * Markdown 1.0.3 test suite won't pass cleanly
		 *
		 * :: If the first character in a new line is not a letter,
		 * let's check to see if there's some kind of block starting
		 * here
		 */
		if ((rndr->ext_flags & MKDEXT_LAX_SPACING) && !isalnum(data[i])) {
			if (prefix_oli(data + i, size - i) || prefix_uli(data + i, size - i)) {
				end = i;

				break;
			}

			/* see if an html block starts here */
			if (data[i] == '<' && rndr->cb.blockhtml && parse_htmlblock(ob, rndr, data + i, size - i, 0)) {
				end = i;

				break;
			}

			/* see if a code fence starts here */
			if ((rndr->ext_flags & MKDEXT_FENCED_CODE) != 0 && is_codefence(data + i, size - i, NULL) != 0) {
				end = i;

				break;
			}
		}

		i = end;
	}

	struct buf work = {data, 0, 0, 0};

	work.size = i;

	while (work.size && data[work.size - 1] == '\n') {
		work.size--;
	}

	if (!level) {
		struct buf *tmp = rndr_newbuf(rndr, BUFFER_BLOCK);

		if (tmp == NULL) {
			return 0;
		}

		parse_inline(tmp, rndr, work.data, work.size);

		if (rndr->cb.paragraph) {
			rndr->cb.paragraph(ob, tmp, rndr->opaque);
		}

		rndr_popbuf(rndr, BUFFER_BLOCK);
	} else {
		if (work.size) {
			i = work.size;
			work.size -= 1;

			while (work.size && data[work.size] != '\n') {
				work.size -= 1;
			}

			size_t beg = work.size + 1;

			while (work.size && data[work.size - 1] == '\n') {
				work.size -= 1;
			}

			if (work.size > 0) {
				struct buf *tmp = rndr_newbuf(rndr, BUFFER_BLOCK);

				if (tmp == NULL) {
					return 0;
				}

				parse_inline(tmp, rndr, work.data, work.size);

				if (rndr->cb.paragraph) {
					rndr->cb.paragraph(ob, tmp, rndr->opaque);
				}

				rndr_popbuf(rndr, BUFFER_BLOCK);
				work.data += beg;
				work.size = i - beg;
			} else {
				work.size = i;
			}
		}

		struct buf *header_work = rndr_newbuf(rndr, BUFFER_SPAN);

		if (header_work == NULL) {
			return 0;
		}

		parse_inline(header_work, rndr, work.data, work.size);

		if (rndr->cb.header) {
			rndr->cb.header(ob, header_work, (int)level, rndr->opaque);
		}

		rndr_popbuf(rndr, BUFFER_SPAN);
	}

	return end;
}

/**
 * handles parsing of a block-level code fragment
 */
static size_t parse_fencedcode(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	struct buf lang = {NULL, 0, 0, 0};
	size_t beg = is_codefence(data, size, &lang);

	if (beg == 0) {
		return 0;
	}

	struct buf *work = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (work == NULL) {
		return 0;
	}

	size_t end;

	while (beg < size) {
		struct buf fence_trail = {NULL, 0, 0, 0};

		size_t fence_end = is_codefence(data + beg, size - beg, &fence_trail);

		if (fence_end != 0 && fence_trail.size == 0) {
			beg += fence_end;

			break;
		}

		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++) {
			;
		}

		if (beg < end) {
			/*
			 * verbatim copy to the working buffer,
			 * escaping entities
			 */
			if (is_empty(data + beg, end - beg)) {
				bufputc(work, '\n');
			} else {
				bufput(work, data + beg, end - beg);
			}
		}

		beg = end;
	}

	if (work->size && work->data[work->size - 1] != '\n') {
		bufputc(work, '\n');
	}

	if (rndr->cb.blockcode) {
		rndr->cb.blockcode(ob, work, (lang.size) ? (&lang) : (NULL), rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_BLOCK);

	return beg;
}

static size_t parse_blockcode(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	struct buf *work = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (work == NULL) {
		return 0;
	}

	size_t beg = 0;
	size_t end;

	while (beg < size) {
		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++) {
		}

		size_t pre = prefix_code(data + beg, end - beg);

		if (pre) {
			/* skipping prefix */
			beg += pre;
		} else if (!is_empty(data + beg, end - beg)) {
			/* non-empty non-prefixed line breaks the pre */
			break;
		}

		if (beg < end) {
			/*
			 * verbatim copy to the working buffer,
			 * escaping entities
			 */
			if (is_empty(data + beg, end - beg)) {
				bufputc(work, '\n');
			} else {
				bufput(work, data + beg, end - beg);
			}
		}

		beg = end;
	}

	while (work->size && work->data[work->size - 1] == '\n') {
		work->size -= 1;
	}

	bufputc(work, '\n');

	if (rndr->cb.blockcode) {
		rndr->cb.blockcode(ob, work, NULL, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_BLOCK);

	return beg;
}

/**
 * parsing of a single list item
 */
/* assuming initial prefix is already removed */
static size_t parse_listitem(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, int *flags)
{
	size_t orgpre = 0;

	/* keeping track of the first indentation prefix */
	while (orgpre < 3 && orgpre < size && data[orgpre] == ' ') {
		orgpre++;
	}

	size_t beg = prefix_uli(data, size);

	if (!beg) {
		beg = prefix_oli(data, size);
	}

	if (!beg) {
		return 0;
	}

	/* skipping to the beginning of the following line */
	size_t end = beg;

	while (end < size && data[end - 1] != '\n') {
		end++;
	}

	/* getting working buffers */
	struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

	if (work == NULL) {
		return 0;
	}

	struct buf *inter = rndr_newbuf(rndr, BUFFER_SPAN);

	if (inter == NULL) {
		return 0;
	}

	/* putting the first line into the working buffer */
	bufput(work, data + beg, end - beg);
	beg = end;

	size_t sublist = 0;
	int in_empty = 0;
	int has_inside_empty = 0;
	int in_fence = 0;

	/* process the following lines */
	while (beg < size) {
		end++;

		while (end < size && data[end - 1] != '\n') {
			end++;
		}

		/* process an empty line */
		if (is_empty(data + beg, end - beg)) {
			in_empty = 1;
			beg = end;

			continue;
		}

		/* calculating the indentation */
		size_t i = 0;

		while (i < 4 && beg + i < end && data[beg + i] == ' ') {
			i++;
		}

		size_t pre = i;

		if (rndr->ext_flags & MKDEXT_FENCED_CODE) {
			if (is_codefence(data + beg + i, end - beg - i, NULL) != 0) {
				in_fence = !in_fence;
			}
		}

		size_t has_next_uli = 0;
		size_t has_next_oli = 0;

		/*
		 * Only check for new list items if we are **not** inside
		 * a fenced code block
		 */
		if (!in_fence) {
			has_next_uli = prefix_uli(data + beg + i, end - beg - i);
			has_next_oli = prefix_oli(data + beg + i, end - beg - i);
		}

		/* checking for ul/ol switch */
		if (in_empty && (((*flags & MKD_LIST_ORDERED) && has_next_uli) || (!(*flags & MKD_LIST_ORDERED) && has_next_oli))) {
			*flags |= MKD_LI_END;

			/* the following item must have same list type */
			break;
		}

		/* checking for a new item */
		if ((has_next_uli && !is_hrule(data + beg + i, end - beg - i)) || has_next_oli) {
			if (in_empty) {
				has_inside_empty = 1;
			}

			if (pre == orgpre) { /* the following item must have */
				/* the same indentation */
				break;
			}

			if (!sublist) {
				sublist = work->size;
			}
		} else if (in_empty && pre == 0) {
			/*
			 * joining only indented stuff after empty lines;
			 * note that now we only require 1 space of indentation
			 * to continue a list
			 */
			*flags |= MKD_LI_END;

			break;
		} else if (in_empty) {
			bufputc(work, '\n');
			has_inside_empty = 1;
		}

		in_empty = 0;

		/* adding the line without prefix into the working buffer */
		bufput(work, data + beg + i, end - beg - i);
		beg = end;
	}

	/* render of li contents */
	if (has_inside_empty) {
		*flags |= MKD_LI_BLOCK;
	}

	if (*flags & MKD_LI_BLOCK) {
		/* intermediate render of block li */
		if (sublist && sublist < work->size) {
			parse_block(inter, rndr, work->data, sublist);
			parse_block(inter, rndr, work->data + sublist, work->size - sublist);
		} else {
			parse_block(inter, rndr, work->data, work->size);
		}
	} else {
		/* intermediate render of inline li */
		if (sublist && sublist < work->size) {
			parse_inline(inter, rndr, work->data, sublist);
			parse_block(inter, rndr, work->data + sublist, work->size - sublist);
		} else {
			parse_inline(inter, rndr, work->data, work->size);
		}
	}

	/* render of li itself */
	if (rndr->cb.listitem) {
		rndr->cb.listitem(ob, inter, *flags, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_SPAN);
	rndr_popbuf(rndr, BUFFER_SPAN);

	return beg;
}

/**
 * parsing ordered or unordered list block
 */
static size_t parse_list(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, int flags)
{
	struct buf *work = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (work == NULL) {
		return 0;
	}

	size_t i = 0;

	while (i < size) {
		size_t j = parse_listitem(work, rndr, data + i, size - i, &flags);
		i += j;

		if (!j || (flags & MKD_LI_END)) {
			break;
		}
	}

	if (rndr->cb.list) {
		rndr->cb.list(ob, work, flags, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_BLOCK);

	return i;
}

/**
 * parsing of atx-style headers
 */
static size_t parse_atxheader(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	size_t level = 0;

	while (level < size && level < 6 && data[level] == '#') {
		level++;
	}

	size_t i;

	for (i = level; i < size && data[i] == ' '; i++) {
		;
	}

	size_t end;

	for (end = i; end < size && data[end] != '\n'; end++) {
		;
	}

	size_t skip = end;

	while (end && data[end - 1] == '#') {
		end--;
	}

	while (end && data[end - 1] == ' ') {
		end--;
	}

	if (end > i) {
		struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

		if (work == NULL) {
			return 0;
		}

		parse_inline(work, rndr, data + i, end - i);

		if (rndr->cb.header) {
			rndr->cb.header(ob, work, (int)level, rndr->opaque);
		}

		rndr_popbuf(rndr, BUFFER_SPAN);
	}

	return skip;
}

/**
 * parse a single footnote definition
 */
static void parse_footnote_def(struct buf *ob, struct sd_markdown *rndr, unsigned int num, uint8_t *data, size_t size)
{
	struct buf *work = rndr_newbuf(rndr, BUFFER_SPAN);

	if (work == NULL) {
		return;
	}

	parse_block(work, rndr, data, size);

	if (rndr->cb.footnote_def) {
		rndr->cb.footnote_def(ob, work, num, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_SPAN);
}

/**
 * render the contents of the footnotes
 */
static void parse_footnote_list(struct buf *ob, struct sd_markdown *rndr, struct footnote_list *footnotes)
{
	if (footnotes->count == 0) {
		return;
	}

	struct buf *work = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (work == NULL) {
		return;
	}

	struct footnote_item *item = footnotes->head;

	while (item) {
		struct footnote_ref *ref_ = item->ref_;
		parse_footnote_def(work, rndr, ref_->num, ref_->contents->data, ref_->contents->size);
		item = item->next;
	}

	if (rndr->cb.footnotes) {
		rndr->cb.footnotes(ob, work, rndr->opaque);
	}

	rndr_popbuf(rndr, BUFFER_BLOCK);
}

/**
 * checking end of HTML block : </tag>[ \t]*\n[ \t*]\n
 */
/* returns the length on match, 0 otherwise */
static size_t htmlblock_end_tag(const char *tag, size_t tag_len, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	/* checking if tag is a match */
	if (tag_len + 3 >= size || strncasecmp((char *)data + 2, tag, tag_len) != 0 || data[tag_len + 2] != '>') {
		return 0;
	}

	/* checking white lines */
	size_t i = tag_len + 3;
	size_t w = 0;

	if (i < size && (w = is_empty(data + i, size - i)) == 0) {
		/* non-blank after tag */
		return 0;
	}

	i += w;
	w = 0;

	if (i < size) {
		w = is_empty(data + i, size - i);
	}

	return i + w;
}

static size_t htmlblock_end(const char *curtag, struct sd_markdown *rndr, uint8_t *data, size_t size, int start_of_line)
{
	size_t tag_size = strlen(curtag);
	size_t i = 1;
	int block_lines = 0;

	while (i < size) {
		i++;

		while (i < size && !(data[i - 1] == '<' && data[i] == '/')) {
			if (data[i] == '\n') {
				block_lines++;
			}

			i++;
		}

		/*
		 * If we are only looking for unindented tags, skip the tag
		 * if it doesn't follow a newline.
		 *
		 * The only exception to this is if the tag is still on the
		 * initial line; in that case it still counts as a closing
		 * tag
		 */
		if (start_of_line && block_lines > 0 && data[i - 2] != '\n') {
			continue;
		}

		if (i + 2 + tag_size >= size) {
			break;
		}

		size_t end_tag = htmlblock_end_tag(curtag, tag_size, rndr, data + i - 1, size - i + 1);

		if (end_tag) {
			return i + end_tag - 1;
		}
	}

	return 0;
}

/**
 * parsing of inline HTML block
 */
static size_t parse_htmlblock(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, int do_render)
{
	/* identification of the opening tag */
	if (size < 2 || data[0] != '<') {
		return 0;
	}

	size_t i = 1;

	while (i < size && data[i] != '>' && data[i] != ' ') {
		i++;
	}

	const char *curtag = NULL;

	if (i < size) {
		curtag = find_block_tag((char *)data + 1, (int)i - 1);
	}

	struct buf work = {data, 0, 0, 0};

	/* handling of special cases */
	if (!curtag) {
		size_t j = 0;

		/* HTML comment, laxist form */
		if (size > 5 && data[1] == '!' && data[2] == '-' && data[3] == '-') {
			i = 5;

			while (i < size && !(data[i - 2] == '-' && data[i - 1] == '-' && data[i] == '>')) {
				i++;
			}

			i++;

			if (i < size) {
				j = is_empty(data + i, size - i);
			}

			if (j) {
				work.size = i + j;

				if (do_render && rndr->cb.blockhtml) {
					rndr->cb.blockhtml(ob, &work, rndr->opaque);
				}

				return work.size;
			}
		}

		/* HR, which is the only self-closing block tag considered */
		if (size > 4 && (data[1] == 'h' || data[1] == 'H') && (data[2] == 'r' || data[2] == 'R')) {
			i = 3;

			while (i < size && data[i] != '>') {
				i++;
			}

			if (i + 1 < size) {
				i++;
				j = is_empty(data + i, size - i);

				if (j) {
					work.size = i + j;

					if (do_render && rndr->cb.blockhtml) {
						rndr->cb.blockhtml(ob, &work, rndr->opaque);
					}

					return work.size;
				}
			}
		}

		/* no special case recognised */
		return 0;
	}

	/* looking for an unindented matching closing tag */
	/* followed by a blank line */
	size_t tag_end = htmlblock_end(curtag, rndr, data, size, 1);

	/* if not found, trying a second pass looking for indented match */
	/* but not if tag is "ins" or "del" (following original Markdown.pl) */
	if (!tag_end && strcmp(curtag, "ins") != 0 && strcmp(curtag, "del") != 0) {
		tag_end = htmlblock_end(curtag, rndr, data, size, 0);
	}

	if (!tag_end) {
		return 0;
	}

	/* the end of the block has been found */
	work.size = tag_end;

	if (do_render && rndr->cb.blockhtml) {
		rndr->cb.blockhtml(ob, &work, rndr->opaque);
	}

	return tag_end;
}

static void parse_table_row(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, size_t columns, int *col_data, int header_flag)
{
	if (!rndr->cb.table_cell || !rndr->cb.table_row) {
		return;
	}

	struct buf *row_work = rndr_newbuf(rndr, BUFFER_SPAN);

	if (row_work == NULL) {
		return;
	}

	size_t i = 0;

	if (i < size && data[i] == '|') {
		i++;
	}

	size_t col;

	for (col = 0; col < columns && i < size; ++col) {
		struct buf *cell_work = rndr_newbuf(rndr, BUFFER_SPAN);

		if (cell_work == NULL) {
			return;
		}

		while (i < size && _isspace(data[i])) {
			i++;
		}

		size_t cell_start = i;

		while (i < size && ((data[i] != '|') || ((i > 0) && (data[i - 1] == '\\')))) {
			i++;
		}

		size_t cell_end = i - 1;

		while (cell_end > cell_start && _isspace(data[cell_end])) {
			cell_end--;
		}

		parse_inline(cell_work, rndr, data + cell_start, 1 + cell_end - cell_start);
		rndr->cb.table_cell(row_work, cell_work, col_data[col] | header_flag, rndr->opaque);

		rndr_popbuf(rndr, BUFFER_SPAN);
		i++;
	}

	for (; col < columns; ++col) {
		struct buf empty_cell = {NULL, 0, 0, 0};
		rndr->cb.table_cell(row_work, &empty_cell, col_data[col] | header_flag, rndr->opaque);
	}

	rndr->cb.table_row(ob, row_work, rndr->opaque);

	rndr_popbuf(rndr, BUFFER_SPAN);
}

static size_t parse_table_header(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size, size_t *columns, int **column_data)
{
	int pipes = 0;
	size_t i = 0;

	while (i < size && data[i] != '\n') {
		if (data[i++] == '|') {
			pipes++;
		}
	}

	if (i == size || pipes == 0) {
		return 0;
	}

	size_t header_end = i;

	while (header_end > 0 && _isspace(data[header_end - 1])) {
		header_end--;
	}

	if (data[0] == '|') {
		pipes--;
	}

	if (header_end && data[header_end - 1] == '|') {
		pipes--;
	}

	*columns = pipes + 1;
	*column_data = calloc(*columns, sizeof(int));

	/* Parse the header underline */
	i++;

	if (i < size && data[i] == '|') {
		i++;
	}

	size_t under_end = i;

	while (under_end < size && data[under_end] != '\n') {
		under_end++;
	}

	size_t col;

	for (col = 0; col < *columns && i < under_end; ++col) {
		while (i < under_end && data[i] == ' ') {
			i++;
		}

		size_t dashes = 0;

		if (data[i] == ':') {
			i++;
			(*column_data)[col] |= MKD_TABLE_ALIGN_L;
			dashes++;
		}

		while (i < under_end && data[i] == '-') {
			i++;
			dashes++;
		}

		if (i < under_end && data[i] == ':') {
			i++;
			(*column_data)[col] |= MKD_TABLE_ALIGN_R;
			dashes++;
		}

		while (i < under_end && data[i] == ' ') {
			i++;
		}

		if (i < under_end && data[i] != '|') {
			break;
		}

		if (dashes < 3) {
			break;
		}

		i++;
	}

	if (col < *columns) {
		return 0;
	}

	parse_table_row(ob, rndr, data, header_end, *columns, *column_data, MKD_TABLE_HEADER);

	return under_end + 1;
}

static size_t parse_table(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	struct buf *header_work = rndr_newbuf(rndr, BUFFER_SPAN);

	if (header_work == NULL) {
		return 0;
	}

	struct buf *body_work = rndr_newbuf(rndr, BUFFER_BLOCK);

	if (body_work == NULL) {
		return 0;
	}

	size_t columns;
	int *col_data = NULL;
	size_t i = parse_table_header(header_work, rndr, data, size, &columns, &col_data);

	if (i > 0) {
		while (i < size) {
			int pipes = 0;
			size_t row_start = i;

			while (i < size && data[i] != '\n') {
				if (data[i++] == '|') {
					pipes++;
				}
			}

			if (pipes == 0 || i == size) {
				i = row_start;

				break;
			}

			parse_table_row(body_work, rndr, data + row_start, i - row_start, columns, col_data, MKD_TABLE_NONE);

			i++;
		}

		if (rndr->cb.table) {
			rndr->cb.table(ob, header_work, body_work, rndr->opaque);
		}
	}

	free(col_data);
	rndr_popbuf(rndr, BUFFER_SPAN);
	rndr_popbuf(rndr, BUFFER_BLOCK);

	return i;
}

/**
 * parsing of one block, returning next uint8_t to parse
 */
static void parse_block(struct buf *ob, struct sd_markdown *rndr, uint8_t *data, size_t size)
{
	if (rndr->work_bufs[BUFFER_SPAN].size + rndr->work_bufs[BUFFER_BLOCK].size > rndr->max_nesting) {
		return;
	}

	size_t i;
	size_t beg = 0;

	while (beg < size) {
		uint8_t *txt_data = data + beg;
		size_t end = size - beg;

		if (is_atxheader(rndr, txt_data, end)) {
			beg += parse_atxheader(ob, rndr, txt_data, end);
		} else if (data[beg] == '<' && rndr->cb.blockhtml && (i = parse_htmlblock(ob, rndr, txt_data, end, 1)) != 0) {
			beg += i;
		} else if ((i = is_empty(txt_data, end)) != 0) {
			beg += i;
		} else if (is_hrule(txt_data, end)) {
			if (rndr->cb.hrule) {
				rndr->cb.hrule(ob, rndr->opaque);
			}

			while (beg < size && data[beg] != '\n') {
				beg++;
			}

			beg++;
		} else if ((rndr->ext_flags & MKDEXT_FENCED_CODE) != 0 && (i = parse_fencedcode(ob, rndr, txt_data, end)) != 0) {
			beg += i;
		} else if ((rndr->ext_flags & MKDEXT_TABLES) != 0 && (i = parse_table(ob, rndr, txt_data, end)) != 0) {
			beg += i;
		} else if (prefix_quote(txt_data, end)) {
			beg += parse_blockquote(ob, rndr, txt_data, end);
		} else if (prefix_code(txt_data, end)) {
			beg += parse_blockcode(ob, rndr, txt_data, end);
		} else if (prefix_uli(txt_data, end)) {
			beg += parse_list(ob, rndr, txt_data, end, 0);
		} else if (prefix_oli(txt_data, end)) {
			beg += parse_list(ob, rndr, txt_data, end, MKD_LIST_ORDERED);
		} else {
			beg += parse_paragraph(ob, rndr, txt_data, end);
		}
	}
}

/* ********************
 * REFERENCE PARSING *
 *********************/

/**
 * returns whether a line is a footnote definition or not
 */
static int is_footnote(const uint8_t *data, size_t beg, size_t end, size_t *last, struct footnote_list *list)
{
	/* up to 3 optional leading spaces */
	if (beg + 3 >= end) {
		return 0;
	}

	size_t i = 0;

	if (data[beg] == ' ') {
		i = 1;

		if (data[beg + 1] == ' ') {
			i = 2;

			if (data[beg + 2] == ' ') {
				i = 3;

				if (data[beg + 3] == ' ') {
					return 0;
				}
			}
		}
	}

	i += beg;

	/* id part: caret followed by anything between brackets */
	if (data[i] != '[') {
		return 0;
	}

	i++;

	if (i >= end || data[i] != '^') {
		return 0;
	}

	i++;
	size_t id_offset = i;

	while (i < end && data[i] != '\n' && data[i] != '\r' && data[i] != ']') {
		i++;
	}

	if (i >= end || data[i] != ']') {
		return 0;
	}

	size_t id_end = i;

	/* spacer: colon (space | tab)* newline? (space | tab)* */
	i++;

	if (i >= end || data[i] != ':') {
		return 0;
	}

	i++;

	while (i < end && data[i] == ' ') {
		i++;
	}

	if (i < end && (data[i] == '\n' || data[i] == '\r')) {
		i++;

		if (i < end && data[i] == '\n' && data[i - 1] == '\r') {
			i++;
		}
	}

	while (i < end && data[i] == ' ') {
		i++;
	}

	if (i >= end || data[i] == '\n' || data[i] == '\r') {
		return 0;
	}

	/* getting content buffer */
	struct buf *contents = bufnew(64);

	if (contents == NULL) {
		return 0;
	}

	size_t start = i;

	int in_empty = 0;

	/* process lines similiar to a list item */
	while (i < end) {
		while (i < end && data[i] != '\n' && data[i] != '\r') {
			i++;
		}

		/* process an empty line */
		if (is_empty(data + start, i - start)) {
			in_empty = 1;

			if (i < end && (data[i] == '\n' || data[i] == '\r')) {
				i++;

				if (i < end && data[i] == '\n' && data[i - 1] == '\r') {
					i++;
				}
			}

			start = i;

			continue;
		}

		/* calculating the indentation */
		size_t ind = 0;

		while (ind < 4 && start + ind < end && data[start + ind] == ' ') {
			ind++;
		}

		/*
		 * joining only indented stuff after empty lines;
		 * note that now we only require 1 space of indentation
		 * to continue, just like lists
		 */
		if (in_empty && ind == 0) {
			break;
		} else if (in_empty) {
			bufputc(contents, '\n');
		}

		in_empty = 0;

		/* adding the line into the content buffer */
		bufput(contents, data + start + ind, i - start - ind);

		/* add carriage return */
		if (i < end) {
			bufput(contents, "\n", 1);

			if (i < end && (data[i] == '\n' || data[i] == '\r')) {
				i++;

				if (i < end && data[i] == '\n' && data[i - 1] == '\r') {
					i++;
				}
			}
		}

		start = i;
	}

	if (last) {
		*last = start;
	}

	if (list) {
		struct footnote_ref *ref_ = create_footnote_ref(list, data + id_offset, id_end - id_offset);

		if (!ref_) {
			return 0;
		}

		if (!add_footnote_ref(list, ref_)) {
			free_footnote_ref(ref_);

			return 0;
		}

		ref_->contents = contents;
	}

	return 1;
}

/**
 * returns whether a line is a reference or not
 */
static int is_ref(const uint8_t *data, size_t beg, size_t end, size_t *last, struct link_ref **refs)
{
	/* int n; */

	/* up to 3 optional leading spaces */
	if (beg + 3 >= end) {
		return 0;
	}

	size_t i = 0;

	if (data[beg] == ' ') {
		i = 1;

		if (data[beg + 1] == ' ') {
			i = 2;

			if (data[beg + 2] == ' ') {
				i = 3;

				if (data[beg + 3] == ' ') {
					return 0;
				}
			}
		}
	}

	i += beg;

	/* id part: anything but a newline between brackets */
	if (data[i] != '[') {
		return 0;
	}

	i++;
	size_t id_offset = i;

	while (i < end && data[i] != '\n' && data[i] != '\r' && data[i] != ']') {
		i++;
	}

	if (i >= end || data[i] != ']') {
		return 0;
	}

	size_t id_end = i;

	/* spacer: colon (space | tab)* newline? (space | tab)* */
	i++;

	if (i >= end || data[i] != ':') {
		return 0;
	}

	i++;

	while (i < end && data[i] == ' ') {
		i++;
	}

	if (i < end && (data[i] == '\n' || data[i] == '\r')) {
		i++;

		if (i < end && data[i] == '\r' && data[i - 1] == '\n') {
			i++;
		}
	}

	while (i < end && data[i] == ' ') {
		i++;
	}

	if (i >= end) {
		return 0;
	}

	/* link: whitespace-free sequence, optionally between angle brackets */
	if (data[i] == '<') {
		i++;
	}

	size_t link_offset = i;

	while (i < end && data[i] != ' ' && data[i] != '\n' && data[i] != '\r') {
		i++;
	}

	size_t link_end;

	if (data[i - 1] == '>') {
		link_end = i - 1;
	} else {
		link_end = i;
	}

	/* optional spacer: (space | tab)* (newline | '\'' | '"' | '(' ) */
	while (i < end && data[i] == ' ') {
		i++;
	}

	if (i < end && data[i] != '\n' && data[i] != '\r' && data[i] != '\'' && data[i] != '"' && data[i] != '(') {
		return 0;
	}

	size_t line_end = 0;

	/* computing end-of-line */
	if (i >= end || data[i] == '\r' || data[i] == '\n') {
		line_end = i;
	}

	if (i + 1 < end && data[i] == '\n' && data[i + 1] == '\r') {
		line_end = i + 1;
	}

	/* optional (space|tab)* spacer after a newline */
	if (line_end) {
		i = line_end + 1;

		while (i < end && data[i] == ' ') {
			i++;
		}
	}

	/*
	 * optional title: any non-newline sequence enclosed in '"()
	 * alone on its line
	 */
	size_t title_end = 0;
	size_t title_offset = 0;

	if (i + 1 < end && (data[i] == '\'' || data[i] == '"' || data[i] == '(')) {
		i++;
		title_offset = i;

		/* looking for EOL */
		while (i < end && data[i] != '\n' && data[i] != '\r') {
			i++;
		}

		if (i + 1 < end && data[i] == '\n' && data[i + 1] == '\r') {
			title_end = i + 1;
		} else {
			title_end = i;
		}

		/* stepping back */
		i -= 1;

		while (i > title_offset && data[i] == ' ') {
			i -= 1;
		}

		if (i > title_offset && (data[i] == '\'' || data[i] == '"' || data[i] == ')')) {
			line_end = title_end;
			title_end = i;
		}
	}

	if (!line_end || link_end == link_offset) {
		/* garbage after the link empty link */
		return 0;
	}

	/* a valid ref has been found, filling-in return structures */
	if (last) {
		*last = line_end;
	}

	if (refs) {
		struct link_ref *ref_ = add_link_ref(refs, data + id_offset, id_end - id_offset);

		if (!ref_) {
			return 0;
		}

		ref_->link = bufnew(link_end - link_offset);

		if (ref_->link == NULL) {
			return 0;
		}

		bufput(ref_->link, data + link_offset, link_end - link_offset);

		if (title_end > title_offset) {
			ref_->title = bufnew(title_end - title_offset);

			if (ref_->title == NULL) {
				return 0;
			}

			bufput(ref_->title, data + title_offset, title_end - title_offset);
		}
	}

	return 1;
}

static void expand_tabs(struct buf *ob, const uint8_t *line, size_t size)
{
	size_t i = 0;
	size_t tab = 0;

	while (i < size) {
		size_t org = i;

		while (i < size && line[i] != '\t') {
			i++;
			tab++;
		}

		if (i > org) {
			bufput(ob, line + org, i - org);
		}

		if (i >= size) {
			break;
		}

		do {
			bufputc(ob, ' ');
			tab++;
		} while (tab % 4);

		i++;
	}
}

/* *********************
 * EXPORTED FUNCTIONS *
 **********************/

struct sd_markdown *sd_markdown_new(unsigned int extensions, size_t max_nesting, const struct sd_callbacks *callbacks, void *opaque)
{
	assert(max_nesting > 0 && callbacks);

	struct sd_markdown *md = malloc(sizeof(struct sd_markdown));

	if (!md) {
		return NULL;
	}

	memcpy(&md->cb, callbacks, sizeof(struct sd_callbacks));

	if (stack_init(&md->work_bufs[BUFFER_BLOCK], 4) < 0) {
		stack_free(&md->work_bufs[BUFFER_BLOCK]);
		free(md);

		return NULL;
	}

	if (stack_init(&md->work_bufs[BUFFER_SPAN], 8) < 0) {
		stack_free(&md->work_bufs[BUFFER_BLOCK]);
		stack_free(&md->work_bufs[BUFFER_SPAN]);
		free(md);

		return NULL;
	}

	memset(md->active_char, 0x00, 256);

	if (md->cb.emphasis || md->cb.double_emphasis || md->cb.triple_emphasis) {
		md->active_char['*'] = MD_CHAR_EMPHASIS;
		md->active_char['_'] = MD_CHAR_EMPHASIS;

		if (extensions & MKDEXT_STRIKETHROUGH) {
			md->active_char['~'] = MD_CHAR_EMPHASIS;
		}

		if (extensions & MKDEXT_INS) {
			md->active_char['+'] = MD_CHAR_EMPHASIS;
		}
	}

	if (md->cb.codespan) {
		md->active_char['`'] = MD_CHAR_CODESPAN;
	}

	if (md->cb.linebreak) {
		md->active_char['\n'] = MD_CHAR_LINEBREAK;
	}

	if (md->cb.image || md->cb.link) {
		md->active_char['['] = MD_CHAR_LINK;
	}

	md->active_char['<'] = MD_CHAR_LANGLE;
	md->active_char['\\'] = MD_CHAR_ESCAPE;
	md->active_char['&'] = MD_CHAR_ENTITITY;

	if (extensions & MKDEXT_AUTOLINK) {
		md->active_char[':'] = MD_CHAR_AUTOLINK_URL;
		md->active_char['@'] = MD_CHAR_AUTOLINK_EMAIL;
		md->active_char['w'] = MD_CHAR_AUTOLINK_WWW;
	}

	if (extensions & MKDEXT_SUPERSCRIPT) {
		md->active_char['^'] = MD_CHAR_SUPERSCRIPT;
	}

	/* Extension data */
	md->ext_flags = extensions;
	md->opaque = opaque;
	md->max_nesting = max_nesting;
	md->in_link_body = 0;

	return md;
}

void sd_markdown_render(struct buf *ob, const uint8_t *document, size_t doc_size, struct sd_markdown *md)
{
#define MARKDOWN_GROW(x) ((x) + ((x) >> 1))
	static const char UTF8_BOM[] = {0xEF, 0xBB, 0xBF};

	struct buf *text = bufnew(64);

	if (!text) {
		return;
	}

	/* Preallocate enough space for our buffer to avoid expanding while copying */
	if (bufgrow(text, doc_size) != BUF_OK) {
		bufrelease(text);

		return;
	}

	/* reset the references table */
	memset(&md->refs, 0x00, REF_TABLE_SIZE * sizeof(void *));

	int footnotes_enabled = md->ext_flags & MKDEXT_FOOTNOTES;

	/* reset the footnotes lists */
	if (footnotes_enabled) {
		memset(&md->footnotes_found, 0x00, sizeof(md->footnotes_found));
		memset(&md->footnotes_used, 0x00, sizeof(md->footnotes_used));
	}

	/* first pass: looking for references, copying everything else */
	size_t beg = 0;

	/*
	 * Skip a possible UTF-8 BOM, even though the Unicode standard
	 * discourages having these in UTF-8 documents
	 */
	if (doc_size >= 3 && memcmp(document, UTF8_BOM, 3) == 0) {
		beg += 3;
	}

	size_t end;

	while (beg < doc_size) { /* iterating over lines */
		if (footnotes_enabled && is_footnote(document, beg, doc_size, &end, &md->footnotes_found)) {
			beg = end;
		} else if (is_ref(document, beg, doc_size, &end, md->refs)) {
			beg = end;
		} else { /* skipping to the next line */
			end = beg;

			while (end < doc_size && document[end] != '\n' && document[end] != '\r') {
				end++;
			}

			/* adding the line body if present */
			if (end > beg) {
				expand_tabs(text, document + beg, end - beg);
			}

			while (end < doc_size && (document[end] == '\n' || document[end] == '\r')) {
				/* add one \n per newline */
				if (document[end] == '\n' || (end + 1 < doc_size && document[end + 1] != '\n')) {
					bufputc(text, '\n');
				}

				end++;
			}

			beg = end;
		}
	}

	/* pre-grow the output buffer to minimize allocations */
	if (bufgrow(ob, MARKDOWN_GROW(text->size)) != BUF_OK) {
		return;
	}

	/* second pass: actual rendering */
	if (md->cb.doc_header) {
		md->cb.doc_header(ob, md->opaque);
	}

	if (text->size) {
		/* adding a final newline if not already present */
		if (text->data[text->size - 1] != '\n' && text->data[text->size - 1] != '\r') {
			bufputc(text, '\n');
		}

		parse_block(ob, md, text->data, text->size);
	}

	/* footnotes */
	if (footnotes_enabled) {
		parse_footnote_list(ob, md, &md->footnotes_used);
	}

	if (md->cb.doc_footer) {
		md->cb.doc_footer(ob, md->opaque);
	}

	if (md->cb.outline) {
		md->cb.outline(ob, md->opaque);
	}

	/* clean-up */
	bufrelease(text);
	free_link_refs(md->refs);

	if (footnotes_enabled) {
		free_footnote_list(&md->footnotes_found, 1);
		free_footnote_list(&md->footnotes_used, 0);
	}

	assert(md->work_bufs[BUFFER_SPAN].size == 0);
	assert(md->work_bufs[BUFFER_BLOCK].size == 0);
}

void sd_markdown_free(struct sd_markdown *md)
{
	for (size_t i = 0; i < (size_t)md->work_bufs[BUFFER_SPAN].asize; ++i) {
		bufrelease(md->work_bufs[BUFFER_SPAN].item[i]);
	}

	for (size_t i = 0; i < (size_t)md->work_bufs[BUFFER_BLOCK].asize; ++i) {
		bufrelease(md->work_bufs[BUFFER_BLOCK].item[i]);
	}

	stack_free(&md->work_bufs[BUFFER_SPAN]);
	stack_free(&md->work_bufs[BUFFER_BLOCK]);

	free(md);
}

void sd_version(int *ver_major, int *ver_minor, int *ver_revision)
{
	*ver_major = SUNDOWN_VER_MAJOR;
	*ver_minor = SUNDOWN_VER_MINOR;
	*ver_revision = SUNDOWN_VER_REVISION;
}

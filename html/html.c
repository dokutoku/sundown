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
#include "html.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "houdini.h"

#define USE_XHTML(opt) (opt->flags & HTML_USE_XHTML)

int sdhtml_is_tag(const uint8_t* tag_data, size_t tag_size, const char* tagname)
{
	if ((tag_size < 3) || (tag_data[0] != '<')) {
		return HTML_TAG_NONE;
	}

	size_t i = 1;
	int closed = 0;

	if (tag_data[i] == '/') {
		closed = 1;
		i++;
	}

	for (; i < tag_size; ++i, ++tagname) {
		if (*tagname == '\0') {
			break;
		}

		if (tag_data[i] != *tagname) {
			return HTML_TAG_NONE;
		}
	}

	if (i == tag_size) {
		return HTML_TAG_NONE;
	}

	if ((isspace(tag_data[i]) != 0) || (tag_data[i] == '>')) {
		return (closed != 0) ? (HTML_TAG_CLOSE) : (HTML_TAG_OPEN);
	}

	return HTML_TAG_NONE;
}

static inline void escape_html(struct buf* ob, const uint8_t* source, size_t length_)
{
	houdini_escape_html0(ob, source, length_, 0);
}

static inline void escape_href(struct buf* ob, const uint8_t* source, size_t length_)
{
	houdini_escape_href(ob, source, length_);
}

/* *******************
 * GENERIC RENDERER *
 ********************/
static int rndr_autolink(struct buf* ob, const struct buf* link, enum mkd_autolink type, void* opaque)
{
	struct html_renderopt* options = opaque;

	if ((link == NULL) || (link->size == 0)) {
		return 0;
	}

	if (((options->flags & HTML_SAFELINK) != 0) && (sd_autolink_issafe(link->data, link->size) == 0) && (type != MKDA_EMAIL)) {
		return 0;
	}

	BUFPUTSL(ob, "<a href=\"");

	if (type == MKDA_EMAIL) {
		BUFPUTSL(ob, "mailto:");
	}

	escape_href(ob, link->data, link->size);

	if (options->link_attributes != NULL) {
		bufputc(ob, '\"');
		options->link_attributes(ob, link, opaque);
		bufputc(ob, '>');
	} else {
		BUFPUTSL(ob, "\">");
	}

	/*
	 * Pretty printing: if we get an email address as
	 * an actual URI, e.g. `mailto:foo@bar.com`, we don't
	 * want to print the `mailto:` prefix
	 */
	if (bufprefix(link, "mailto:") == 0) {
		escape_html(ob, link->data + 7, link->size - 7);
	} else {
		escape_html(ob, link->data, link->size);
	}

	BUFPUTSL(ob, "</a>");

	return 1;
}

static void rndr_blockcode(struct buf* ob, const struct buf* text, const struct buf* lang, void* opaque)
{
	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	if ((lang != NULL) && (lang->size != 0)) {
		BUFPUTSL(ob, "<pre><code class=\"");

		for (size_t i = 0, cls = 0; i < lang->size; ++i, ++cls) {
			while ((i < lang->size) && (isspace(lang->data[i]) != 0)) {
				i++;
			}

			if (i < lang->size) {
				size_t org = i;

				while ((i < lang->size) && (isspace(lang->data[i]) == 0)) {
					i++;
				}

				if (lang->data[org] == '.') {
					org++;
				}

				if (cls != 0) {
					bufputc(ob, ' ');
				}

				escape_html(ob, lang->data + org, i - org);
			}
		}

		BUFPUTSL(ob, "\">");
	} else {
		BUFPUTSL(ob, "<pre><code>");
	}

	if (text != NULL) {
		escape_html(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</code></pre>\n");
}

static void rndr_blockquote(struct buf* ob, const struct buf* text, void* opaque)
{
	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	BUFPUTSL(ob, "<blockquote>\n");

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</blockquote>\n");
}

static int rndr_codespan(struct buf* ob, const struct buf* text, void* opaque)
{
	BUFPUTSL(ob, "<code>");

	if (text != NULL) {
		escape_html(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</code>");

	return 1;
}

static int rndr_ins(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<ins>");
	bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</ins>");

	return 1;
}

static int rndr_strikethrough(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<del>");
	bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</del>");

	return 1;
}

static int rndr_double_emphasis(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<strong>");
	bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</strong>");

	return 1;
}

static int rndr_emphasis(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<em>");

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</em>");

	return 1;
}

static int rndr_linebreak(struct buf* ob, void* opaque)
{
	struct html_renderopt* options = opaque;
	bufputs(ob, (USE_XHTML(options)) ? ("<br/>\n") : ("<br>\n"));

	return 1;
}

static void rndr_header(struct buf* ob, const struct buf* text, int level, void* opaque)
{
	struct html_renderopt* options = opaque;

	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	if (options->flags & HTML_OUTLINE) {
		if (options->outline_data.current_level >= level) {
			BUFPUTSL(ob, "</section>");
			options->outline_data.open_section_count--;
		}

		bufprintf(ob, "<section class=\"section%d\">\n", level);
		options->outline_data.open_section_count++;
		options->outline_data.current_level = level;
	}

	if (options->flags & HTML_TOC) {
		bufprintf(ob, "<h%d id=\"toc_%d\">", level, options->toc_data.header_count++);
	} else {
		bufprintf(ob, "<h%d>", level);
	}

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	bufprintf(ob, "</h%d>\n", level);
}

static int rndr_link(struct buf* ob, const struct buf* link, const struct buf* title, const struct buf* content, void* opaque)
{
	struct html_renderopt* options = opaque;

	if ((link != NULL) && ((options->flags & HTML_SAFELINK) != 0) && (sd_autolink_issafe(link->data, link->size) == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<a href=\"");

	if ((link != NULL) && (link->size != 0)) {
		escape_href(ob, link->data, link->size);
	}

	if ((title != NULL) && (title->size != 0)) {
		BUFPUTSL(ob, "\" title=\"");
		escape_html(ob, title->data, title->size);
	}

	if (options->link_attributes != NULL) {
		bufputc(ob, '\"');
		options->link_attributes(ob, link, opaque);
		bufputc(ob, '>');
	} else {
		BUFPUTSL(ob, "\">");
	}

	if ((content != NULL) && (content->size != 0)) {
		bufput(ob, content->data, content->size);
	}

	BUFPUTSL(ob, "</a>");

	return 1;
}

static void rndr_list(struct buf* ob, const struct buf* text, int flags, void* opaque)
{
	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	bufput(ob, (flags & MKD_LIST_ORDERED) ? ("<ol>\n") : ("<ul>\n"), 5);

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	bufput(ob, (flags & MKD_LIST_ORDERED) ? ("</ol>\n") : ("</ul>\n"), 6);
}

static void rndr_listitem(struct buf* ob, const struct buf* text, int flags, void* opaque)
{
	BUFPUTSL(ob, "<li>");

	if (text != NULL) {
		size_t size = text->size;

		while ((size != 0) && (text->data[size - 1] == '\n')) {
			size--;
		}

		bufput(ob, text->data, size);
	}

	BUFPUTSL(ob, "</li>\n");
}

static void rndr_paragraph(struct buf* ob, const struct buf* text, void* opaque)
{
	struct html_renderopt* options = opaque;

	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	if ((text == NULL) || (text->size == 0)) {
		return;
	}

	size_t i = 0;

	while ((i < text->size) && (isspace(text->data[i]) != 0)) {
		i++;
	}

	if (i == text->size) {
		return;
	}

	BUFPUTSL(ob, "<p>");

	if (options->flags & HTML_HARD_WRAP) {
		while (i < text->size) {
			size_t org = i;

			while ((i < text->size) && (text->data[i] != '\n')) {
				i++;
			}

			if (i > org) {
				bufput(ob, text->data + org, i - org);
			}

			/*
			 * do not insert a line break if this newline
			 * is the last character on the paragraph
			 */
			if (i >= (text->size - 1)) {
				break;
			}

			rndr_linebreak(ob, opaque);
			i++;
		}
	} else {
		bufput(ob, &text->data[i], text->size - i);
	}

	BUFPUTSL(ob, "</p>\n");
}

static void rndr_raw_block(struct buf* ob, const struct buf* text, void* opaque)
{
	if (text == NULL) {
		return;
	}

	size_t sz = text->size;

	while ((sz > 0) && (text->data[sz - 1] == '\n')) {
		sz--;
	}

	size_t org = 0;

	while ((org < sz) && (text->data[org] == '\n')) {
		org++;
	}

	if (org >= sz) {
		return;
	}

	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	bufput(ob, text->data + org, sz - org);
	bufputc(ob, '\n');
}

static int rndr_triple_emphasis(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<strong><em>");
	bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</em></strong>");

	return 1;
}

static void rndr_hrule(struct buf* ob, void* opaque)
{
	struct html_renderopt* options = opaque;

	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	bufputs(ob, (USE_XHTML(options)) ? ("<hr/>\n") : ("<hr>\n"));
}

static int rndr_image(struct buf* ob, const struct buf* link, const struct buf* title, const struct buf* alt, void* opaque)
{
	struct html_renderopt* options = opaque;

	if ((link == NULL) || (link->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<img src=\"");
	escape_href(ob, link->data, link->size);
	BUFPUTSL(ob, "\" alt=\"");

	if ((alt != NULL) && (alt->size != 0)) {
		escape_html(ob, alt->data, alt->size);
	}

	if ((title != NULL) && (title->size != 0)) {
		BUFPUTSL(ob, "\" title=\"");
		escape_html(ob, title->data, title->size);
	}

	bufputs(ob, USE_XHTML(options) ? ("\"/>") : ("\">"));

	return 1;
}

static int rndr_raw_html(struct buf* ob, const struct buf* text, void* opaque)
{
	struct html_renderopt* options = opaque;

	/*
	 * HTML_ESCAPE overrides SKIP_HTML, SKIP_STYLE, SKIP_LINKS and SKIP_IMAGES
	 * It doens't see if there are any valid tags, just escape all of them.
	 */
	if ((options->flags & HTML_ESCAPE) != 0) {
		escape_html(ob, text->data, text->size);

		return 1;
	}

	if ((options->flags & HTML_SKIP_HTML) != 0) {
		return 1;
	}

	if (((options->flags & HTML_SKIP_STYLE) != 0) && (sdhtml_is_tag(text->data, text->size, "style") != HTML_TAG_NONE)) {
		return 1;
	}

	if (((options->flags & HTML_SKIP_LINKS) != 0) && (sdhtml_is_tag(text->data, text->size, "a") != HTML_TAG_NONE)) {
		return 1;
	}

	if (((options->flags & HTML_SKIP_IMAGES) != 0) && (sdhtml_is_tag(text->data, text->size, "img") != HTML_TAG_NONE)) {
		return 1;
	}

	bufput(ob, text->data, text->size);

	return 1;
}

static void rndr_table(struct buf* ob, const struct buf* header, const struct buf* body_, void* opaque)
{
	if (ob->size != 0) {
		bufputc(ob, '\n');
	}

	BUFPUTSL(ob, "<table><thead>\n");

	if (header != NULL) {
		bufput(ob, header->data, header->size);
	}

	BUFPUTSL(ob, "</thead><tbody>\n");

	if (body_ != NULL) {
		bufput(ob, body_->data, body_->size);
	}

	BUFPUTSL(ob, "</tbody></table>\n");
}

static void rndr_tablerow(struct buf* ob, const struct buf* text, void* opaque)
{
	BUFPUTSL(ob, "<tr>\n");

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</tr>\n");
}

static void rndr_tablecell(struct buf* ob, const struct buf* text, int flags, void* opaque)
{
	if (flags & MKD_TABLE_HEADER) {
		BUFPUTSL(ob, "<th");
	} else {
		BUFPUTSL(ob, "<td");
	}

	switch (flags & MKD_TABLE_ALIGNMASK) {
		case MKD_TABLE_ALIGN_CENTER:
			BUFPUTSL(ob, " style=\"text-align: center\">");

			break;

		case MKD_TABLE_ALIGN_L:
			BUFPUTSL(ob, " style=\"text-align: left\">");

			break;

		case MKD_TABLE_ALIGN_R:
			BUFPUTSL(ob, " style=\"text-align: right\">");

			break;

		default:
			BUFPUTSL(ob, ">");

			break;
	}

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	if (flags & MKD_TABLE_HEADER) {
		BUFPUTSL(ob, "</th>\n");
	} else {
		BUFPUTSL(ob, "</td>\n");
	}
}

static int rndr_superscript(struct buf* ob, const struct buf* text, void* opaque)
{
	if ((text == NULL) || (text->size == 0)) {
		return 0;
	}

	BUFPUTSL(ob, "<sup>");
	bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</sup>");

	return 1;
}

static void rndr_normal_text(struct buf* ob, const struct buf* text, void* opaque)
{
	if (text != NULL) {
		escape_html(ob, text->data, text->size);
	}
}

static void rndr_finalize(struct buf* ob, void* opaque)
{
	struct html_renderopt* options = opaque;

	if (options->flags & HTML_OUTLINE) {
		for (int i = 0; i < options->outline_data.open_section_count; i++) {
			BUFPUTSL(ob, "\n</section>\n");
		}
	}
}

static void rndr_footnotes(struct buf* ob, const struct buf* text, void* opaque)
{
	BUFPUTSL(ob, "<div class=\"footnotes\">\n<hr />\n<ol>\n");

	if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "\n</ol>\n</div>\n");
}

static void rndr_footnote_def(struct buf* ob, const struct buf* text, unsigned int num, void* opaque)
{
	size_t i = 0;
	int pfound = 0;

	/* insert anchor at the end of first paragraph block */
	if (text != NULL) {
		while ((i + 3) < text->size) {
			if (text->data[i++] != '<') {
				continue;
			}

			if (text->data[i++] != '/') {
				continue;
			}

			if ((text->data[i++] != 'p') && (text->data[i] != 'P')) {
				continue;
			}

			if (text->data[i] != '>') {
				continue;
			}

			i -= 3;
			pfound = 1;

			break;
		}
	}

	bufprintf(ob, "\n<li id=\"fn%d\">\n", num);

	if (pfound != 0) {
		bufput(ob, text->data, i);
		bufprintf(ob, "&nbsp;<a href=\"#fnref%d\" rev=\"footnote\">&#8617;</a>", num);
		bufput(ob, text->data + i, text->size - i);
	} else if (text != NULL) {
		bufput(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</li>\n");
}

static int rndr_footnote_ref(struct buf* ob, unsigned int num, void* opaque)
{
	bufprintf(ob, "<sup id=\"fnref%d\"><a href=\"#fn%d\" rel=\"footnote\">%d</a></sup>", num, num, num);

	return 1;
}

static void toc_header(struct buf* ob, const struct buf* text, int level, void* opaque)
{
	struct html_renderopt* options = opaque;

	/*
	 * set the level offset if this is the first header
	 * we're parsing for the document
	 */
	if (options->toc_data.current_level == 0) {
		options->toc_data.level_offset = level - 1;
	}

	level -= options->toc_data.level_offset;

	if (level > options->toc_data.current_level) {
		while (level > options->toc_data.current_level) {
			BUFPUTSL(ob, "<ul>\n<li>\n");
			options->toc_data.current_level++;
		}
	} else if (level < options->toc_data.current_level) {
		BUFPUTSL(ob, "</li>\n");

		while (level < options->toc_data.current_level) {
			BUFPUTSL(ob, "</ul>\n</li>\n");
			options->toc_data.current_level--;
		}

		BUFPUTSL(ob, "<li>\n");
	} else {
		BUFPUTSL(ob, "</li>\n<li>\n");
	}

	bufprintf(ob, "<a href=\"#toc_%d\">", options->toc_data.header_count++);

	if (text != NULL) {
		escape_html(ob, text->data, text->size);
	}

	BUFPUTSL(ob, "</a>\n");
}

static int toc_link(struct buf* ob, const struct buf* link, const struct buf* title, const struct buf* content, void* opaque)
{
	if ((content != NULL) && (content->size != 0)) {
		bufput(ob, content->data, content->size);
	}

	return 1;
}

static void toc_finalize(struct buf* ob, void* opaque)
{
	struct html_renderopt* options = opaque;

	while (options->toc_data.current_level > 0) {
		BUFPUTSL(ob, "</li>\n</ul>\n");
		options->toc_data.current_level--;
	}
}

void sdhtml_toc_renderer(struct sd_callbacks* callbacks, struct html_renderopt* options)
{
	static const struct sd_callbacks cb_default =
	{
		NULL,
		NULL,
		NULL,
		toc_header,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,

		NULL,
		rndr_codespan,
		rndr_double_emphasis,
		rndr_emphasis,
		NULL,
		NULL,
		toc_link,
		NULL,
		rndr_triple_emphasis,
		rndr_ins,
		rndr_strikethrough,
		rndr_superscript,
		NULL,

		NULL,
		NULL,

		NULL,
		toc_finalize,

		NULL,
	};

	memset(options, 0x00, sizeof(struct html_renderopt));
	options->flags = HTML_TOC;

	memcpy(callbacks, &cb_default, sizeof(struct sd_callbacks));
}

void sdhtml_renderer(struct sd_callbacks* callbacks, struct html_renderopt* options, unsigned int render_flags)
{
	static const struct sd_callbacks cb_default =
	{
		rndr_blockcode,
		rndr_blockquote,
		rndr_raw_block,
		rndr_header,
		rndr_hrule,
		rndr_list,
		rndr_listitem,
		rndr_paragraph,
		rndr_table,
		rndr_tablerow,
		rndr_tablecell,
		rndr_footnotes,
		rndr_footnote_def,

		rndr_autolink,
		rndr_codespan,
		rndr_double_emphasis,
		rndr_emphasis,
		rndr_image,
		rndr_linebreak,
		rndr_link,
		rndr_raw_html,
		rndr_triple_emphasis,
		rndr_ins,
		rndr_strikethrough,
		rndr_superscript,
		rndr_footnote_ref,

		NULL,
		rndr_normal_text,

		NULL,
		NULL,

		/* rndr_finalize */
		NULL,
	};

	/* Prepare the options pointer */
	memset(options, 0x00, sizeof(struct html_renderopt));
	options->flags = render_flags;

	/* Prepare the callbacks */
	memcpy(callbacks, &cb_default, sizeof(struct sd_callbacks));

	if (render_flags & HTML_OUTLINE) {
		callbacks->outline = rndr_finalize;

		options->outline_data.open_section_count = 0;
		options->outline_data.current_level = 0;
	}

	if (render_flags & HTML_SKIP_IMAGES) {
		callbacks->image = NULL;
	}

	if (render_flags & HTML_SKIP_LINKS) {
		callbacks->link = NULL;
		callbacks->autolink = NULL;
	}

	if ((render_flags & HTML_SKIP_HTML) || (render_flags & HTML_ESCAPE)) {
		callbacks->blockhtml = NULL;
	}
}

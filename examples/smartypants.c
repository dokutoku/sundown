/*
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
#include "buffer.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_UNIT 1024
#define OUTPUT_UNIT 64

int main(int argc, char **argv)
{
	FILE *in_ = stdin;

	/* opening the file if given from the command line */
	if (argc > 1) {
		in_ = fopen(argv[1], "r");
		if (!in_) {
			fprintf(stderr, "Unable to open input file \"%s\": %s\n", argv[0], strerror(errno));
			return 1;
		}
	}

	/* reading everything */
	struct buf *ib = bufnew(READ_UNIT);

	if (bufgrow(ib, READ_UNIT) != BUF_OK) {
		fprintf(stderr, "Error: bufgrow()\n");

		if (in_ != stdin)
			fclose(in_);

		bufrelease(ib);

		return -1;
	}

	size_t ret;

	while ((ret = fread(ib->data + ib->size, 1, ib->asize - ib->size, in_)) > 0) {
		ib->size += ret;

		if (bufgrow(ib, ib->size + READ_UNIT) != BUF_OK) {
			fprintf(stderr, "Error: bufgrow()\n");

			if (in_ != stdin)
				fclose(in_);

			bufrelease(ib);

			return -1;
		}
	}

	if (in_ != stdin)
		fclose(in_);

	/* performing markdown parsing */
	struct buf *ob = bufnew(OUTPUT_UNIT);

	sdhtml_smartypants(ob, ib->data, ib->size);

	/* writing the result to stdout */
	(void)fwrite(ob->data, 1, ob->size, stdout);

	/* cleanup */
	bufrelease(ib);
	bufrelease(ob);

	return 0;
}


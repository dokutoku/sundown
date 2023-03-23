/*
 * Copyright (c) 2008, Natacha Porté
 * Copyright (c) 2011, Vicent Martí
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

/* 16MB */
#define BUFFER_MAX_ALLOC_SIZE (1024 * 1024 * 16)

#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* MSVC compat */
#if defined(_MSC_VER)
#define _buf_vsnprintf _vsnprintf
#else
#define _buf_vsnprintf vsnprintf
#endif

int bufprefix(const struct buf* buf, const char* prefix)
{
	assert((buf != NULL) && (buf->unit != 0));

	for (size_t i = 0; i < buf->size; ++i) {
		if (prefix[i] == '\0') {
			return 0;
		}

		if (buf->data[i] != prefix[i]) {
			return buf->data[i] - prefix[i];
		}
	}

	return 0;
}

/**
 * increasing the allocated size to the given value
 */
int bufgrow(struct buf* buf, size_t neosz)
{
	assert((buf != NULL) && (buf->unit != 0));

	if (neosz > BUFFER_MAX_ALLOC_SIZE) {
		return BUF_ENOMEM;
	}

	if (buf->asize >= neosz) {
		return BUF_OK;
	}

	size_t neoasz = buf->asize + buf->unit;

	while (neoasz < neosz) {
		neoasz += buf->unit;
	}

	void* neodata = realloc(buf->data, neoasz);

	if (neodata == NULL) {
		return BUF_ENOMEM;
	}

	buf->data = neodata;
	buf->asize = neoasz;

	return BUF_OK;
}

/**
 * allocation of a new buffer
 */
struct buf* bufnew(size_t unit)
{
	struct buf* ret = malloc(sizeof(struct buf));

	if (ret != NULL) {
		ret->data = NULL;
		ret->asize = 0;
		ret->size = 0;
		ret->unit = unit;
	}

	return ret;
}

/**
 * NULL-termination of the string array
 */
const char* bufcstr(struct buf* buf)
{
	assert((buf != NULL) && (buf->unit != 0));

	if ((buf->size < buf->asize) && (buf->data[buf->size] == '\0')) {
		return (char*)buf->data;
	}

	if (((buf->size + 1) <= buf->asize) || (bufgrow(buf, buf->size + 1) == BUF_OK)) {
		buf->data[buf->size] = '\0';

		return (char*)buf->data;
	}

	return NULL;
}

/**
 * formatted printing to a buffer
 */
void bufprintf(struct buf* buf, const char* fmt, ...)
{
	assert((buf != NULL) && (buf->unit != 0));

	if ((buf->size >= buf->asize) && (bufgrow(buf, buf->size + 1) != BUF_OK)) {
		return;
	}

	va_list ap;
	va_start(ap, fmt);
	int n = _buf_vsnprintf((char*)buf->data + buf->size, buf->asize - buf->size, fmt, ap);
	va_end(ap);

	if (n < 0) {
#ifdef _MSC_VER
		va_start(ap, fmt);
		n = _vscprintf(fmt, ap);
		va_end(ap);
#else
		return;
#endif
	}

	if ((size_t)n >= (buf->asize - buf->size)) {
		if (bufgrow(buf, buf->size + n + 1) != BUF_OK) {
			return;
		}

		va_start(ap, fmt);
		n = _buf_vsnprintf((char*)buf->data + buf->size, buf->asize - buf->size, fmt, ap);
		va_end(ap);
	}

	if (n < 0) {
		return;
	}

	buf->size += n;
}

/**
 * appends raw data to a buffer
 */
void bufput(struct buf* buf, const void* data, size_t len)
{
	assert((buf != NULL) && (buf->unit != 0));

	if (((buf->size + len) > buf->asize) && (bufgrow(buf, buf->size + len) != BUF_OK)) {
		return;
	}

	memcpy(buf->data + buf->size, data, len);
	buf->size += len;
}

/**
 * appends a NUL-terminated string to a buffer
 */
void bufputs(struct buf* buf, const char* str)
{
	bufput(buf, str, strlen(str));
}

/**
 * appends a single uint8_t to a buffer
 */
void bufputc(struct buf* buf, uint8_t c)
{
	assert((buf != NULL) && (buf->unit != 0));

	if (((buf->size + 1) > buf->asize) && (bufgrow(buf, buf->size + 1) != BUF_OK)) {
		return;
	}

	buf->data[buf->size] = c;
	buf->size += 1;
}

/**
 * decrease the reference count and free the buffer if needed
 */
void bufrelease(struct buf* buf)
{
	if (buf == NULL) {
		return;
	}

	free(buf->data);
	free(buf);
}

/**
 * frees internal data of the buffer
 */
void bufreset(struct buf* buf)
{
	if (buf == NULL) {
		return;
	}

	free(buf->data);
	buf->data = NULL;
	buf->asize = 0;
	buf->size = 0;
}

/**
 * removes a given number of bytes from the head of the array
 */
void bufslurp(struct buf* buf, size_t len)
{
	assert((buf != NULL) && (buf->unit != 0));

	if (len >= buf->size) {
		buf->size = 0;

		return;
	}

	buf->size -= len;
	memmove(buf->data, buf->data + len, buf->size);
}

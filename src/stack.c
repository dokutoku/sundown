#include "stack.h"
#include <string.h>

int stack_grow(struct stack* st, size_t new_size)
{
	if (st->asize >= new_size) {
		return 0;
	}

	void** new_st = realloc(st->item, new_size * sizeof(void*));

	if (new_st == NULL) {
		return -1;
	}

	memset(new_st + st->asize, 0x00, (new_size - st->asize) * sizeof(void*));

	st->item = new_st;
	st->asize = new_size;

	if (st->size > new_size) {
		st->size = new_size;
	}

	return 0;
}

void stack_free(struct stack* st)
{
	if (st == NULL) {
		return;
	}

	free(st->item);

	st->item = NULL;
	st->size = 0;
	st->asize = 0;
}

int stack_init(struct stack* st, size_t initial_size)
{
	st->item = NULL;
	st->size = 0;
	st->asize = 0;

	if (initial_size == 0) {
		initial_size = 8;
	}

	return stack_grow(st, initial_size);
}

void* stack_pop(struct stack* st)
{
	if (st->size == 0) {
		return NULL;
	}

	return st->item[--st->size];
}

int stack_push(struct stack* st, void* item)
{
	if (stack_grow(st, st->size * 2) < 0) {
		return -1;
	}

	st->item[st->size++] = item;

	return 0;
}

void* stack_top(struct stack* st)
{
	if (st->size == 0) {
		return NULL;
	}

	return st->item[st->size - 1];
}

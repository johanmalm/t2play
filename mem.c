// SPDX-License-Identifier: GPL-2.0-only
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>

void
die_if_null(void *ptr)
{
	if (!ptr) {
		perror("Failed to allocate memory");
		exit(EXIT_FAILURE);
	}
}

void *
xzalloc(size_t size)
{
	if (!size) {
		return NULL;
	}
	void *ptr = calloc(1, size);
	die_if_null(ptr);
	return ptr;
}

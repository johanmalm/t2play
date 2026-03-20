/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_MEM_H
#define LABWC_MEM_H

#include <stdlib.h>

void die_if_null(void *ptr);
void *xzalloc(size_t size);
#define znew(expr)       ((__typeof__(expr) *)xzalloc(sizeof(expr)))

char *xstrdup(const char *str);
#define xstrdup_replace(ptr, str) do { \
	free(ptr); (ptr) = xstrdup(str); \
} while (0)

#define zfree(ptr) do { \
	free(ptr); (ptr) = NULL; \
} while (0)

#endif /* LABWC_MEM_H */

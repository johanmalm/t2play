// SPDX-License-Identifier: GPL-2.0-only
#include "common/string-helpers.h"
#include <stdarg.h>
#include <stdio.h>
#include "common/mem.h"

bool
string_null_or_empty(const char *s)
{
	return !s || !*s;
}

char *
strdup_printf(const char *fmt, ...)
{
	size_t size = 0;
	char *p = NULL;
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(p, size, fmt, ap);
	va_end(ap);

	if (n < 0) {
		return NULL;
	}

	size = (size_t)n + 1;
	p = xzalloc(size);

	va_start(ap, fmt);
	n = vsnprintf(p, size, fmt, ap);
	va_end(ap);

	if (n < 0) {
		free(p);
		return NULL;
	}
	return p;
}

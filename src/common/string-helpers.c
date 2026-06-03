// SPDX-License-Identifier: GPL-2.0-only
#include "common/string-helpers.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

enum str_flags {
	STR_FLAG_NONE = 0,
	STR_FLAG_IGNORE_CASE,
};

static bool
_str_endswith(const char *const string, const char *const suffix, uint32_t flags)
{
	size_t len_str = string ? strlen(string) : 0;
	size_t len_sfx = suffix ? strlen(suffix) : 0;

	if (len_str < len_sfx) {
		return false;
	}

	if (len_sfx == 0) {
		return true;
	}

	if (flags & STR_FLAG_IGNORE_CASE) {
		return strcasecmp(string + len_str - len_sfx, suffix) == 0;
	} else {
		return strcmp(string + len_str - len_sfx, suffix) == 0;
	}
}

bool
str_endswith_ignore_case(const char *const string, const char *const suffix)
{
	return _str_endswith(string, suffix, STR_FLAG_IGNORE_CASE);
}

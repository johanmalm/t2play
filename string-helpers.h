/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_STRING_HELPERS_H
#define LABWC_STRING_HELPERS_H
#include <stdbool.h>

bool string_null_or_empty(const char *s);
char *strdup_printf(const char *fmt, ...);

#endif /* LABWC_STRING_HELPERS_H */

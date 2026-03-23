/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ARRAY_H
#define LABWC_ARRAY_H

#include <wayland-server-core.h>

static inline size_t
wl_array_len(struct wl_array *array)
{
	return array->size / sizeof(const char *);
}

#define wl_array_for_each_reverse(pos, array)                                          \
	for (pos = !(array)->data ? NULL                                               \
		: (void *)((const char *)(array)->data + (array)->size - sizeof(pos)); \
		pos && (const char *)pos >= (const char *)(array)->data;               \
		(pos)--)

#define array_add(_arr, _val) do {                           \
		__typeof__(_val) *_entry = wl_array_add(     \
			(_arr), sizeof(__typeof__(_val)));   \
		die_if_null(_entry);                         \
		*_entry = (_val);                            \
	} while (0)

#endif /* LABWC_ARRAY_H */

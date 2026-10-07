#ifndef CLEANUP_H
#define CLEANUP_H
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __cleanup(f) __attribute__((__cleanup__(f)))

static inline void freep(void *p)
{
	free(*(void **)p);
}

static inline void fclosep(FILE **p)
{
	if (*p) {
		fclose(*p);
	}
}

static inline void closep(int *p)
{
	if (*p >= 0) {
		close(*p);
	}
}

#define cleanup_free __cleanup(freep)
#define cleanup_fclose __cleanup(fclosep)
#define cleanup_close __cleanup(closep)

#endif /* CLEANUP_H */

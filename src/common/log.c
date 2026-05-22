#include "common/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static enum log_importance log_importance = LOG_ERROR;

static const char *verbosity_colors[] = {
	[LOG_SILENT] = "",
	[LOG_ERROR ] = "\x1B[1;31m",
	[LOG_INFO  ] = "\x1B[1;34m",
	[LOG_DEBUG ] = "\x1B[1;33m",
};

void
log_init(enum log_importance verbosity)
{
	log_importance = verbosity;
}

void
_log(enum log_importance verbosity, const char *fmt, ...)
{
	if (verbosity > log_importance) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	struct tm result;
	time_t t = time(NULL);
	struct tm *tm_info = localtime_r(&t, &result);
	char buffer[26];
	strftime(buffer, sizeof(buffer), "%F %T - ", tm_info);
	fprintf(stderr, "%s", buffer);
	unsigned c = (verbosity < LOG_SIZE) ? verbosity : LOG_SIZE - 1;
	if (isatty(STDERR_FILENO)) {
		fprintf(stderr, "%s", verbosity_colors[c]);
	}
	vfprintf(stderr, fmt, args);
	if (isatty(STDERR_FILENO)) {
		fprintf(stderr, "\x1B[0m");
	}
	fprintf(stderr, "\n");
	va_end(args);
}

const char *
_strip_path(const char *path)
{
	if (*path == '.') {
		while (*path == '.' || *path == '/') {
			++path;
		}
	}
	return path;
}

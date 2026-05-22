#ifndef LOG_H
#define LOG_H
#include <stdarg.h>

enum log_importance {
	LOG_SILENT = 0,
	LOG_ERROR = 1,
	LOG_INFO = 2,
	LOG_DEBUG = 3,

	LOG_SIZE,
};

void log_init(enum log_importance verbosity);
void _log(enum log_importance verbosity, const char *format, ...);
const char *_strip_path(const char *path);

#define warn(fmt, ...) \
	_log(LOG_ERROR, "[%s:%d] error: " fmt, _strip_path(__FILE__), __LINE__, ##__VA_ARGS__)

#define info(fmt, ...) \
	_log(LOG_INFO, "[%s:%d] info: " fmt, _strip_path(__FILE__), __LINE__, ##__VA_ARGS__)

#define debug(fmt, ...) \
	_log(LOG_DEBUG, "[%s:%d] " fmt, _strip_path(__FILE__), __LINE__, ##__VA_ARGS__)

#define die(fmt, ...) \
	_log(LOG_ERROR, "[%s:%d] fatal: " fmt, _strip_path(__FILE__), __LINE__, ##__VA_ARGS__); \
	exit(EXIT_FAILURE)

#endif /* LOG_H */

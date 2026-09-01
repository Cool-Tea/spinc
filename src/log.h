#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <assert.h>

#include "error.h"
#include "session.h"

#define LOG_PATH_SEP '/'
#define LOG_PATH_MAX_LEN 1024
#define LOG_FILE session_log_file()
#ifndef LOG_LEVEL
#define LOG_LEVEL (enum log_level) session_log_level()
#endif

#define DEBUG_PREFIX "\033[2;36m [DEBUG]"
#define INFO_PREFIX "\033[0;37m [INFO]"
#define WARN_PREFIX "\033[1;33m [WARN]"
#define ERROR_PREFIX "\033[1;31m [ERROR]"

#define log_assert(cond) assert(cond)

#define log(level, fmt, ...)                                             \
  do {                                                                   \
    if (level >= LOG_LEVEL) {                                            \
      log_assert(level > ALL && level < DISABLE && "Invalid log level"); \
      fprintf(LOG_FILE, level##_PREFIX " [%s:%s:%d] " fmt "\033[0m\n",   \
              __FILE__, __func__, __LINE__, ##__VA_ARGS__);              \
      fflush(LOG_FILE);                                                  \
    }                                                                    \
  } while (0)

enum log_level {
  ALL = 0,
  DEBUG,
  INFO,
  WARN,
  ERROR,
  DISABLE,
};

#endif  // LOG_H
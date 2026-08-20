#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <assert.h>

#define log_assert(cond) assert(cond)

enum log_level {
  ALL = 0,
  DEBUG,
  INFO,
  WARN,
  ERROR,
  DISABLE,
};

#define DEFAULT_LOG_LEVEL ALL
#ifndef LOG_LEVEL
#define LOG_LEVEL DEFAULT_LOG_LEVEL
#endif

#define DEBUG_PREFIX "\033[3;36m [DEBUG]"
#define INFO_PREFIX "\033[0;37m [INFO]"
#define WARN_PREFIX "\033[1;33m [WARN]"
#define ERROR_PREFIX "\033[1;31m [ERROR]"

#define log(level, fmt, ...)                                             \
  do {                                                                   \
    if (level >= LOG_LEVEL) {                                            \
      log_assert(level > ALL && level < DISABLE && "Invalid log level"); \
      printf(level##_PREFIX " [%s:%s:%d] " fmt "\033[0m\n", __FILE__,    \
             __func__, __LINE__, ##__VA_ARGS__);                         \
      fflush(stdout);                                                    \
    }                                                                    \
  } while (0)

#endif  // LOG_H
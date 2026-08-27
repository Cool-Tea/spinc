#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#define log_assert(cond) assert(cond)

#ifdef LOG_LEVEL
#define log(level, fmt, ...)                                             \
  do {                                                                   \
    if (level >= LOG_LEVEL) {                                            \
      log_assert(level > ALL && level < DISABLE && "Invalid log level"); \
      fprintf(glog_file, level##_PREFIX " [%s:%s:%d] " fmt "\033[0m\n",  \
              __FILE__, __func__, __LINE__, ##__VA_ARGS__);              \
      fflush(glog_file);                                                 \
    }                                                                    \
  } while (0)
#else
#define log(level, fmt, ...)                                             \
  do {                                                                   \
    if (level >= glog_level) {                                           \
      log_assert(level > ALL && level < DISABLE && "Invalid log level"); \
      fprintf(glog_file, level##_PREFIX " [%s:%s:%d] " fmt "\033[0m\n",  \
              __FILE__, __func__, __LINE__, ##__VA_ARGS__);              \
      fflush(glog_file);                                                 \
    }                                                                    \
  } while (0)
#endif

#define DEBUG_PREFIX "\033[2;36m [DEBUG]"
#define INFO_PREFIX "\033[0;37m [INFO]"
#define WARN_PREFIX "\033[1;33m [WARN]"
#define ERROR_PREFIX "\033[1;31m [ERROR]"

#define LOG_PATH_SEP '/'
#define LOG_PATH_MAX_LEN 1024

enum log_level {
  ALL = 0,
  DEBUG,
  INFO,
  WARN,
  ERROR,
  DISABLE,
};

bool log_init(const char* log_dir, enum log_level level);
void log_quit();

extern FILE* glog_file;
extern enum log_level glog_level;

#endif  // LOG_H
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "log.h"

static FILE* log_file;
static enum log_level log_level;
static char path_buf[LOG_PATH_MAX_LEN];

inline static void ensure_log_dir(const char* log_dir, size_t log_dir_len) {
  struct stat st = {0};
  size_t path_len = 0;
  const char* end = log_dir + log_dir_len;
  for (const char *part = log_dir, *next; part < end; part = next) {
    next = strchr(part, LOG_PATH_SEP);
    if (!next) next = end;
    else next++;
    size_t len = next - part;
    memcpy(path_buf + path_len, part, len);
    path_len += len;
    path_buf[path_len] = '\0';
    if (stat(path_buf, &st) == -1) {
      mkdir(path_buf, 0755);
    }
  }
}

err_t log_init(const char* log_dir, enum log_level level) {
  log_level = level;

  if (!log_dir) {
    fprintf(stderr, "warning: log_dir is NULL. Default to stderr\n");
    log_file = stderr;
    return ERROR_NONE;
  }

  char filename[32];
  time_t now = time(NULL);
  struct tm* t = localtime(&now);
  size_t flen = strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S.log", t);

  size_t log_dir_len = strlen(log_dir);
  if (log_dir_len + flen + 1 > LOG_PATH_MAX_LEN) {
    fprintf(stderr, "error: log_dir path is too long\n");
    return ERROR_OUT_OF_BOUNDS;
  }

  ensure_log_dir(log_dir, log_dir_len);
  snprintf(path_buf, sizeof(path_buf), "%s/%s", log_dir, filename);
  log_file = fopen(path_buf, "w");
  if (!log_file) {
    fprintf(stderr, "error: failed to create log file\n");
    return ERROR_IO;
  }
  fprintf(stderr, "Log file: %s\n", path_buf);
  return ERROR_NONE;
}

void log_quit() {
  if (log_file) {
    fclose(log_file);
    log_file = NULL;
  }
}

FILE* log_get_file() { return log_file; }

enum log_level log_get_level() { return log_level; }
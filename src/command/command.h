#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>

#include "error.h"

#define DEFINE_COMMAND(name, description, func) \
  (command_t) { name, description, func }

typedef struct command {
  const char* name;
  const char* description;
  err_t (*func)(const char* line);
} command_t;

size_t command_get_all(const command_t** cmds);
err_t command_find(const char* name, size_t name_len, const command_t** cmd);

#endif
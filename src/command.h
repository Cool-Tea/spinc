#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdbool.h>

#define DEFINE_COMMAND(name, description, func) \
  (command_t) { name, description, func }

typedef struct command {
  const char* name;
  const char* description;
  bool (*func)(const char* line);
} command_t;

bool command_init(const command_t* cmds, size_t n_cmds);
void command_quit();
size_t command_get_all(const command_t** cmds);
const command_t* command_find(const char* name);

#endif
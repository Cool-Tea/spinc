#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>

#include "log.h"
#include "command.h"

static const command_t* cmds = NULL;
static size_t n_cmd = 0;

static char* command_generator(const char* text, int state) {
  static size_t index;
  static size_t len;
  if (!state) {
    index = 0;
    len = strlen(text) - 1; /* text includes the leading '/' */
  }
  while (index < n_cmd) {
    const command_t* cmd = &cmds[index++];
    if (len == 0 || strncmp(cmd->name, text + 1, len) == 0) {
      char* match = malloc(strlen(cmd->name) + 2);
      match[0] = '/';
      strcpy(match + 1, cmd->name);
      return match;
    }
  }
  return NULL;
}

static char** command_completion(const char* text, int start, int end) {
  (void)start;
  (void)end;
  char** matches = NULL;
  /* text points to the word being completed (== &rl_line_buffer[start]) */
  if (text[0] == '/') {
    rl_attempted_completion_over = 1;
    matches = rl_completion_matches(text, command_generator);
  }
  return matches;
}

bool command_init(const command_t* commands, size_t n_command) {
  log(INFO, "Initializing command system with %zu commands", n_command);
  cmds = commands;
  n_cmd = n_command;
  rl_attempted_completion_function = command_completion;
  return true;
}

void command_quit() {
  log(INFO, "Cleaning up command system");
  cmds = NULL;
  n_cmd = 0;
}

size_t command_get_all(const command_t** commands) {
  if (commands) *commands = cmds;
  return n_cmd;
}

const command_t* command_find(const char* name) {
  for (size_t i = 0; i < n_cmd; i++) {
    size_t len = strlen(cmds[i].name);
    if (strncmp(cmds[i].name, name, len) == 0) {
      return &cmds[i];
    }
  }
  return NULL;
}
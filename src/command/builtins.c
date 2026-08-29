#include <stdio.h>
#include <stdlib.h>

#include "command.h"

err_t help_command(const char* line) {
  (void)line;
  const command_t* cmds;
  size_t n_cmd = command_get_all(&cmds);
  for (size_t i = 0; i < n_cmd; i++) {
    printf("\033[36m%8s  %s\033[0m\n", cmds[i].name, cmds[i].description);
  }
  return ERROR_NONE;
}

// clang-format off

__attribute__((noreturn))
err_t exit_command(const char* line) {
  (void)line;
  exit(EXIT_SUCCESS);
}

// clang-format on
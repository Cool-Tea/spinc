#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "command.h"
#include "session.h"

#define HISTORY_COMMAND_MAX_LINE_SIZE 100

err_t help_command(const char* line) {
  (void)line;
  const command_t* cmds;
  size_t n_cmd = command_get_all(&cmds);
  printf("\033[1;36m--- Available commands ---\033[0m\n");
  int max_len = 0;
  for (size_t i = 0; i < n_cmd; i++) {
    int len = strlen(cmds[i].name);
    if (len > max_len) max_len = len;
  }
  for (size_t i = 0; i < n_cmd; i++) {
    printf("\033[1;36m%-*s  %s\033[0m\n", max_len + 2, cmds[i].name,
           cmds[i].description);
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

err_t history_command(const char* line) {
  (void)line;
  size_t n_turns = session_turn_count();
  if (n_turns == 0) {
    printf("\033[1;36mNo conversation history available.\033[0m\n");
    return ERROR_NONE;
  }

  printf("\033[1;36m--- Conversation History ---\033[0m\n");
  for (size_t i = 0; i < n_turns; ++i) {
    char* description = NULL;
    size_t len = 0;
    err_t err = session_turn_description(i, &description, &len);
    if (err != ERROR_NONE) {
      printf("\033[1;36mTurn %zu\033[0m\n", i + 1);
    } else {
      printf("\033[1;36mTurn %zu:\033[0m %.*s\n", i + 1,
             HISTORY_COMMAND_MAX_LINE_SIZE, description);
      free(description);
    }
  }
  return ERROR_NONE;
}

err_t rewind_command(const char* line) {
  const char* arg = line + 7;  // Skip "rewind "
  while (*arg && isspace((unsigned char)*arg)) ++arg;
  if (!*arg) {
    printf("\033[1;36mUsage: /rewind <turn_index>\033[0m\n");
    return ERROR_NONE;
  }
  long turn_count = session_turn_count();
  long turn_index = strtol(arg, NULL, 10);
  if (turn_index < 1 || turn_index > turn_count) {
    printf("\033[1;36mTurn index must fall within range [1, %ld].\033[0m\n",
           turn_count);
    return ERROR_NONE;
  }
  err_t err = session_rewind((size_t)turn_index - 1);
  if (err != ERROR_NONE) {
    printf("\033[1;36mFailed to rewind turn %ld: %s\033[0m\n", turn_index,
           error_str(err));
  } else {
    printf("\033[1;36mRewind turn %ld, content discarded.\033[0m\n",
           turn_index);
  }
  return err;
}
#include <string.h>

#include "command/command.h"
#include "command/builtins.h"

static const command_t cmds[] = {
    DEFINE_COMMAND("help", "Show this help message", help_command),
    DEFINE_COMMAND("exit", "Exit the program", exit_command),
    DEFINE_COMMAND("quit", "Exit the program", exit_command),
    DEFINE_COMMAND("history", "Show the conversation history", history_command),
    DEFINE_COMMAND("rewind", "Rewind the conversation", rewind_command),
};
static size_t n_cmd = sizeof(cmds) / sizeof(command_t);

size_t command_get_all(const command_t** commands) {
  if (commands) *commands = cmds;
  return n_cmd;
}

err_t command_find(const char* name, size_t name_len, const command_t** cmd) {
  if (!name || !cmd) return ERROR_NULLPTR;
  for (size_t i = 0; i < n_cmd; i++) {
    size_t len = strlen(cmds[i].name);
    if (len > name_len) continue;
    if (strncmp(cmds[i].name, name, len) == 0) {
      *cmd = &cmds[i];
      return ERROR_NONE;
    }
  }
  return ERROR_NOT_FOUND;
}
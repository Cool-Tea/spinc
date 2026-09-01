#ifndef COMMAND_BUILTINS_H
#define COMMAND_BUILTINS_H

#include "command/command.h"

err_t help_command(const char* line);
err_t exit_command(const char* line);
err_t history_command(const char* line);
err_t rewind_command(const char* line);

#endif  // COMMAND_BUILTINS_H
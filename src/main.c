#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "log.h"
#include "http.h"
#include "agent.h"
#include "command.h"

#include "config.h"

typedef struct cmdopts {
  const char* prompt;
} cmdopts_t;

static cmdopts_t cmdopts = {0};
static agent_t* agent = NULL;
static char* line = NULL;

static bool parse_options(int argc, char* argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    switch (opt) {
      case 'p': cmdopts.prompt = optarg; break;
      default: break;
    }
  }
  return true;
}

static bool agent_init() {
  log(INFO, "Initializing agent with model: %s", model.name);
  agent = agent_new(&model, system_prompt, tools, n_tool);
  if (!agent) {
    return false;
  }
  return true;
}

static void cleanup() {
  if (agent) {
    agent_delete(agent);
    agent = NULL;
  }
  if (line) {
    free(line);
    line = NULL;
  }
}

static char* rl_get() {
  char* trimmed = line = readline("\033[1;36muser\033[1;32m>\033[0m ");
  if (line && *line) {
    while (isspace((unsigned char)*trimmed)) trimmed++;
    add_history(trimmed);
  }
  return trimmed;
}

int main(int argc, char* argv[]) {
  if (!parse_options(argc, argv)) {
    return 1;
  }

  const char* base_url = model.base_url;
  const char* api_key = model.api_key;
  if (!base_url || !*base_url || !api_key || !*api_key) {
    fprintf(stderr,
            " \033[1;31m[ERROR] base_url or api_key is not set in "
            "config.h\033[0m\n");
    return 1;
  }

  if (!log_init(log_dir, log_level)) {
    fprintf(stderr,
            " \033[1;31m[ERROR] error: failed to initialize logging\033[0m\n");
    return 1;
  }
  atexit(log_quit);

  if (!http_init()) {
    log(ERROR, "Failed to initialize HTTP client");
    return 1;
  }
  atexit(http_quit);

  if (!command_init(commands, n_command)) {
    log(ERROR, "Failed to initialize command system");
    return 1;
  }
  atexit(command_quit);

  if (!agent_init()) {
    log(ERROR, "Failed to initialize agent");
    return 1;
  }
  atexit(cleanup);

  bool success = true;
  bool (*run)(agent_t*, const char*) =
      model.stream ? agent_run_stream : agent_run;
  if (cmdopts.prompt) {
    success = run(agent, cmdopts.prompt);
  } else {
    while (1) {
      char* trimmed_line = rl_get();
      if (!trimmed_line || !*trimmed_line) continue;
      if (*trimmed_line == '/') {
        const command_t* cmd = command_find(trimmed_line + 1);
        if (cmd) success = cmd->func(trimmed_line);
        else printf("\033[1;31mUnknown command: %s\033[0m\n", trimmed_line);
      } else {
        success = run(agent, trimmed_line);
      }
      if (line) free(line);
    }
  }

  return !success;
}
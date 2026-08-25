#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>

#include "log.h"
#include "http.h"
#include "agent.h"

#include "config.h"

typedef struct cmdopts {
  const char* prompt;
} cmdopts_t;

static cmdopts_t cmdopts = {0};

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

  if (!http_init()) {
    log(ERROR, "Failed to initialize HTTP client");
    return 1;
  }

  agent_t* agent = agent_new(&model, system_prompt, tools, n_tool);
  if (!agent) {
    log(ERROR, "Failed to create agent");
    return 1;
  }

  bool success = true;
  if (cmdopts.prompt) {
    success = agent_run(agent, cmdopts.prompt);
  } else {
    while (1) {
      char* line = readline("\033[1;36muser\033[1;32m>\033[0m ");
      if (!line) continue;
      if (strncmp(line, "/exit", 5) == 0 || strncmp(line, "/quit", 5) == 0)
        break;
      success = agent_run(agent, line);
      if (line) free(line);
    }
  }

  agent_delete(agent);
  http_quit();
  log_quit();
  return !success;
}
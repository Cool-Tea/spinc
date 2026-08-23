#include <stdio.h>
#include <unistd.h>

#include "log.h"
#include "http.h"
#include "agent.h"

#include "config.h"

int main(int argc, char* argv[]) {
  const char* prompt = NULL;
  if (getopt(argc, argv, "p:") == 'p') prompt = optarg;
  if (!prompt) {
    fprintf(stderr, "error: -p flag is required\n");
    return 1;
  }

  const char* base_url = model.base_url;
  const char* api_key = model.api_key;
  if (!base_url || !*base_url || !api_key || !*api_key) {
    fprintf(stderr, "error: base_url or api_key is not set in config.h\n");
    return 1;
  }

  if (!log_init(log_dir, log_level)) {
    fprintf(stderr, "error: failed to initialize logging\n");
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

  agent_run(agent, prompt);

  agent_delete(agent);
  http_quit();
  log_quit();
  return 0;
}
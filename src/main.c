#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "log.h"
#include "error.h"
#include "session.h"
#include "command/command.h"

#include "config.h"

typedef struct cmdopts {
  const char* prompt;
  const char* uuid;
} cmdopts_t;

static cmdopts_t cmdopts = {0};
static char* line = NULL;

static void parse_options(int argc, char* argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "p:r:")) != -1) {
    switch (opt) {
      case 'p': cmdopts.prompt = optarg; break;
      case 'r': cmdopts.uuid = optarg; break;
      default: break;
    }
  }
}

static char* command_generator(const char* text, int state) {
  static size_t index;
  static size_t len;
  if (!state) {
    index = 0;
    len = strlen(text) - 1; /* text includes the leading '/' */
  }
  const command_t* cmds = NULL;
  size_t n_cmd = command_get_all(&cmds);
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

static err_t readline_init() {
  log(INFO, "Initializing readline");
  rl_attempted_completion_function = command_completion;
  return ERROR_NONE;
}

static void readline_quit() {
  free(line);
  line = NULL;
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
  parse_options(argc, argv);

  const char* base_url = model.base_url;
  const char* api_key = model.api_key;
  if (!base_url || !*base_url || !api_key || !*api_key) {
    log(ERROR, "base_url or api_key is not set in config.h");
    return 1;
  }

  atexit(session_quit);
  atexit(http_quit);
  atexit(readline_quit);

  err_t err = ERROR_NONE;
  sesscfg_t config = {
      .run_dir = run_dir,
      .uuid_str = cmdopts.uuid,
      .log_level = log_level,
      .provider = provider_type,
      .model = &model,
      .system_prompt = system_prompt,
      .tools = tools,
      .n_tool = n_tool,
  };
  if ((err = session_init(&config)) != ERROR_NONE) {
    log(ERROR, "Failed to initialize session: %s\033[0m\n", error_str(err));
    return 1;
  }

  if ((err = http_init()) != ERROR_NONE) {
    log(ERROR, "Failed to initialize HTTP client: %s", error_str(err));
    return 1;
  }

  if ((err = readline_init()) != ERROR_NONE) {
    log(ERROR, "Failed to initialize readline: %s", error_str(err));
    return 1;
  }

  err_t (*run)(const char*) = model.stream ? session_run_stream : session_run;
  if (cmdopts.prompt) {
    err = run(cmdopts.prompt);
  } else {
    while (1) {
      char* trimmed_line = rl_get();
      if (!trimmed_line || !*trimmed_line) continue;
      if (*trimmed_line == '/') {
        const command_t* cmd = NULL;
        err = command_find(trimmed_line + 1, strlen(trimmed_line + 1), &cmd);
        if (err == ERROR_NONE) err = cmd->func(trimmed_line);
        else printf("\033[1;31mUnknown command: %s\033[0m\n", trimmed_line);
      } else {
        err = run(trimmed_line);
        if (err == ERROR_NONE) session_save();
      }
      if (line) free(line);
    }
  }

  return err;
}
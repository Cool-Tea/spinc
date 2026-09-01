#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "log.h"
#include "session.h"
#include "tool/tool.h"

static session_t current = {0};

static void ensure_directory(const char* dir, size_t dir_len) {
  struct stat st = {0};
  size_t path_len = 0;
  char* dir_mut = (char*)dir;  // Cast away const for in-place modification
  const char* end = dir + dir_len;
  for (const char *part = dir, *next; part < end; part = next) {
    next = memchr(part, PATH_SEP, end - part);
    if (!next) next = end;
    size_t len = next - part;
    path_len += len;
    dir_mut[path_len] = '\0';
    if (stat(dir_mut, &st) == -1) {
      mkdir(dir_mut, 0755);
    }
    dir_mut[path_len] = PATH_SEP;
    ++next;
    ++path_len;
  }
}

static err_t init_log(char* buf, size_t buf_len) {
  size_t log_dir_len = snprintf(buf, buf_len, "%s/log", current.dir);
  ensure_directory(buf, log_dir_len);
  char time_str[32];
  time_t now = time(NULL);
  struct tm* t = localtime(&now);
  strftime(time_str, sizeof(time_str), "%Y%m%d", t);
  snprintf(buf, buf_len, "%s/log/%s.log", current.dir, time_str);
  current.logf = fopen(buf, "a");
  if (!current.logf) return ERROR_IO;
  return ERROR_NONE;
}

err_t session_init(const sesscfg_t* config) {
  if (!config) return ERROR_NULLPTR;

  current.log_level = config->log_level;

  if (config->uuid_str) {
    uuid_parse(config->uuid_str, current.id);
  } else {
    uuid_generate(current.id);
  }
  char uuid_str[37];
  uuid_unparse(current.id, uuid_str);
  printf("\033[1;34mSession uuid: %s\033[0m\n", uuid_str);

  size_t buf_len = snprintf(NULL, 0, "%s/sessions/%s/log/19700101.log",
                            config->run_dir, uuid_str) +
                   1;
  char* buf = malloc(buf_len);
  if (!buf) return ERROR_OUT_OF_MEMORY;

  size_t sess_len =
      snprintf(buf, buf_len, "%s/sessions/%s", config->run_dir, uuid_str);
  current.dir = strdup(buf);
  current.dir_len = sess_len;
  if (!current.dir) {
    free(buf);
    return ERROR_OUT_OF_MEMORY;
  }

  ensure_directory(buf, sess_len);

  err_t err = init_log(buf, buf_len);
  if (err != ERROR_NONE) {
    free(buf);
    return err;
  }

  log(INFO, "Initializing toolset with %zu tools", config->n_tool);
  const toolset_t* toolset = global_toolset();
  err = toolset_new(&current.toolset);
  if (err != ERROR_NONE) {
    free(buf);
    log(ERROR, "Failed to initialize toolset: %s", error_str(err));
    return err;
  }
  for (size_t i = 0; i < config->n_tool; ++i) {
    const tool_t* tool = NULL;
    err_t err = toolset_find(toolset, config->tools[i],
                             strlen(config->tools[i]), &tool);
    if (err != ERROR_NONE) {
      log(WARN, "Skip not found tool '%s'", config->tools[i]);
      continue;
    }
    log(INFO, "Adding tool: %s", tool->def.name);
    err = toolset_add(current.toolset, tool);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to add tool '%s': %s", tool->def.name, error_str(err));
    }
  }

  const provider_t* provider = provider_get(config->provider);
  const model_t* model = config->model;
  log(INFO, "Initializing agent with model %s from provider %s", model->name,
      provider->name());
  err = agent_new(provider, model, config->system_prompt, current.toolset,
                  &current.agent);
  if (err != ERROR_NONE) {
    log(ERROR, "Failed to create agent: %s", provider->error_str(err));
    free(buf);
    return err;
  }

  snprintf(buf, buf_len, "%s/context.json", current.dir);
  log(INFO, "Loading context from file: %s", buf);
  FILE* ctxf = fopen(buf, "r");
  free(buf);
  if (ctxf) {
    fseek(ctxf, 0, SEEK_END);
    size_t ctx_len = ftell(ctxf);
    fseek(ctxf, 0, SEEK_SET);
    char* ctx_str = malloc(ctx_len + 1);
    if (!ctx_str) {
      log(ERROR, "Failed to allocate memory for context string");
      fclose(ctxf);
      return ERROR_OUT_OF_MEMORY;
    }
    fread(ctx_str, 1, ctx_len, ctxf);
    ctx_str[ctx_len] = '\0';
    err = provider->deserialize(current.agent->ctx, ctx_str, ctx_len);
    free(ctx_str);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to deserialize context: %s", provider->error_str(err));
      return err;
    }
    fclose(ctxf);
  }

  current.ready = true;
  return ERROR_NONE;
}

void session_quit() {
  if (!current.ready) return;
  session_save();
  char uuid_str[37];
  uuid_unparse(current.id, uuid_str);
  printf(
      "\033[1;34mYou can resume this session later with the uuid: %s\033[0m\n",
      uuid_str);
  if (current.agent) {
    current.agent->provider->delete_context(current.agent->ctx);
    free(current.agent);
    current.agent = NULL;
  }
  if (current.toolset) {
    toolset_delete(current.toolset);
    current.toolset = NULL;
  }
  if (current.logf) {
    fclose(current.logf);
    current.logf = NULL;
  }
  current.ready = false;
}

err_t session_save() {
  const provider_t* provider = current.agent->provider;
  size_t ctx_len = 0;
  char* ctx_str = NULL;
  err_t err = provider->serialize(current.agent->ctx, &ctx_str, &ctx_len);
  if (err != ERROR_NONE) {
    log(ERROR, "Failed to serialize provider context: %s",
        provider->error_str(err));
    return err;
  }

  size_t buf_len = snprintf(NULL, 0, "%s/context.json", current.dir) + 1;
  char* buf = malloc(buf_len);
  if (!buf) return ERROR_OUT_OF_MEMORY;
  snprintf(buf, buf_len, "%s/context.json", current.dir);
  FILE* ctxf = fopen(buf, "w");
  if (!ctxf) {
    log(ERROR, "Failed to open context file for writing: %s", buf);
    free(buf);
    free(ctx_str);
    return ERROR_IO;
  }
  fwrite(ctx_str, 1, ctx_len, ctxf);
  fclose(ctxf);
  free(buf);
  free(ctx_str);

  return ERROR_NONE;
}

err_t session_run(const char* user_input) {
  if (!current.ready) return ERROR_NOT_READY;
  return agent_run(current.agent, user_input);
}

err_t session_run_stream(const char* user_input) {
  if (!current.ready) return ERROR_NOT_READY;
  return agent_run_stream(current.agent, user_input);
}

FILE* session_log_file() { return current.logf ? current.logf : stderr; }

int session_log_level() { return current.log_level; }

const char* session_directory() { return current.dir; }

session_t* session_current() { return &current; }

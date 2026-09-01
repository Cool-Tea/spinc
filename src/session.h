#ifndef SESSION_H
#define SESSION_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <uuid/uuid.h>

#include "error.h"
#include "model.h"
#include "agent.h"
#include "tool/tool.h"
#include "provider/provider.h"

#define PATH_SEP '/'

typedef struct session_config {
  const char* run_dir;
  const char* uuid_str;
  int log_level;
  protyp_t provider;
  const model_t* model;
  const char* system_prompt;
  size_t n_tool;
  const char** tools;
} sesscfg_t;

typedef struct session {
  size_t dir_len;
  char* dir;
  bool ready;
  int log_level;
  uuid_t id;
  agent_t* agent;
  toolset_t* toolset;
  FILE* logf;
} session_t;

err_t session_init(const sesscfg_t* config);
void session_quit();
err_t session_save();
err_t session_run(const char* user_input);
err_t session_run_stream(const char* user_input);
size_t session_turn_count();
err_t session_turn_description(size_t index, char** description, size_t* len);
err_t session_rewind(size_t turn_index);
FILE* session_log_file();
int session_log_level();
const char* session_directory();
session_t* session_current();

#endif  // SESSION_H
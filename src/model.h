#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdbool.h>
#include "sjson.h"

#include "tool.h"

typedef enum protocol { OPENAI, ANTHROPIC } protocol_t;

typedef struct model {
  protocol_t protocol;
  float top_p;
  const char* name;
  const char* base_url;
  const char* api_key;
  const char* thinking;
  const char* reasoning_effort;
  long max_tokens;
} model_t;

typedef struct context {
  protocol_t protocol;
  const char* system_prompt;
  jnode_t* messages;
  size_t n_tools;
  const tool_t* tools;
} context_t;

typedef struct model_response {
  bool finished;
  char* raw;
  jnode_t* json;
  const char* content;
  const char* stop_reason;
  const char* reasoning;
  size_t n_tool_call;
  struct tool_call {
    const char* id;
    const char* name;
    char* args;
  }* tool_calls;
} mdres_t;

bool context_init(context_t* ctx, protocol_t protocol,
                  const char* system_prompt, const tool_t* tools,
                  size_t n_tools);
void context_clear(context_t* ctx);
void context_update(context_t* ctx, const mdres_t* res);
void context_add_user_message(context_t* ctx, const char* message);
void context_add_tool_message(context_t* ctx, const char* id,
                              const char* tool_name, const char* result);

mdres_t* call_api(const model_t* model, const context_t* ctx);
void model_response_delete(mdres_t* res);

#endif  // MODEL_H
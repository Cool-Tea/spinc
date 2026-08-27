#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdbool.h>
#include "sjson.h"

#include "tool.h"

typedef enum protocol : unsigned char { OPENAI, ANTHROPIC } protocol_t;

typedef struct model {
  protocol_t protocol;
  bool stream;
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

typedef struct tool_call {
  const char* id;
  const char* name;
  char* args;
} tool_call_t;

typedef struct model_response {
  bool finished;
  bool need_tool_call;
  char* raw;
  jnode_t* json;
  const char* id;
  const char* content;
  const char* stop_reason;
  const char* reasoning;
  size_t n_tool_call;
  tool_call_t* tool_calls;
} mdlres_t;

bool context_init(context_t* ctx, protocol_t protocol,
                  const char* system_prompt, const tool_t* tools,
                  size_t n_tools);
void context_clear(context_t* ctx);
void context_update(context_t* ctx, const mdlres_t* res);
void context_update_stream(context_t* ctx, const mdlres_t* res);
void context_add_user_message(context_t* ctx, const char* message);
void context_add_tool_message(context_t* ctx, const char* id,
                              const char* tool_name, const char* result);
size_t context_get_last_message_tool_calls(context_t* ctx, tool_call_t** calls);

mdlres_t* call_api(const model_t* model, const context_t* ctx);
bool call_api_stream(const model_t* model, const context_t* ctx,
                     void (*callback)(const mdlres_t* chunk, void* userp),
                     void* userp);
void model_response_delete(mdlres_t* res);

#endif  // MODEL_H
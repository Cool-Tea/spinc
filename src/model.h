#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include "sjson.h"

#include "tool.h"

typedef enum protocol { OPENAI, ANTHROPIC } protocol_t;

typedef struct model {
  const char* name;
  const char* base_url;
  const char* api_key;
} model_t;

char* call_api(const model_t* model, protocol_t protocol, jnode_t* messages,
               const tool_t* tools, size_t n_tools);

#endif  // MODEL_H
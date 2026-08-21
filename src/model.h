#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdbool.h>
#include "sjson.h"

#include "tool.h"

typedef enum protocol : unsigned char { OPENAI, ANTHROPIC } protocol_t;

typedef struct model {
  protocol_t protocol;
  bool thinking;
  float top_p;
  const char* name;
  const char* base_url;
  const char* api_key;
  const char* reasoning_effort;
} model_t;

char* call_api(const model_t* model, jnode_t* messages, const tool_t* tools,
               size_t n_tools);

#endif  // MODEL_H
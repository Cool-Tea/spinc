#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include "sjson.h"

#include "tool.h"
#include "model.h"

typedef struct agent {
  const model_t* model;
  context_t ctx;
} agent_t;

agent_t* agent_new(const model_t* model, const char* system_prompt,
                   const tool_t* tools, size_t n_tools);
void agent_delete(agent_t* agent);
bool agent_run(agent_t* agent, const char* user_input);

#endif  // AGENT_H
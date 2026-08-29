#ifndef AGENT_H
#define AGENT_H

#include "error.h"
#include "provider/provider.h"

typedef struct agent {
  const provider_t* provider;
  void* ctx;
} agent_t;

err_t agent_new(const provider_t* provider, const model_t* model,
                const char* system_prompt, const toolset_t* toolset,
                agent_t** agent);
void agent_delete(agent_t* agent);
err_t agent_run(agent_t* agent, const char* user_input);
err_t agent_run_stream(agent_t* agent, const char* user_input);

#endif  // AGENT_H
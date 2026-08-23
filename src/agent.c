#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "agent.h"

agent_t* agent_new(const model_t* model, const char* system_prompt,
                   const tool_t* tools, size_t n_tools) {
  log(INFO, "Creating agent for model %s", model->name);
  agent_t* agent = malloc(sizeof(agent_t));
  if (!agent) return NULL;
  agent->model = model;
  if (!context_init(&agent->ctx, model->protocol, system_prompt, tools,
                    n_tools)) {
    free(agent);
    return NULL;
  }
  return agent;
}

void agent_delete(agent_t* agent) {
  if (!agent) return;
  context_clear(&agent->ctx);
  free(agent);
}

bool agent_run(agent_t* agent, const char* user_input) {
  context_add_user_message(&agent->ctx, user_input);

  bool running = true;
  while (running) {
    mdres_t* resp = call_api(agent->model, &agent->ctx);
    if (!resp) {
      log(ERROR, "Failed to call API");
      return false;
    }
    log(DEBUG, "Response: %s", resp->raw);

    context_update(&agent->ctx, resp);

    running = !resp->finished;
    if (resp->reasoning) {
      printf("[Reasoning] %s\n", resp->reasoning);
    }
    if (resp->content) {
      printf("%s\n", resp->content);
    }
    if (resp->n_tool_call) {
      for (size_t i = 0; i < resp->n_tool_call; ++i) {
        struct tool_call* call = &resp->tool_calls[i];
        const char* id = call->id;
        const char* name = call->name;
        const char* args = call->args;
        const tool_t* tool = NULL;
        for (size_t j = 0; j < agent->ctx.n_tools; ++j) {
          if (strcmp(agent->ctx.tools[j].def.name, name) == 0) {
            tool = &agent->ctx.tools[j];
            break;
          }
        }
        if (!tool) {
          log(ERROR, "Tool %s not found", name);
          model_response_delete(resp);
          context_add_tool_message(&agent->ctx, id, name,
                                   "{\"error\": \"Tool not found\"}");
        } else {
          printf("[Tool Call] %s\n", name);
          char* result = tool->func(args);
          log(DEBUG, "Tool '%s' returned: %s", tool->def.name, result);
          context_add_tool_message(&agent->ctx, id, name, result);
          free(result);
        }
      }
    }
    model_response_delete(resp);
  }
  return true;
}
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "agent.h"

struct sse_context {
  enum { START, REASONING, CONTENT, TOOL_CALL } line_type;
  bool* running;
  agent_t* agent;
};

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
    mdlres_t* resp = call_api(agent->model, &agent->ctx);
    if (!resp) {
      log(ERROR, "Failed to call API");
      return false;
    }
    log(DEBUG, "Response: %s", resp->raw);

    context_update(&agent->ctx, resp);

    running = !resp->finished;
    if (resp->reasoning) {
      printf("\033[2m[Reasoning] %s\033[0m\n", resp->reasoning);
    }
    if (resp->content) {
      printf("\033[1m%s\033[0m\n", resp->content);
    }
    if (resp->need_tool_call) {
      for (size_t i = 0; i < resp->n_tool_call; ++i) {
        tool_call_t* call = &resp->tool_calls[i];
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
          printf("\033[2m[Tool Call] %s\033[0m\n", name);
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

static void sse_callback(const mdlres_t* chunk, void* userp) {
  struct sse_context* ctx = (struct sse_context*)userp;
  agent_t* agent = ctx->agent;

  log(DEBUG, "Response: %s", chunk->raw);

  context_update_stream(&agent->ctx, chunk);

  *ctx->running = !chunk->finished;
  if (chunk->finished) printf("\n");
  if (chunk->reasoning) {
    if (ctx->line_type != REASONING) {
      if (ctx->line_type != START) printf("\n");
      printf("\033[2m[Reasoning] \033[0m");
    }
    ctx->line_type = REASONING;
    printf("\033[2m%s\033[0m", chunk->reasoning);
    fflush(stdout);
  }
  if (chunk->content) {
    if (ctx->line_type != START && ctx->line_type != CONTENT) printf("\n");
    ctx->line_type = CONTENT;
    printf("\033[1m%s\033[0m", chunk->content);
    fflush(stdout);
  }
  if (chunk->need_tool_call) {
    tool_call_t* calls = NULL;
    size_t n_tool_call =
        context_get_last_message_tool_calls(&agent->ctx, &calls);
    for (size_t i = 0; i < n_tool_call; ++i) {
      tool_call_t* call = &calls[i];
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
        context_add_tool_message(&agent->ctx, id, name,
                                 "{\"error\": \"Tool not found\"}");
      } else {
        if (ctx->line_type != START && ctx->line_type != TOOL_CALL) {
          printf("\n");
        }
        printf("\033[2m[Tool Call] %s\033[0m", name);
        ctx->line_type = TOOL_CALL;
        char* result = tool->func(args);
        log(DEBUG, "Tool '%s' returned: %s", tool->def.name, result);
        context_add_tool_message(&agent->ctx, id, name, result);
        free(result);
      }
      free(call->args);
    }
    free(calls);
  }
}

bool agent_run_stream(agent_t* agent, const char* user_input) {
  context_add_user_message(&agent->ctx, user_input);

  bool running = true;
  struct sse_context ctx = {
      .line_type = START, .running = &running, .agent = agent};
  while (running) {
    if (!call_api_stream(agent->model, &agent->ctx, sse_callback, &ctx)) {
      log(ERROR, "Failed to call API (streaming)");
      return false;
    }
  }
  return true;
}
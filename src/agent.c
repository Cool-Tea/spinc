#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "agent.h"

struct sse_context {
  enum { SSE_START, SSE_REASONING, SSE_CONTENT, SSE_TOOL_CALL } line_type;
  bool* running;
  agent_t* agent;
};

err_t agent_new(const provider_t* provider, const model_t* model,
                const char* system_prompt, const toolset_t* toolset,
                agent_t** agent) {
  *agent = malloc(sizeof(agent_t));
  if (!*agent) return ERROR_OUT_OF_MEMORY;
  agent_t* ag = *agent;
  ag->provider = provider;
  ag->ctx = NULL;
  err_t err = provider->create_context(&ag->ctx);
  if (err != ERROR_NONE) {
    free(ag);
    *agent = NULL;
    return err;
  }
  err = provider->set_model(ag->ctx, model);
  if (err != ERROR_NONE) {
    provider->delete_context(ag->ctx);
    free(ag);
    *agent = NULL;
    return err;
  }
  err = provider->set_system_prompt(ag->ctx, system_prompt);
  if (err != ERROR_NONE) {
    provider->delete_context(ag->ctx);
    free(ag);
    *agent = NULL;
    return err;
  }
  err = provider->set_toolset(ag->ctx, toolset);
  if (err != ERROR_NONE) {
    provider->delete_context(ag->ctx);
    free(ag);
    *agent = NULL;
    return err;
  }
  return ERROR_NONE;
}

void agent_delete(agent_t* agent) {
  if (!agent) return;
  agent->provider->delete_context(agent->ctx);
  free(agent);
}

static size_t rewind_turn_index(const provider_t* provider, void* context) {
  size_t turns = provider->turn_count(context);
  return turns > 0 ? turns - 1 : 0;
}

err_t agent_run(agent_t* agent, const char* user_input) {
  const provider_t* provider = agent->provider;
  provider->add_user_message(agent->ctx, user_input);

  bool running = true;
  size_t resp_len = 0;
  char* resp = NULL;
  err_t err = ERROR_NONE;
  while (running) {
    err = provider->call(agent->ctx, &resp, &resp_len);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to call API: %s", provider->error_str(err));
      provider->rewind(agent->ctx, rewind_turn_index(provider, agent->ctx));
      return err;
    }
    log(DEBUG, "API response: %*s", (int)resp_len, resp);

    err = provider->update(agent->ctx, resp, resp_len);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to update context: %s", provider->error_str(err));
      free(resp);
      provider->rewind(agent->ctx, rewind_turn_index(provider, agent->ctx));
      return err;
    }
    free(resp);

    running = !provider->is_finished(agent->ctx);

    const char* reasoning = provider->latest_reasoning(agent->ctx);
    if (reasoning) {
      printf("\033[2m[Reasoning] %s\033[0m\n", reasoning);
    }

    const char* content = provider->latest_content(agent->ctx);
    if (content) {
      printf("\033[1m%s\033[0m\n", content);
    }

    size_t n_call = 0;
    toolcall_t* calls = NULL;
    err = provider->latest_tool_calls(agent->ctx, &calls, &n_call);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to get latest tool calls: %s",
          provider->error_str(err));
      provider->rewind(agent->ctx, rewind_turn_index(provider, agent->ctx));
      return err;
    } else if (n_call > 0) {
      const toolset_t* toolset = provider->get_toolset(agent->ctx);
      for (size_t i = 0; i < n_call; ++i) {
        toolcall_t* call = &calls[i];
        const char* id = call->call_id;
        const char* name = call->name;
        const char* args = call->args;
        const tool_t* tool = NULL;
        err = toolset_find(toolset, name, call->name_len, &tool);
        if (err != ERROR_NONE) {
          const char* error_str = provider->error_str(err);
          log(ERROR, "Failed to find tool %s: %s", name, error_str);
          err = provider->add_tool_message(agent->ctx, id, name, error_str);
          if (err != ERROR_NONE) {
            log(ERROR, "Failed to add tool message: %s",
                provider->error_str(err));
            break;
          }
        } else {
          printf("\033[2m[Tool Call] %s\033[0m\n", name);
          size_t result_len = 0;
          char* result = NULL;
          err = tool->func(args, call->args_len, &result, &result_len);
          if (err != ERROR_NONE) {
            const char* error_str = provider->error_str(err);
            log(ERROR, "Failed to execute tool '%s': %s", tool->def.name,
                error_str);
            err = provider->add_tool_message(agent->ctx, id, name, error_str);
            if (err != ERROR_NONE) {
              log(ERROR, "Failed to add tool message: %s",
                  provider->error_str(err));
              break;
            }
          } else {
            log(DEBUG, "Tool '%s' returned: %s", tool->def.name, result);
            err = provider->add_tool_message(agent->ctx, id, name, result);
            free(result);
            if (err != ERROR_NONE) {
              log(ERROR, "Failed to add tool message: %s",
                  provider->error_str(err));
              break;
            }
          }
        }
      }
      free(calls);
      if (err != ERROR_NONE) {
        provider->rewind(agent->ctx, rewind_turn_index(provider, agent->ctx));
        return err;
      }
    }
  }

  return ERROR_NONE;
}

static err_t sse_callback(void* context, const event_t* event, void* userp) {
  struct sse_context* ctx = (struct sse_context*)userp;
  agent_t* agent = ctx->agent;
  const provider_t* provider = agent->provider;

  log(DEBUG, "Event: type=%*s, data=%*s", (int)event->event_len, event->event,
      (int)event->data_len, event->data);

  err_t err =
      provider->update(context, (const char*)event->data, event->data_len);
  if (err != ERROR_NONE) {
    log(ERROR, "Failed to update context: %s", provider->error_str(err));
    provider->rewind(context, rewind_turn_index(provider, context));
    return err;
  }

  bool finished = provider->is_finished(context);
  if (finished) {
    *ctx->running = false;
    printf("\n");
  }

  const char* reasoning = provider->latest_reasoning(context);
  if (reasoning) {
    if (ctx->line_type != SSE_REASONING) {
      if (ctx->line_type != SSE_START) printf("\n");
      printf("\033[2m[Reasoning] \033[0m");
    }
    ctx->line_type = SSE_REASONING;
    printf("\033[2m%s\033[0m", reasoning);
    fflush(stdout);
  }

  const char* content = provider->latest_content(context);
  if (content) {
    if (ctx->line_type != SSE_START && ctx->line_type != SSE_CONTENT)
      printf("\n");
    ctx->line_type = SSE_CONTENT;
    printf("\033[1m%s\033[0m", content);
    fflush(stdout);
  }

  size_t n_call = 0;
  toolcall_t* calls = NULL;
  err = provider->latest_tool_calls(context, &calls, &n_call);
  if (err != ERROR_NONE) {
    log(ERROR, "Failed to get latest tool calls: %s", provider->error_str(err));
    provider->rewind(context, rewind_turn_index(provider, context));
    return err;
  } else if (n_call > 0) {
    const toolset_t* toolset = provider->get_toolset(context);
    for (size_t i = 0; i < n_call; ++i) {
      toolcall_t* call = &calls[i];
      const char* id = call->call_id;
      const char* name = call->name;
      const char* args = call->args;
      const tool_t* tool = NULL;
      err = toolset_find(toolset, name, call->name_len, &tool);
      if (err != ERROR_NONE) {
        const char* error_str = provider->error_str(err);
        log(ERROR, "Failed to find tool %s: %s", name, error_str);
        err = provider->add_tool_message(context, id, name, error_str);
        if (err != ERROR_NONE) {
          log(ERROR, "Failed to add tool message: %s",
              provider->error_str(err));
          break;
        }
      } else {
        if (ctx->line_type != SSE_START) printf("\n");
        printf("\033[2m[Tool Call] %s\033[0m", name);
        ctx->line_type = SSE_TOOL_CALL;
        size_t result_len = 0;
        char* result = NULL;
        err = tool->func(args, call->args_len, &result, &result_len);
        if (err != ERROR_NONE) {
          const char* error_str = provider->error_str(err);
          log(ERROR, "Failed to execute tool '%s': %s", tool->def.name,
              error_str);
          err = provider->add_tool_message(context, id, name, error_str);
          if (err != ERROR_NONE) {
            log(ERROR, "Failed to add tool message: %s",
                provider->error_str(err));
            break;
          }
        } else {
          log(DEBUG, "Tool '%s' returned: %s", tool->def.name, result);
          err = provider->add_tool_message(context, id, name, result);
          free(result);
          if (err != ERROR_NONE) {
            log(ERROR, "Failed to add tool message: %s",
                provider->error_str(err));
            break;
          }
        }
      }
    }
    free(calls);
    if (err != ERROR_NONE) {
      provider->rewind(context, rewind_turn_index(provider, context));
      return err;
    }
  }
  return ERROR_NONE;
}

err_t agent_run_stream(agent_t* agent, const char* user_input) {
  const provider_t* provider = agent->provider;
  provider->add_user_message(agent->ctx, user_input);

  bool running = true;
  err_t err = ERROR_NONE;
  struct sse_context ctx = {
      .line_type = SSE_START, .running = &running, .agent = agent};
  while (running) {
    err = provider->call_stream(agent->ctx, sse_callback, &ctx);
    if (err != ERROR_NONE) {
      log(ERROR, "Failed to call API (streaming): %s",
          provider->error_str(err));
      provider->rewind(agent->ctx, rewind_turn_index(provider, agent->ctx));
      return err;
    }
  }
  return ERROR_NONE;
}
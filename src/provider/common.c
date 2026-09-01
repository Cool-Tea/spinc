#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sjson.h"

#include "provider/common.h"

err_t pctx_new(pctx_t** out) {
  if (!out) return ERROR_NULLPTR;
  *out = NULL;
  pctx_t* ctx = malloc(sizeof(pctx_t));
  if (!ctx) return ERROR_OUT_OF_MEMORY;
  memset(ctx, 0, sizeof(pctx_t));
  err_t err = msglist_new(&ctx->messages);
  if (err != ERROR_NONE) {
    free(ctx);
    return err;
  }
  *out = ctx;
  return ERROR_NONE;
}

static void pctx_free_latest_calls(pctx_t* ctx) {
  if (!ctx->latest_calls) return;
  for (size_t i = 0; i < ctx->n_latest_call; ++i) {
    free(ctx->latest_calls[i].id);
    free(ctx->latest_calls[i].call_id);
    free(ctx->latest_calls[i].name);
    free(ctx->latest_calls[i].args);
  }
  free(ctx->latest_calls);
  ctx->latest_calls = NULL;
  ctx->n_latest_call = 0;
}

void pctx_clear_latest(pctx_t* ctx) {
  if (!ctx) return;
  free(ctx->latest_content);
  free(ctx->latest_reasoning);
  free(ctx->latest_stop_reason);
  ctx->latest_content = NULL;
  ctx->latest_reasoning = NULL;
  ctx->latest_stop_reason = NULL;
  pctx_free_latest_calls(ctx);
  ctx->tool_calls_ready = false;
}

void pctx_delete(pctx_t* ctx) {
  if (!ctx) return;
  free(ctx->model);
  if (ctx->toolset) toolset_delete(ctx->toolset);
  free(ctx->system_prompt);
  free(ctx->stream_turn_id);
  if (ctx->snapshot) msglist_delete(ctx->snapshot);
  if (ctx->messages) msglist_delete(ctx->messages);
  pctx_clear_latest(ctx);
  free(ctx);
}

err_t pctx_set_model(pctx_t* ctx, const model_t* model) {
  if (!ctx || !model) return ERROR_NULLPTR;
  if (!ctx->model) {
    ctx->model = malloc(sizeof(model_t));
    if (!ctx->model) return ERROR_OUT_OF_MEMORY;
  }
  memcpy(ctx->model, model, sizeof(model_t));
  return ERROR_NONE;
}

model_t* pctx_get_model(pctx_t* ctx) { return ctx ? ctx->model : NULL; }

err_t pctx_set_toolset(pctx_t* ctx, const toolset_t* toolset) {
  if (!ctx || !toolset) return ERROR_NULLPTR;
  toolset_t* copy = NULL;
  err_t err = toolset_copy(toolset, &copy);
  if (err != ERROR_NONE) return err;
  if (ctx->toolset) toolset_delete(ctx->toolset);
  ctx->toolset = copy;
  return ERROR_NONE;
}

toolset_t* pctx_get_toolset(pctx_t* ctx) { return ctx ? ctx->toolset : NULL; }

err_t pctx_set_system_prompt(pctx_t* ctx, const char* system_prompt) {
  if (!ctx || !system_prompt) return ERROR_NULLPTR;
  char* copy = strdup(system_prompt);
  if (!copy) return ERROR_OUT_OF_MEMORY;
  if (ctx->system_prompt) free(ctx->system_prompt);
  ctx->system_prompt = copy;
  return ERROR_NONE;
}

const char* pctx_get_system_prompt(const pctx_t* ctx) {
  return ctx ? ctx->system_prompt : NULL;
}

void pctx_take_snapshot(pctx_t* ctx) {
  if (!ctx || ctx->snapshot) return;
  msglist_copy(ctx->messages, &ctx->snapshot);
}

void pctx_rewind(pctx_t* ctx) {
  if (!ctx) return;
  if (ctx->snapshot) {
    msglist_delete(ctx->messages);
    ctx->messages = ctx->snapshot;
    ctx->snapshot = NULL;
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
  pctx_clear_latest(ctx);
}

void pctx_clear_messages(pctx_t* ctx) {
  if (!ctx) return;
  if (ctx->snapshot) {
    msglist_delete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  if (ctx->messages) msglist_delete(ctx->messages);
  msglist_new(&ctx->messages);
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
  pctx_clear_latest(ctx);
}

void pctx_pop_message(pctx_t* ctx) {
  if (!ctx || !ctx->messages) return;
  if (ctx->messages->n_message > 0) msglist_pop_message_delete(ctx->messages);
  pctx_clear_latest(ctx);
}

char* pctx_new_turn_id(void) {
  static size_t counter = 0;
  char buf[64];
  snprintf(buf, sizeof(buf), "turn_%zu", counter++);
  return strdup(buf);
}

err_t pctx_append_text(char** text, size_t* text_len, const char* frag) {
  if (!text || !frag) return ERROR_NULLPTR;
  jnode_t* js = jstring_new(0, *text ? *text : "");
  if (!js) return ERROR_OUT_OF_MEMORY;
  jstring_concat(js, frag);
  char* joined = strdup(jstring_content(js));
  jdelete(js);
  if (!joined) return ERROR_OUT_OF_MEMORY;
  free(*text);
  *text = joined;
  if (text_len) *text_len = strlen(joined);
  return ERROR_NONE;
}

err_t pctx_add_user_message(pctx_t* ctx, const char* text) {
  if (!ctx || !text) return ERROR_NULLPTR;
  message_t* m = message_new(USER);
  if (!m) return ERROR_OUT_OF_MEMORY;
  m->user.text = strdup(text);
  if (!m->user.text) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  m->user.text_len = strlen(m->user.text);
  return msglist_add_message(ctx->messages, m);
}

err_t pctx_add_assistant_message(pctx_t* ctx, const char* text) {
  if (!ctx || !text) return ERROR_NULLPTR;
  message_t* m = message_new(ASSISTANT);
  if (!m) return ERROR_OUT_OF_MEMORY;
  m->assistant.text = strdup(text);
  if (!m->assistant.text) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  m->assistant.text_len = strlen(m->assistant.text);
  return msglist_add_message(ctx->messages, m);
}

err_t pctx_add_tool_message(pctx_t* ctx, const char* call_id,
                            const char* tool_name, const char* result) {
  if (!ctx || !call_id || !result) return ERROR_NULLPTR;
  message_t* m = message_new(TOOL_RESULT);
  if (!m) return ERROR_OUT_OF_MEMORY;
  m->tool_result.call_id = strdup(call_id);
  m->tool_result.name = tool_name ? strdup(tool_name) : NULL;
  m->tool_result.result = strdup(result);
  if (!m->tool_result.call_id || !m->tool_result.result ||
      (tool_name && !m->tool_result.name)) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  m->tool_result.call_id_len = strlen(m->tool_result.call_id);
  if (m->tool_result.name)
    m->tool_result.name_len = strlen(m->tool_result.name);
  m->tool_result.result_len = strlen(m->tool_result.result);
  return msglist_add_message(ctx->messages, m);
}

size_t pctx_message_count(const pctx_t* ctx) {
  return ctx ? ctx->messages->n_message : 0;
}

err_t pctx_set_tool_calls(pctx_t* ctx, size_t n, const char* const* ids,
                          const char* const* names, const char* const* args) {
  if (!ctx) return ERROR_NULLPTR;
  pctx_free_latest_calls(ctx);
  if (n == 0) return ERROR_NONE;
  ctx->latest_calls = calloc(n, sizeof(toolcall_t));
  if (!ctx->latest_calls) return ERROR_OUT_OF_MEMORY;
  ctx->n_latest_call = n;
  for (size_t i = 0; i < n; ++i) {
    toolcall_t* call = &ctx->latest_calls[i];
    if (ids && ids[i]) {
      call->call_id = strdup(ids[i]);
      call->call_id_len = strlen(ids[i]);
    }
    if (names && names[i]) {
      call->name = strdup(names[i]);
      call->name_len = strlen(names[i]);
    }
    if (args && args[i]) {
      call->args = strdup(args[i]);
      call->args_len = strlen(args[i]);
    }
  }
  return ERROR_NONE;
}

err_t pctx_latest_tool_calls(pctx_t* ctx, toolcall_t** calls, size_t* n) {
  if (!ctx || !calls || !n) return ERROR_NULLPTR;
  *calls = NULL;
  *n = 0;
  if (!ctx->tool_calls_ready || ctx->n_latest_call == 0) return ERROR_NONE;
  toolcall_t* out = malloc(ctx->n_latest_call * sizeof(toolcall_t));
  if (!out) return ERROR_OUT_OF_MEMORY;
  memcpy(out, ctx->latest_calls, ctx->n_latest_call * sizeof(toolcall_t));
  *calls = out;
  *n = ctx->n_latest_call;
  return ERROR_NONE;
}

// Rebuild latest_calls from the TOOL_CALL messages of the trailing assistant
// turn (a suffix of REASONING/ASSISTANT/TOOL_CALL messages sharing one id).
err_t pctx_latest_calls_from_turn(pctx_t* ctx) {
  if (!ctx || !ctx->messages) return ERROR_NULLPTR;
  size_t n = ctx->messages->n_message;
  const char* tid = NULL;
  size_t start = n;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (!tid) tid = mid;
    if (mid && tid && strcmp(mid, tid) != 0) break;
    start = i - 1;
  }

  size_t n_tool = 0;
  for (size_t i = start; i < n; ++i) {
    if (ctx->messages->messages[i].type == TOOL_CALL) ++n_tool;
  }
  if (n_tool == 0) return pctx_set_tool_calls(ctx, 0, NULL, NULL, NULL);

  const char** ids = calloc(n_tool, sizeof(char*));
  const char** names = calloc(n_tool, sizeof(char*));
  const char** args = calloc(n_tool, sizeof(char*));
  if (!ids || !names || !args) {
    free(ids);
    free(names);
    free(args);
    return ERROR_OUT_OF_MEMORY;
  }
  size_t k = 0;
  for (size_t i = start; i < n && k < n_tool; ++i) {
    message_t* m = &ctx->messages->messages[i];
    if (m->type != TOOL_CALL) continue;
    ids[k] = m->tool_call.call_id;
    names[k] = m->tool_call.name;
    args[k] = m->tool_call.args;
    ++k;
  }
  err_t err = pctx_set_tool_calls(ctx, k, ids, names, args);
  free(ids);
  free(names);
  free(args);
  return err;
}

err_t pctx_serialize(const pctx_t* ctx, char** data, size_t* len) {
  if (!ctx || !data || !len) return ERROR_NULLPTR;
  *data = NULL;
  *len = 0;
  jnode_t* root = jobject_new();
  if (!root) return ERROR_OUT_OF_MEMORY;
  jnode_t* jmsg = NULL;
  err_t err = msglist_to_json(ctx->messages, &jmsg);
  if (err != ERROR_NONE) {
    jdelete(root);
    return err;
  }
  jobject_put(root, "messages", jmsg);
  *data = jto_string(root);
  jdelete(root);
  if (!*data) return ERROR_OUT_OF_MEMORY;
  *len = strlen(*data);
  return ERROR_NONE;
}

err_t pctx_deserialize(pctx_t* ctx, const char* data, size_t len) {
  if (!ctx || !data) return ERROR_NULLPTR;
  jnode_t* root = jfrom_string(data, (int)len);
  if (!root) return ERROR_UNKNOWN;
  jnode_t* jmsg = jobject_get(root, "messages");
  if (jis_array(jmsg)) {
    msglist_t* ml = NULL;
    err_t err = msglist_from_json(jmsg, &ml);
    if (err == ERROR_NONE) {
      msglist_delete(ctx->messages);
      ctx->messages = ml;
    }
  }
  jdelete(root);
  return ERROR_NONE;
}

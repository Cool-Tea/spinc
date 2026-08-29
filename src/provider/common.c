#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "provider/common.h"

err_t pctx_new(pctx_t** out) {
  if (!out) return ERROR_NULLPTR;
  *out = NULL;
  pctx_t* ctx = calloc(1, sizeof(pctx_t));
  if (!ctx) return ERROR_OUT_OF_MEMORY;
  ctx->messages = jarray_new();
  if (!ctx->messages) {
    free(ctx);
    return ERROR_OUT_OF_MEMORY;
  }
  *out = ctx;
  return ERROR_NONE;
}

static void pctx_free_latest_calls(pctx_t* ctx) {
  if (!ctx->latest_calls) return;
  for (size_t i = 0; i < ctx->n_latest_call; ++i) {
    free((void*)ctx->latest_calls[i].id);
    free((void*)ctx->latest_calls[i].name);
    free((void*)ctx->latest_calls[i].args);
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
  free(ctx->system_prompt);
  if (ctx->snapshot) jdelete(ctx->snapshot);
  if (ctx->messages) jdelete(ctx->messages);
  pctx_clear_latest(ctx);
  free(ctx);
}

void pctx_take_snapshot(pctx_t* ctx) {
  if (!ctx || ctx->snapshot) return;
  ctx->snapshot = jcopy(ctx->messages);
}

void pctx_rewind(pctx_t* ctx) {
  if (!ctx) return;
  if (ctx->snapshot) {
    jdelete(ctx->messages);
    ctx->messages = ctx->snapshot;
    ctx->snapshot = NULL;
  }
  pctx_clear_latest(ctx);
}

void pctx_clear_messages(pctx_t* ctx) {
  if (!ctx) return;
  if (ctx->snapshot) {
    jdelete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  jdelete(ctx->messages);
  ctx->messages = jarray_new();
  pctx_clear_latest(ctx);
}

void pctx_pop_message(pctx_t* ctx) {
  if (!ctx || !ctx->messages) return;
  if (jarray_size(ctx->messages) > 0) jarray_pop(ctx->messages);
  pctx_clear_latest(ctx);
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
      call->id = strdup(ids[i]);
      call->id_len = strlen(ids[i]);
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

err_t pctx_serialize(const pctx_t* ctx, char** data, size_t* len) {
  if (!ctx || !data || !len) return ERROR_NULLPTR;
  *data = NULL;
  *len = 0;
  jnode_t* root = jobject_new();
  if (!root) return ERROR_OUT_OF_MEMORY;
  jobject_put(root, "system_prompt",
              jstring_new(0, ctx->system_prompt ? ctx->system_prompt : ""));
  jobject_put(root, "messages", jcopy(ctx->messages));
  *data = jto_string(root);
  jdelete(root);
  if (!*data) return ERROR_OUT_OF_MEMORY;
  *len = strlen(*data);
  return ERROR_NONE;
}

err_t pctx_deserialize(const char* data, size_t len, pctx_t** out) {
  if (!data || !out) return ERROR_NULLPTR;
  *out = NULL;
  jnode_t* root = jfrom_string(data, (int)len);
  if (!root) return ERROR_UNKNOWN;
  pctx_t* ctx = NULL;
  err_t err = pctx_new(&ctx);
  if (err != ERROR_NONE) {
    jdelete(root);
    return err;
  }
  jnode_t* jsp = jobject_get(root, "system_prompt");
  if (jis_string(jsp)) ctx->system_prompt = strdup(jstring_content(jsp));
  jnode_t* jmsg = jobject_get(root, "messages");
  if (jis_array(jmsg)) {
    jdelete(ctx->messages);
    ctx->messages = jcopy(jmsg);
  }
  jdelete(root);
  *out = ctx;
  return ERROR_NONE;
}

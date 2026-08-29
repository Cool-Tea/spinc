#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "provider/common.h"
#include "provider/openai.h"
#include "provider/deepseek.h"

// DeepSeek speaks the OpenAI-compatible Chat Completions protocol, so it
// reuses the shared implementation (openai.c) and only differs in its
// type/name plus the reasoning extensions.

static err_t deepseek_update_full(pctx_t* ctx, jnode_t* jfirst) {
  err_t err = openai_update_full(ctx, jfirst);
  if (err != ERROR_NONE) return err;

  jnode_t* jmessage = jobject_get(jfirst, "message");
  jnode_t* jreasoning = jobject_get(jmessage, "reasoning_content");
  if (jis_string(jreasoning) && jstring_len(jreasoning)) {
    ctx->latest_reasoning = strdup(jstring_content(jreasoning));
  }

  return ERROR_NONE;
}

static err_t deepseek_update_stream(pctx_t* ctx, jnode_t* jfirst,
                                    jnode_t* jdelta) {
  err_t err = openai_update_stream(ctx, jfirst, jdelta);
  if (err != ERROR_NONE) return err;

  jnode_t* jlast = jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jreasoning = jobject_get(jdelta, "reasoning_content");
  if (jis_string(jreasoning)) {
    free(ctx->latest_reasoning);
    ctx->latest_reasoning = strdup(jstring_content(jreasoning));
    jnode_t* jmsg_r = jobject_get(jlast, "reasoning_content");
    if (!jis_string(jmsg_r)) {
      jobject_put(jlast, "reasoning_content", jcopy(jreasoning));
    } else {
      jstring_concat(jmsg_r, jstring_content(jreasoning));
    }
  }

  return ERROR_NONE;
}

static protyp_t deepseek_type() { return DEEPSEEK; }
static const char* deepseek_name() { return "DeepSeek"; }

err_t deepseek_update(void* context, const char* response, size_t len) {
  pctx_t* ctx = context;
  if (!ctx || !response) return ERROR_NULLPTR;
  if (openai_done_marker(response, len)) {
    pctx_clear_latest(ctx);
    return ERROR_NONE;
  }
  pctx_clear_latest(ctx);
  pctx_take_snapshot(ctx);

  jnode_t* json = jfrom_string(response, (int)len);
  if (!json) {
    log(ERROR, "Failed to parse response JSON: %s", jerror());
    return ERROR_UNKNOWN;
  }
  jnode_t* jchoices = jobject_get(json, "choices");
  jnode_t* jfirst =
      jchoices && jarray_size(jchoices) > 0 ? jarray_get(jchoices, 0) : NULL;
  if (!jfirst) {
    jdelete(json);
    log(ERROR, "Response has no choices");
    return ERROR_UNKNOWN;
  }
  jnode_t* jdelta = jobject_get(jfirst, "delta");
  err_t err = ERROR_NONE;
  if (jdelta) {
    err = deepseek_update_stream(ctx, jfirst, jdelta);
  } else {
    err = deepseek_update_full(ctx, jfirst);
  }
  jdelete(json);
  return err;
}

static const provider_t deepseek_provider = {
    .type = deepseek_type,
    .name = deepseek_name,
    .error_str = cc_error_str,
    .create_context = cc_create_context,
    .delete_context = cc_delete_context,
    .serialize = cc_serialize,
    .deserialize = cc_deserialize,
    .set_model = cc_set_model,
    .get_model = cc_get_model,
    .set_toolset = cc_set_toolset,
    .get_toolset = cc_get_toolset,
    .set_system_prompt = cc_set_system_prompt,
    .get_system_prompt = cc_get_system_prompt,
    .message_count = cc_message_count,
    .add_user_message = cc_add_user_message,
    .add_assistant_message = cc_add_assistant_message,
    .add_tool_message = cc_add_tool_message,
    .clear_messages = cc_clear_messages,
    .pop_message = cc_pop_message,
    .call = cc_call,
    .call_stream = cc_call_stream,
    .update = deepseek_update,
    .rewind = cc_rewind,
    .is_finished = cc_is_finished,
    .latest_stop_reason = cc_latest_stop_reason,
    .latest_reasoning = cc_latest_reasoning,
    .latest_content = cc_latest_content,
    .latest_tool_calls = cc_latest_tool_calls,
};

const provider_t* get_deepseek_provider() { return &deepseek_provider; }

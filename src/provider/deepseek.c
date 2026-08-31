#include <stdlib.h>
#include <string.h>

#include "provider/common.h"
#include "provider/openai.h"
#include "provider/deepseek.h"

// DeepSeek speaks the OpenAI-compatible Chat Completions protocol, so it
// reuses the shared implementation (openai.c) and only differs in its
// type/name plus the reasoning extensions. The shared code parses
// reasoning_content into REASONING messages; the request builder echoes it
// back only when `use_reasoning_content` is set (this provider).

static protyp_t deepseek_type() { return DEEPSEEK; }
static const char* deepseek_name() { return "DeepSeek"; }

err_t deepseek_create_context(void** context) {
  if (!context) return ERROR_NULLPTR;
  err_t err = cc_create_context(context);
  if (err != ERROR_NONE) return err;
  ((pctx_t*)*context)->use_reasoning_content = true;
  return ERROR_NONE;
}

err_t deepseek_deserialize(const char* data, size_t len, void** context) {
  if (!context) return ERROR_NULLPTR;
  err_t err = cc_deserialize(data, len, context);
  if (err != ERROR_NONE) return err;
  ((pctx_t*)*context)->use_reasoning_content = true;
  return ERROR_NONE;
}

static const provider_t deepseek_provider = {
    .type = deepseek_type,
    .name = deepseek_name,
    .error_str = cc_error_str,
    .create_context = deepseek_create_context,
    .delete_context = cc_delete_context,
    .serialize = cc_serialize,
    .deserialize = deepseek_deserialize,
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
    .update = cc_update,
    .rewind = cc_rewind,
    .is_finished = cc_is_finished,
    .latest_stop_reason = cc_latest_stop_reason,
    .latest_reasoning = cc_latest_reasoning,
    .latest_content = cc_latest_content,
    .latest_tool_calls = cc_latest_tool_calls,
};

const provider_t* get_deepseek_provider() { return &deepseek_provider; }

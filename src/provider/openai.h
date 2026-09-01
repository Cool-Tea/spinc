#ifndef OPENAI_H
#define OPENAI_H

#include "sjson.h"

#include "provider/provider.h"
#include "provider/common.h"

// Shared OpenAI-compatible utils.
bool openai_done_marker(const char* data, size_t len);
err_t openai_update_full(pctx_t* ctx, jnode_t* jfirst);
err_t openai_update_stream(pctx_t* ctx, jnode_t* jfirst, jnode_t* jdelta);

// Convert the general conversation into the OpenAI Chat Completions "messages"
// array (system prompt prepended, assistant turns grouped).
jnode_t* openai_messages_to_json(const pctx_t* ctx, jnode_t* jarr);

err_t cc_create_context(void** context);
void cc_delete_context(void* context);
err_t cc_serialize(const void* context, char** data, size_t* len);
err_t cc_deserialize(void* context, const char* data, size_t len);
err_t cc_set_model(void* context, const model_t* model);
model_t* cc_get_model(void* context);
err_t cc_set_toolset(void* context, const toolset_t* toolset);
toolset_t* cc_get_toolset(void* context);
err_t cc_set_system_prompt(void* context, const char* system_prompt);
const char* cc_get_system_prompt(const void* context);
size_t cc_message_count(const void* context);
err_t cc_add_user_message(void* context, const char* message);
err_t cc_add_assistant_message(void* context, const char* message);
err_t cc_add_tool_message(void* context, const char* id, const char* tool_name,
                          const char* result);
void cc_clear_messages(void* context);
void cc_pop_message(void* context);
err_t cc_call(void* context, char** response, size_t* len);
err_t cc_call_stream(void* context, strmcb_t callback, void* userp);
err_t cc_update(void* context, const char* response, size_t len);
void cc_rewind(void* context);
bool cc_is_finished(const void* context);
const char* cc_latest_stop_reason(const void* context);
const char* cc_latest_reasoning(const void* context);
const char* cc_latest_content(const void* context);
err_t cc_latest_tool_calls(const void* context, toolcall_t** tool_calls,
                           size_t* n_tool_call);
const char* cc_error_str(err_t err);

const provider_t* get_openai_compatible_provider();

#endif  // OPENAI_H
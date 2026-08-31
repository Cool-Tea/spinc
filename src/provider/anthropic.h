#ifndef ANTHROPIC_H
#define ANTHROPIC_H

#include "provider/provider.h"

// Convert the general conversation into the Anthropic Messages API "messages"
// array (assistant turns grouped into content blocks, user/tool turns merged).
jnode_t* anthropic_messages_to_json(const pctx_t* ctx, jnode_t* jarr);

err_t anthropic_create_context(void** context);
void anthropic_delete_context(void* context);
err_t anthropic_serialize(void* context, char** data, size_t* len);
err_t anthropic_deserialize(const char* data, size_t len, void** context);
err_t anthropic_set_model(void* context, const model_t* model);
model_t* anthropic_get_model(void* context);
err_t anthropic_set_toolset(void* context, const toolset_t* toolset);
toolset_t* anthropic_get_toolset(void* context);
err_t anthropic_set_system_prompt(void* context, const char* system_prompt);
const char* anthropic_get_system_prompt(void* context);
size_t anthropic_message_count(void* context);
err_t anthropic_add_user_message(void* context, const char* message);
err_t anthropic_add_assistant_message(void* context, const char* message);
err_t anthropic_add_tool_message(void* context, const char* id,
                                 const char* tool_name, const char* result);
void anthropic_clear_messages(void* context);
void anthropic_pop_message(void* context);
err_t anthropic_call(void* context, char** response, size_t* len);
err_t anthropic_call_stream(void* context, strmcb_t callback, void* userp);
err_t anthropic_update(void* context, const char* response, size_t len);
void anthropic_rewind(void* context);
bool anthropic_is_finished(void* context);
const char* anthropic_latest_stop_reason(void* context);
const char* anthropic_latest_reasoning(void* context);
const char* anthropic_latest_content(void* context);
err_t anthropic_latest_tool_calls(void* context, toolcall_t** calls,
                                  size_t* n_tool_call);
const provider_t* get_anthropic_compatible_provider();

#endif  // ANTHROPIC_H
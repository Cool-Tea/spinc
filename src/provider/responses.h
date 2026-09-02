#ifndef RESPONSES_H
#define RESPONSES_H

#include "sjson.h"

#include "provider/provider.h"
#include "provider/common.h"

err_t rp_create_context(void** context);
void rp_delete_context(void* context);
err_t rp_serialize(const void* context, char** data, size_t* len);
err_t rp_deserialize(void* context, const char* data, size_t len);
err_t rp_set_model(void* context, const model_t* model);
model_t* rp_get_model(void* context);
err_t rp_set_toolset(void* context, const toolset_t* toolset);
toolset_t* rp_get_toolset(void* context);
err_t rp_set_system_prompt(void* context, const char* system_prompt);
const char* rp_get_system_prompt(const void* context);
size_t rp_message_count(const void* context);
size_t rp_turn_count(const void* context);
err_t rp_get_turn_description(const void* context, size_t index,
                              char** description, size_t* len);
err_t rp_add_user_message(void* context, const char* message);
err_t rp_add_assistant_message(void* context, const char* message);
err_t rp_add_tool_message(void* context, const char* id, const char* tool_name,
                          const char* result);
void rp_clear_messages(void* context);
void rp_pop_message(void* context);
err_t rp_call(void* context, char** response, size_t* len);
err_t rp_call_stream(void* context, strmcb_t callback, void* userp);
err_t rp_update(void* context, const char* response, size_t len);
void rp_rewind(void* context, size_t turn_index);
bool rp_is_finished(const void* context);
const char* rp_latest_stop_reason(const void* context);
const char* rp_latest_reasoning(const void* context);
const char* rp_latest_content(const void* context);
err_t rp_latest_tool_calls(const void* context, toolcall_t** tool_calls,
                           size_t* n_tool_call);
const char* rp_error_str(err_t err);

const provider_t* get_openai_responses_provider();

#endif  // RESPONSES_H

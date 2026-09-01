#ifndef PROVIDER_COMMON_H
#define PROVIDER_COMMON_H

#include <stddef.h>
#include <stdbool.h>

#include "error.h"
#include "model.h"
#include "message.h"
#include "tool/tool.h"

// Shared provider context used by every provider implementation. It holds the
// conversation history as provider-independent messages (message.h), a rewind
// snapshot, and the "latest response" state that the agent reads after each
// update() call.
typedef struct pctx {
  model_t* model;       // owned shallow copy of the model config
  toolset_t* toolset;   // owned copy (freed by pctx_delete)
  char* system_prompt;  // owned

  msglist_t* messages;  // conversation history (system prompt excluded)
  msglist_t* snapshot;  // deep copy of messages for rewind()

  // Transient: id of the assistant turn currently being accumulated by a
  // stream. Used to group REASONING/ASSISTANT/TOOL_CALL messages into one
  // assistant turn.
  char* stream_turn_id;

  // Latest response state; owned strings, replaced on each update().
  bool finished;
  char* latest_content;
  char* latest_reasoning;
  char* latest_stop_reason;
  toolcall_t* latest_calls;  // owned; call_id/name/args strings owned
  size_t n_latest_call;
  bool tool_calls_ready;  // set once a complete tool-use response is received
} pctx_t;

err_t pctx_new(pctx_t** out);
void pctx_delete(pctx_t* ctx);

err_t pctx_set_model(pctx_t* ctx, const model_t* model);
model_t* pctx_get_model(pctx_t* ctx);
err_t pctx_set_toolset(pctx_t* ctx, const toolset_t* toolset);
toolset_t* pctx_get_toolset(pctx_t* ctx);
err_t pctx_set_system_prompt(pctx_t* ctx, const char* system_prompt);
const char* pctx_get_system_prompt(const pctx_t* ctx);

// Free the latest-response state (but not the conversation).
void pctx_clear_latest(pctx_t* ctx);

// Snapshot the conversation for rewind(); a no-op if a snapshot already
// exists (e.g. mid-stream).
void pctx_take_snapshot(pctx_t* ctx);
// Restore the snapshot (if any) and clear the latest state.
void pctx_rewind(pctx_t* ctx);

// Generate a fresh assistant-turn id (owned by the caller).
char* pctx_new_turn_id(void);
// Append a fragment to an owned string (reuses sjson string helpers).
err_t pctx_append_text(char** text, size_t* text_len, const char* frag);

// Append a message to the conversation history in the general form.
err_t pctx_add_user_message(pctx_t* ctx, const char* text);
err_t pctx_add_assistant_message(pctx_t* ctx, const char* text);
err_t pctx_add_tool_message(pctx_t* ctx, const char* call_id,
                            const char* tool_name, const char* result);
size_t pctx_message_count(const pctx_t* ctx);
void pctx_clear_messages(pctx_t* ctx);
void pctx_pop_message(pctx_t* ctx);

// Build the owned latest_calls from parallel borrowed string arrays (NULL
// entries are allowed). The strings are copied.
err_t pctx_set_tool_calls(pctx_t* ctx, size_t n, const char* const* ids,
                          const char* const* names, const char* const* args);
// Rebuild latest_calls from the TOOL_CALL messages of the trailing assistant
// turn. Called once a complete tool-use response is in the conversation.
err_t pctx_latest_calls_from_turn(pctx_t* ctx);
// Return a heap copy of latest_calls that the caller must free() with
// free(). Only returns calls once tool_calls_ready is set.
err_t pctx_latest_tool_calls(pctx_t* ctx, toolcall_t** calls, size_t* n);

err_t pctx_serialize(const pctx_t* ctx, char** data, size_t* len);
err_t pctx_deserialize(pctx_t* ctx, const char* data, size_t len);

#endif  // PROVIDER_COMMON_H

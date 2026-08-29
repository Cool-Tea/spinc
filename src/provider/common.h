#ifndef PROVIDER_COMMON_H
#define PROVIDER_COMMON_H

#include <stddef.h>
#include <stdbool.h>

#include "error.h"
#include "model.h"
#include "tool/tool.h"
#include "sjson.h"

// A tool call requested by the model. `id`/`name`/`args` point into memory
// owned by the provider (or a copy returned by pctx_latest_tool_calls).
typedef struct toolcall {
  size_t id_len;
  const char* id;
  size_t name_len;
  const char* name;
  size_t args_len;
  const char* args;
} toolcall_t;

// Shared provider context used by every provider implementation. It holds the
// conversation history, a rewind snapshot, and the "latest response" state
// that the agent reads after each update() call.
typedef struct pctx {
  model_t* model;       // owned copy of the model config
  toolset_t* toolset;   // borrowed (owned by the caller)
  char* system_prompt;  // owned

  jnode_t* messages;  // conversation history (system prompt excluded)
  jnode_t* snapshot;  // deep copy of messages for rewind()

  // Latest response state; owned strings, replaced on each update().
  bool finished;
  char* latest_content;
  char* latest_reasoning;
  char* latest_stop_reason;
  toolcall_t* latest_calls;  // owned; id/name/args strings owned
  size_t n_latest_call;
  bool tool_calls_ready;  // set once a complete tool-use response is received
} pctx_t;

err_t pctx_new(pctx_t** out);
void pctx_delete(pctx_t* ctx);

// Free the latest-response state (but not the conversation).
void pctx_clear_latest(pctx_t* ctx);

// Snapshot the conversation for rewind(); a no-op if a snapshot already
// exists (e.g. mid-stream).
void pctx_take_snapshot(pctx_t* ctx);
// Restore the snapshot (if any) and clear the latest state.
void pctx_rewind(pctx_t* ctx);

void pctx_clear_messages(pctx_t* ctx);
void pctx_pop_message(pctx_t* ctx);

// Build the owned latest_calls from parallel borrowed string arrays (NULL
// entries are allowed). The strings are copied.
err_t pctx_set_tool_calls(pctx_t* ctx, size_t n, const char* const* ids,
                          const char* const* names, const char* const* args);
// Return a heap copy of latest_calls that the caller must free() with
// free(). Only returns calls once tool_calls_ready is set.
err_t pctx_latest_tool_calls(pctx_t* ctx, toolcall_t** calls, size_t* n);

err_t pctx_serialize(const pctx_t* ctx, char** data, size_t* len);
err_t pctx_deserialize(const char* data, size_t len, pctx_t** out);

#endif  // PROVIDER_COMMON_H

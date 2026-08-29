#ifndef PROVIDER_H
#define PROVIDER_H

#include <stddef.h>
#include <stdbool.h>

#include "error.h"
#include "http.h"
#include "model.h"
#include "tool/tool.h"
#include "provider/common.h"

typedef enum provider_type {
  OPENAI_COMPATIBLE,
  ANTHROPIC_COMPATIBLE,
  DEEPSEEK,
} protyp_t;

typedef err_t (*strmcb_t)(void* context, const event_t* event, void* userp);

/**
 * @brief A structure representing a provider.
 * This structure contains the necessary functions to interact with a specific
 * provider. All pointer params marked as const are copied into the context
 * All pointer params not marked as const are moved into the context and will be
 * freed by the provider when no longer needed.
 */
typedef struct provider {
  /* Get the provider type */
  protyp_t (*type)();
  /* Get the provider name */
  const char* (*name)();
  /* Get the error string (both builtin and custom errors are supported) */
  const char* (*error_str)(err_t err);

  /* Provider-specific context */
  err_t (*create_context)(void** context);
  /* Delete the context */
  void (*delete_context)(void* context);

  /* Serialize the context */
  err_t (*serialize)(void* context, char** data, size_t* len);
  /* Deserialize the context */
  err_t (*deserialize)(const char* data, size_t len, void** context);

  /* Set the model */
  err_t (*set_model)(void* context, const model_t* model);
  /* Get the internal model pointer */
  model_t* (*get_model)(void* context);

  /* Set the toolset pointer */
  err_t (*set_toolset)(void* context, const toolset_t* toolset);
  /* Get the internal toolset pointer */
  toolset_t* (*get_toolset)(void* context);

  /* Set the system prompt */
  err_t (*set_system_prompt)(void* context, const char* system_prompt);
  /* Get the system prompt */
  const char* (*get_system_prompt)(void* context);

  /* Get the number of messages */
  size_t (*message_count)(void* context);
  /* Add a user message */
  err_t (*add_user_message)(void* context, const char* message);
  /* Add an assistant message */
  err_t (*add_assistant_message)(void* context, const char* message);
  /* Add a tool message */
  err_t (*add_tool_message)(void* context, const char* id,
                            const char* tool_name, const char* result);
  /* Clear all messages */
  void (*clear_messages)(void* context);
  /* Pop the last message */
  void (*pop_message)(void* context);

  /* Call the provider with the current context */
  err_t (*call)(void* context, char** response, size_t* len);
  /* Call the provider in streaming mode */
  err_t (*call_stream)(void* context, strmcb_t callback, void* userp);
  /* Update the context with the given response */
  err_t (*update)(void* context, const char* response, size_t len);
  /* Rewind the context to the previous state */
  void (*rewind)(void* context);

  /* Check if the provider has finished */
  bool (*is_finished)(void* context);
  /* Get the latest stop reason if available */
  const char* (*latest_stop_reason)(void* context);
  /* Get the latest reasoning if available */
  const char* (*latest_reasoning)(void* context);
  /* Get the latest content if available */
  const char* (*latest_content)(void* context);
  /* Get the latest tool calls if available */
  err_t (*latest_tool_calls)(void* context, toolcall_t** tool_calls,
                             size_t* n_tool_call);
} provider_t;

const provider_t* get_provider(protyp_t type);

#endif  // PROVIDER_H
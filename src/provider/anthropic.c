#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "log.h"
#include "http.h"
#include "provider/anthropic.h"
#include "provider/common.h"

#define URL_LEN 512
#define AUTH_HEADER_LEN 512
#define ANTHROPIC_VERSION "2023-06-01"
// max_tokens is required by the Messages API; fall back when config has -1.
#define ANTHROPIC_DEFAULT_MAX_TOKENS 8192

/* ==============================
 *        Request building
 * ============================== */

static jnode_t* anthropic_serialize_tool(const tool_t* tool) {
  const tooldef_t* tooldef = &tool->def;
  jnode_t* jtool = jobject_new();
  jobject_put(jtool, "name", jstring_new(0, tooldef->name));
  jobject_put(jtool, "description", jstring_new(0, tooldef->description));
  jnode_t* schema = jobject_new();
  jobject_put(jtool, "input_schema", schema);
  jobject_put(schema, "type", jstring_new(0, "object"));
  jnode_t* props = jobject_new();
  jobject_put(schema, "properties", props);
  for (size_t i = 0; i < tooldef->n_param; ++i) {
    const paramdef_t* p = &tooldef->params[i];
    jnode_t* pdef = jobject_new();
    jobject_put(pdef, "type", jstring_new(0, p->type));
    jobject_put(pdef, "description", jstring_new(0, p->description));
    jobject_put(props, p->name, pdef);
  }
  jnode_t* required = jarray_new();
  jobject_put(schema, "required", required);
  for (size_t i = 0; i < tooldef->n_param; ++i) {
    const paramdef_t* p = &tooldef->params[i];
    if (p->required) jarray_add(required, jstring_new(0, p->name));
  }
  return jtool;
}

static jnode_t* anthropic_body(const pctx_t* ctx, bool stream) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, ctx->model->name));
  jobject_put(jbody, "system",
              jstring_new(0, ctx->system_prompt ? ctx->system_prompt : ""));
  jobject_put(jbody, "messages", jcopy(ctx->messages));

  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  if (ctx->toolset) {
    for (size_t i = 0; i < ctx->toolset->n_tool; ++i) {
      jarray_add(jtools, anthropic_serialize_tool(&ctx->toolset->tools[i]));
    }
  }

  if (ctx->model->thinking) {
    jnode_t* jthinking = jobject_new();
    jobject_put(jbody, "thinking", jthinking);
    jobject_put(jthinking, "type", jstring_new(0, ctx->model->thinking));
  }
  if (ctx->model->reasoning_effort) {
    jnode_t* joutput_config = jobject_new();
    jobject_put(jbody, "output_config", joutput_config);
    jobject_put(joutput_config, "effort",
                jstring_new(0, ctx->model->reasoning_effort));
  }
  jobject_put(jbody, "top_p", jnumber_new(ctx->model->top_p));
  long max_tokens = ctx->model->max_tokens;
  if (max_tokens < 0) max_tokens = ANTHROPIC_DEFAULT_MAX_TOKENS;
  jobject_put(jbody, "max_tokens", jnumber_new(max_tokens));
  jobject_put(jbody, "stream", jbool_new(stream));
  return jbody;
}

/* ==============================
 *           HTTP layer
 * ============================== */

static err_t anthropic_http_call(const pctx_t* ctx, bool stream,
                                 response_t* resp_out) {
  char url[URL_LEN];
  char auth[AUTH_HEADER_LEN];
  snprintf(url, sizeof(url), "%s/v1/messages", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "X-Api-Key: %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json",
                           "anthropic-version: " ANTHROPIC_VERSION};

  jnode_t* jbody = anthropic_body(ctx, stream);
  if (!jbody) return ERROR_OUT_OF_MEMORY;
  char* body = jto_string(jbody);
  jdelete(jbody);
  if (!body) return ERROR_OUT_OF_MEMORY;
  log(DEBUG, "Request body: %s", body);

  request_t request = {
      .url = url,
      .n_header = sizeof(headers) / sizeof(headers[0]),
      .headers = headers,
      .body = body,
  };
  response_t resp = {0};
  err_t err = http_post(&request, &resp);
  free(body);
  if (err != ERROR_NONE) {
    log(ERROR, "HTTP request failed: %s", error_str(err));
    if (resp.data) free(resp.data);
    return err;
  }
  if (resp.status != 200) {
    log(ERROR, "HTTP request failed with status %ld: %s", resp.status,
        resp.data ? (char*)resp.data : "");
    free(resp.data);
    return ERROR_CURL;
  }
  *resp_out = resp;
  return ERROR_NONE;
}

/* ==============================
 *       Response parsing
 * ============================== */

static bool anthropic_done_marker(const char* data, size_t len) {
  while (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\r')) {
    ++data;
    --len;
  }
  return len == 6 && memcmp(data, "[DONE]", 6) == 0;
}

// Collect the tool_use blocks of `jcontent` into the latest tool calls. The
// `input` object of each block is serialized to a JSON string.
static err_t anthropic_build_tool_calls(pctx_t* ctx, jnode_t* jcontent) {
  if (!jis_array(jcontent)) return ERROR_NONE;
  size_t n = 0;
  for (int i = 0; i < jarray_size(jcontent); ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    if (jis_string(jtype) && strcmp(jstring_content(jtype), "tool_use") == 0) {
      ++n;
    }
  }
  if (n == 0) return ERROR_NONE;

  const char** ids = calloc(n, sizeof(char*));
  const char** names = calloc(n, sizeof(char*));
  const char** args = calloc(n, sizeof(char*));
  if (!ids || !names || !args) {
    free(ids);
    free(names);
    free(args);
    return ERROR_OUT_OF_MEMORY;
  }
  size_t k = 0;
  for (int i = 0; i < jarray_size(jcontent) && k < n; ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    if (!jis_string(jtype) || strcmp(jstring_content(jtype), "tool_use") != 0) {
      continue;
    }
    jnode_t* jid = jobject_get(jblock, "id");
    jnode_t* jname = jobject_get(jblock, "name");
    jnode_t* jinput = jobject_get(jblock, "input");
    ids[k] = jis_string(jid) ? jstring_content(jid) : NULL;
    names[k] = jis_string(jname) ? jstring_content(jname) : NULL;
    if (jis_string(jinput)) {
      args[k] = strdup(jstring_content(jinput));
    } else if (jinput) {
      args[k] = jto_string(jinput);
    }
    ++k;
  }
  err_t err = pctx_set_tool_calls(ctx, k, ids, names, args);
  for (size_t i = 0; i < k; ++i) free((char*)args[i]);
  free(ids);
  free(names);
  free(args);
  return err;
}

// Non-streaming response: append the assistant message and expose the latest
// state.
static err_t anthropic_update_full(pctx_t* ctx, jnode_t* json) {
  jnode_t* jstop = jobject_get(json, "stop_reason");
  const char* stop = jis_string(jstop) ? jstring_content(jstop) : NULL;
  if (stop) {
    ctx->latest_stop_reason = strdup(stop);
    // Any terminal reason other than a tool-use request ends the turn.
    ctx->finished = (strcmp(stop, "tool_use") != 0);
    if (strcmp(stop, "tool_use") == 0) ctx->tool_calls_ready = true;
  }

  jnode_t* jcontent = jobject_get(json, "content");
  if (jis_string(jcontent)) {
    if (jstring_len(jcontent)) {
      ctx->latest_content = strdup(jstring_content(jcontent));
    }
    return ERROR_NONE;
  }
  if (!jis_array(jcontent)) return ERROR_NONE;

  // Append the assistant message (content blocks) to the conversation.
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "assistant"));
  jobject_put(jmessage, "content", jcopy(jcontent));
  jarray_add(ctx->messages, jmessage);

  // Concatenate text blocks into latest_content and thinking into
  // latest_reasoning.
  jnode_t* jtext = jstring_new(0, "");
  jnode_t* jthinking = jstring_new(0, "");
  size_t n = jarray_size(jcontent);
  for (size_t i = 0; i < n; ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    const char* type = jis_string(jtype) ? jstring_content(jtype) : NULL;
    if (type && strcmp(type, "text") == 0) {
      jnode_t* jblock_text = jobject_get(jblock, "text");
      if (jis_string(jblock_text)) {
        jstring_concat(jtext, jstring_content(jblock_text));
      }
    } else if (type && strcmp(type, "thinking") == 0) {
      jnode_t* jblock_thinking = jobject_get(jblock, "thinking");
      if (jis_string(jblock_thinking)) {
        jstring_concat(jthinking, jstring_content(jblock_thinking));
      }
    }
  }
  if (jstring_len(jtext)) ctx->latest_content = strdup(jstring_content(jtext));
  if (jstring_len(jthinking)) {
    ctx->latest_reasoning = strdup(jstring_content(jthinking));
  }
  jdelete(jtext);
  jdelete(jthinking);

  if (ctx->tool_calls_ready) {
    err_t err = anthropic_build_tool_calls(ctx, jcontent);
    if (err != ERROR_NONE) return err;
  }
  return ERROR_NONE;
}

// Streaming event: merge the delta into the assistant message being built.
static err_t anthropic_update_stream(pctx_t* ctx, jnode_t* json) {
  jnode_t* jtype = jobject_get(json, "type");
  if (!jis_string(jtype)) return ERROR_NONE;
  const char* type = jstring_content(jtype);

  if (strcmp(type, "message_start") == 0) {
    jnode_t* jmessage = jobject_new();
    jobject_put(jmessage, "role", jstring_new(0, "assistant"));
    jobject_put(jmessage, "content", jarray_new());
    jarray_add(ctx->messages, jmessage);
    return ERROR_NONE;
  }
  if (strcmp(type, "message_stop") == 0 || strcmp(type, "ping") == 0) {
    return ERROR_NONE;
  }
  if (strcmp(type, "error") == 0) {
    jnode_t* jerror = jobject_get(json, "error");
    jnode_t* jmessage = jerror ? jobject_get(jerror, "message") : NULL;
    if (jis_string(jmessage)) {
      log(ERROR, "Anthropic stream error: %s", jstring_content(jmessage));
    }
    return ERROR_NONE;
  }

  // The message_delta event has no "index"; it carries the stop_reason.
  if (strcmp(type, "message_delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    jnode_t* jstop = jdelta ? jobject_get(jdelta, "stop_reason") : NULL;
    if (jis_string(jstop)) {
      const char* stop = jstring_content(jstop);
      free(ctx->latest_stop_reason);
      ctx->latest_stop_reason = strdup(stop);
      // Any terminal reason other than a tool-use request ends the turn.
      ctx->finished = (strcmp(stop, "tool_use") != 0);
      if (strcmp(stop, "tool_use") == 0) {
        ctx->tool_calls_ready = true;
        if (jarray_size(ctx->messages) > 0) {
          jnode_t* jlast =
              jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
          err_t err =
              anthropic_build_tool_calls(ctx, jobject_get(jlast, "content"));
          if (err != ERROR_NONE) return err;
        }
      }
    }
    return ERROR_NONE;
  }

  // content_block_* events address a block inside the last assistant message.
  if (jarray_size(ctx->messages) == 0) return ERROR_NONE;
  jnode_t* jlast = jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jcontent = jobject_get(jlast, "content");
  if (!jis_array(jcontent)) return ERROR_NONE;

  jnode_t* jindex_node = jobject_get(json, "index");
  int index =
      jis_number(jindex_node) ? (int)jas_number(jindex_node)->value : -1;

  if (strcmp(type, "content_block_start") == 0) {
    jnode_t* jcb = jobject_get(json, "content_block");
    if (!jis_object(jcb)) return ERROR_NONE;
    jnode_t* jblock = jcopy(jcb);
    // tool_use input arrives as input_json_delta fragments; accumulate them
    // as a string until the block is complete.
    jnode_t* jblock_type = jobject_get(jblock, "type");
    if (jis_string(jblock_type) &&
        strcmp(jstring_content(jblock_type), "tool_use") == 0) {
      jobject_put(jblock, "input", jstring_new(0, ""));
    }
    if (index < 0 || index >= jarray_size(jcontent)) {
      jarray_add(jcontent, jblock);
    } else {
      jarray_remove(jcontent, index);
      if (index >= jarray_size(jcontent)) {
        jarray_add(jcontent, jblock);
      } else {
        jarray_insert(jcontent, index, jblock);
      }
    }
    return ERROR_NONE;
  }

  if (index < 0 || index >= jarray_size(jcontent)) return ERROR_NONE;
  jnode_t* jblock = jarray_get(jcontent, index);

  if (strcmp(type, "content_block_delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    jnode_t* jdelta_type = jdelta ? jobject_get(jdelta, "type") : NULL;
    if (!jis_string(jdelta_type)) return ERROR_NONE;
    const char* delta_type = jstring_content(jdelta_type);
    if (strcmp(delta_type, "text_delta") == 0) {
      jnode_t* jtext = jobject_get(jdelta, "text");
      if (!jis_string(jtext)) return ERROR_NONE;
      free(ctx->latest_content);
      ctx->latest_content = strdup(jstring_content(jtext));
      jnode_t* jblock_text = jobject_get(jblock, "text");
      if (!jis_string(jblock_text)) {
        jobject_put(jblock, "text", jcopy(jtext));
      } else {
        jstring_concat(jblock_text, jstring_content(jtext));
      }
    } else if (strcmp(delta_type, "thinking_delta") == 0) {
      jnode_t* jthinking = jobject_get(jdelta, "thinking");
      if (!jis_string(jthinking)) return ERROR_NONE;
      free(ctx->latest_reasoning);
      ctx->latest_reasoning = strdup(jstring_content(jthinking));
      jnode_t* jblock_thinking = jobject_get(jblock, "thinking");
      if (!jis_string(jblock_thinking)) {
        jobject_put(jblock, "thinking", jcopy(jthinking));
      } else {
        jstring_concat(jblock_thinking, jstring_content(jthinking));
      }
    } else if (strcmp(delta_type, "input_json_delta") == 0) {
      jnode_t* jpartial = jobject_get(jdelta, "partial_json");
      if (!jis_string(jpartial)) return ERROR_NONE;
      jnode_t* jinput = jobject_get(jblock, "input");
      if (!jis_string(jinput)) {
        jobject_put(jblock, "input", jcopy(jpartial));
      } else {
        jstring_concat(jinput, jstring_content(jpartial));
      }
    }
    return ERROR_NONE;
  }

  if (strcmp(type, "content_block_stop") == 0) {
    // The accumulated tool_use input string is now complete JSON; turn it
    // into a real object so it can be sent back to the API.
    jnode_t* jblock_type = jobject_get(jblock, "type");
    if (!jis_string(jblock_type) ||
        strcmp(jstring_content(jblock_type), "tool_use") != 0) {
      return ERROR_NONE;
    }
    jnode_t* jinput = jobject_get(jblock, "input");
    if (jis_string(jinput)) {
      jnode_t* jparsed = jfrom_string(jstring_content(jinput), 0);
      jobject_put(jblock, "input", jparsed ? jparsed : jobject_new());
    }
    return ERROR_NONE;
  }

  return ERROR_NONE;
}

/* ==============================
 *       Provider interface
 * ============================== */

err_t anthropic_create_context(void** context) {
  if (!context) return ERROR_NULLPTR;
  return pctx_new((pctx_t**)context);
}

void anthropic_delete_context(void* context) { pctx_delete((pctx_t*)context); }

err_t anthropic_serialize(void* context, char** data, size_t* len) {
  return pctx_serialize((const pctx_t*)context, data, len);
}

err_t anthropic_deserialize(const char* data, size_t len, void** context) {
  if (!context) return ERROR_NULLPTR;
  *context = NULL;
  return pctx_deserialize(data, len, (pctx_t**)context);
}

err_t anthropic_set_model(void* context, const model_t* model) {
  pctx_t* ctx = context;
  if (!ctx || !model) return ERROR_NULLPTR;
  model_t* copy = malloc(sizeof(model_t));
  if (!copy) return ERROR_OUT_OF_MEMORY;
  *copy = *model;
  free(ctx->model);
  ctx->model = copy;
  return ERROR_NONE;
}

model_t* anthropic_get_model(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->model : NULL;
}

err_t anthropic_set_toolset(void* context, const toolset_t* toolset) {
  pctx_t* ctx = context;
  if (!ctx || !toolset) return ERROR_NULLPTR;
  ctx->toolset = (toolset_t*)toolset;
  return ERROR_NONE;
}

toolset_t* anthropic_get_toolset(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->toolset : NULL;
}

err_t anthropic_set_system_prompt(void* context, const char* system_prompt) {
  pctx_t* ctx = context;
  if (!ctx || !system_prompt) return ERROR_NULLPTR;
  char* copy = strdup(system_prompt);
  if (!copy) return ERROR_OUT_OF_MEMORY;
  free(ctx->system_prompt);
  ctx->system_prompt = copy;
  return ERROR_NONE;
}

const char* anthropic_get_system_prompt(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->system_prompt : NULL;
}

size_t anthropic_message_count(void* context) {
  pctx_t* ctx = context;
  return ctx ? (size_t)jarray_size(ctx->messages) : 0;
}

// Anthropic requires user/assistant roles to alternate, so consecutive user
// messages are merged.
err_t anthropic_add_user_message(void* context, const char* message) {
  pctx_t* ctx = context;
  if (!ctx || !message) return ERROR_NULLPTR;

  if (jarray_size(ctx->messages) > 0) {
    jnode_t* jlast = jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
    jnode_t* jrole = jobject_get(jlast, "role");
    if (jis_string(jrole) && strcmp(jstring_content(jrole), "user") == 0) {
      jnode_t* jcontent = jobject_get(jlast, "content");
      if (jis_string(jcontent)) {
        jstring_concat(jcontent, message);
        return ERROR_NONE;
      }
      if (jis_array(jcontent)) {
        jnode_t* jblock = jobject_new();
        jobject_put(jblock, "type", jstring_new(0, "text"));
        jobject_put(jblock, "text", jstring_new(0, message));
        jarray_add(jcontent, jblock);
        return ERROR_NONE;
      }
    }
  }

  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "user"));
  jobject_put(jmessage, "content", jstring_new(0, message));
  jarray_add(ctx->messages, jmessage);
  return ERROR_NONE;
}

err_t anthropic_add_assistant_message(void* context, const char* message) {
  pctx_t* ctx = context;
  if (!ctx || !message) return ERROR_NULLPTR;
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "assistant"));
  jobject_put(jmessage, "content", jstring_new(0, message));
  jarray_add(ctx->messages, jmessage);
  return ERROR_NONE;
}

err_t anthropic_add_tool_message(void* context, const char* id,
                                 const char* tool_name, const char* result) {
  (void)tool_name;
  pctx_t* ctx = context;
  if (!ctx || !id || !result) return ERROR_NULLPTR;

  jnode_t* jtool_result = jobject_new();
  jobject_put(jtool_result, "type", jstring_new(0, "tool_result"));
  jobject_put(jtool_result, "tool_use_id", jstring_new(0, id));
  jobject_put(jtool_result, "content", jstring_new(0, result));

  // All tool_results for one assistant turn belong in a single user message
  // that directly follows it. Reuse the last message if it is already a user
  // message (e.g. a previous tool_result).
  if (jarray_size(ctx->messages) > 0) {
    jnode_t* jlast = jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
    jnode_t* jrole = jobject_get(jlast, "role");
    if (jis_string(jrole) && strcmp(jstring_content(jrole), "user") == 0) {
      jnode_t* jcontent = jobject_get(jlast, "content");
      if (jis_string(jcontent)) {
        // Convert the plain string content into a block array so a
        // tool_result can be appended. jobject_put below deletes the old
        // string value, so the array must hold its own copy.
        jnode_t* jarr = jarray_new();
        jarray_add(jarr, jstring_new(0, jstring_content(jcontent)));
        jobject_put(jlast, "content", jarr);
        jcontent = jarr;
      }
      if (jis_array(jcontent)) {
        jarray_add(jcontent, jtool_result);
        return ERROR_NONE;
      }
    }
  }

  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "user"));
  jnode_t* jcontent = jarray_new();
  jobject_put(jmessage, "content", jcontent);
  jarray_add(jcontent, jtool_result);
  jarray_add(ctx->messages, jmessage);
  return ERROR_NONE;
}

void anthropic_clear_messages(void* context) {
  pctx_clear_messages((pctx_t*)context);
}

void anthropic_pop_message(void* context) {
  pctx_pop_message((pctx_t*)context);
}

err_t anthropic_call(void* context, char** response, size_t* len) {
  pctx_t* ctx = context;
  if (!ctx || !response || !len) return ERROR_NULLPTR;
  if (!ctx->model) return ERROR_NULLPTR;
  *response = NULL;
  *len = 0;
  if (ctx->snapshot) {
    jdelete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  ctx->finished = false;

  response_t resp = {0};
  err_t err = anthropic_http_call(ctx, false, &resp);
  if (err != ERROR_NONE) return err;
  *response = (char*)resp.data;  // owned by the caller now
  *len = resp.data_size;
  return ERROR_NONE;
}

struct anthropic_sse {
  void* context;
  strmcb_t callback;
  void* userp;
};

err_t anthropic_sse_forward(const event_t* event, void* userp) {
  struct anthropic_sse* w = (struct anthropic_sse*)userp;
  return w->callback(w->context, event, w->userp);
}

err_t anthropic_call_stream(void* context, strmcb_t callback, void* userp) {
  pctx_t* ctx = context;
  if (!ctx || !callback) return ERROR_NULLPTR;
  if (!ctx->model) return ERROR_NULLPTR;
  if (ctx->snapshot) {
    jdelete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  ctx->finished = false;

  char url[URL_LEN];
  char auth[AUTH_HEADER_LEN];
  snprintf(url, sizeof(url), "%s/v1/messages", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "X-Api-Key: %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json",
                           "anthropic-version: " ANTHROPIC_VERSION};

  jnode_t* jbody = anthropic_body(ctx, true);
  if (!jbody) return ERROR_OUT_OF_MEMORY;
  char* body = jto_string(jbody);
  jdelete(jbody);
  if (!body) return ERROR_OUT_OF_MEMORY;
  log(DEBUG, "Request body: %s", body);

  request_t request = {
      .url = url,
      .n_header = sizeof(headers) / sizeof(headers[0]),
      .headers = headers,
      .body = body,
  };
  struct anthropic_sse wrapper = {
      .context = context, .callback = callback, .userp = userp};
  err_t err = http_sse(&request, anthropic_sse_forward, &wrapper);
  free(body);
  if (err != ERROR_NONE) {
    log(ERROR, "SSE request failed: %s", error_str(err));
  }
  return err;
}

err_t anthropic_update(void* context, const char* response, size_t len) {
  pctx_t* ctx = context;
  if (!ctx || !response) return ERROR_NULLPTR;
  if (anthropic_done_marker(response, len)) {
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
  jnode_t* jtype = jobject_get(json, "type");
  const char* type = jis_string(jtype) ? jstring_content(jtype) : NULL;
  err_t err = ERROR_NONE;
  if (type && strcmp(type, "message") == 0) {
    err = anthropic_update_full(ctx, json);
  } else if (type) {
    err = anthropic_update_stream(ctx, json);
  } else {
    // Some compatible endpoints omit the "type" field.
    err = anthropic_update_full(ctx, json);
  }
  jdelete(json);
  return err;
}

void anthropic_rewind(void* context) { pctx_rewind((pctx_t*)context); }

bool anthropic_is_finished(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->finished : false;
}

const char* anthropic_latest_stop_reason(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_stop_reason : NULL;
}

const char* anthropic_latest_reasoning(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_reasoning : NULL;
}

const char* anthropic_latest_content(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_content : NULL;
}

err_t anthropic_latest_tool_calls(void* context, toolcall_t** calls,
                                  size_t* n_tool_call) {
  return pctx_latest_tool_calls((pctx_t*)context, calls, n_tool_call);
}

static const char* anthropic_error_str(err_t err) {
  if (is_custom(err)) return "Custom error";
  return error_str(err);
}

/* ==============================
 *          Provider
 * ============================== */

static protyp_t anthropic_type() { return ANTHROPIC_COMPATIBLE; }
static const char* anthropic_name() { return "Anthropic Compatible"; }

static const provider_t anthropic_provider = {
    .type = anthropic_type,
    .name = anthropic_name,
    .error_str = anthropic_error_str,
    .create_context = anthropic_create_context,
    .delete_context = anthropic_delete_context,
    .serialize = anthropic_serialize,
    .deserialize = anthropic_deserialize,
    .set_model = anthropic_set_model,
    .get_model = anthropic_get_model,
    .set_toolset = anthropic_set_toolset,
    .get_toolset = anthropic_get_toolset,
    .set_system_prompt = anthropic_set_system_prompt,
    .get_system_prompt = anthropic_get_system_prompt,
    .message_count = anthropic_message_count,
    .add_user_message = anthropic_add_user_message,
    .add_assistant_message = anthropic_add_assistant_message,
    .add_tool_message = anthropic_add_tool_message,
    .clear_messages = anthropic_clear_messages,
    .pop_message = anthropic_pop_message,
    .call = anthropic_call,
    .call_stream = anthropic_call_stream,
    .update = anthropic_update,
    .rewind = anthropic_rewind,
    .is_finished = anthropic_is_finished,
    .latest_stop_reason = anthropic_latest_stop_reason,
    .latest_reasoning = anthropic_latest_reasoning,
    .latest_content = anthropic_latest_content,
    .latest_tool_calls = anthropic_latest_tool_calls,
};

const provider_t* get_anthropic_compatible_provider() {
  return &anthropic_provider;
}

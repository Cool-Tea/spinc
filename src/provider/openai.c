#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sjson.h"

#include "log.h"
#include "http.h"
#include "provider/openai.h"

#define URL_LEN 512
#define AUTH_HEADER_LEN 512

/* ==============================
 *        Request building
 * ============================== */

static jnode_t* openai_system_message(const char* content) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "system"));
  jobject_put(jmessage, "content", jstring_new(0, content ? content : ""));
  return jmessage;
}

static jnode_t* openai_message(const char* role, const char* content) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, role));
  jobject_put(jmessage, "content", jstring_new(0, content ? content : ""));
  return jmessage;
}

// Messages with NULL ids (or equal ids) belong to the same assistant turn.
static bool openai_same_turn(const char* a, const char* b) {
  if (!a || !b) return true;
  return strcmp(a, b) == 0;
}

// Convert the general message list into the OpenAI Chat Completions "messages"
// array. An assistant turn (REASONING/ASSISTANT/TOOL_CALL sharing one id) is
// grouped into a single assistant message: reasoning goes to reasoning_content,
// text to content, and tool calls to the tool_calls array. Tool results become
// role "tool" messages.
jnode_t* openai_messages_to_json(const pctx_t* ctx, jnode_t* jarr) {
  // The system prompt is prepended here so the stored conversation only holds
  // user/assistant/tool messages.
  jarray_add(jarr, openai_system_message(ctx->system_prompt));

  jnode_t* jcur = NULL;       // assistant message being built
  const char* cur_id = NULL;  // turn id of the current assistant message
  for (size_t i = 0; i < ctx->messages->n_message; ++i) {
    const message_t* m = &ctx->messages->messages[i];
    switch (m->type) {
      case SYSTEM:
        jarray_add(jarr, openai_system_message(m->system.text));
        jcur = NULL;
        cur_id = NULL;
        break;
      case USER:
        jarray_add(jarr, openai_message("user", m->user.text));
        jcur = NULL;
        cur_id = NULL;
        break;
      case ASSISTANT: {
        const char* mid = message_id(m);
        if (!jcur || !openai_same_turn(cur_id, mid)) {
          jcur = openai_message("assistant", "");
          jarray_add(jarr, jcur);
          cur_id = mid;
        }
        jobject_put(jcur, "content",
                    jstring_new(0, m->assistant.text ? m->assistant.text : ""));
        break;
      }
      case REASONING: {
        const char* mid = message_id(m);
        if (!jcur || !openai_same_turn(cur_id, mid)) {
          jcur = openai_message("assistant", "");
          jarray_add(jarr, jcur);
          cur_id = mid;
        }
        jobject_put(jcur, "reasoning_content",
                    jstring_new(0, m->reasoning.text ? m->reasoning.text : ""));
        break;
      }
      case TOOL_CALL: {
        const char* mid = message_id(m);
        if (!jcur || !openai_same_turn(cur_id, mid)) {
          jcur = openai_message("assistant", "");
          jarray_add(jarr, jcur);
          cur_id = mid;
        }
        jnode_t* jcalls = jobject_get(jcur, "tool_calls");
        if (!jis_array(jcalls)) {
          jcalls = jarray_new();
          jobject_put(jcur, "tool_calls", jcalls);
        }
        jnode_t* jcall = jobject_new();
        jobject_put(
            jcall, "id",
            jstring_new(0, m->tool_call.call_id ? m->tool_call.call_id : ""));
        jobject_put(jcall, "type", jstring_new(0, "function"));
        jnode_t* jfunc = jobject_new();
        jobject_put(jcall, "function", jfunc);
        jobject_put(jfunc, "name",
                    jstring_new(0, m->tool_call.name ? m->tool_call.name : ""));
        jobject_put(jfunc, "arguments",
                    jstring_new(0, m->tool_call.args ? m->tool_call.args : ""));
        jarray_add(jcalls, jcall);
        break;
      }
      case TOOL_RESULT: {
        jnode_t* jm = jobject_new();
        jobject_put(jm, "role", jstring_new(0, "tool"));
        jobject_put(
            jm, "tool_call_id",
            jstring_new(0,
                        m->tool_result.call_id ? m->tool_result.call_id : ""));
        jobject_put(
            jm, "content",
            jstring_new(0, m->tool_result.result ? m->tool_result.result : ""));
        jarray_add(jarr, jm);
        jcur = NULL;
        cur_id = NULL;
        break;
      }
    }
  }
  return jarr;
}

static jnode_t* openai_serialize_tool(const tool_t* tool) {
  const tooldef_t* tooldef = &tool->def;
  jnode_t* jtool = jobject_new();
  jobject_put(jtool, "type", jstring_new(0, "function"));
  jnode_t* func = jobject_new();
  jobject_put(jtool, "function", func);
  jobject_put(func, "name", jstring_new(0, tooldef->name));
  jobject_put(func, "description", jstring_new(0, tooldef->description));
  jnode_t* params = jobject_new();
  jobject_put(func, "parameters", params);
  jobject_put(params, "type", jstring_new(0, "object"));
  jnode_t* props = jobject_new();
  jobject_put(params, "properties", props);
  for (size_t i = 0; i < tooldef->n_param; ++i) {
    const paramdef_t* p = &tooldef->params[i];
    jnode_t* pdef = jobject_new();
    jobject_put(pdef, "type", jstring_new(0, p->type));
    jobject_put(pdef, "description", jstring_new(0, p->description));
    jobject_put(props, p->name, pdef);
  }
  jnode_t* required = jarray_new();
  jobject_put(params, "required", required);
  for (size_t i = 0; i < tooldef->n_param; ++i) {
    const paramdef_t* p = &tooldef->params[i];
    if (p->required) jarray_add(required, jstring_new(0, p->name));
  }
  return jtool;
}

static jnode_t* openai_body(const pctx_t* ctx, bool stream) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, ctx->model->name));

  // Convert the general conversation into the API-specific messages array
  // (the system prompt is prepended by openai_messages_to_json).
  jobject_put(jbody, "messages", openai_messages_to_json(ctx, jarray_new()));

  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  if (ctx->toolset) {
    for (size_t i = 0; i < ctx->toolset->n_tool; ++i) {
      jarray_add(jtools, openai_serialize_tool(&ctx->toolset->tools[i]));
    }
  }

  if (ctx->model->thinking) {
    jnode_t* jthinking = jobject_new();
    jobject_put(jbody, "thinking", jthinking);
    jobject_put(jthinking, "type", jstring_new(0, ctx->model->thinking));
  }
  if (ctx->model->reasoning_effort) {
    jobject_put(jbody, "reasoning_effort",
                jstring_new(0, ctx->model->reasoning_effort));
  }
  jobject_put(jbody, "top_p", jnumber_new(ctx->model->top_p));
  if (ctx->model->max_tokens >= 0) {
    jobject_put(jbody, "max_tokens", jnumber_new(ctx->model->max_tokens));
  }
  jobject_put(jbody, "stream", jbool_new(stream));
  return jbody;
}

/* ==============================
 *           HTTP layer
 * ============================== */

static err_t openai_http_call(const pctx_t* ctx, bool stream,
                              response_t* resp_out) {
  char url[URL_LEN];
  char auth[AUTH_HEADER_LEN];
  snprintf(url, sizeof(url), "%s/chat/completions", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json"};

  jnode_t* jbody = openai_body(ctx, stream);
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

// True when the SSE payload is the terminating "[DONE]" marker.
bool openai_done_marker(const char* data, size_t len) {
  while (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\r')) {
    ++data;
    --len;
  }
  return len == 6 && memcmp(data, "[DONE]", 6) == 0;
}

// Streaming helpers ---------------------------------------------------------

// Return (and lazily create) the id of the assistant turn being accumulated by
// the stream. A fresh id is generated when the trailing messages no longer
// belong to the current stream turn.
static const char* openai_stream_turn_id(pctx_t* ctx) {
  size_t n = ctx->messages->n_message;
  if (ctx->stream_turn_id && n > 0) {
    message_t* last = &ctx->messages->messages[n - 1];
    if (last->type == REASONING || last->type == ASSISTANT ||
        last->type == TOOL_CALL) {
      const char* mid = message_id(last);
      if (mid && strcmp(mid, ctx->stream_turn_id) == 0) {
        return ctx->stream_turn_id;
      }
    }
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = pctx_new_turn_id();
  return ctx->stream_turn_id;
}

// Get the trailing message of `type` within the current stream turn, creating
// it (with the turn id) if absent.
static err_t openai_ensure_turn_message(pctx_t* ctx, msgtyp_t type,
                                        message_t** out) {
  *out = NULL;
  const char* tid = openai_stream_turn_id(ctx);
  if (!tid) return ERROR_OUT_OF_MEMORY;
  size_t n = ctx->messages->n_message;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    if (m->type == type) {
      *out = m;
      return ERROR_NONE;
    }
  }
  message_t* m = message_new(type);
  if (!m) return ERROR_OUT_OF_MEMORY;
  if (message_set_id(m, tid) != ERROR_NONE) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  msglist_add_message(ctx->messages, m);
  // msglist_add_message takes ownership of the wrapper; return the slot.
  *out = &ctx->messages->messages[ctx->messages->n_message - 1];
  return ERROR_NONE;
}

// Get (or create) the TOOL_CALL message at stream `index` within the current
// turn. Gaps are filled so out-of-order fragments are merged correctly.
static err_t openai_stream_tool_call(pctx_t* ctx, int index, message_t** out) {
  *out = NULL;
  if (index < 0) return ERROR_NONE;
  const char* tid = openai_stream_turn_id(ctx);
  if (!tid) return ERROR_OUT_OF_MEMORY;
  size_t n = ctx->messages->n_message;
  size_t start = n;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    start = i - 1;
  }
  size_t existing = 0;
  for (size_t i = start; i < n; ++i) {
    message_t* m = &ctx->messages->messages[i];
    if (m->type != TOOL_CALL) continue;
    if (existing == (size_t)index) {
      *out = m;
      return ERROR_NONE;
    }
    ++existing;
  }
  // Append new tool-call messages until `index` is covered.
  for (size_t k = existing; k <= (size_t)index; ++k) {
    message_t* m = message_new(TOOL_CALL);
    if (!m) return ERROR_OUT_OF_MEMORY;
    if (message_set_id(m, tid) != ERROR_NONE) {
      message_delete(m);
      return ERROR_OUT_OF_MEMORY;
    }
    msglist_add_message(ctx->messages, m);
    // msglist_add_message takes ownership of the wrapper; return the slot.
    *out = &ctx->messages->messages[ctx->messages->n_message - 1];
  }
  return ERROR_NONE;
}

// Non-streaming response: convert the assistant message into general
// REASONING/ASSISTANT/TOOL_CALL messages (all sharing one turn id) and expose
// the latest state.
err_t openai_update_full(pctx_t* ctx, jnode_t* jfirst) {
  jnode_t* jmessage = jobject_get(jfirst, "message");
  if (!jmessage) {
    log(ERROR, "Response missing message");
    return ERROR_UNKNOWN;
  }

  char* turn_id = pctx_new_turn_id();
  if (!turn_id) return ERROR_OUT_OF_MEMORY;

  jnode_t* jreasoning = jobject_get(jmessage, "reasoning_content");
  if (jis_string(jreasoning) && jstring_len(jreasoning)) {
    message_t* m = message_new(REASONING);
    if (!m) {
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->reasoning.text = strdup(jstring_content(jreasoning));
    if (!m->reasoning.text) {
      message_delete(m);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->reasoning.text_len = strlen(m->reasoning.text);
    if (message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    msglist_add_message(ctx->messages, m);
    ctx->latest_reasoning = strdup(jstring_content(jreasoning));
  }

  // content (string, or an array of text blocks on some endpoints).
  char* content_text = NULL;
  jnode_t* jcontent = jobject_get(jmessage, "content");
  if (jis_string(jcontent) && jstring_len(jcontent)) {
    content_text = strdup(jstring_content(jcontent));
  } else if (jis_array(jcontent)) {
    jnode_t* jacc = jstring_new(0, "");
    for (int i = 0; i < jarray_size(jcontent); ++i) {
      jnode_t* jblock = jarray_get(jcontent, i);
      jnode_t* jtype = jobject_get(jblock, "type");
      if (jis_string(jtype) && strcmp(jstring_content(jtype), "text") == 0) {
        jnode_t* jtext = jobject_get(jblock, "text");
        if (jis_string(jtext)) jstring_concat(jacc, jstring_content(jtext));
      }
    }
    if (jstring_len(jacc)) content_text = strdup(jstring_content(jacc));
    jdelete(jacc);
  }
  if (content_text) {
    message_t* m = message_new(ASSISTANT);
    if (!m) {
      free(content_text);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->assistant.text = content_text;
    m->assistant.text_len = strlen(content_text);
    if (message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    msglist_add_message(ctx->messages, m);
    ctx->latest_content = strdup(content_text);
  }

  // tool_calls.
  jnode_t* jtool_calls = jobject_get(jmessage, "tool_calls");
  if (jis_array(jtool_calls)) {
    for (int i = 0; i < jarray_size(jtool_calls); ++i) {
      jnode_t* jcall = jarray_get(jtool_calls, i);
      jnode_t* jfunc = jobject_get(jcall, "function");
      jnode_t* jid = jobject_get(jcall, "id");
      jnode_t* jname = jfunc ? jobject_get(jfunc, "name") : NULL;
      jnode_t* jargs = jfunc ? jobject_get(jfunc, "arguments") : NULL;
      message_t* m = message_new(TOOL_CALL);
      if (!m) {
        free(turn_id);
        return ERROR_OUT_OF_MEMORY;
      }
      if (message_set_id(m, turn_id) != ERROR_NONE) {
        message_delete(m);
        free(turn_id);
        return ERROR_OUT_OF_MEMORY;
      }
      if (jis_string(jid)) {
        m->tool_call.call_id = strdup(jstring_content(jid));
        m->tool_call.call_id_len =
            m->tool_call.call_id ? strlen(m->tool_call.call_id) : 0;
      }
      if (jis_string(jname)) {
        m->tool_call.name = strdup(jstring_content(jname));
        m->tool_call.name_len =
            m->tool_call.name ? strlen(m->tool_call.name) : 0;
      }
      if (jis_string(jargs)) {
        m->tool_call.args = strdup(jstring_content(jargs));
        m->tool_call.args_len =
            m->tool_call.args ? strlen(m->tool_call.args) : 0;
      }
      msglist_add_message(ctx->messages, m);
    }
  }
  free(turn_id);

  jnode_t* jfinish = jobject_get(jfirst, "finish_reason");
  const char* finish = jis_string(jfinish) ? jstring_content(jfinish) : NULL;
  if (finish) {
    ctx->latest_stop_reason = strdup(finish);
    // Any terminal reason other than a tool-call request ends the turn.
    ctx->finished = (strcmp(finish, "tool_calls") != 0);
    if (strcmp(finish, "tool_calls") == 0) {
      ctx->tool_calls_ready = true;
      err_t err = pctx_latest_calls_from_turn(ctx);
      if (err != ERROR_NONE) return err;
    }
  }
  return ERROR_NONE;
}

// Streaming delta: merge into the trailing assistant turn. Tool calls are only
// exposed once the finish_reason arrives, so partial fragments are never
// executed.
err_t openai_update_stream(pctx_t* ctx, jnode_t* jfirst, jnode_t* jdelta) {
  // role delta: begin (or continue) the assistant turn.
  jnode_t* jdelta_role = jobject_get(jdelta, "role");
  if (jis_string(jdelta_role)) {
    if (strcmp(jstring_content(jdelta_role), "assistant") == 0) {
      openai_stream_turn_id(ctx);
    }
  }

  jnode_t* jreasoning = jobject_get(jdelta, "reasoning_content");
  if (jis_string(jreasoning) && jstring_len(jreasoning)) {
    message_t* m = NULL;
    err_t err = openai_ensure_turn_message(ctx, REASONING, &m);
    if (err != ERROR_NONE) return err;
    err = pctx_append_text(&m->reasoning.text, &m->reasoning.text_len,
                           jstring_content(jreasoning));
    if (err != ERROR_NONE) return err;
    free(ctx->latest_reasoning);
    ctx->latest_reasoning = strdup(jstring_content(jreasoning));
  }

  // content.
  jnode_t* jcontent = jobject_get(jdelta, "content");
  if (jis_string(jcontent) && jstring_len(jcontent)) {
    message_t* m = NULL;
    err_t err = openai_ensure_turn_message(ctx, ASSISTANT, &m);
    if (err != ERROR_NONE) return err;
    err = pctx_append_text(&m->assistant.text, &m->assistant.text_len,
                           jstring_content(jcontent));
    if (err != ERROR_NONE) return err;
    free(ctx->latest_content);
    ctx->latest_content = strdup(jstring_content(jcontent));
  }

  // tool_calls fragments.
  jnode_t* jtool_calls = jobject_get(jdelta, "tool_calls");
  if (jis_array(jtool_calls)) {
    for (int i = 0; i < jarray_size(jtool_calls); ++i) {
      jnode_t* jdc = jarray_get(jtool_calls, i);
      jnode_t* jindex_node = jobject_get(jdc, "index");
      int index =
          jis_number(jindex_node) ? (int)jas_number(jindex_node)->value : -1;
      message_t* m = NULL;
      err_t err = openai_stream_tool_call(ctx, index, &m);
      if (err != ERROR_NONE) return err;
      if (!m) continue;

      jnode_t* jdid = jobject_get(jdc, "id");
      if (jis_string(jdid) && !m->tool_call.call_id) {
        m->tool_call.call_id = strdup(jstring_content(jdid));
        m->tool_call.call_id_len =
            m->tool_call.call_id ? strlen(m->tool_call.call_id) : 0;
      }
      jnode_t* jdf = jobject_get(jdc, "function");
      if (jis_object(jdf)) {
        jnode_t* jdn = jobject_get(jdf, "name");
        if (jis_string(jdn)) {
          err = pctx_append_text(&m->tool_call.name, &m->tool_call.name_len,
                                 jstring_content(jdn));
          if (err != ERROR_NONE) return err;
        }
        jnode_t* jda = jobject_get(jdf, "arguments");
        if (jis_string(jda)) {
          err = pctx_append_text(&m->tool_call.args, &m->tool_call.args_len,
                                 jstring_content(jda));
          if (err != ERROR_NONE) return err;
        }
      }
    }
  }

  // finish_reason.
  jnode_t* jfinish = jobject_get(jfirst, "finish_reason");
  if (jis_string(jfinish)) {
    const char* finish = jstring_content(jfinish);
    free(ctx->latest_stop_reason);
    ctx->latest_stop_reason = strdup(finish);
    // Any terminal reason other than a tool-call request ends the turn.
    if (strcmp(finish, "tool_calls") != 0) ctx->finished = true;
    if (strcmp(finish, "tool_calls") == 0) {
      ctx->tool_calls_ready = true;
      err_t err = pctx_latest_calls_from_turn(ctx);
      if (err != ERROR_NONE) return err;
    }
    free(ctx->stream_turn_id);
    ctx->stream_turn_id = NULL;
  }
  return ERROR_NONE;
}

/* ==============================
 *       Provider interface
 * ============================== */

err_t cc_create_context(void** context) {
  if (!context) return ERROR_NULLPTR;
  return pctx_new((pctx_t**)context);
}

void cc_delete_context(void* context) { pctx_delete((pctx_t*)context); }

err_t cc_serialize(const void* context, char** data, size_t* len) {
  return pctx_serialize((const pctx_t*)context, data, len);
}

err_t cc_deserialize(void* context, const char* data, size_t len) {
  if (!context) return ERROR_NULLPTR;
  return pctx_deserialize((pctx_t*)context, data, len);
}

err_t cc_set_model(void* context, const model_t* model) {
  return pctx_set_model((pctx_t*)context, model);
}

model_t* cc_get_model(void* context) {
  return pctx_get_model((pctx_t*)context);
}

err_t cc_set_toolset(void* context, const toolset_t* toolset) {
  return pctx_set_toolset((pctx_t*)context, toolset);
}

toolset_t* cc_get_toolset(void* context) {
  return pctx_get_toolset((pctx_t*)context);
}

err_t cc_set_system_prompt(void* context, const char* system_prompt) {
  return pctx_set_system_prompt((pctx_t*)context, system_prompt);
}

const char* cc_get_system_prompt(const void* context) {
  return pctx_get_system_prompt((const pctx_t*)context);
}

size_t cc_message_count(const void* context) {
  return pctx_message_count((const pctx_t*)context);
}

err_t cc_add_user_message(void* context, const char* message) {
  return pctx_add_user_message((pctx_t*)context, message);
}

err_t cc_add_assistant_message(void* context, const char* message) {
  return pctx_add_assistant_message((pctx_t*)context, message);
}

err_t cc_add_tool_message(void* context, const char* id, const char* tool_name,
                          const char* result) {
  return pctx_add_tool_message((pctx_t*)context, id, tool_name, result);
}

void cc_clear_messages(void* context) { pctx_clear_messages((pctx_t*)context); }

void cc_pop_message(void* context) { pctx_pop_message((pctx_t*)context); }

err_t cc_call(void* context, char** response, size_t* len) {
  pctx_t* ctx = context;
  if (!ctx || !response || !len) return ERROR_NULLPTR;
  if (!ctx->model) return ERROR_NULLPTR;
  *response = NULL;
  *len = 0;
  // A new turn starts; drop the previous snapshot, stream id and finished
  // state.
  if (ctx->snapshot) {
    msglist_delete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
  ctx->finished = false;

  response_t resp = {0};
  err_t err = openai_http_call(ctx, false, &resp);
  if (err != ERROR_NONE) return err;
  *response = (char*)resp.data;  // owned by the caller now
  *len = resp.data_len;
  return ERROR_NONE;
}

struct cc_sse {
  void* context;
  strmcb_t callback;
  void* userp;
};

static err_t cc_sse_forward(const event_t* event, void* userp) {
  struct cc_sse* w = (struct cc_sse*)userp;
  return w->callback(w->context, event, w->userp);
}

err_t cc_call_stream(void* context, strmcb_t callback, void* userp) {
  pctx_t* ctx = context;
  if (!ctx || !callback) return ERROR_NULLPTR;
  if (!ctx->model) return ERROR_NULLPTR;
  if (ctx->snapshot) {
    msglist_delete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
  ctx->finished = false;

  char url[URL_LEN];
  char auth[AUTH_HEADER_LEN];
  snprintf(url, sizeof(url), "%s/chat/completions", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json"};

  jnode_t* jbody = openai_body(ctx, true);
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
  struct cc_sse wrapper = {
      .context = context, .callback = callback, .userp = userp};
  err_t err = http_sse(&request, cc_sse_forward, &wrapper);
  free(body);
  if (err != ERROR_NONE) {
    log(ERROR, "SSE request failed: %s", error_str(err));
  }
  return err;
}

err_t cc_update(void* context, const char* response, size_t len) {
  pctx_t* ctx = context;
  if (!ctx || !response) return ERROR_NULLPTR;
  // The "[DONE]" marker terminates the stream. Clear the latest state so the
  // agent does not re-execute the tool calls it already handled.
  if (openai_done_marker(response, len)) {
    pctx_clear_latest(ctx);
    free(ctx->stream_turn_id);
    ctx->stream_turn_id = NULL;
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
    err = openai_update_stream(ctx, jfirst, jdelta);
  } else {
    err = openai_update_full(ctx, jfirst);
  }
  jdelete(json);
  return err;
}

void cc_rewind(void* context) { pctx_rewind((pctx_t*)context); }

bool cc_is_finished(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->finished : false;
}

const char* cc_latest_stop_reason(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_stop_reason : NULL;
}

const char* cc_latest_reasoning(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_reasoning : NULL;
}

const char* cc_latest_content(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_content : NULL;
}

err_t cc_latest_tool_calls(const void* context, toolcall_t** tool_calls,
                           size_t* n_tool_call) {
  return pctx_latest_tool_calls((pctx_t*)context, tool_calls, n_tool_call);
}

const char* cc_error_str(err_t err) {
  if (is_custom(err)) return "Custom error";
  return error_str(err);
}

/* ==============================
 *          Provider
 * ============================== */

static protyp_t cc_type() { return OPENAI_COMPATIBLE; }
static const char* cc_name() { return "OpenAI Compatible"; }

static const provider_t openai_provider = {
    .type = cc_type,
    .name = cc_name,
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
    .update = cc_update,
    .rewind = cc_rewind,
    .is_finished = cc_is_finished,
    .latest_stop_reason = cc_latest_stop_reason,
    .latest_reasoning = cc_latest_reasoning,
    .latest_content = cc_latest_content,
    .latest_tool_calls = cc_latest_tool_calls,
};

const provider_t* get_openai_compatible_provider() { return &openai_provider; }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sjson.h"

#include "log.h"
#include "http.h"
#include "provider/responses.h"

#define URL_LEN 512
#define AUTH_HEADER_LEN 512

/* ==============================
 *        Request building
 * ============================== */

// A user input item:
// {"role":"user","content":[{"type":"input_text","text":...}]}
static jnode_t* responses_user_item(const char* text) {
  jnode_t* jitem = jobject_new();
  jobject_put(jitem, "role", jstring_new(0, "user"));
  jnode_t* jcontent = jarray_new();
  jobject_put(jitem, "content", jcontent);
  jnode_t* jpart = jobject_new();
  jobject_put(jpart, "type", jstring_new(0, "input_text"));
  jobject_put(jpart, "text", jstring_new(0, text ? text : ""));
  jarray_add(jcontent, jpart);
  return jitem;
}

// An assistant message item:
// {"role":"assistant","content":[{"type":"output_text","text":...}]}
static jnode_t* responses_assistant_item(const char* text) {
  jnode_t* jitem = jobject_new();
  jobject_put(jitem, "role", jstring_new(0, "assistant"));
  jnode_t* jcontent = jarray_new();
  jobject_put(jitem, "content", jcontent);
  jnode_t* jpart = jobject_new();
  jobject_put(jpart, "type", jstring_new(0, "output_text"));
  jobject_put(jpart, "text", jstring_new(0, text ? text : ""));
  jarray_add(jcontent, jpart);
  return jitem;
}

// A function call input item. NOTE: unlike Chat Completions, function calls are
// NOT content parts of an assistant message; they are TOP-LEVEL input items.
static jnode_t* responses_function_call_item(const char* call_id,
                                             const char* name,
                                             const char* arguments) {
  jnode_t* jitem = jobject_new();
  jobject_put(jitem, "type", jstring_new(0, "function_call"));
  jobject_put(jitem, "call_id", jstring_new(0, call_id ? call_id : ""));
  jobject_put(jitem, "name", jstring_new(0, name ? name : ""));
  jobject_put(jitem, "arguments", jstring_new(0, arguments ? arguments : ""));
  return jitem;
}

// A function call output input item (tool result).
static jnode_t* responses_function_call_output_item(const char* call_id,
                                                    const char* output) {
  jnode_t* jitem = jobject_new();
  jobject_put(jitem, "type", jstring_new(0, "function_call_output"));
  jobject_put(jitem, "call_id", jstring_new(0, call_id ? call_id : ""));
  jobject_put(jitem, "output", jstring_new(0, output ? output : ""));
  return jitem;
}

// A reasoning input item carrying the chain-of-thought plain text:
// {"type":"reasoning","id":...,"content":[{"type":"reasoning_text",
//  "text":...}]}. DeepSeek requires the reasoning_content of previous turns to
// be echoed back on every request that carries tools (it 400s otherwise); the
// plain-text content is merged into the adjacent assistant message. Summary /
// encrypted content are not supported and are therefore not emitted.
static jnode_t* responses_reasoning_item(const char* id, const char* text) {
  jnode_t* jitem = jobject_new();
  jobject_put(jitem, "type", jstring_new(0, "reasoning"));
  if (id) jobject_put(jitem, "id", jstring_new(0, id));
  jnode_t* jcontent = jarray_new();
  jobject_put(jitem, "content", jcontent);
  jnode_t* jpart = jobject_new();
  jobject_put(jpart, "type", jstring_new(0, "reasoning_text"));
  jobject_put(jpart, "text", jstring_new(0, text ? text : ""));
  jarray_add(jcontent, jpart);
  return jitem;
}

// Convert the general conversation into the Responses API "input" array.
//
// Mapping rules (the flat message list is walked in order):
//   USER        -> message item (role user, input_text part)
//   ASSISTANT   -> message item (role assistant, output_text part)
//   REASONING   -> top-level reasoning item (plain-text chain of thought)
//   TOOL_CALL   -> top-level function_call item (call_id/name/arguments)
//   TOOL_RESULT -> top-level function_call_output item (call_id/output)
// SYSTEM messages are dropped: the system prompt is sent in the top-level
// "instructions" field instead.
static jnode_t* responses_input_to_json(const pctx_t* ctx, jnode_t* jarr) {
  for (size_t i = 0; i < ctx->messages->n_message; ++i) {
    const message_t* m = &ctx->messages->messages[i];
    switch (m->type) {
      case SYSTEM:
        // Not echoed (instructions covers the system prompt).
        break;
      case USER: jarray_add(jarr, responses_user_item(m->user.text)); break;
      case REASONING:
        if (m->reasoning.text && m->reasoning.text_len) {
          jarray_add(
              jarr, responses_reasoning_item(message_id(m), m->reasoning.text));
        }
        break;
      case ASSISTANT:
        if (m->assistant.text && m->assistant.text_len) {
          jarray_add(jarr, responses_assistant_item(m->assistant.text));
        }
        break;
      case TOOL_CALL:
        jarray_add(jarr, responses_function_call_item(m->tool_call.call_id,
                                                      m->tool_call.name,
                                                      m->tool_call.args));
        break;
      case TOOL_RESULT:
        jarray_add(jarr, responses_function_call_output_item(
                             m->tool_result.call_id, m->tool_result.result));
        break;
    }
  }
  return jarr;
}

static jnode_t* responses_serialize_tool(const tool_t* tool) {
  const tooldef_t* tooldef = &tool->def;
  // In the Responses API the function schema lives directly on the tool (no
  // nested "function" object as in Chat Completions).
  jnode_t* jtool = jobject_new();
  jobject_put(jtool, "type", jstring_new(0, "function"));
  jobject_put(jtool, "name", jstring_new(0, tooldef->name));
  jobject_put(jtool, "description", jstring_new(0, tooldef->description));
  jnode_t* params = jobject_new();
  jobject_put(jtool, "parameters", params);
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

static jnode_t* responses_body(const pctx_t* ctx, bool stream) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, ctx->model->name));
  // The system prompt is a top-level "instructions" field (not an input item).
  jobject_put(jbody, "instructions",
              jstring_new(0, ctx->system_prompt ? ctx->system_prompt : ""));
  jobject_put(jbody, "input", responses_input_to_json(ctx, jarray_new()));

  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  if (ctx->toolset) {
    for (size_t i = 0; i < ctx->toolset->n_tool; ++i) {
      jarray_add(jtools, responses_serialize_tool(&ctx->toolset->tools[i]));
    }
  }

  // Reasoning is configured through the "reasoning" object (effort), not a
  // separate "thinking" field. Prefer reasoning_effort when set; fall back to
  // "high" when the DeepSeek-style thinking toggle is enabled.
  const char* effort = ctx->model->reasoning_effort;
  if (!effort && ctx->model->thinking &&
      strcmp(ctx->model->thinking, "enabled") == 0) {
    effort = "high";
  }
  if (effort) {
    jnode_t* jreasoning = jobject_new();
    jobject_put(jbody, "reasoning", jreasoning);
    jobject_put(jreasoning, "effort", jstring_new(0, effort));
  }
  jobject_put(jbody, "top_p", jnumber_new(ctx->model->top_p));
  // The Responses API names the output limit max_output_tokens (not
  // max_tokens).
  if (ctx->model->max_tokens >= 0) {
    jobject_put(jbody, "max_output_tokens",
                jnumber_new(ctx->model->max_tokens));
  }
  jobject_put(jbody, "stream", jbool_new(stream));
  return jbody;
}

/* ==============================
 *           HTTP layer
 * ============================== */

static err_t responses_http_call(const pctx_t* ctx, bool stream,
                                 response_t* resp_out) {
  char url[URL_LEN];
  char auth[AUTH_HEADER_LEN];
  snprintf(url, sizeof(url), "%s/responses", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json"};

  jnode_t* jbody = responses_body(ctx, stream);
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

// True when the SSE payload is the terminating "[DONE]" marker (sent by some
// compatible proxies). The Responses API itself ends with a
// response.completed/.incomplete/.failed event instead.
static bool responses_done_marker(const char* data, size_t len) {
  while (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\r')) {
    ++data;
    --len;
  }
  return len == 6 && memcmp(data, "[DONE]", 6) == 0;
}

// Streaming helpers ---------------------------------------------------------

// Return the id of the assistant turn being accumulated by the stream,
// generating one on first use. One Responses API response == one turn.
static const char* responses_turn_id(pctx_t* ctx) {
  if (!ctx->stream_turn_id) {
    ctx->stream_turn_id = pctx_new_turn_id();
  }
  return ctx->stream_turn_id;
}

// Append a fresh message of `type` to the current stream turn. Unlike
// responses_turn_ensure, this never reuses an existing message: every
// function-call output item must become its own TOOL_CALL message so parallel
// calls do not clobber each other.
static err_t responses_turn_append(pctx_t* ctx, msgtyp_t type,
                                   message_t** out) {
  *out = NULL;
  const char* tid = responses_turn_id(ctx);
  if (!tid) return ERROR_OUT_OF_MEMORY;
  message_t* m = message_new(type);
  if (!m) return ERROR_OUT_OF_MEMORY;
  if (message_set_id(m, tid) != ERROR_NONE) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  msglist_add_message(ctx->messages, m);
  *out = &ctx->messages->messages[ctx->messages->n_message - 1];
  return ERROR_NONE;
}

// Get the last message of `type` within the current stream turn, creating it
// (with the turn id) if absent. Content is streamed item-by-item, so the last
// message of a type is always the one currently being accumulated.
static err_t responses_turn_ensure(pctx_t* ctx, msgtyp_t type,
                                   message_t** out) {
  *out = NULL;
  const char* tid = responses_turn_id(ctx);
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
  return responses_turn_append(ctx, type, out);
}

// Find a TOOL_CALL message by its function_call item id within the current
// stream turn. Streaming deltas reference the ITEM id (fc_...) through
// "item_id"; the general form stores that item id in tool_call.call_id.
static message_t* responses_find_tool_call(pctx_t* ctx, const char* item_id) {
  if (!item_id) return NULL;
  const char* tid = ctx->stream_turn_id;
  size_t n = ctx->messages->n_message;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    if (m->type == TOOL_CALL && m->tool_call.call_id &&
        strcmp(m->tool_call.call_id, item_id) == 0) {
      return m;
    }
  }
  return NULL;
}

// Concatenate the text parts of an item (content and/or summary arrays).
// msg item:  content[].{output_text|text}.text
// reasoning: content[].reasoning_text.text, summary[].summary_text.text
static void responses_collect_text(jnode_t* jacc, jnode_t* jparts) {
  if (!jis_array(jparts)) return;
  for (int i = 0; i < jarray_size(jparts); ++i) {
    jnode_t* jpart = jarray_get(jparts, i);
    jnode_t* jtype = jobject_get(jpart, "type");
    if (!jis_string(jtype)) continue;
    const char* type = jstring_content(jtype);
    if (strcmp(type, "output_text") == 0 || strcmp(type, "text") == 0 ||
        strcmp(type, "reasoning_text") == 0 ||
        strcmp(type, "summary_text") == 0) {
      jnode_t* jtext = jobject_get(jpart, "text");
      if (jis_string(jtext)) jstring_concat(jacc, jstring_content(jtext));
    }
  }
}

// Replace a message's owned text with `text` (no-op when text is NULL).
static void responses_replace_text(char** dst, size_t* dst_len,
                                   const char* text) {
  if (!text) return;
  free(*dst);
  *dst = strdup(text);
  *dst_len = *dst ? strlen(*dst) : 0;
}

// Non-streaming response: convert the output items into general
// REASONING/ASSISTANT/TOOL_CALL messages (all sharing the response id) and
// expose the latest state.
static err_t responses_update_full(pctx_t* ctx, jnode_t* json) {
  // The turn id: all messages produced by this response share it.
  jnode_t* jid = jobject_get(json, "id");
  char* turn_id =
      jis_string(jid) ? strdup(jstring_content(jid)) : pctx_new_turn_id();
  if (!turn_id) return ERROR_OUT_OF_MEMORY;

  jnode_t* joutput = jobject_get(json, "output");
  if (!jis_array(joutput)) {
    free(turn_id);
    log(ERROR, "Response missing output");
    return ERROR_UNKNOWN;
  }

  // Gather the accumulated reasoning text, message text and function calls.
  jnode_t* jreasoning = jstring_new(0, "");
  jnode_t* jtext = jstring_new(0, "");
  bool has_fc = false;
  for (int i = 0; i < jarray_size(joutput); ++i) {
    jnode_t* jitem = jarray_get(joutput, i);
    jnode_t* jtype = jobject_get(jitem, "type");
    if (!jis_string(jtype)) continue;
    const char* type = jstring_content(jtype);
    if (strcmp(type, "reasoning") == 0) {
      jnode_t* jcontent = jobject_get(jitem, "content");
      jnode_t* jsummary = jobject_get(jitem, "summary");
      responses_collect_text(jreasoning, jcontent);
      responses_collect_text(jreasoning, jsummary);
    } else if (strcmp(type, "message") == 0) {
      jnode_t* jcontent = jobject_get(jitem, "content");
      responses_collect_text(jtext, jcontent);
    } else if (strcmp(type, "function_call") == 0) {
      has_fc = true;
    }
  }

  // REASONING message.
  if (jstring_len(jreasoning)) {
    message_t* m = message_new(REASONING);
    if (!m) {
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->reasoning.text = strdup(jstring_content(jreasoning));
    if (!m->reasoning.text || message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->reasoning.text_len = strlen(m->reasoning.text);
    msglist_add_message(ctx->messages, m);
    ctx->latest_reasoning = strdup(jstring_content(jreasoning));
  }

  // ASSISTANT message.
  if (jstring_len(jtext)) {
    message_t* m = message_new(ASSISTANT);
    if (!m) {
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->assistant.text = strdup(jstring_content(jtext));
    if (!m->assistant.text || message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    m->assistant.text_len = strlen(m->assistant.text);
    msglist_add_message(ctx->messages, m);
    ctx->latest_content = strdup(jstring_content(jtext));
  }

  // TOOL_CALL messages, in output order.
  for (int i = 0; i < jarray_size(joutput); ++i) {
    jnode_t* jitem = jarray_get(joutput, i);
    jnode_t* jtype = jobject_get(jitem, "type");
    if (!jis_string(jtype) ||
        strcmp(jstring_content(jtype), "function_call") != 0) {
      continue;
    }
    jnode_t* jfc_id = jobject_get(jitem, "id");
    jnode_t* jname = jobject_get(jitem, "name");
    jnode_t* jargs = jobject_get(jitem, "arguments");
    char* args = NULL;
    if (jis_string(jargs)) {
      args = strdup(jstring_content(jargs));
    } else if (jargs) {
      args = jto_string(jargs);  // some endpoints send an object
    }
    message_t* m = message_new(TOOL_CALL);
    if (!m) {
      free(args);
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    if (message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      free(args);
      jdelete(jreasoning);
      jdelete(jtext);
      free(turn_id);
      return ERROR_OUT_OF_MEMORY;
    }
    // The ITEM id (fc_...) is stored in tool_call.call_id. Deltas reference it
    // via "item_id", and the echoed function_call/function_call_output items
    // only need to be internally consistent, so no separate call_id is kept.
    if (jis_string(jfc_id)) {
      m->tool_call.call_id = strdup(jstring_content(jfc_id));
      m->tool_call.call_id_len =
          m->tool_call.call_id ? strlen(m->tool_call.call_id) : 0;
    }
    if (jis_string(jname)) {
      m->tool_call.name = strdup(jstring_content(jname));
      m->tool_call.name_len = m->tool_call.name ? strlen(m->tool_call.name) : 0;
    }
    if (args) {
      m->tool_call.args = args;
      m->tool_call.args_len = strlen(args);
    }
    msglist_add_message(ctx->messages, m);
  }
  jdelete(jreasoning);
  jdelete(jtext);

  // Finish semantics: a completed response that requested function calls keeps
  // the turn open so the agent can run the tools and call again; everything
  // else ends the turn (avoids infinite loops on truncation/failure).
  jnode_t* jstatus = jobject_get(json, "status");
  const char* status = jis_string(jstatus) ? jstring_content(jstatus) : NULL;
  bool ok = !status || strcmp(status, "completed") == 0;
  if (has_fc && ok) {
    // The turn stays open: the agent runs the tools and calls again.
    ctx->finished = false;
    ctx->tool_calls_ready = true;
    ctx->latest_stop_reason = strdup("function_call");
    err_t err = pctx_latest_calls_from_turn(ctx);
    free(turn_id);
    return err;
  }
  ctx->finished = true;
  ctx->latest_stop_reason = strdup(status ? status : "completed");
  if (status && strcmp(status, "failed") == 0) {
    jnode_t* jerror = jobject_get(json, "error");
    jnode_t* jmsg = jerror ? jobject_get(jerror, "message") : NULL;
    if (jis_string(jmsg)) {
      log(ERROR, "Responses API error: %s", jstring_content(jmsg));
    }
  }
  free(turn_id);
  return ERROR_NONE;
}

// True when the current stream turn already holds assistant-kind messages
// (REASONING/ASSISTANT/TOOL_CALL). Used to tell whether output_item/delta
// events were streamed before the terminal event arrived.
static bool responses_turn_has_any(pctx_t* ctx) {
  const char* tid = ctx->stream_turn_id;
  size_t n = ctx->messages->n_message;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    return true;
  }
  return false;
}

// Terminal event (response.completed/.incomplete/.failed): sync the stored
// messages with the embedded full response, then expose the latest state.

// Fallback helper: ensure the trailing turn carries a non-empty message of
// `type` with the given full text. This is a no-op when the streaming events
// already delivered the text (the normal path); it only repairs the case where
// a server delivers the complete items solely in the terminal event.
static err_t responses_sync_turn_text(pctx_t* ctx, msgtyp_t type,
                                      const char* text) {
  if (!text || !*text) return ERROR_NONE;
  const char* tid = ctx->stream_turn_id;
  size_t n = ctx->messages->n_message;
  message_t* empty_msg = NULL;
  bool has_text = false;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    if (m->type != type) continue;
    const char* cur = type == ASSISTANT ? m->assistant.text : m->reasoning.text;
    if (cur && *cur) {
      has_text = true;
    } else if (!empty_msg) {
      empty_msg = m;
    }
  }
  if (has_text) return ERROR_NONE;
  if (empty_msg) {
    char** dst = type == ASSISTANT ? &empty_msg->assistant.text
                                   : &empty_msg->reasoning.text;
    size_t* dst_len = type == ASSISTANT ? &empty_msg->assistant.text_len
                                        : &empty_msg->reasoning.text_len;
    responses_replace_text(dst, dst_len, text);
    return ERROR_NONE;
  }
  const char* tid2 = responses_turn_id(ctx);
  if (!tid2) return ERROR_OUT_OF_MEMORY;
  message_t* m = message_new(type);
  if (!m) return ERROR_OUT_OF_MEMORY;
  if (type == ASSISTANT) {
    m->assistant.text = strdup(text);
    m->assistant.text_len = m->assistant.text ? strlen(m->assistant.text) : 0;
  } else {
    m->reasoning.text = strdup(text);
    m->reasoning.text_len = m->reasoning.text ? strlen(m->reasoning.text) : 0;
  }
  if (message_set_id(m, tid2) != ERROR_NONE) {
    message_delete(m);
    return ERROR_OUT_OF_MEMORY;
  }
  msglist_add_message(ctx->messages, m);
  return ERROR_NONE;
}

static err_t responses_finalize_stream(pctx_t* ctx, jnode_t* json) {
  jnode_t* jresponse = jobject_get(json, "response");
  jnode_t* joutput = jresponse ? jobject_get(jresponse, "output") : NULL;

  // If no item/delta events were streamed for this response, the embedded
  // full response is all we have: parse it like a non-streaming response (this
  // preserves output ordering and produces complete messages).
  if (jis_array(joutput) && !responses_turn_has_any(ctx)) {
    err_t err = responses_update_full(ctx, jresponse);
    free(ctx->stream_turn_id);
    ctx->stream_turn_id = NULL;
    return err;
  }

  // Sync the stored messages with the embedded full response. The dangerous
  // gap is empty function-call arguments (a server may skip the
  // arguments.delta events and only include the complete item here); message
  // and reasoning text are repaired only when they are still empty.
  bool has_fc = false;
  if (jis_array(joutput)) {
    jnode_t* jmsg = jstring_new(0, "");
    jnode_t* jreas = jstring_new(0, "");
    for (int i = 0; i < jarray_size(joutput); ++i) {
      jnode_t* jitem = jarray_get(joutput, i);
      jnode_t* jtype = jobject_get(jitem, "type");
      if (!jis_string(jtype)) continue;
      const char* type = jstring_content(jtype);
      if (strcmp(type, "function_call") == 0) {
        has_fc = true;
        jnode_t* jfc_id = jobject_get(jitem, "id");
        jnode_t* jname = jobject_get(jitem, "name");
        jnode_t* jargs = jobject_get(jitem, "arguments");
        const char* item_id =
            jis_string(jfc_id) ? jstring_content(jfc_id) : NULL;
        message_t* m = responses_find_tool_call(ctx, item_id);
        if (!m) {
          // The server never announced this item; append a fresh message.
          err_t err = responses_turn_append(ctx, TOOL_CALL, &m);
          if (err != ERROR_NONE) {
            jdelete(jmsg);
            jdelete(jreas);
            return err;
          }
          if (item_id) {
            m->tool_call.call_id = strdup(item_id);
            m->tool_call.call_id_len = strlen(item_id);
          }
        }
        if (jis_string(jname) && !m->tool_call.name) {
          m->tool_call.name = strdup(jstring_content(jname));
          m->tool_call.name_len =
              m->tool_call.name ? strlen(m->tool_call.name) : 0;
        }
        // Overwrite with the authoritative full arguments when present.
        if (jis_string(jargs) && jstring_len(jargs)) {
          responses_replace_text(&m->tool_call.args, &m->tool_call.args_len,
                                 jstring_content(jargs));
        }
      } else if (strcmp(type, "message") == 0) {
        jnode_t* jcontent = jobject_get(jitem, "content");
        responses_collect_text(jmsg, jcontent);
      } else if (strcmp(type, "reasoning") == 0) {
        jnode_t* jcontent = jobject_get(jitem, "content");
        jnode_t* jsummary = jobject_get(jitem, "summary");
        responses_collect_text(jreas, jcontent);
        responses_collect_text(jreas, jsummary);
      }
    }
    if (jstring_len(jmsg)) {
      err_t err =
          responses_sync_turn_text(ctx, ASSISTANT, jstring_content(jmsg));
      if (err != ERROR_NONE) {
        jdelete(jmsg);
        jdelete(jreas);
        return err;
      }
    }
    if (jstring_len(jreas)) {
      err_t err =
          responses_sync_turn_text(ctx, REASONING, jstring_content(jreas));
      if (err != ERROR_NONE) {
        jdelete(jmsg);
        jdelete(jreas);
        return err;
      }
    }
    jdelete(jmsg);
    jdelete(jreas);
  } else if (!jresponse) {
    // No embedded response object: fall back to the trailing turn.
    size_t n = ctx->messages->n_message;
    const char* tid = ctx->stream_turn_id;
    for (size_t i = n; i > 0; --i) {
      message_t* m = &ctx->messages->messages[i - 1];
      if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
        break;
      const char* mid = message_id(m);
      if (mid && tid && strcmp(mid, tid) != 0) break;
      if (m->type == TOOL_CALL) has_fc = true;
    }
  }

  jnode_t* jstatus = jobject_get(json, "status");
  if (jresponse && !jis_string(jstatus)) {
    jstatus = jobject_get(jresponse, "status");
  }
  const char* status = jis_string(jstatus) ? jstring_content(jstatus) : NULL;
  bool ok = !status || strcmp(status, "completed") == 0;
  if (has_fc && ok) {
    // The turn stays open: the agent runs the tools and calls again.
    ctx->finished = false;
    ctx->tool_calls_ready = true;
    ctx->latest_stop_reason = strdup("function_call");
    err_t err = pctx_latest_calls_from_turn(ctx);
    free(ctx->stream_turn_id);
    ctx->stream_turn_id = NULL;
    return err;
  }
  ctx->finished = true;
  ctx->latest_stop_reason = strdup(status ? status : "completed");
  if (status && strcmp(status, "failed") == 0) {
    jnode_t* jerror = jobject_get(json, "error");
    jnode_t* jmsg = jerror ? jobject_get(jerror, "message") : NULL;
    if (jis_string(jmsg)) {
      log(ERROR, "Responses API error: %s", jstring_content(jmsg));
    }
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
  return ERROR_NONE;
}

// Streaming event: merge the delta into the general assistant turn being
// built. One response == one turn; output items map to general messages
// (REASONING/ASSISTANT/TOOL_CALL) appended in arrival order.
static err_t responses_update_stream(pctx_t* ctx, jnode_t* json) {
  jnode_t* jtype = jobject_get(json, "type");
  if (!jis_string(jtype)) return ERROR_NONE;
  const char* type = jstring_content(jtype);

  if (strcmp(type, "response.created") == 0) {
    // Use the response id as the turn id so all items of this response are
    // grouped into one assistant turn.
    jnode_t* jresponse = jobject_get(json, "response");
    jnode_t* jid = jresponse ? jobject_get(jresponse, "id") : NULL;
    free(ctx->stream_turn_id);
    ctx->stream_turn_id =
        jis_string(jid) ? strdup(jstring_content(jid)) : pctx_new_turn_id();
    if (!ctx->stream_turn_id) return ERROR_OUT_OF_MEMORY;
    return ERROR_NONE;
  }

  // Terminal events: the stream ends with one of these (no [DONE] marker).
  if (strcmp(type, "response.completed") == 0 ||
      strcmp(type, "response.incomplete") == 0 ||
      strcmp(type, "response.failed") == 0) {
    return responses_finalize_stream(ctx, json);
  }

  if (strcmp(type, "response.in_progress") == 0 ||
      strcmp(type, "response.content_part.added") == 0 ||
      strcmp(type, "response.content_part.done") == 0 ||
      strcmp(type, "response.output_item.done") == 0) {
    // output_item.done carries the full item, but the complete text/arguments
    // arrive in the *_done events and the terminal event, so nothing to do.
    return ERROR_NONE;
  }

  if (strcmp(type, "response.output_item.added") == 0) {
    // Create the general message that corresponds to this output item.
    jnode_t* jitem = jobject_get(json, "item");
    if (!jis_object(jitem)) return ERROR_NONE;
    jnode_t* jitype = jobject_get(jitem, "type");
    if (!jis_string(jitype)) return ERROR_NONE;
    const char* itype = jstring_content(jitype);
    message_t* m = NULL;
    if (strcmp(itype, "reasoning") == 0) {
      err_t err = responses_turn_ensure(ctx, REASONING, &m);
      if (err != ERROR_NONE) return err;
      (void)m;
      return ERROR_NONE;
    }
    if (strcmp(itype, "message") == 0) {
      err_t err = responses_turn_ensure(ctx, ASSISTANT, &m);
      if (err != ERROR_NONE) return err;
      (void)m;
      return ERROR_NONE;
    }
    if (strcmp(itype, "function_call") == 0) {
      // Every function_call output item becomes its own TOOL_CALL message.
      err_t err = responses_turn_append(ctx, TOOL_CALL, &m);
      if (err != ERROR_NONE) return err;
      jnode_t* jfc_id = jobject_get(jitem, "id");
      jnode_t* jname = jobject_get(jitem, "name");
      if (jis_string(jfc_id)) {
        m->tool_call.call_id = strdup(jstring_content(jfc_id));
        m->tool_call.call_id_len =
            m->tool_call.call_id ? strlen(m->tool_call.call_id) : 0;
      }
      if (jis_string(jname)) {
        m->tool_call.name = strdup(jstring_content(jname));
        m->tool_call.name_len =
            m->tool_call.name ? strlen(m->tool_call.name) : 0;
      }
      // arguments arrive via response.function_call_arguments.* events.
      return ERROR_NONE;
    }
    // Other item types (web_search_call, custom_tool_call, ...): ignore.
    return ERROR_NONE;
  }

  // Output text deltas of a message item.
  if (strcmp(type, "response.output_text.delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    if (!jis_string(jdelta) || !jstring_len(jdelta)) return ERROR_NONE;
    message_t* m = NULL;
    err_t err = responses_turn_ensure(ctx, ASSISTANT, &m);
    if (err != ERROR_NONE) return err;
    err = pctx_append_text(&m->assistant.text, &m->assistant.text_len,
                           jstring_content(jdelta));
    if (err != ERROR_NONE) return err;
    free(ctx->latest_content);
    ctx->latest_content = strdup(jstring_content(jdelta));
    return ERROR_NONE;
  }
  if (strcmp(type, "response.output_text.done") == 0) {
    // Full text; only surface it if no deltas were streamed for this message
    // (otherwise the agent would re-print the whole message).
    jnode_t* jtext = jobject_get(json, "text");
    if (!jis_string(jtext)) return ERROR_NONE;
    message_t* m = NULL;
    err_t err = responses_turn_ensure(ctx, ASSISTANT, &m);
    if (err != ERROR_NONE) return err;
    bool was_empty = !m->assistant.text || !m->assistant.text_len;
    responses_replace_text(&m->assistant.text, &m->assistant.text_len,
                           jstring_content(jtext));
    if (was_empty) {
      free(ctx->latest_content);
      ctx->latest_content = strdup(jstring_content(jtext));
    }
    return ERROR_NONE;
  }

  // Reasoning text deltas. DeepSeek uses response.reasoning_text.delta;
  // OpenAI may use response.reasoning_summary_text.delta.
  if (strcmp(type, "response.reasoning_text.delta") == 0 ||
      strcmp(type, "response.reasoning_summary_text.delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    if (!jis_string(jdelta) || !jstring_len(jdelta)) return ERROR_NONE;
    message_t* m = NULL;
    err_t err = responses_turn_ensure(ctx, REASONING, &m);
    if (err != ERROR_NONE) return err;
    err = pctx_append_text(&m->reasoning.text, &m->reasoning.text_len,
                           jstring_content(jdelta));
    if (err != ERROR_NONE) return err;
    free(ctx->latest_reasoning);
    ctx->latest_reasoning = strdup(jstring_content(jdelta));
    return ERROR_NONE;
  }
  if (strcmp(type, "response.reasoning_text.done") == 0 ||
      strcmp(type, "response.reasoning_summary_text.done") == 0) {
    jnode_t* jtext = jobject_get(json, "text");
    if (!jis_string(jtext)) return ERROR_NONE;
    message_t* m = NULL;
    err_t err = responses_turn_ensure(ctx, REASONING, &m);
    if (err != ERROR_NONE) return err;
    bool was_empty = !m->reasoning.text || !m->reasoning.text_len;
    responses_replace_text(&m->reasoning.text, &m->reasoning.text_len,
                           jstring_content(jtext));
    if (was_empty) {
      free(ctx->latest_reasoning);
      ctx->latest_reasoning = strdup(jstring_content(jtext));
    }
    return ERROR_NONE;
  }

  // Function call arguments. The delta/.done events reference the function
  // call ITEM id through "item_id".
  if (strcmp(type, "response.function_call_arguments.delta") == 0) {
    jnode_t* jitem_id = jobject_get(json, "item_id");
    jnode_t* jdelta = jobject_get(json, "delta");
    if (!jis_string(jdelta) || !jstring_len(jdelta)) return ERROR_NONE;
    const char* item_id =
        jis_string(jitem_id) ? jstring_content(jitem_id) : NULL;
    message_t* m = responses_find_tool_call(ctx, item_id);
    if (m) {
      err_t err = pctx_append_text(&m->tool_call.args, &m->tool_call.args_len,
                                   jstring_content(jdelta));
      if (err != ERROR_NONE) return err;
    }
    return ERROR_NONE;
  }
  if (strcmp(type, "response.function_call_arguments.done") == 0) {
    jnode_t* jitem_id = jobject_get(json, "item_id");
    jnode_t* jargs = jobject_get(json, "arguments");
    if (!jis_string(jargs)) return ERROR_NONE;
    const char* item_id =
        jis_string(jitem_id) ? jstring_content(jitem_id) : NULL;
    message_t* m = responses_find_tool_call(ctx, item_id);
    if (m) {
      responses_replace_text(&m->tool_call.args, &m->tool_call.args_len,
                             jstring_content(jargs));
    }
    return ERROR_NONE;
  }

  if (strcmp(type, "error") == 0) {
    jnode_t* jmessage = jobject_get(json, "message");
    if (jis_string(jmessage)) {
      log(ERROR, "Responses stream error: %s", jstring_content(jmessage));
    }
    return ERROR_NONE;
  }

  return ERROR_NONE;
}

/* ==============================
 *       Provider interface
 * ============================== */

err_t rp_create_context(void** context) {
  if (!context) return ERROR_NULLPTR;
  return pctx_new((pctx_t**)context);
}

void rp_delete_context(void* context) { pctx_delete((pctx_t*)context); }

err_t rp_serialize(const void* context, char** data, size_t* len) {
  return pctx_serialize((const pctx_t*)context, data, len);
}

err_t rp_deserialize(void* context, const char* data, size_t len) {
  if (!context) return ERROR_NULLPTR;
  return pctx_deserialize((pctx_t*)context, data, len);
}

err_t rp_set_model(void* context, const model_t* model) {
  return pctx_set_model((pctx_t*)context, model);
}

model_t* rp_get_model(void* context) {
  return pctx_get_model((pctx_t*)context);
}

err_t rp_set_toolset(void* context, const toolset_t* toolset) {
  return pctx_set_toolset((pctx_t*)context, toolset);
}

toolset_t* rp_get_toolset(void* context) {
  return pctx_get_toolset((pctx_t*)context);
}

err_t rp_set_system_prompt(void* context, const char* system_prompt) {
  return pctx_set_system_prompt((pctx_t*)context, system_prompt);
}

const char* rp_get_system_prompt(const void* context) {
  return pctx_get_system_prompt((const pctx_t*)context);
}

size_t rp_message_count(const void* context) {
  return pctx_message_count((const pctx_t*)context);
}

size_t rp_turn_count(const void* context) {
  return pctx_turn_count((const pctx_t*)context);
}

err_t rp_get_turn_description(const void* context, size_t index,
                              char** description, size_t* len) {
  return pctx_get_turn_description((const pctx_t*)context, index, description,
                                   len);
}

err_t rp_add_user_message(void* context, const char* message) {
  return pctx_add_user_message((pctx_t*)context, message);
}

err_t rp_add_assistant_message(void* context, const char* message) {
  return pctx_add_assistant_message((pctx_t*)context, message);
}

err_t rp_add_tool_message(void* context, const char* id, const char* tool_name,
                          const char* result) {
  return pctx_add_tool_message((pctx_t*)context, id, tool_name, result);
}

void rp_clear_messages(void* context) { pctx_clear_messages((pctx_t*)context); }

void rp_pop_message(void* context) { pctx_pop_message((pctx_t*)context); }

err_t rp_call(void* context, char** response, size_t* len) {
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
  err_t err = responses_http_call(ctx, false, &resp);
  if (err != ERROR_NONE) return err;
  *response = (char*)resp.data;  // owned by the caller now
  *len = resp.data_len;
  return ERROR_NONE;
}

struct rp_sse {
  void* context;
  strmcb_t callback;
  void* userp;
};

static err_t rp_sse_forward(const event_t* event, void* userp) {
  struct rp_sse* w = (struct rp_sse*)userp;
  return w->callback(w->context, event, w->userp);
}

err_t rp_call_stream(void* context, strmcb_t callback, void* userp) {
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
  snprintf(url, sizeof(url), "%s/responses", ctx->model->base_url);
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->model->api_key);
  const char* headers[] = {auth, "Content-Type: application/json"};

  jnode_t* jbody = responses_body(ctx, true);
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
  struct rp_sse wrapper = {
      .context = context, .callback = callback, .userp = userp};
  err_t err = http_sse(&request, rp_sse_forward, &wrapper);
  free(body);
  if (err != ERROR_NONE) {
    log(ERROR, "SSE request failed: %s", error_str(err));
  }
  return err;
}

err_t rp_update(void* context, const char* response, size_t len) {
  pctx_t* ctx = context;
  if (!ctx || !response) return ERROR_NULLPTR;
  // "[DONE]" terminates the stream on some compatible proxies. Clear the
  // latest state so the agent does not re-execute already handled tool calls.
  if (responses_done_marker(response, len)) {
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
  jnode_t* jtype = jobject_get(json, "type");
  err_t err = ERROR_NONE;
  if (jis_string(jtype)) {
    // A streaming event (its JSON payload carries the event name in "type").
    err = responses_update_stream(ctx, json);
  } else {
    // A full (non-streaming) response object.
    err = responses_update_full(ctx, json);
  }
  jdelete(json);
  return err;
}

void rp_rewind(void* context, size_t turn_index) {
  pctx_rewind((pctx_t*)context, turn_index);
}

bool rp_is_finished(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->finished : false;
}

const char* rp_latest_stop_reason(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_stop_reason : NULL;
}

const char* rp_latest_reasoning(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_reasoning : NULL;
}

const char* rp_latest_content(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_content : NULL;
}

err_t rp_latest_tool_calls(const void* context, toolcall_t** tool_calls,
                           size_t* n_tool_call) {
  return pctx_latest_tool_calls((pctx_t*)context, tool_calls, n_tool_call);
}

const char* rp_error_str(err_t err) {
  if (is_custom(err)) return "Custom error";
  return error_str(err);
}

/* ==============================
 *          Provider
 * ============================== */

static protyp_t rp_type() { return OPENAI_RESPONSES; }
static const char* rp_name() { return "OpenAI Responses"; }

static const provider_t responses_provider = {
    .type = rp_type,
    .name = rp_name,
    .error_str = rp_error_str,
    .create_context = rp_create_context,
    .delete_context = rp_delete_context,
    .serialize = rp_serialize,
    .deserialize = rp_deserialize,
    .set_model = rp_set_model,
    .get_model = rp_get_model,
    .set_toolset = rp_set_toolset,
    .get_toolset = rp_get_toolset,
    .set_system_prompt = rp_set_system_prompt,
    .get_system_prompt = rp_get_system_prompt,
    .message_count = rp_message_count,
    .turn_count = rp_turn_count,
    .get_turn_description = rp_get_turn_description,
    .add_user_message = rp_add_user_message,
    .add_assistant_message = rp_add_assistant_message,
    .add_tool_message = rp_add_tool_message,
    .clear_messages = rp_clear_messages,
    .pop_message = rp_pop_message,
    .call = rp_call,
    .call_stream = rp_call_stream,
    .update = rp_update,
    .rewind = rp_rewind,
    .is_finished = rp_is_finished,
    .latest_stop_reason = rp_latest_stop_reason,
    .latest_reasoning = rp_latest_reasoning,
    .latest_content = rp_latest_content,
    .latest_tool_calls = rp_latest_tool_calls,
};

const provider_t* get_openai_responses_provider() {
  return &responses_provider;
}

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

// Convert the general message list into the Anthropic Messages API "messages"
// array. An assistant turn (REASONING/ASSISTANT/TOOL_CALL sharing one id) is
// grouped into a single assistant message with content blocks: thinking (only
// when model->thinking is set), text, and tool_use blocks. A user turn
// (USER/TOOL_RESULT) is grouped into a single user message so roles always
// alternate.
jnode_t* anthropic_messages_to_json(const pctx_t* ctx, jnode_t* jarr) {
  jnode_t* jcur = NULL;       // message being built
  bool cur_user = false;      // whether jcur is a user message
  const char* cur_id = NULL;  // turn id of the current assistant message
  for (size_t i = 0; i < ctx->messages->n_message; ++i) {
    const message_t* m = &ctx->messages->messages[i];
    switch (m->type) {
      case SYSTEM:
        // The system prompt is sent in the top-level "system" field.
        break;
      case USER: {
        if (!jcur || !cur_user) {
          jcur = jobject_new();
          jobject_put(jcur, "role", jstring_new(0, "user"));
          jobject_put(jcur, "content", jarray_new());
          jarray_add(jarr, jcur);
          cur_user = true;
        }
        jnode_t* jcontent = jobject_get(jcur, "content");
        jnode_t* jblock = jobject_new();
        jobject_put(jblock, "type", jstring_new(0, "text"));
        jobject_put(jblock, "text",
                    jstring_new(0, m->user.text ? m->user.text : ""));
        jarray_add(jcontent, jblock);
        break;
      }
      case TOOL_RESULT: {
        if (!jcur || !cur_user) {
          jcur = jobject_new();
          jobject_put(jcur, "role", jstring_new(0, "user"));
          jobject_put(jcur, "content", jarray_new());
          jarray_add(jarr, jcur);
          cur_user = true;
        }
        jnode_t* jcontent = jobject_get(jcur, "content");
        jnode_t* jblock = jobject_new();
        jobject_put(jblock, "type", jstring_new(0, "tool_result"));
        jobject_put(
            jblock, "tool_use_id",
            jstring_new(0,
                        m->tool_result.call_id ? m->tool_result.call_id : ""));
        jobject_put(
            jblock, "content",
            jstring_new(0, m->tool_result.result ? m->tool_result.result : ""));
        jarray_add(jcontent, jblock);
        break;
      }
      case REASONING:
      case ASSISTANT:
      case TOOL_CALL: {
        const char* mid = message_id(m);
        bool join =
            jcur && !cur_user && (!mid || !cur_id || strcmp(mid, cur_id) == 0);
        if (!join) {
          jcur = jobject_new();
          jobject_put(jcur, "role", jstring_new(0, "assistant"));
          jobject_put(jcur, "content", jarray_new());
          jarray_add(jarr, jcur);
          cur_user = false;
          cur_id = mid;
        }
        jnode_t* jcontent = jobject_get(jcur, "content");
        if (m->type == REASONING) {
          if (ctx->model->thinking) {
            jnode_t* jblock = jobject_new();
            jobject_put(jblock, "type", jstring_new(0, "thinking"));
            jobject_put(
                jblock, "thinking",
                jstring_new(0, m->reasoning.text ? m->reasoning.text : ""));
            jarray_add(jcontent, jblock);
          }
        } else if (m->type == ASSISTANT) {
          if (m->assistant.text && m->assistant.text_len) {
            jnode_t* jblock = jobject_new();
            jobject_put(jblock, "type", jstring_new(0, "text"));
            jobject_put(jblock, "text", jstring_new(0, m->assistant.text));
            jarray_add(jcontent, jblock);
          }
        } else {  // TOOL_CALL
          jnode_t* jblock = jobject_new();
          jobject_put(jblock, "type", jstring_new(0, "tool_use"));
          jobject_put(
              jblock, "id",
              jstring_new(0, m->tool_call.call_id ? m->tool_call.call_id : ""));
          jobject_put(
              jblock, "name",
              jstring_new(0, m->tool_call.name ? m->tool_call.name : ""));
          // The general form stores args as a JSON string; the API wants an
          // object.
          jnode_t* jinput = NULL;
          if (m->tool_call.args && m->tool_call.args_len) {
            jinput =
                jfrom_string(m->tool_call.args, (int)m->tool_call.args_len);
          }
          jobject_put(jblock, "input", jinput ? jinput : jobject_new());
          jarray_add(jcontent, jblock);
        }
        break;
      }
    }
  }
  return jarr;
}

static jnode_t* anthropic_body(const pctx_t* ctx, bool stream) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, ctx->model->name));
  jobject_put(jbody, "system",
              jstring_new(0, ctx->system_prompt ? ctx->system_prompt : ""));
  // Convert the general conversation into the API-specific messages array.
  jobject_put(jbody, "messages", anthropic_messages_to_json(ctx, jarray_new()));

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

// Streaming helpers ---------------------------------------------------------

// Find the trailing message of `type` within the current stream turn.
static message_t* anthropic_turn_find(pctx_t* ctx, msgtyp_t type) {
  size_t n = ctx->messages->n_message;
  const char* tid = ctx->stream_turn_id;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      return NULL;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) return NULL;
    if (m->type == type) return m;
  }
  return NULL;
}

// Find the message for content-block `index` within the current stream turn.
// Content blocks map 1:1 to the trailing-turn messages (in block order), so
// block index == position within the turn. Only tool_use blocks get
// input_json_delta fragments.
static message_t* anthropic_turn_message_at(pctx_t* ctx, int index) {
  if (index < 0) return NULL;
  size_t n = ctx->messages->n_message;
  const char* tid = ctx->stream_turn_id;
  size_t start = n;
  for (size_t i = n; i > 0; --i) {
    message_t* m = &ctx->messages->messages[i - 1];
    if (m->type != REASONING && m->type != ASSISTANT && m->type != TOOL_CALL)
      break;
    const char* mid = message_id(m);
    if (mid && tid && strcmp(mid, tid) != 0) break;
    start = i - 1;
  }
  if ((size_t)index >= n - start) return NULL;
  message_t* m = &ctx->messages->messages[start + index];
  return m->type == TOOL_CALL ? m : NULL;
}

// Non-streaming response: convert the content blocks into general
// REASONING/ASSISTANT/TOOL_CALL messages (all sharing the response message id)
// and expose the latest state.
static err_t anthropic_update_full(pctx_t* ctx, jnode_t* json) {
  jnode_t* jstop = jobject_get(json, "stop_reason");
  const char* stop = jis_string(jstop) ? jstring_content(jstop) : NULL;
  if (stop) {
    ctx->latest_stop_reason = strdup(stop);
    // Any terminal reason other than a tool-use request ends the turn.
    ctx->finished = (strcmp(stop, "tool_use") != 0);
    if (strcmp(stop, "tool_use") == 0) ctx->tool_calls_ready = true;
  }

  jnode_t* jid = jobject_get(json, "id");
  const char* id = jis_string(jid) ? jstring_content(jid) : NULL;
  char* turn_id = id ? strdup(id) : pctx_new_turn_id();
  if (!turn_id) return ERROR_OUT_OF_MEMORY;

  jnode_t* jcontent = jobject_get(json, "content");
  if (jis_string(jcontent)) {
    if (jstring_len(jcontent)) {
      message_t* m = message_new(ASSISTANT);
      if (!m) {
        free(turn_id);
        return ERROR_OUT_OF_MEMORY;
      }
      if (message_set_id(m, turn_id) != ERROR_NONE) {
        message_delete(m);
        free(turn_id);
        return ERROR_OUT_OF_MEMORY;
      }
      m->assistant.text = strdup(jstring_content(jcontent));
      if (!m->assistant.text) {
        message_delete(m);
        free(turn_id);
        return ERROR_OUT_OF_MEMORY;
      }
      m->assistant.text_len = strlen(m->assistant.text);
      msglist_add_message(ctx->messages, m);
      ctx->latest_content = strdup(jstring_content(jcontent));
    }
    free(turn_id);
    return ERROR_NONE;
  }
  if (!jis_array(jcontent)) {
    free(turn_id);
    return ERROR_NONE;
  }

  // Collect text/thinking and the tool_use blocks.
  jnode_t* jtext = jstring_new(0, "");
  jnode_t* jthinking = jstring_new(0, "");
  size_t n = jarray_size(jcontent);
  size_t n_tool = 0;
  for (size_t i = 0; i < n; ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    const char* type = jis_string(jtype) ? jstring_content(jtype) : NULL;
    if (type && strcmp(type, "tool_use") == 0) ++n_tool;
  }
  for (size_t i = 0; i < n; ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    const char* type = jis_string(jtype) ? jstring_content(jtype) : NULL;
    if (type && strcmp(type, "text") == 0) {
      jnode_t* jt = jobject_get(jblock, "text");
      if (jis_string(jt)) jstring_concat(jtext, jstring_content(jt));
    } else if (type && strcmp(type, "thinking") == 0) {
      jnode_t* jt = jobject_get(jblock, "thinking");
      if (jis_string(jt)) jstring_concat(jthinking, jstring_content(jt));
    }
  }

  // Append REASONING, then ASSISTANT, then TOOL_CALL messages in turn order.
  if (jstring_len(jthinking)) {
    message_t* m = message_new(REASONING);
    if (m) {
      m->reasoning.text = strdup(jstring_content(jthinking));
      if (m->reasoning.text && message_set_id(m, turn_id) == ERROR_NONE) {
        m->reasoning.text_len = strlen(m->reasoning.text);
        msglist_add_message(ctx->messages, m);
        ctx->latest_reasoning = strdup(jstring_content(jthinking));
      } else {
        message_delete(m);
      }
    }
  }
  if (jstring_len(jtext)) {
    message_t* m = message_new(ASSISTANT);
    if (m) {
      m->assistant.text = strdup(jstring_content(jtext));
      if (m->assistant.text && message_set_id(m, turn_id) == ERROR_NONE) {
        m->assistant.text_len = strlen(m->assistant.text);
        msglist_add_message(ctx->messages, m);
        ctx->latest_content = strdup(jstring_content(jtext));
      } else {
        message_delete(m);
      }
    }
  }
  for (size_t i = 0; i < n && n_tool > 0; ++i) {
    jnode_t* jblock = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jblock, "type");
    const char* type = jis_string(jtype) ? jstring_content(jtype) : NULL;
    if (!type || strcmp(type, "tool_use") != 0) continue;
    jnode_t* jcall_id = jobject_get(jblock, "id");
    jnode_t* jname = jobject_get(jblock, "name");
    jnode_t* jinput = jobject_get(jblock, "input");
    char* args = NULL;
    if (jis_string(jinput)) {
      args = strdup(jstring_content(jinput));
    } else if (jinput) {
      args = jto_string(jinput);
    }
    message_t* m = message_new(TOOL_CALL);
    if (!m) {
      free(args);
      break;
    }
    if (message_set_id(m, turn_id) != ERROR_NONE) {
      message_delete(m);
      free(args);
      break;
    }
    if (jis_string(jcall_id)) {
      m->tool_call.call_id = strdup(jstring_content(jcall_id));
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
  jdelete(jtext);
  jdelete(jthinking);

  if (ctx->tool_calls_ready) {
    err_t err = pctx_latest_calls_from_turn(ctx);
    if (err != ERROR_NONE) {
      free(turn_id);
      return err;
    }
  }
  free(turn_id);
  return ERROR_NONE;
}

// Streaming event: merge the delta into the general assistant turn being
// built. Content blocks map 1:1 to general messages (REASONING for thinking,
// ASSISTANT for text, TOOL_CALL for tool_use).
static err_t anthropic_update_stream(pctx_t* ctx, jnode_t* json) {
  jnode_t* jtype = jobject_get(json, "type");
  if (!jis_string(jtype)) return ERROR_NONE;
  const char* type = jstring_content(jtype);

  if (strcmp(type, "message_start") == 0) {
    // Record the API message id so later content blocks are grouped into one
    // assistant turn. No placeholder message is created.
    jnode_t* jmessage = jobject_get(json, "message");
    jnode_t* jid = jmessage ? jobject_get(jmessage, "id") : NULL;
    free(ctx->stream_turn_id);
    ctx->stream_turn_id =
        jis_string(jid) ? strdup(jstring_content(jid)) : pctx_new_turn_id();
    return ERROR_NONE;
  }
  if (strcmp(type, "message_stop") == 0) {
    // End of the assistant message; the turn is complete.
    free(ctx->stream_turn_id);
    ctx->stream_turn_id = NULL;
    return ERROR_NONE;
  }
  if (strcmp(type, "ping") == 0) {
    // Heartbeat: may arrive at any point mid-turn. It must NOT clear the
    // stream turn id, or later content blocks would get a fresh id and the
    // turn (and its tool_use input accumulation) would break.
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
        err_t err = pctx_latest_calls_from_turn(ctx);
        if (err != ERROR_NONE) return err;
      }
    }
    return ERROR_NONE;
  }

  if (strcmp(type, "content_block_start") == 0) {
    jnode_t* jcb = jobject_get(json, "content_block");
    if (!jis_object(jcb)) return ERROR_NONE;
    // Some proxies omit message_start; fall back to a generated turn id.
    if (!ctx->stream_turn_id) {
      ctx->stream_turn_id = pctx_new_turn_id();
      if (!ctx->stream_turn_id) return ERROR_OUT_OF_MEMORY;
    }
    jnode_t* jcb_type = jobject_get(jcb, "type");
    const char* cbtype =
        jis_string(jcb_type) ? jstring_content(jcb_type) : NULL;
    if (!cbtype) return ERROR_NONE;
    message_t* m = NULL;
    if (strcmp(cbtype, "thinking") == 0) {
      m = message_new(REASONING);
    } else if (strcmp(cbtype, "text") == 0) {
      m = message_new(ASSISTANT);
    } else if (strcmp(cbtype, "tool_use") == 0) {
      m = message_new(TOOL_CALL);
      if (m) {
        jnode_t* jid = jobject_get(jcb, "id");
        jnode_t* jname = jobject_get(jcb, "name");
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
        // args accumulate via input_json_delta fragments.
      }
    } else {
      // Unknown block type: nothing to store.
      return ERROR_NONE;
    }
    if (!m) return ERROR_OUT_OF_MEMORY;
    if (message_set_id(m, ctx->stream_turn_id) != ERROR_NONE) {
      message_delete(m);
      return ERROR_OUT_OF_MEMORY;
    }
    msglist_add_message(ctx->messages, m);
    return ERROR_NONE;
  }

  if (strcmp(type, "content_block_delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    jnode_t* jdelta_type = jdelta ? jobject_get(jdelta, "type") : NULL;
    if (!jis_string(jdelta_type)) return ERROR_NONE;
    const char* delta_type = jstring_content(jdelta_type);
    if (strcmp(delta_type, "text_delta") == 0) {
      jnode_t* jtext = jobject_get(jdelta, "text");
      if (!jis_string(jtext)) return ERROR_NONE;
      message_t* asst = anthropic_turn_find(ctx, ASSISTANT);
      if (asst) {
        pctx_append_text(&asst->assistant.text, &asst->assistant.text_len,
                         jstring_content(jtext));
      }
      free(ctx->latest_content);
      ctx->latest_content = strdup(jstring_content(jtext));
    } else if (strcmp(delta_type, "thinking_delta") == 0) {
      jnode_t* jthinking = jobject_get(jdelta, "thinking");
      if (!jis_string(jthinking)) return ERROR_NONE;
      message_t* reas = anthropic_turn_find(ctx, REASONING);
      if (reas) {
        pctx_append_text(&reas->reasoning.text, &reas->reasoning.text_len,
                         jstring_content(jthinking));
      }
      free(ctx->latest_reasoning);
      ctx->latest_reasoning = strdup(jstring_content(jthinking));
    } else if (strcmp(delta_type, "input_json_delta") == 0) {
      jnode_t* jpartial = jobject_get(jdelta, "partial_json");
      if (!jis_string(jpartial)) return ERROR_NONE;
      jnode_t* jindex_node = jobject_get(json, "index");
      int index =
          jis_number(jindex_node) ? (int)jas_number(jindex_node)->value : -1;
      message_t* tc = anthropic_turn_message_at(ctx, index);
      if (tc) {
        pctx_append_text(&tc->tool_call.args, &tc->tool_call.args_len,
                         jstring_content(jpartial));
      }
    }
    return ERROR_NONE;
  }

  if (strcmp(type, "content_block_stop") == 0) {
    // The general form keeps tool_use input as a JSON string; nothing to do.
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

err_t anthropic_serialize(const void* context, char** data, size_t* len) {
  return pctx_serialize((const pctx_t*)context, data, len);
}

err_t anthropic_deserialize(void* context, const char* data, size_t len) {
  if (!context) return ERROR_NULLPTR;
  return pctx_deserialize((pctx_t*)context, data, len);
}

err_t anthropic_set_model(void* context, const model_t* model) {
  return pctx_set_model((pctx_t*)context, model);
}

model_t* anthropic_get_model(void* context) {
  return pctx_get_model((pctx_t*)context);
}

err_t anthropic_set_toolset(void* context, const toolset_t* toolset) {
  return pctx_set_toolset((pctx_t*)context, toolset);
}

toolset_t* anthropic_get_toolset(void* context) {
  return pctx_get_toolset((pctx_t*)context);
}

err_t anthropic_set_system_prompt(void* context, const char* system_prompt) {
  return pctx_set_system_prompt((pctx_t*)context, system_prompt);
}

const char* anthropic_get_system_prompt(const void* context) {
  return pctx_get_system_prompt((const pctx_t*)context);
}

size_t anthropic_message_count(const void* context) {
  return pctx_message_count((const pctx_t*)context);
}

// Messages are stored in the general form; role alternation and user/tool
// merging happen in the request builder (anthropic_messages_to_json).
err_t anthropic_add_user_message(void* context, const char* message) {
  return pctx_add_user_message((pctx_t*)context, message);
}

err_t anthropic_add_assistant_message(void* context, const char* message) {
  return pctx_add_assistant_message((pctx_t*)context, message);
}

err_t anthropic_add_tool_message(void* context, const char* id,
                                 const char* tool_name, const char* result) {
  return pctx_add_tool_message((pctx_t*)context, id, tool_name, result);
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
  err_t err = anthropic_http_call(ctx, false, &resp);
  if (err != ERROR_NONE) return err;
  *response = (char*)resp.data;  // owned by the caller now
  *len = resp.data_len;
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
    msglist_delete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  free(ctx->stream_turn_id);
  ctx->stream_turn_id = NULL;
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

bool anthropic_is_finished(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->finished : false;
}

const char* anthropic_latest_stop_reason(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_stop_reason : NULL;
}

const char* anthropic_latest_reasoning(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_reasoning : NULL;
}

const char* anthropic_latest_content(const void* context) {
  const pctx_t* ctx = context;
  return ctx ? ctx->latest_content : NULL;
}

err_t anthropic_latest_tool_calls(const void* context, toolcall_t** calls,
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

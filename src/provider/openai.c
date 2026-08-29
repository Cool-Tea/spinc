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

  // The system prompt is prepended here so the stored conversation only holds
  // user/assistant/tool messages.
  jnode_t* jmessages = jarray_new();
  jobject_put(jbody, "messages", jmessages);
  jarray_add(jmessages, openai_system_message(ctx->system_prompt));
  for (int i = 0; i < jarray_size(ctx->messages); ++i) {
    jarray_add(jmessages, jcopy(jarray_get(ctx->messages, i)));
  }

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

static err_t openai_build_tool_calls(pctx_t* ctx, jnode_t* jtool_calls) {
  if (!jis_array(jtool_calls) || jarray_size(jtool_calls) == 0) {
    return ERROR_NONE;
  }
  size_t n = jarray_size(jtool_calls);
  const char** ids = calloc(n, sizeof(char*));
  const char** names = calloc(n, sizeof(char*));
  const char** args = calloc(n, sizeof(char*));
  if (!ids || !names || !args) {
    free(ids);
    free(names);
    free(args);
    return ERROR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < n; ++i) {
    jnode_t* jcall = jarray_get(jtool_calls, i);
    jnode_t* jid = jobject_get(jcall, "id");
    jnode_t* jfunc = jobject_get(jcall, "function");
    jnode_t* jname = jfunc ? jobject_get(jfunc, "name") : NULL;
    jnode_t* jargs = jfunc ? jobject_get(jfunc, "arguments") : NULL;
    ids[i] = jis_string(jid) ? jstring_content(jid) : NULL;
    names[i] = jis_string(jname) ? jstring_content(jname) : NULL;
    args[i] = jis_string(jargs) ? jstring_content(jargs) : NULL;
  }
  err_t err = pctx_set_tool_calls(ctx, n, ids, names, args);
  free(ids);
  free(names);
  free(args);
  return err;
}

// Non-streaming response: append the assistant message and expose the latest
// state.
err_t openai_update_full(pctx_t* ctx, jnode_t* jfirst) {
  jnode_t* jmessage = jobject_get(jfirst, "message");
  if (!jmessage) {
    log(ERROR, "Response missing message");
    return ERROR_UNKNOWN;
  }
  jarray_add(ctx->messages, jcopy(jmessage));

  jnode_t* jfinish = jobject_get(jfirst, "finish_reason");
  const char* finish = jis_string(jfinish) ? jstring_content(jfinish) : NULL;
  if (finish) {
    ctx->latest_stop_reason = strdup(finish);
    // Any terminal reason other than a tool-call request ends the turn.
    ctx->finished = (strcmp(finish, "tool_calls") != 0);
    if (strcmp(finish, "tool_calls") == 0) {
      ctx->tool_calls_ready = true;
      err_t err =
          openai_build_tool_calls(ctx, jobject_get(jmessage, "tool_calls"));
      if (err != ERROR_NONE) return err;
    }
  }

  jnode_t* jcontent = jobject_get(jmessage, "content");
  if (jis_string(jcontent) && jstring_len(jcontent)) {
    ctx->latest_content = strdup(jstring_content(jcontent));
  }

  return ERROR_NONE;
}

// Streaming delta: merge into the last assistant message. Tool calls are only
// exposed once the finish_reason arrives, so partial fragments are never
// executed.
err_t openai_update_stream(pctx_t* ctx, jnode_t* jfirst, jnode_t* jdelta) {
  jnode_t* jdelta_role = jobject_get(jdelta, "role");
  if (jis_string(jdelta_role)) {
    const char* role = jstring_content(jdelta_role);
    jnode_t* jlast =
        jarray_size(ctx->messages) > 0
            ? jarray_get(ctx->messages, jarray_size(ctx->messages) - 1)
            : NULL;
    const char* last_role = NULL;
    if (jlast) {
      jnode_t* jlr = jobject_get(jlast, "role");
      if (jis_string(jlr)) last_role = jstring_content(jlr);
    }
    if (!jlast || !last_role || strcmp(last_role, role) != 0) {
      jnode_t* jmessage = jobject_new();
      jobject_put(jmessage, "role", jstring_new(0, role));
      jarray_add(ctx->messages, jmessage);
    }
  }
  if (jarray_size(ctx->messages) == 0) return ERROR_NONE;
  jnode_t* jlast = jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);

  jnode_t* jcontent = jobject_get(jdelta, "content");
  if (jis_string(jcontent)) {
    free(ctx->latest_content);
    ctx->latest_content = strdup(jstring_content(jcontent));
    jnode_t* jmsg_content = jobject_get(jlast, "content");
    if (!jis_string(jmsg_content)) {
      jobject_put(jlast, "content", jcopy(jcontent));
    } else {
      jstring_concat(jmsg_content, jstring_content(jcontent));
    }
  }

  jnode_t* jtool_calls = jobject_get(jdelta, "tool_calls");
  if (jis_array(jtool_calls)) {
    jnode_t* jlast_calls = jobject_get(jlast, "tool_calls");
    if (!jis_array(jlast_calls)) {
      jlast_calls = jarray_new();
      jobject_put(jlast, "tool_calls", jlast_calls);
    }
    size_t n = jarray_size(jtool_calls);
    for (size_t i = 0; i < n; ++i) {
      jnode_t* jdc = jarray_get(jtool_calls, i);
      jnode_t* jindex_node = jobject_get(jdc, "index");
      int index = jis_number(jindex_node) ? (int)jas_number(jindex_node)->value
                                          : (int)jarray_size(jlast_calls);
      if (index >= jarray_size(jlast_calls)) {
        jarray_add(jlast_calls, jcopy(jdc));
        continue;
      }
      jnode_t* jlc = jarray_get(jlast_calls, index);

      jnode_t* jdid = jobject_get(jdc, "id");
      if (jis_string(jdid) && !jis_string(jobject_get(jlc, "id"))) {
        jobject_put(jlc, "id", jcopy(jdid));
      }

      jnode_t* jdf = jobject_get(jdc, "function");
      if (jis_empty(jdf)) continue;
      jnode_t* jlf = jobject_get(jlc, "function");
      if (jis_empty(jlf)) {
        jobject_put(jlc, "function", jcopy(jdf));
        continue;
      }
      jnode_t* jdn = jobject_get(jdf, "name");
      if (jis_string(jdn)) {
        jnode_t* jln = jobject_get(jlf, "name");
        if (jis_empty(jln)) jobject_put(jlf, "name", jcopy(jdn));
        else jstring_concat(jln, jstring_content(jdn));
      }
      jnode_t* jda = jobject_get(jdf, "arguments");
      if (jis_string(jda)) {
        jnode_t* jla = jobject_get(jlf, "arguments");
        if (jis_empty(jla)) jobject_put(jlf, "arguments", jcopy(jda));
        else jstring_concat(jla, jstring_content(jda));
      }
    }
  }

  jnode_t* jfinish = jobject_get(jfirst, "finish_reason");
  if (jis_string(jfinish)) {
    const char* finish = jstring_content(jfinish);
    free(ctx->latest_stop_reason);
    ctx->latest_stop_reason = strdup(finish);
    // Any terminal reason other than a tool-call request ends the turn.
    if (strcmp(finish, "tool_calls") != 0) ctx->finished = true;
    if (strcmp(finish, "tool_calls") == 0) {
      ctx->tool_calls_ready = true;
      err_t err =
          openai_build_tool_calls(ctx, jobject_get(jlast, "tool_calls"));
      if (err != ERROR_NONE) return err;
    }
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

err_t cc_serialize(void* context, char** data, size_t* len) {
  return pctx_serialize((const pctx_t*)context, data, len);
}

err_t cc_deserialize(const char* data, size_t len, void** context) {
  if (!context) return ERROR_NULLPTR;
  *context = NULL;
  return pctx_deserialize(data, len, (pctx_t**)context);
}

err_t cc_set_model(void* context, const model_t* model) {
  pctx_t* ctx = context;
  if (!ctx || !model) return ERROR_NULLPTR;
  model_t* copy = malloc(sizeof(model_t));
  if (!copy) return ERROR_OUT_OF_MEMORY;
  *copy = *model;
  free(ctx->model);
  ctx->model = copy;
  return ERROR_NONE;
}

model_t* cc_get_model(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->model : NULL;
}

err_t cc_set_toolset(void* context, const toolset_t* toolset) {
  pctx_t* ctx = context;
  if (!ctx || !toolset) return ERROR_NULLPTR;
  ctx->toolset = (toolset_t*)toolset;
  return ERROR_NONE;
}

toolset_t* cc_get_toolset(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->toolset : NULL;
}

err_t cc_set_system_prompt(void* context, const char* system_prompt) {
  pctx_t* ctx = context;
  if (!ctx || !system_prompt) return ERROR_NULLPTR;
  char* copy = strdup(system_prompt);
  if (!copy) return ERROR_OUT_OF_MEMORY;
  free(ctx->system_prompt);
  ctx->system_prompt = copy;
  return ERROR_NONE;
}

const char* cc_get_system_prompt(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->system_prompt : NULL;
}

size_t cc_message_count(void* context) {
  pctx_t* ctx = context;
  return ctx ? (size_t)jarray_size(ctx->messages) : 0;
}

static err_t cc_add_message(pctx_t* ctx, const char* role,
                            const char* content) {
  if (!ctx || !role || !content) return ERROR_NULLPTR;
  jnode_t* jmessage = jobject_new();
  if (!jmessage) return ERROR_OUT_OF_MEMORY;
  jobject_put(jmessage, "role", jstring_new(0, role));
  jobject_put(jmessage, "content", jstring_new(0, content));
  jarray_add(ctx->messages, jmessage);
  return ERROR_NONE;
}

err_t cc_add_user_message(void* context, const char* message) {
  return cc_add_message((pctx_t*)context, "user", message);
}

err_t cc_add_assistant_message(void* context, const char* message) {
  return cc_add_message((pctx_t*)context, "assistant", message);
}

err_t cc_add_tool_message(void* context, const char* id, const char* tool_name,
                          const char* result) {
  (void)tool_name;
  pctx_t* ctx = context;
  if (!ctx || !id || !result) return ERROR_NULLPTR;
  jnode_t* jmessage = jobject_new();
  if (!jmessage) return ERROR_OUT_OF_MEMORY;
  jobject_put(jmessage, "role", jstring_new(0, "tool"));
  jobject_put(jmessage, "tool_call_id", jstring_new(0, id));
  jobject_put(jmessage, "content", jstring_new(0, result));
  jarray_add(ctx->messages, jmessage);
  return ERROR_NONE;
}

void cc_clear_messages(void* context) { pctx_clear_messages((pctx_t*)context); }

void cc_pop_message(void* context) { pctx_pop_message((pctx_t*)context); }

err_t cc_call(void* context, char** response, size_t* len) {
  pctx_t* ctx = context;
  if (!ctx || !response || !len) return ERROR_NULLPTR;
  if (!ctx->model) return ERROR_NULLPTR;
  *response = NULL;
  *len = 0;
  // A new turn starts; drop the previous snapshot and finished state.
  if (ctx->snapshot) {
    jdelete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
  ctx->finished = false;

  response_t resp = {0};
  err_t err = openai_http_call(ctx, false, &resp);
  if (err != ERROR_NONE) return err;
  *response = (char*)resp.data;  // owned by the caller now
  *len = resp.data_size;
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
    jdelete(ctx->snapshot);
    ctx->snapshot = NULL;
  }
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

bool cc_is_finished(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->finished : false;
}

const char* cc_latest_stop_reason(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_stop_reason : NULL;
}

const char* cc_latest_reasoning(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_reasoning : NULL;
}

const char* cc_latest_content(void* context) {
  pctx_t* ctx = context;
  return ctx ? ctx->latest_content : NULL;
}

err_t cc_latest_tool_calls(void* context, toolcall_t** tool_calls,
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

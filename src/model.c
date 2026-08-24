#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sjson.h"

#include "log.h"
#include "http.h"
#include "model.h"

#define URL_LEN 512
#define AUTH_HEADER_LEN 512

static const char* protocol_names[] = {
    [OPENAI] = "OpenAI",
    [ANTHROPIC] = "Anthropic",
};

static char url[URL_LEN];
static char auth_header[AUTH_HEADER_LEN];

static jnode_t* openai_system_message(const char* content) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "system"));
  jobject_put(jmessage, "content", jstring_new(0, content));
  return jmessage;
}

static jnode_t* openai_user_message(const char* content) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "user"));
  jobject_put(jmessage, "content", jstring_new(0, content));
  return jmessage;
}

static jnode_t* openai_tool_message(const char* id, const char* result) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "tool"));
  jobject_put(jmessage, "tool_call_id", jstring_new(0, id));
  jobject_put(jmessage, "content", jstring_new(0, result));
  return jmessage;
}

static jnode_t* anthropic_user_message(const char* content) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "user"));
  jobject_put(jmessage, "content", jstring_new(0, content));
  return jmessage;
}

static jnode_t* anthropic_tool_message(const char* id, const char* result) {
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "user"));
  jnode_t* jcontent = jarray_new();
  jobject_put(jmessage, "content", jcontent);
  jnode_t* jtool_use = jobject_new();
  jarray_add(jcontent, jtool_use);
  jobject_put(jtool_use, "type", jstring_new(0, "tool_result"));
  jobject_put(jtool_use, "tool_use_id", jstring_new(0, id));
  jobject_put(jtool_use, "content", jstring_new(0, result));
  return jmessage;
}

static bool openai_context(context_t* ctx, const char* system_prompt,
                           const tool_t* tools, size_t n_tools) {
  ctx->system_prompt = system_prompt;
  ctx->messages = jarray_new();
  if (!ctx->messages) return false;
  jarray_add(ctx->messages, openai_system_message(system_prompt));
  ctx->n_tools = n_tools;
  ctx->tools = tools;
  return true;
}

static bool anthropic_context(context_t* ctx, const char* system_prompt,
                              const tool_t* tools, size_t n_tools) {
  ctx->system_prompt = system_prompt;
  ctx->messages = jarray_new();
  if (!ctx->messages) return false;
  ctx->n_tools = n_tools;
  ctx->tools = tools;
  return true;
}

bool context_init(context_t* ctx, protocol_t protocol,
                  const char* system_prompt, const tool_t* tools,
                  size_t n_tools) {
  ctx->protocol = protocol;
  switch (protocol) {
    case OPENAI: return openai_context(ctx, system_prompt, tools, n_tools);
    case ANTHROPIC:
      return anthropic_context(ctx, system_prompt, tools, n_tools);
  }
  return false;
}

void context_clear(context_t* ctx) {
  if (!ctx) return;
  if (ctx->messages) jdelete(ctx->messages);
}

static void openai_context_update(context_t* ctx, const mdres_t* res) {
  jnode_t* json = res->json;
  jnode_t* jchoices = jobject_get(json, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jmessage = jobject_get(jfirst, "message");
  jarray_add(ctx->messages, jcopy(jmessage));
}

static void anthropic_context_update(context_t* ctx, const mdres_t* res) {
  jnode_t* json = res->json;
  jnode_t* jcontent = jobject_get(json, "content");
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "assistant"));
  jobject_put(jmessage, "content", jcopy(jcontent));
  jarray_add(ctx->messages, jmessage);
}

void context_update(context_t* ctx, const mdres_t* res) {
  switch (ctx->protocol) {
    case OPENAI: openai_context_update(ctx, res); break;
    case ANTHROPIC: anthropic_context_update(ctx, res); break;
  }
}

void context_add_user_message(context_t* ctx, const char* message) {
  switch (ctx->protocol) {
    case OPENAI: jarray_add(ctx->messages, openai_user_message(message)); break;
    case ANTHROPIC:
      jarray_add(ctx->messages, anthropic_user_message(message));
      break;
  }
}

void context_add_tool_message(context_t* ctx, const char* id,
                              const char* tool_name, const char* result) {
  (void)tool_name;
  switch (ctx->protocol) {
    case OPENAI:
      jarray_add(ctx->messages, openai_tool_message(id, result));
      break;
    case ANTHROPIC:
      jarray_add(ctx->messages, anthropic_tool_message(id, result));
      break;
  }
}

static mdres_t* model_response_new() {
  mdres_t* res = malloc(sizeof(mdres_t));
  if (!res) return NULL;
  memset(res, 0, sizeof(mdres_t));
  return res;
}

static jnode_t* openai_serialize_tool(const tool_t* tool) {
  const tooldef_t* tooldef = &tool->def;
  jnode_t* jtool = jobject_new();
  jobject_put(jtool, "type", jstring_new(0, "function"));
  jnode_t* func = jobject_new();
  jobject_put(jtool, "function", func);
  {
    jobject_put(func, "name", jstring_new(0, tooldef->name));
    jobject_put(func, "description", jstring_new(0, tooldef->description));
    jnode_t* params = jobject_new();
    jobject_put(func, "parameters", params);
    {
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
        if (p->required) {
          jarray_add(required, jstring_new(0, p->name));
        }
      }
    }
  }
  return jtool;
}

static jnode_t* openai_body(const model_t* model, const context_t* ctx) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, model->name));
  jobject_put(jbody, "messages", jcopy(ctx->messages));
  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  for (size_t i = 0; i < ctx->n_tools; ++i) {
    jnode_t* tool = openai_serialize_tool(&ctx->tools[i]);
    jarray_add(jtools, tool);
  }
  if (model->thinking) {
    jnode_t* jthinking = jobject_new();
    jobject_put(jbody, "thinking", jthinking);
    jobject_put(jthinking, "type", jstring_new(0, model->thinking));
  }
  if (model->reasoning_effort) {
    jobject_put(jbody, "reasoning_effort",
                jstring_new(0, model->reasoning_effort));
  }
  jobject_put(jbody, "top_p", jnumber_new(model->top_p));
  if (model->max_tokens >= 0) {
    jobject_put(jbody, "max_tokens", jnumber_new(model->max_tokens));
  }
  return jbody;
}

static mdres_t* openai_parse(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jchoices = jobject_get(jresp, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jfinish_reason = jobject_get(jfirst, "finish_reason");
  res->stop_reason = jstring_content(jfinish_reason);
  res->finished = (strcmp(res->stop_reason, "stop") == 0);
  jnode_t* jmessage = jobject_get(jfirst, "message");
  jnode_t* jreasoning = jobject_get(jmessage, "reasoning_content");
  if (jreasoning && jstring_len(jreasoning))
    res->reasoning = jstring_content(jreasoning);
  jnode_t* jcontent = jobject_get(jmessage, "content");
  if (jcontent && jstring_len(jcontent))
    res->content = jstring_content(jcontent);
  jnode_t* jtool_calls = jobject_get(jmessage, "tool_calls");
  if (jtool_calls) {
    res->n_tool_call = jarray_size(jtool_calls);
    res->tool_calls = malloc(res->n_tool_call * sizeof(struct tool_call));
    if (!res->tool_calls) {
      model_response_delete(res);
      return NULL;
    }
    for (size_t i = 0; i < res->n_tool_call; ++i) {
      jnode_t* jcall = jarray_get(jtool_calls, i);
      jnode_t* jid = jobject_get(jcall, "id");
      jnode_t* jfunc = jobject_get(jcall, "function");
      jnode_t* jname = jobject_get(jfunc, "name");
      jnode_t* jargs = jobject_get(jfunc, "arguments");
      res->tool_calls[i].id = jstring_content(jid);
      res->tool_calls[i].name = jstring_content(jname);
      res->tool_calls[i].args = strdup(jstring_content(jargs));
    }
  }

  return res;
}

static mdres_t* call_openai_api(const model_t* model, const context_t* ctx) {
  snprintf(url, sizeof(url), "%s/chat/completions", model->base_url);
  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
           model->api_key);
  const char* headers[] = {auth_header, "Content-Type: application/json"};

  jnode_t* jbody = openai_body(model, ctx);
  char* body = jto_string(jbody);
  jdelete(jbody);
  log(DEBUG, "Request body: %s", body);

  request_t request = {
      .url = url,
      .n_header = sizeof(headers) / sizeof(headers[0]),
      .headers = headers,
      .body = body,
  };
  response_t* resp = http_post(&request);
  free(body);
  if (!resp) {
    log(ERROR, "HTTP request failed");
    return NULL;
  }
  if (resp->status != 200) {
    log(ERROR, "HTTP request failed with status %ld", resp->status);
    if (resp->data) log(ERROR, "Reason: %s", resp->data);
    response_delete(resp);
    return NULL;
  }

  char* data = (char*)resp->data;
  resp->data = NULL;  // Prevent double free
  response_delete(resp);
  return openai_parse(data);
}

static jnode_t* anthropic_serialize_tool(const tool_t* tool) {
  const tooldef_t* tooldef = &tool->def;
  jnode_t* jtool = jobject_new();
  jobject_put(jtool, "name", jstring_new(0, tooldef->name));
  jobject_put(jtool, "description", jstring_new(0, tooldef->description));
  jnode_t* schema = jobject_new();
  jobject_put(jtool, "input_schema", schema);
  {
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
      if (p->required) {
        jarray_add(required, jstring_new(0, p->name));
      }
    }
  }
  return jtool;
}

static jnode_t* anthropic_body(const model_t* model, const context_t* ctx) {
  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, model->name));
  jobject_put(jbody, "system", jstring_new(0, ctx->system_prompt));
  jobject_put(jbody, "messages", jcopy(ctx->messages));
  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  for (size_t i = 0; i < ctx->n_tools; ++i) {
    jnode_t* tool = anthropic_serialize_tool(&ctx->tools[i]);
    jarray_add(jtools, tool);
  }
  if (model->thinking) {
    jnode_t* jthinking = jobject_new();
    jobject_put(jbody, "thinking", jthinking);
    jobject_put(jthinking, "type", jstring_new(0, model->thinking));
  }
  if (model->reasoning_effort) {
    jnode_t* joutput_config = jobject_new();
    jobject_put(jbody, "output_config", joutput_config);
    jobject_put(joutput_config, "effort",
                jstring_new(0, model->reasoning_effort));
  }
  jobject_put(jbody, "top_p", jnumber_new(model->top_p));
  if (model->max_tokens >= 0) {
    jobject_put(jbody, "max_tokens", jnumber_new(model->max_tokens));
  }
  return jbody;
}

static mdres_t* anthropic_parse(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jstop_reason = jobject_get(jresp, "stop_reason");
  res->stop_reason = jstring_content(jstop_reason);
  res->finished = (strcmp(res->stop_reason, "end_turn") == 0);
  jnode_t* jcontent = jobject_get(jresp, "content");
  if (jis_string(jcontent)) {
    res->content = jstring_content(jcontent);
  } else {
    size_t n_content = jarray_size(jcontent);
    for (size_t i = 0; i < n_content; ++i) {
      jnode_t* jitem = jarray_get(jcontent, i);
      jnode_t* jtype = jobject_get(jitem, "type");
      const char* type = jstring_content(jtype);
      if (strcmp(type, "text") == 0) {
        jnode_t* jtext = jobject_get(jitem, "text");
        res->content = jstring_content(jtext);
      } else if (strcmp(type, "thinking") == 0) {
        jnode_t* jthinking = jobject_get(jitem, "thinking");
        res->reasoning = jstring_content(jthinking);
      } else if (strcmp(type, "tool_use") == 0) {
        res->n_tool_call++;
      }
    }
    if (res->n_tool_call > 0) {
      res->tool_calls = malloc(res->n_tool_call * sizeof(struct tool_call));
      if (!res->tool_calls) {
        model_response_delete(res);
        return NULL;
      }
      for (size_t i = 0, tool_index = 0; i < n_content; ++i) {
        jnode_t* jitem = jarray_get(jcontent, i);
        jnode_t* jtype = jobject_get(jitem, "type");
        const char* type = jstring_content(jtype);
        if (strcmp(type, "tool_use")) continue;
        jnode_t* jid = jobject_get(jitem, "id");
        jnode_t* jname = jobject_get(jitem, "name");
        jnode_t* jinput = jobject_get(jitem, "input");
        res->tool_calls[tool_index].id = jstring_content(jid);
        res->tool_calls[tool_index].name = jstring_content(jname);
        res->tool_calls[tool_index].args = jto_string(jinput);
        ++tool_index;
      }
    }
  }

  return res;
}

static mdres_t* call_anthropic_api(const model_t* model, const context_t* ctx) {
  snprintf(url, sizeof(url), "%s/v1/messages", model->base_url);
  snprintf(auth_header, sizeof(auth_header), "X-Api-Key: %s", model->api_key);
  const char* headers[] = {auth_header, "Content-Type: application/json"};

  jnode_t* jbody = anthropic_body(model, ctx);
  char* body = jto_string(jbody);
  jdelete(jbody);
  log(DEBUG, "Request body: %s", body);

  request_t request = {
      .url = url,
      .n_header = sizeof(headers) / sizeof(headers[0]),
      .headers = headers,
      .body = body,
  };
  response_t* resp = http_post(&request);
  free(body);
  if (!resp) {
    log(ERROR, "HTTP request failed");
    return NULL;
  }
  if (resp->status != 200) {
    log(ERROR, "HTTP request failed with status %ld", resp->status);
    if (resp->data) log(ERROR, "Reason: %s", resp->data);
    response_delete(resp);
    return NULL;
  }

  char* data = (char*)resp->data;
  resp->data = NULL;  // Prevent double free
  response_delete(resp);
  return anthropic_parse(data);
}

mdres_t* call_api(const model_t* model, const context_t* ctx) {
  log(INFO, "Calling API for model %s with protocol %s", model->name,
      protocol_names[model->protocol]);
  switch (model->protocol) {
    case OPENAI: return call_openai_api(model, ctx);
    case ANTHROPIC: return call_anthropic_api(model, ctx);
  }
  return NULL;
}

void model_response_delete(mdres_t* res) {
  if (!res) return;
  if (res->raw) free(res->raw);
  if (res->json) jdelete(res->json);
  if (res->tool_calls) {
    for (size_t i = 0; i < res->n_tool_call; ++i) {
      if (res->tool_calls[i].args) free(res->tool_calls[i].args);
    }
    free(res->tool_calls);
  }
  free(res);
}

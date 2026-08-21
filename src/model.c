#include <stdio.h>
#include <stdlib.h>
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

static jnode_t* tool_to_json(const tool_t* tool) {
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

static char* call_openai_api(const model_t* model, jnode_t* messages,
                             const tool_t* tools, size_t n_tools) {
  snprintf(url, sizeof(url), "%s/chat/completions", model->base_url);
  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
           model->api_key);
  const char* headers[] = {auth_header, "Content-Type: application/json"};

  jnode_t* jbody = jobject_new();
  jobject_put(jbody, "model", jstring_new(0, model->name));
  jobject_put(jbody, "messages", jcopy(messages));
  jnode_t* jtools = jarray_new();
  jobject_put(jbody, "tools", jtools);
  for (size_t i = 0; i < n_tools; ++i) {
    jnode_t* tool = tool_to_json(&tools[i]);
    jarray_add(jtools, tool);
  }
  jnode_t* jthinking = jobject_new();
  jobject_put(jbody, "thinking", jthinking);
  {
    const char* thinking = model->thinking ? "enabled" : "disabled";
    jobject_put(jthinking, "type", jstring_new(0, thinking));
  }
  if (model->reasoning_effort) {
    jobject_put(jthinking, "reasoning_effort",
                jstring_new(0, model->reasoning_effort));
  }
  jobject_put(jthinking, "top_p", jnumber_new(model->top_p));
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
  return data;
}

char* call_api(const model_t* model, jnode_t* messages, const tool_t* tools,
               size_t n_tools) {
  log(INFO, "Calling API for model %s with protocol %s", model->name,
      protocol_names[model->protocol]);
  switch (model->protocol) {
    case OPENAI: return call_openai_api(model, messages, tools, n_tools);
    case ANTHROPIC:
      log(ERROR, "Anthropic protocol not implemented yet");
      return NULL;
  }
  return NULL;
}

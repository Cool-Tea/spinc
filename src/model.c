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

struct sse_context {
  void (*callback)(const mdlres_t* chunk, void* userp);
  void* userp;
};

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

static void openai_context_update(context_t* ctx, const mdlres_t* res) {
  jnode_t* json = res->json;
  jnode_t* jchoices = jobject_get(json, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jmessage = jobject_get(jfirst, "message");
  jarray_add(ctx->messages, jcopy(jmessage));
}

static void anthropic_context_update(context_t* ctx, const mdlres_t* res) {
  jnode_t* json = res->json;
  jnode_t* jcontent = jobject_get(json, "content");
  jnode_t* jmessage = jobject_new();
  jobject_put(jmessage, "role", jstring_new(0, "assistant"));
  jobject_put(jmessage, "content", jcopy(jcontent));
  jarray_add(ctx->messages, jmessage);
}

void context_update(context_t* ctx, const mdlres_t* res) {
  switch (ctx->protocol) {
    case OPENAI: openai_context_update(ctx, res); break;
    case ANTHROPIC: anthropic_context_update(ctx, res); break;
  }
}

static void anthropic_context_update_stream(context_t* ctx,
                                            const mdlres_t* res) {
  jnode_t* json = res->json;
  if (!json) return;
  jnode_t* jtype = jobject_get(json, "type");
  if (jis_empty(jtype)) return;
  const char* type = jstring_content(jtype);

  if (strcmp(type, "message_start") == 0) {
    // A new assistant message begins; create its shell so subsequent
    // content_block_* events can fill it in.
    jnode_t* jmessage = jobject_new();
    jobject_put(jmessage, "role", jstring_new(0, "assistant"));
    jobject_put(jmessage, "content", jarray_new());
    jarray_add(ctx->messages, jmessage);
    return;
  }

  jnode_t* jindex = jobject_get(json, "index");
  if (jis_empty(jindex)) return;
  int index = jas_number(jindex)->value;

  jnode_t* jlast_msg =
      jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jcontent = jobject_get(jlast_msg, "content");

  if (strcmp(type, "content_block_start") == 0) {
    jnode_t* jcontent_block = jobject_get(json, "content_block");
    if (jis_empty(jcontent_block)) return;
    jnode_t* jblock = jcopy(jcontent_block);
    // tool_use input arrives as input_json_delta fragments, so accumulate
    // them as a string until the block is complete.
    jnode_t* jblock_type = jobject_get(jblock, "type");
    if (!jis_empty(jblock_type) &&
        strcmp(jstring_content(jblock_type), "tool_use") == 0) {
      jobject_put(jblock, "input", jstring_new(0, ""));
    }
    if (index >= jarray_size(jcontent)) {
      jarray_add(jcontent, jblock);
    } else {
      jarray_remove(jcontent, index);
      if (index >= jarray_size(jcontent)) {
        jarray_add(jcontent, jblock);
      } else {
        jarray_insert(jcontent, index, jblock);
      }
    }
    return;
  }

  if (index >= jarray_size(jcontent)) return;
  jnode_t* jblock = jarray_get(jcontent, index);

  if (strcmp(type, "content_block_delta") == 0) {
    jnode_t* jdelta = jobject_get(json, "delta");
    if (jis_empty(jdelta)) return;
    jnode_t* jdelta_type = jobject_get(jdelta, "type");
    if (jis_empty(jdelta_type)) return;
    const char* delta_type = jstring_content(jdelta_type);
    if (strcmp(delta_type, "text_delta") == 0) {
      jnode_t* jtext = jobject_get(jdelta, "text");
      if (jis_empty(jtext)) return;
      jnode_t* jblock_text = jobject_get(jblock, "text");
      if (jis_empty(jblock_text)) {
        jobject_put(jblock, "text", jcopy(jtext));
      } else {
        jstring_concat(jblock_text, jstring_content(jtext));
      }
    } else if (strcmp(delta_type, "thinking_delta") == 0) {
      jnode_t* jthinking = jobject_get(jdelta, "thinking");
      if (jis_empty(jthinking)) return;
      jnode_t* jblock_thinking = jobject_get(jblock, "thinking");
      if (jis_empty(jblock_thinking)) {
        jobject_put(jblock, "thinking", jcopy(jthinking));
      } else {
        jstring_concat(jblock_thinking, jstring_content(jthinking));
      }
    } else if (strcmp(delta_type, "input_json_delta") == 0) {
      jnode_t* jpartial = jobject_get(jdelta, "partial_json");
      if (jis_empty(jpartial)) return;
      jnode_t* jinput = jobject_get(jblock, "input");
      if (jis_empty(jinput)) {
        jobject_put(jblock, "input", jcopy(jpartial));
      } else {
        jstring_concat(jinput, jstring_content(jpartial));
      }
    }
    return;
  }

  if (strcmp(type, "content_block_stop") == 0) {
    // The accumulated tool_use input string is now complete JSON; turn it
    // into a real object so the context can be sent back to the API.
    jnode_t* jblock_type = jobject_get(jblock, "type");
    if (jis_empty(jblock_type)) return;
    if (strcmp(jstring_content(jblock_type), "tool_use") != 0) return;
    jnode_t* jinput = jobject_get(jblock, "input");
    if (jis_string(jinput)) {
      jnode_t* jparsed = jfrom_string(jstring_content(jinput));
      jobject_put(jblock, "input", jparsed ? jparsed : jobject_new());
    }
    return;
  }
}

static void openai_context_update_stream(context_t* ctx, const mdlres_t* res) {
  jnode_t* json = res->json;
  jnode_t* jchoices = jobject_get(json, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jdelta = jobject_get(jfirst, "delta");
  if (jis_empty(jdelta)) return;

  jnode_t* jlast_msg =
      jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);

  jnode_t* jdelta_role = jobject_get(jdelta, "role");
  if (!jis_empty(jdelta_role)) {
    jnode_t* jlast_role = jobject_get(jlast_msg, "role");
    const char* role = jstring_content(jdelta_role);
    const char* last_role = jstring_content(jlast_role);
    if (strcmp(last_role, role)) {
      jnode_t* jmessage = jobject_new();
      jarray_add(ctx->messages, jmessage);
      jobject_put(jmessage, "role", jstring_new(0, role));
      jlast_msg = jmessage;
    }
  }

  jnode_t* jdelta_content = jobject_get(jdelta, "content");
  if (!jis_empty(jdelta_content)) {
    jnode_t* jlast_content = jobject_get(jlast_msg, "content");
    if (jis_empty(jlast_content)) {
      jobject_put(jlast_msg, "content", jcopy(jdelta_content));
    } else {
      jstring_concat(jlast_content, jstring_content(jdelta_content));
    }
  }

  jnode_t* jdelta_reasoning = jobject_get(jdelta, "reasoning_content");
  if (!jis_empty(jdelta_reasoning)) {
    jnode_t* jlast_reasoning = jobject_get(jlast_msg, "reasoning_content");
    if (jis_empty(jlast_reasoning)) {
      jobject_put(jlast_msg, "reasoning_content", jcopy(jdelta_reasoning));
    } else {
      jstring_concat(jlast_reasoning, jstring_content(jdelta_reasoning));
    }
  }

  jnode_t* jdelta_tool_calls = jobject_get(jdelta, "tool_calls");
  if (!jis_empty(jdelta_tool_calls)) {
    jnode_t* jlast_tool_calls = jobject_get(jlast_msg, "tool_calls");
    if (jis_empty(jlast_tool_calls)) {
      jlast_tool_calls = jarray_new();
      jobject_put(jlast_msg, "tool_calls", jlast_tool_calls);
      jlast_tool_calls = jlast_tool_calls;
    }
    size_t n_delta_tool_calls = jarray_size(jdelta_tool_calls);
    for (size_t i = 0; i < n_delta_tool_calls; ++i) {
      jnode_t* jdelta_call = jarray_get(jdelta_tool_calls, i);
      jnode_t* jindex = jobject_get(jdelta_call, "index");
      int index = jas_number(jindex)->value;
      if (index >= jarray_size(jlast_tool_calls)) {
        jarray_add(jlast_tool_calls, jcopy(jdelta_call));
      } else {
        jnode_t* jlast_call = jarray_get(jlast_tool_calls, index);

        jnode_t* jdelta_func = jobject_get(jdelta_call, "function");
        if (jis_empty(jdelta_func)) continue;

        jnode_t* jlast_func = jobject_get(jlast_call, "function");
        if (jis_empty(jlast_func)) {
          jobject_put(jlast_call, "function", jcopy(jdelta_func));
          continue;
        }

        jnode_t* jdelta_name = jobject_get(jdelta_func, "name");
        if (!jis_empty(jdelta_name)) {
          jnode_t* jlast_name = jobject_get(jlast_func, "name");
          if (jis_empty(jlast_name)) {
            jobject_put(jlast_func, "name", jcopy(jdelta_name));
          } else {
            jstring_concat(jlast_name, jstring_content(jdelta_name));
          }
        }

        jnode_t* jdelta_args = jobject_get(jdelta_func, "arguments");
        if (!jis_empty(jdelta_args)) {
          jnode_t* jlast_args = jobject_get(jlast_func, "arguments");
          if (jis_empty(jlast_args)) {
            jobject_put(jlast_func, "arguments", jcopy(jdelta_args));
          } else {
            jstring_concat(jlast_args, jstring_content(jdelta_args));
          }
        }
      }
    }
  }
}

void context_update_stream(context_t* ctx, const mdlres_t* res) {
  switch (ctx->protocol) {
    case OPENAI: openai_context_update_stream(ctx, res); break;
    case ANTHROPIC: anthropic_context_update_stream(ctx, res); break;
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

static void openai_context_add_tool_message(context_t* ctx, const char* id,
                                            const char* result) {
  jarray_add(ctx->messages, openai_tool_message(id, result));
}

static void anthropic_context_add_tool_message(context_t* ctx, const char* id,
                                               const char* result) {
  jnode_t* jlast_msg =
      jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jrole = jobject_get(jlast_msg, "role");
  const char* role = jstring_content(jrole);
  if (strcmp(role, "user")) {
    jarray_add(ctx->messages, anthropic_tool_message(id, result));
  } else {
    jnode_t* jcontent = jobject_get(jlast_msg, "content");
    jnode_t* jtool_result = jobject_new();
    jarray_add(jcontent, jtool_result);
    jobject_put(jtool_result, "type", jstring_new(0, "tool_result"));
    jobject_put(jtool_result, "tool_use_id", jstring_new(0, id));
    jobject_put(jtool_result, "content", jstring_new(0, result));
  }
}

void context_add_tool_message(context_t* ctx, const char* id,
                              const char* tool_name, const char* result) {
  (void)tool_name;
  switch (ctx->protocol) {
    case OPENAI: openai_context_add_tool_message(ctx, id, result); break;
    case ANTHROPIC: anthropic_context_add_tool_message(ctx, id, result); break;
  }
}

size_t openai_context_get_last_message_tool_calls(context_t* ctx,
                                                  tool_call_t** calls) {
  jnode_t* jlast_msg =
      jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jtool_calls = jobject_get(jlast_msg, "tool_calls");
  if (jis_empty(jtool_calls)) return 0;
  size_t n_tool_calls = jarray_size(jtool_calls);
  *calls = malloc(n_tool_calls * sizeof(tool_call_t));
  if (!*calls) return 0;
  for (size_t i = 0; i < n_tool_calls; ++i) {
    jnode_t* jcall = jarray_get(jtool_calls, i);
    jnode_t* jid = jobject_get(jcall, "id");
    jnode_t* jfunc = jobject_get(jcall, "function");
    jnode_t* jname = jobject_get(jfunc, "name");
    jnode_t* jargs = jobject_get(jfunc, "arguments");
    (*calls)[i].id = jstring_content(jid);
    (*calls)[i].name = jstring_content(jname);
    (*calls)[i].args = strdup(jstring_content(jargs));
  }
  return n_tool_calls;
}

static size_t anthropic_context_get_last_message_tool_calls(
    context_t* ctx, tool_call_t** calls) {
  jnode_t* jlast_msg =
      jarray_get(ctx->messages, jarray_size(ctx->messages) - 1);
  jnode_t* jcontent = jobject_get(jlast_msg, "content");
  if (jis_empty(jcontent)) return 0;

  size_t n_content = jarray_size(jcontent);
  size_t n_tool_calls = 0;
  for (size_t i = 0; i < n_content; ++i) {
    jnode_t* jitem = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jitem, "type");
    if (!jis_empty(jtype) && strcmp(jstring_content(jtype), "tool_use") == 0) {
      n_tool_calls++;
    }
  }
  if (n_tool_calls == 0) return 0;

  *calls = malloc(n_tool_calls * sizeof(tool_call_t));
  if (!*calls) return 0;

  size_t tool_index = 0;
  for (size_t i = 0; i < n_content; ++i) {
    jnode_t* jitem = jarray_get(jcontent, i);
    jnode_t* jtype = jobject_get(jitem, "type");
    if (strcmp(jstring_content(jtype), "tool_use")) continue;
    jnode_t* jid = jobject_get(jitem, "id");
    jnode_t* jname = jobject_get(jitem, "name");
    jnode_t* jinput = jobject_get(jitem, "input");
    (*calls)[tool_index].id = jstring_content(jid);
    (*calls)[tool_index].name = jstring_content(jname);
    (*calls)[tool_index].args = jis_string(jinput)
                                    ? strdup(jstring_content(jinput))
                                    : jto_string(jinput);
    ++tool_index;
  }
  return n_tool_calls;
}

size_t context_get_last_message_tool_calls(context_t* ctx,
                                           tool_call_t** calls) {
  switch (ctx->protocol) {
    case OPENAI: return openai_context_get_last_message_tool_calls(ctx, calls);
    case ANTHROPIC:
      return anthropic_context_get_last_message_tool_calls(ctx, calls);
  }
  return 0;
}

static mdlres_t* model_response_new() {
  mdlres_t* res = malloc(sizeof(mdlres_t));
  if (!res) return NULL;
  memset(res, 0, sizeof(mdlres_t));
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
  if (model->stream) {
    jobject_put(jbody, "stream", jbool_new(true));
  }
  return jbody;
}

static mdlres_t* openai_parse(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdlres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jid = jobject_get(jresp, "id");
  res->id = jstring_content(jid);
  jnode_t* jchoices = jobject_get(jresp, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jfinish_reason = jobject_get(jfirst, "finish_reason");
  res->stop_reason = jstring_content(jfinish_reason);
  res->finished = (strcmp(res->stop_reason, "stop") == 0);
  res->need_tool_call = (strcmp(res->stop_reason, "tool_calls") == 0);
  jnode_t* jmessage = jobject_get(jfirst, "message");
  jnode_t* jreasoning = jobject_get(jmessage, "reasoning_content");
  if (!jis_empty(jreasoning) && jstring_len(jreasoning))
    res->reasoning = jstring_content(jreasoning);
  jnode_t* jcontent = jobject_get(jmessage, "content");
  if (!jis_empty(jcontent) && jstring_len(jcontent))
    res->content = jstring_content(jcontent);
  jnode_t* jtool_calls = jobject_get(jmessage, "tool_calls");
  if (!jis_empty(jtool_calls)) {
    res->n_tool_call = jarray_size(jtool_calls);
    res->tool_calls = malloc(res->n_tool_call * sizeof(struct tool_call));
    if (!res->tool_calls) {
      model_response_delete(res);
      return NULL;
    }
    for (size_t i = 0; i < res->n_tool_call; ++i) {
      jnode_t* jcall = jarray_get(jtool_calls, i);
      jnode_t* jcall_id = jobject_get(jcall, "id");
      jnode_t* jfunc = jobject_get(jcall, "function");
      jnode_t* jname = jobject_get(jfunc, "name");
      jnode_t* jargs = jobject_get(jfunc, "arguments");
      res->tool_calls[i].id = jstring_content(jcall_id);
      res->tool_calls[i].name = jstring_content(jname);
      res->tool_calls[i].args = strdup(jstring_content(jargs));
    }
  }

  return res;
}

static mdlres_t* openai_parse_stream(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdlres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jid = jobject_get(jresp, "id");
  res->id = jstring_content(jid);
  jnode_t* jchoices = jobject_get(jresp, "choices");
  jnode_t* jfirst = jarray_get(jchoices, 0);
  jnode_t* jfinish_reason = jobject_get(jfirst, "finish_reason");
  res->stop_reason =
      jis_empty(jfinish_reason) ? NULL : jstring_content(jfinish_reason);
  res->finished =
      res->stop_reason ? (strcmp(res->stop_reason, "stop") == 0) : false;
  res->need_tool_call =
      res->stop_reason ? (strcmp(res->stop_reason, "tool_calls") == 0) : false;
  jnode_t* jdelta = jobject_get(jfirst, "delta");
  jnode_t* jreasoning = jobject_get(jdelta, "reasoning_content");
  if (!jis_empty(jreasoning) && jstring_len(jreasoning))
    res->reasoning = jstring_content(jreasoning);
  log(DEBUG, "Parsed SSE chunk: reasoning=%s",
      res->reasoning ? res->reasoning : "NULL");
  jnode_t* jcontent = jobject_get(jdelta, "content");
  if (!jis_empty(jcontent) && jstring_len(jcontent))
    res->content = jstring_content(jcontent);
  jnode_t* jtool_calls = jobject_get(jdelta, "tool_calls");
  if (!jis_empty(jtool_calls)) {
    res->n_tool_call = jarray_size(jtool_calls);
    res->tool_calls = malloc(res->n_tool_call * sizeof(struct tool_call));
    if (!res->tool_calls) {
      model_response_delete(res);
      return NULL;
    }
    for (size_t i = 0; i < res->n_tool_call; ++i) {
      jnode_t* jcall = jarray_get(jtool_calls, i);
      jnode_t* jcall_id = jobject_get(jcall, "id");
      res->tool_calls[i].id =
          jis_empty(jcall_id) ? NULL : jstring_content(jcall_id);
      jnode_t* jfunc = jobject_get(jcall, "function");
      if (!jis_empty(jfunc)) {
        jnode_t* jname = jobject_get(jfunc, "name");
        res->tool_calls[i].name =
            jis_empty(jname) ? NULL : jstring_content(jname);
        jnode_t* jargs = jobject_get(jfunc, "arguments");
        res->tool_calls[i].args =
            jis_empty(jargs) ? NULL : strdup(jstring_content(jargs));
      }
    }
  }

  return res;
}

static mdlres_t* call_openai_api(const model_t* model, const context_t* ctx) {
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

static void openai_sse_callback(const response_t* event, void* userp) {
  struct sse_context* ctx = (struct sse_context*)userp;
  char* resp = strndup((const char*)event->data, event->data_size);
  mdlres_t* chunk = openai_parse_stream(resp);
  if (!chunk) {
    log(ERROR, "Failed to parse SSE chunk");
    free(resp);
    return;
  }
  ctx->callback(chunk, ctx->userp);
  model_response_delete(chunk);
}

static bool call_openai_api_stream(const model_t* model, const context_t* ctx,
                                   void (*callback)(const mdlres_t* chunk,
                                                    void* userp),
                                   void* userp) {
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
  struct sse_context sse_ctx = {.callback = callback, .userp = userp};
  bool res = http_sse(&request, openai_sse_callback, &sse_ctx);
  free(body);
  return res;
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
  if (model->stream) {
    jobject_put(jbody, "stream", jbool_new(true));
  }
  return jbody;
}

static mdlres_t* anthropic_parse(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdlres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jstop_reason = jobject_get(jresp, "stop_reason");
  res->stop_reason = jstring_content(jstop_reason);
  res->finished = (strcmp(res->stop_reason, "end_turn") == 0);
  res->need_tool_call = (strcmp(res->stop_reason, "tool_use") == 0);
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

static mdlres_t* anthropic_parse_stream(char* response) {
  jnode_t* jresp = jfrom_string(response);
  if (!jresp) {
    log(ERROR, "Failed to parse JSON response");
    return NULL;
  }

  mdlres_t* res = model_response_new();
  if (!res) {
    jdelete(jresp);
    return NULL;
  }

  res->raw = response;
  res->json = jresp;

  jnode_t* jtype = jobject_get(jresp, "type");
  if (jis_empty(jtype)) return res;
  const char* type = jstring_content(jtype);

  if (strcmp(type, "message_start") == 0) {
    jnode_t* jmessage = jobject_get(jresp, "message");
    jnode_t* jid = jobject_get(jmessage, "id");
    if (!jis_empty(jid)) res->id = jstring_content(jid);
  } else if (strcmp(type, "content_block_delta") == 0) {
    jnode_t* jdelta = jobject_get(jresp, "delta");
    jnode_t* jdelta_type = jobject_get(jdelta, "type");
    if (jis_empty(jdelta_type)) return res;
    const char* delta_type = jstring_content(jdelta_type);
    if (strcmp(delta_type, "text_delta") == 0) {
      jnode_t* jtext = jobject_get(jdelta, "text");
      if (!jis_empty(jtext)) res->content = jstring_content(jtext);
    } else if (strcmp(delta_type, "thinking_delta") == 0) {
      jnode_t* jthinking = jobject_get(jdelta, "thinking");
      if (!jis_empty(jthinking)) res->reasoning = jstring_content(jthinking);
    }
    // input_json_delta carries tool_use fragments; it is consumed by
    // anthropic_context_update_stream directly from res->json.
  } else if (strcmp(type, "message_delta") == 0) {
    jnode_t* jdelta = jobject_get(jresp, "delta");
    jnode_t* jstop_reason = jobject_get(jdelta, "stop_reason");
    if (!jis_empty(jstop_reason)) {
      res->stop_reason = jstring_content(jstop_reason);
      res->finished = (strcmp(res->stop_reason, "end_turn") == 0);
      res->need_tool_call = (strcmp(res->stop_reason, "tool_use") == 0);
    }
  } else if (strcmp(type, "error") == 0) {
    jnode_t* jerror = jobject_get(jresp, "error");
    jnode_t* jmessage = jobject_get(jerror, "message");
    if (!jis_empty(jmessage)) {
      log(ERROR, "Anthropic stream error: %s", jstring_content(jmessage));
    }
  }

  return res;
}

static mdlres_t* call_anthropic_api(const model_t* model,
                                    const context_t* ctx) {
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

static void anthropic_sse_callback(const response_t* event, void* userp) {
  struct sse_context* ctx = (struct sse_context*)userp;
  char* resp = strndup((const char*)event->data, event->data_size);
  mdlres_t* chunk = anthropic_parse_stream(resp);
  if (!chunk) {
    log(ERROR, "Failed to parse SSE chunk");
    free(resp);
    return;
  }
  ctx->callback(chunk, ctx->userp);
  model_response_delete(chunk);
}

static bool call_anthropic_api_stream(
    const model_t* model, const context_t* ctx,
    void (*callback)(const mdlres_t* chunk, void* userp), void* userp) {
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
  struct sse_context sse_ctx = {.callback = callback, .userp = userp};
  bool res = http_sse(&request, anthropic_sse_callback, &sse_ctx);
  free(body);
  return res;
}

mdlres_t* call_api(const model_t* model, const context_t* ctx) {
  log(INFO, "Calling API for model %s with protocol %s", model->name,
      protocol_names[model->protocol]);
  switch (model->protocol) {
    case OPENAI: return call_openai_api(model, ctx);
    case ANTHROPIC: return call_anthropic_api(model, ctx);
  }
  return NULL;
}

bool call_api_stream(const model_t* model, const context_t* ctx,
                     void (*callback)(const mdlres_t* chunk, void* userp),
                     void* userp) {
  log(INFO, "Calling API (streaming) for model %s with protocol %s",
      model->name, protocol_names[model->protocol]);
  switch (model->protocol) {
    case OPENAI: return call_openai_api_stream(model, ctx, callback, userp);
    case ANTHROPIC:
      return call_anthropic_api_stream(model, ctx, callback, userp);
  }
  return false;
}

void model_response_delete(mdlres_t* res) {
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

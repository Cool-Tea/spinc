#include <stdlib.h>
#include <string.h>

#include "message.h"

/* ==============================
 *      Internal helpers
 * ============================== */

static void message_free_fields(message_t* message) {
  if (!message) return;
  switch (message->type) {
    case SYSTEM: free(message->system.text); break;
    case USER: free(message->user.text); break;
    case ASSISTANT:
      free(message->assistant.id);
      free(message->assistant.text);
      break;
    case REASONING:
      free(message->reasoning.id);
      free(message->reasoning.text);
      break;
    case TOOL_CALL:
      free(message->tool_call.id);
      free(message->tool_call.call_id);
      free(message->tool_call.name);
      free(message->tool_call.args);
      break;
    case TOOL_RESULT:
      free(message->tool_result.call_id);
      free(message->tool_result.name);
      free(message->tool_result.result);
      break;
  }
}

/* ==============================
 *       Single messages
 * ============================== */

message_t* message_new(msgtyp_t type) {
  message_t* m = calloc(1, sizeof(message_t));
  if (!m) return NULL;
  m->type = type;
  return m;
}

void message_delete(message_t* message) {
  if (!message) return;
  message_free_fields(message);
  free(message);
}

// The id is only meaningful for messages that belong to an assistant turn
// (ASSISTANT/REASONING/TOOL_CALL share the same turn id).
const char* message_id(const message_t* message) {
  if (!message) return NULL;
  switch (message->type) {
    case ASSISTANT: return message->assistant.id;
    case REASONING: return message->reasoning.id;
    case TOOL_CALL: return message->tool_call.id;
    default: return NULL;
  }
}

err_t message_set_id(message_t* message, const char* id) {
  if (!message || !id) return ERROR_NULLPTR;
  char* copy = strdup(id);
  if (!copy) return ERROR_OUT_OF_MEMORY;
  switch (message->type) {
    case ASSISTANT:
      free(message->assistant.id);
      message->assistant.id = copy;
      message->assistant.id_len = strlen(copy);
      break;
    case REASONING:
      free(message->reasoning.id);
      message->reasoning.id = copy;
      message->reasoning.id_len = strlen(copy);
      break;
    case TOOL_CALL:
      free(message->tool_call.id);
      message->tool_call.id = copy;
      message->tool_call.id_len = strlen(copy);
      break;
    default:
      // No id field for this type; ignore.
      free(copy);
      break;
  }
  return ERROR_NONE;
}

err_t message_copy(const message_t* src, message_t** dst) {
  if (!src || !dst) return ERROR_NULLPTR;
  *dst = NULL;
  message_t* m = calloc(1, sizeof(message_t));
  if (!m) return ERROR_OUT_OF_MEMORY;
  m->type = src->type;
  switch (src->type) {
    case SYSTEM:
      m->system.text = strndup(src->system.text, src->system.text_len);
      m->system.text_len = src->system.text_len;
      if (src->system.text && !m->system.text) goto oom;
      break;
    case USER:
      m->user.text = strndup(src->user.text, src->user.text_len);
      m->user.text_len = src->user.text_len;
      if (src->user.text && !m->user.text) goto oom;
      break;
    case ASSISTANT:
      m->assistant.id = strndup(src->assistant.id, src->assistant.id_len);
      m->assistant.id_len = src->assistant.id_len;
      m->assistant.text = strndup(src->assistant.text, src->assistant.text_len);
      m->assistant.text_len = src->assistant.text_len;
      if (src->assistant.id && !m->assistant.id) goto oom;
      if (src->assistant.text && !m->assistant.text) goto oom;
      break;
    case REASONING:
      m->reasoning.id = strndup(src->reasoning.id, src->reasoning.id_len);
      m->reasoning.id_len = src->reasoning.id_len;
      m->reasoning.text = strndup(src->reasoning.text, src->reasoning.text_len);
      m->reasoning.text_len = src->reasoning.text_len;
      if (src->reasoning.id && !m->reasoning.id) goto oom;
      if (src->reasoning.text && !m->reasoning.text) goto oom;
      break;
    case TOOL_CALL:
      m->tool_call.id = strndup(src->tool_call.id, src->tool_call.id_len);
      m->tool_call.id_len = src->tool_call.id_len;
      m->tool_call.call_id =
          strndup(src->tool_call.call_id, src->tool_call.call_id_len);
      m->tool_call.call_id_len = src->tool_call.call_id_len;
      m->tool_call.name = strndup(src->tool_call.name, src->tool_call.name_len);
      m->tool_call.name_len = src->tool_call.name_len;
      m->tool_call.args = strndup(src->tool_call.args, src->tool_call.args_len);
      m->tool_call.args_len = src->tool_call.args_len;
      if (src->tool_call.id && !m->tool_call.id) goto oom;
      if (src->tool_call.call_id && !m->tool_call.call_id) goto oom;
      if (src->tool_call.name && !m->tool_call.name) goto oom;
      if (src->tool_call.args && !m->tool_call.args) goto oom;
      break;
    case TOOL_RESULT:
      m->tool_result.call_id =
          strndup(src->tool_result.call_id, src->tool_result.call_id_len);
      m->tool_result.call_id_len = src->tool_result.call_id_len;
      m->tool_result.name =
          strndup(src->tool_result.name, src->tool_result.name_len);
      m->tool_result.name_len = src->tool_result.name_len;
      m->tool_result.result =
          strndup(src->tool_result.result, src->tool_result.result_len);
      m->tool_result.result_len = src->tool_result.result_len;
      if (src->tool_result.call_id && !m->tool_result.call_id) goto oom;
      if (src->tool_result.name && !m->tool_result.name) goto oom;
      if (src->tool_result.result && !m->tool_result.result) goto oom;
      break;
  }
  *dst = m;
  return ERROR_NONE;
oom:
  message_delete(m);
  return ERROR_OUT_OF_MEMORY;
}

// Serialize a single message in the general (provider-independent) form:
//   {"type": "user"|"assistant"|"reasoning"|"tool_call"|"tool_result"|"system",
//    <type-specific fields>}
err_t message_to_json(const message_t* message, jnode_t** node) {
  if (!message || !node) return ERROR_NULLPTR;
  *node = NULL;
  jnode_t* jo = jobject_new();
  if (!jo) return ERROR_OUT_OF_MEMORY;

  const char* type;
  switch (message->type) {
    case SYSTEM: type = "system"; break;
    case USER: type = "user"; break;
    case ASSISTANT: type = "assistant"; break;
    case REASONING: type = "reasoning"; break;
    case TOOL_CALL: type = "tool_call"; break;
    case TOOL_RESULT: type = "tool_result"; break;
    default: type = "unknown"; break;
  }
  jobject_put(jo, "type", jstring_new(0, type));
  const char* mid = message_id(message);
  if (mid) jobject_put(jo, "id", jstring_new(0, mid));

  switch (message->type) {
    case SYSTEM:
      if (message->system.text)
        jobject_put(
            jo, "text",
            jstring_new((int)message->system.text_len, message->system.text));
      break;
    case USER:
      if (message->user.text)
        jobject_put(
            jo, "text",
            jstring_new((int)message->user.text_len, message->user.text));
      break;
    case ASSISTANT:
      if (message->assistant.text)
        jobject_put(jo, "text",
                    jstring_new((int)message->assistant.text_len,
                                message->assistant.text));
      break;
    case REASONING:
      if (message->reasoning.text)
        jobject_put(jo, "text",
                    jstring_new((int)message->reasoning.text_len,
                                message->reasoning.text));
      break;
    case TOOL_CALL:
      if (message->tool_call.call_id)
        jobject_put(jo, "call_id",
                    jstring_new((int)message->tool_call.call_id_len,
                                message->tool_call.call_id));
      if (message->tool_call.name)
        jobject_put(jo, "name",
                    jstring_new((int)message->tool_call.name_len,
                                message->tool_call.name));
      if (message->tool_call.args)
        jobject_put(jo, "args",
                    jstring_new((int)message->tool_call.args_len,
                                message->tool_call.args));
      break;
    case TOOL_RESULT:
      if (message->tool_result.call_id)
        jobject_put(jo, "call_id",
                    jstring_new((int)message->tool_result.call_id_len,
                                message->tool_result.call_id));
      if (message->tool_result.name)
        jobject_put(jo, "name",
                    jstring_new((int)message->tool_result.name_len,
                                message->tool_result.name));
      if (message->tool_result.result)
        jobject_put(jo, "result",
                    jstring_new((int)message->tool_result.result_len,
                                message->tool_result.result));
      break;
  }
  *node = jo;
  return ERROR_NONE;
}

err_t message_from_json(const jnode_t* node, message_t** message) {
  if (!node || !message) return ERROR_NULLPTR;
  *message = NULL;
  // sjson getters are not const-correct; they do not mutate the node.
  jnode_t* jn = (jnode_t*)node;
  if (!jis_object(jn)) return ERROR_UNKNOWN;

  jnode_t* jtype = jobject_get(jn, "type");
  if (!jis_string(jtype)) return ERROR_UNKNOWN;
  const char* type = jstring_content(jtype);

  msgtyp_t mtype;
  if (strcmp(type, "system") == 0) mtype = SYSTEM;
  else if (strcmp(type, "user") == 0) mtype = USER;
  else if (strcmp(type, "assistant") == 0) mtype = ASSISTANT;
  else if (strcmp(type, "reasoning") == 0) mtype = REASONING;
  else if (strcmp(type, "tool_call") == 0) mtype = TOOL_CALL;
  else if (strcmp(type, "tool_result") == 0) mtype = TOOL_RESULT;
  else return ERROR_UNKNOWN;

  message_t* m = calloc(1, sizeof(message_t));
  if (!m) return ERROR_OUT_OF_MEMORY;
  m->type = mtype;

  // The general form stores the turn id under "id"; it lands in the
  // type-specific id field (ignored for types without one).
  jnode_t* jid = jobject_get(jn, "id");
  if (jis_string(jid)) {
    err_t e = message_set_id(m, jstring_content(jid));
    if (e != ERROR_NONE) {
      message_delete(m);
      return e;
    }
  }

  switch (mtype) {
    case SYSTEM:
    case USER:
    case ASSISTANT:
    case REASONING: {
      jnode_t* jtext = jobject_get(jn, "text");
      if (jis_string(jtext)) {
        char* t = strdup(jstring_content(jtext));
        if (!t) goto oom;
        size_t tl = strlen(t);
        if (mtype == SYSTEM) {
          m->system.text = t;
          m->system.text_len = tl;
        } else if (mtype == USER) {
          m->user.text = t;
          m->user.text_len = tl;
        } else if (mtype == ASSISTANT) {
          m->assistant.text = t;
          m->assistant.text_len = tl;
        } else {
          m->reasoning.text = t;
          m->reasoning.text_len = tl;
        }
      }
      break;
    }
    case TOOL_CALL: {
      jnode_t* jcall_id = jobject_get(jn, "call_id");
      jnode_t* jname = jobject_get(jn, "name");
      jnode_t* jargs = jobject_get(jn, "args");
      if (jis_string(jcall_id)) {
        m->tool_call.call_id = strdup(jstring_content(jcall_id));
        if (!m->tool_call.call_id) goto oom;
        m->tool_call.call_id_len = strlen(m->tool_call.call_id);
      }
      if (jis_string(jname)) {
        m->tool_call.name = strdup(jstring_content(jname));
        if (!m->tool_call.name) goto oom;
        m->tool_call.name_len = strlen(m->tool_call.name);
      }
      if (jis_string(jargs)) {
        m->tool_call.args = strdup(jstring_content(jargs));
        if (!m->tool_call.args) goto oom;
        m->tool_call.args_len = strlen(m->tool_call.args);
      }
      break;
    }
    case TOOL_RESULT: {
      jnode_t* jcall_id = jobject_get(jn, "call_id");
      jnode_t* jname = jobject_get(jn, "name");
      jnode_t* jresult = jobject_get(jn, "result");
      if (jis_string(jcall_id)) {
        m->tool_result.call_id = strdup(jstring_content(jcall_id));
        if (!m->tool_result.call_id) goto oom;
        m->tool_result.call_id_len = strlen(m->tool_result.call_id);
      }
      if (jis_string(jname)) {
        m->tool_result.name = strdup(jstring_content(jname));
        if (!m->tool_result.name) goto oom;
        m->tool_result.name_len = strlen(m->tool_result.name);
      }
      if (jis_string(jresult)) {
        m->tool_result.result = strdup(jstring_content(jresult));
        if (!m->tool_result.result) goto oom;
        m->tool_result.result_len = strlen(m->tool_result.result);
      }
      break;
    }
  }
  *message = m;
  return ERROR_NONE;
oom:
  message_delete(m);
  return ERROR_OUT_OF_MEMORY;
}

/* ==============================
 *        Message lists
 * ============================== */

err_t msglist_new(msglist_t** msglist) {
  if (!msglist) return ERROR_NULLPTR;
  *msglist = NULL;
  msglist_t* ml = calloc(1, sizeof(msglist_t));
  if (!ml) return ERROR_OUT_OF_MEMORY;
  *msglist = ml;
  return ERROR_NONE;
}

void msglist_delete(msglist_t* msglist) {
  if (!msglist) return;
  for (size_t i = 0; i < msglist->n_message; ++i) {
    message_free_fields(&msglist->messages[i]);
  }
  free(msglist->messages);
  free(msglist);
}

err_t msglist_copy(const msglist_t* src, msglist_t** dst) {
  if (!src || !dst) return ERROR_NULLPTR;
  *dst = NULL;
  msglist_t* ml = NULL;
  err_t err = msglist_new(&ml);
  if (err != ERROR_NONE) return err;
  err = msglist_copy_messages(src, ml);
  if (err != ERROR_NONE) {
    msglist_delete(ml);
    return err;
  }
  *dst = ml;
  return ERROR_NONE;
}

err_t msglist_copy_messages(const msglist_t* src, msglist_t* dst) {
  if (!src || !dst) return ERROR_NULLPTR;
  for (size_t i = 0; i < src->n_message; ++i) {
    message_t* m = NULL;
    err_t err = message_copy(&src->messages[i], &m);
    if (err != ERROR_NONE) return err;
    err = msglist_add_message(dst, m);
    if (err != ERROR_NONE) return err;
  }
  return ERROR_NONE;
}

err_t msglist_add_message(msglist_t* msglist, message_t* message) {
  if (!msglist || !message) return ERROR_NULLPTR;
  if (msglist->n_message == msglist->cap) {
    size_t new_cap = msglist->cap ? msglist->cap * 2 : 4;
    message_t* new_messages =
        realloc(msglist->messages, new_cap * sizeof(message_t));
    if (!new_messages) return ERROR_OUT_OF_MEMORY;
    msglist->messages = new_messages;
    msglist->cap = new_cap;
  }
  // Move the message content (and its string ownership) into the list.
  msglist->messages[msglist->n_message++] = *message;
  free(message);
  return ERROR_NONE;
}

err_t msglist_add_message_copy(msglist_t* msglist, const message_t* message) {
  if (!msglist || !message) return ERROR_NULLPTR;
  message_t* m = NULL;
  err_t err = message_copy(message, &m);
  if (err != ERROR_NONE) return err;
  return msglist_add_message(msglist, m);
}

void msglist_pop_message(msglist_t* msglist, message_t** message) {
  if (!message) return;
  *message = NULL;
  if (!msglist || msglist->n_message == 0) return;
  message_t* out = malloc(sizeof(message_t));
  if (!out) return;
  *out = msglist->messages[msglist->n_message - 1];
  msglist->n_message--;
  *message = out;
}

void msglist_pop_message_delete(msglist_t* msglist) {
  if (!msglist || msglist->n_message == 0) return;
  message_t* m = NULL;
  msglist_pop_message(msglist, &m);
  if (m) message_delete(m);
}

err_t msglist_to_json(const msglist_t* msglist, jnode_t** node) {
  if (!msglist || !node) return ERROR_NULLPTR;
  *node = NULL;
  jnode_t* jarr = jarray_new();
  if (!jarr) return ERROR_OUT_OF_MEMORY;
  for (size_t i = 0; i < msglist->n_message; ++i) {
    jnode_t* jm = NULL;
    err_t err = message_to_json(&msglist->messages[i], &jm);
    if (err != ERROR_NONE) {
      jdelete(jarr);
      return err;
    }
    jarray_add(jarr, jm);
  }
  *node = jarr;
  return ERROR_NONE;
}

err_t msglist_from_json(const jnode_t* node, msglist_t** msglist) {
  if (!node || !msglist) return ERROR_NULLPTR;
  *msglist = NULL;
  // sjson getters are not const-correct; they do not mutate the node.
  jnode_t* jn = (jnode_t*)node;
  if (!jis_array(jn)) return ERROR_UNKNOWN;
  msglist_t* ml = NULL;
  err_t err = msglist_new(&ml);
  if (err != ERROR_NONE) return err;
  for (int i = 0; i < jarray_size(jn); ++i) {
    message_t* m = NULL;
    err = message_from_json(jarray_get(jn, i), &m);
    if (err != ERROR_NONE) {
      msglist_delete(ml);
      return err;
    }
    err = msglist_add_message(ml, m);
    if (err != ERROR_NONE) {
      msglist_delete(ml);
      return err;
    }
  }
  *msglist = ml;
  return ERROR_NONE;
}

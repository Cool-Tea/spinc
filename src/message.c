#include "message.h"

jnode_t* system_message(char* content) {
  jnode_t* jmsg = jobject_new();
  jobject_put(jmsg, "role", jstring_new(0, "system"));
  jobject_put(jmsg, "content", jstring_own(content));
  return jmsg;
}

jnode_t* user_message(char* content) {
  jnode_t* jmsg = jobject_new();
  jobject_put(jmsg, "role", jstring_new(0, "user"));
  jobject_put(jmsg, "content", jstring_own(content));
  return jmsg;
}

jnode_t* assistant_message(char* content) {
  jnode_t* jmsg = jobject_new();
  jobject_put(jmsg, "role", jstring_new(0, "assistant"));
  jobject_put(jmsg, "content", jstring_own(content));
  return jmsg;
}

jnode_t* tool_message(char* tool_call_id, char* content) {
  jnode_t* jmsg = jobject_new();
  jobject_put(jmsg, "tool_call_id", jstring_own(tool_call_id));
  jobject_put(jmsg, "role", jstring_new(0, "tool"));
  jobject_put(jmsg, "content", jstring_own(content));
  return jmsg;
}

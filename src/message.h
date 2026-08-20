#ifndef MESSAGE_H
#define MESSAGE_H

#include "sjson.h"

jnode_t* system_message(char* content);
jnode_t* user_message(char* content);
jnode_t* assistant_message(char* content);
jnode_t* tool_message(char* tool_call_id, char* content);

#endif  // MESSAGE_H
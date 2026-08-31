#ifndef MESSAGE_H
#define MESSAGE_H

#include <stddef.h>
#include "sjson.h"

#include "error.h"

typedef enum message_type {
  SYSTEM,
  USER,
  ASSISTANT,
  REASONING,
  TOOL_CALL,
  TOOL_RESULT,
} msgtyp_t;

typedef struct system_message {
  size_t text_len;
  char* text;
} sysmsg_t;

typedef struct user_message {
  size_t text_len;
  char* text;
} usrmsg_t;

typedef struct assistant_message {
  size_t id_len;
  char* id;
  size_t text_len;
  char* text;
} asstmsg_t;

typedef struct reasoning_message {
  size_t id_len;
  char* id;
  size_t text_len;
  char* text;
} reasmsg_t;

typedef struct toolcall {
  size_t id_len;
  char* id;
  size_t call_id_len;
  char* call_id;
  size_t name_len;
  char* name;
  size_t args_len;
  char* args;
} toolcall_t;

typedef struct toolresult {
  size_t call_id_len;
  char* call_id;
  size_t name_len;
  char* name;
  size_t result_len;
  char* result;
} toolres_t;

typedef struct message {
  msgtyp_t type;
  union {
    sysmsg_t system;
    usrmsg_t user;
    asstmsg_t assistant;
    reasmsg_t reasoning;
    toolcall_t tool_call;
    toolres_t tool_result;
  };
} message_t;

typedef struct message_list {
  size_t n_message;
  size_t cap;
  message_t* messages;
} msglist_t;

message_t* message_new(msgtyp_t type);  // zeroed message of the given type
void message_delete(message_t* message);
err_t message_copy(const message_t* src, message_t** dst);
// Return the turn/grouping id (ASSISTANT/REASONING/TOOL_CALL only, else NULL).
const char* message_id(const message_t* message);
// Duplicate `id` into the type-specific id field (no-op for types without one).
err_t message_set_id(message_t* message, const char* id);
err_t message_to_json(const message_t* message, jnode_t** node);
err_t message_from_json(const jnode_t* node, message_t** message);

err_t msglist_new(msglist_t** msglist);
void msglist_delete(msglist_t* msglist);
err_t msglist_copy(const msglist_t* src, msglist_t** dst);
err_t msglist_copy_messages(const msglist_t* src, msglist_t* dst);
err_t msglist_add_message(msglist_t* msglist,
                          message_t* message);  // owns message
err_t msglist_add_message_copy(msglist_t* msglist, const message_t* message);
void msglist_pop_message(
    msglist_t* msglist, message_t** message);  // transfers ownership of message
void msglist_pop_message_delete(
    msglist_t* msglist);  // deletes the popped message
err_t msglist_to_json(const msglist_t* msglist, jnode_t** node);
err_t msglist_from_json(const jnode_t* node, msglist_t** msglist);

#endif
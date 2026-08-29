#ifndef TOOL_UTILS_H
#define TOOL_UTILS_H

#include "sjson.h"

// Check a condition; on failure record the error into the tool result object
// (`res`) and jump to the cleanup label `name##_end`. The error is
// model-visible, so the tool still returns ERROR_NONE with the JSON result
// (which then contains the "error" key).
#define TOOL_CHECK(name, condition, msg, ...)                \
  if (!(condition)) {                                        \
    log(ERROR, "[%s] tool error" msg, #name, ##__VA_ARGS__); \
    tool_error(res, msg, ##__VA_ARGS__);                     \
    goto name##_end;                                         \
  }

// clang-format off

// Record an error message into the tool result object `res` under the "error"
// key. The message is also logged. `res` must be a valid object.
__attribute__((format(printf, 2, 3)))
void tool_error(jnode_t* res, const char* msg, ...);

// clang-format on

#endif  // TOOL_UTILS_H

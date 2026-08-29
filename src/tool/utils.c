#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "sjson.h"

#include "log.h"
#include "tool/utils.h"

void tool_error(jnode_t* res, const char* msg, ...) {
  va_list args1;
  va_start(args1, msg);
  va_list args2;
  va_copy(args2, args1);
  int len = vsnprintf(NULL, 0, msg, args1);
  va_end(args1);
  char* buf = malloc(len + 1);
  vsnprintf(buf, len + 1, msg, args2);
  va_end(args2);
  log(ERROR, "Error: %s", buf);
  jobject_put(res, "error", jstring_own(buf));
}
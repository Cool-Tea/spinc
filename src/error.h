#ifndef ERROR_H
#define ERROR_H

#include <stdbool.h>

typedef enum error_code {
  /* Success */
  ERROR_NONE = 0,
  /* Builtin errors start here */
  ERROR_NULLPTR,
  ERROR_OUT_OF_MEMORY,
  ERROR_OUT_OF_BOUNDS,
  ERROR_IO,
  ERROR_NOT_FOUND,
  ERROR_CURL,
  ERROR_UNKNOWN,
  /* Custom errors start here */
  ERROR_CUSTOM = 0x1000,
} err_t;

inline static bool is_error(err_t err) { return err != ERROR_NONE; }
inline static bool is_custom(err_t err) { return err > ERROR_CUSTOM; }
const char* error_str(err_t err);

#endif  // ERROR_H
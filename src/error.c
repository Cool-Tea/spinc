#include "error.h"

// clang-format off

static const char* error_strings[] = {
    [ERROR_NONE]          = "No error",
    [ERROR_NULLPTR]       = "Null pointer",
    [ERROR_OUT_OF_MEMORY] = "Out of memory",
    [ERROR_OUT_OF_BOUNDS] = "Out of bounds",
    [ERROR_IO]            = "I/O error",
    [ERROR_NOT_FOUND]     = "Tool not found",
    [ERROR_NOT_READY]     = "Not ready",
    [ERROR_SERDE]         = "(De)Serialization error",
    [ERROR_CURL]          = "CURL error",
    [ERROR_UNKNOWN]       = "Unknown error",
};

// clang-format on

const char* error_str(err_t err) { return error_strings[err]; }
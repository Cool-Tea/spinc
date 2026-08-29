#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>

typedef struct model {
  bool stream;
  float top_p;
  const char* name;
  const char* base_url;
  const char* api_key;
  const char* thinking;
  const char* reasoning_effort;
  long max_tokens;
} model_t;

#endif  // MODEL_H
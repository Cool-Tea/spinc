#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdbool.h>

typedef struct request {
  const char* url;
  size_t n_header;
  const char** headers;
  const char* body;
} request_t;

typedef struct response {
  long status;
  size_t data_size;
  unsigned char* data;
} response_t;

bool http_init();
void http_quit();
void response_delete(response_t* response);
response_t* http_post(const request_t* request);

#endif  // HTTP_H
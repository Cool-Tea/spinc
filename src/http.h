#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "error.h"

typedef struct request {
  const char* url;
  size_t n_header;
  const char** headers;
  size_t body_size;
  const char* body;
} request_t;

typedef struct response {
  long status;
  size_t data_size;
  unsigned char* data;
} response_t;

typedef struct event {
  size_t event_len;
  const char* event;
  size_t data_len;
  const unsigned char* data;
} event_t;

err_t http_init();
void http_quit();
err_t http_post(const request_t* request, response_t* response);
err_t http_sse(const request_t* request,
               err_t (*callback)(const event_t* event, void* userp),
               void* userp);

#endif  // HTTP_H
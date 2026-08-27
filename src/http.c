#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "log.h"
#include "http.h"

struct sse_context {
  size_t len;
  size_t cap;
  char* buffer;
  void (*callback)(const response_t* event, void* userp);
  void* userp;
};

bool http_init() {
  log(INFO, "Initializing HTTP client");
  CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
  return res == CURLE_OK;
}

void http_quit() {
  log(INFO, "Cleaning up HTTP client");
  curl_global_cleanup();
}

static size_t http_write_callback(void* contents, size_t size, size_t nmemb,
                                  void* userp) {
  size_t real_size = size * nmemb;
  response_t* resp = (response_t*)userp;

  unsigned char* ptr = realloc(resp->data, resp->data_size + real_size + 1);
  if (!ptr) {
    return 0;  // Out of memory
  }

  resp->data = ptr;
  memcpy(&(resp->data[resp->data_size]), contents, real_size);
  resp->data_size += real_size;
  resp->data[resp->data_size] = '\0';

  return real_size;
}

static response_t* response_new() {
  response_t* resp = malloc(sizeof(response_t));
  if (!resp) return NULL;
  resp->status = 0;
  resp->data_size = 0;
  resp->data = NULL;
  return resp;
}

void response_delete(response_t* response) {
  if (response) {
    if (response->data) free(response->data);
    free(response);
  }
}

response_t* http_post(const request_t* request) {
  log(INFO, "Sending HTTP POST request to %s", request->url);

  CURL* curl = curl_easy_init();
  if (!curl) return NULL;

  response_t* resp = response_new();
  if (!resp) {
    curl_easy_cleanup(curl);
    return NULL;
  }

  struct curl_slist* headers = NULL;
  for (size_t i = 0; i < request->n_header; ++i) {
    headers = curl_slist_append(headers, request->headers[i]);
  }

  curl_easy_setopt(curl, CURLOPT_URL, request->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    curl_easy_cleanup(curl);
    log(ERROR, "curl error: %s", curl_easy_strerror(res));
    response_delete(resp);
    return NULL;
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);

  curl_easy_cleanup(curl);
  return resp;
}

static size_t http_sse_callback(void* contents, size_t size, size_t nmemb,
                                void* userp) {
  size_t real_size = size * nmemb;
  struct sse_context* ctx = (struct sse_context*)userp;
  if (ctx->cap < ctx->len + real_size + 1) {
    size_t new_cap = ctx->len + real_size + 1;
    char* new_buffer = realloc(ctx->buffer, new_cap);
    if (!new_buffer) {
      return 0;  // Out of memory
    }
    ctx->buffer = new_buffer;
    ctx->cap = new_cap;
  }
  memcpy(&(ctx->buffer[ctx->len]), contents, real_size);
  ctx->len += real_size;
  ctx->buffer[ctx->len] = '\0';

  char *line = ctx->buffer, *end = ctx->buffer + ctx->len;
  while (line < end) {
    char* next_line = memchr(line, '\n', end - line);
    if (!next_line) break;
    if (strncmp(line, "data: ", 6) == 0) {
      line += 6;  // Skip "data: "
      size_t data_len = next_line - line;
      line[data_len] = '\0';
      if (strcmp(line, "[DONE]") == 0) {
        line = next_line + 1;
        break;
      }
      response_t event = {
          .status = 200, .data_size = data_len, .data = (void*)line};
      ctx->callback(&event, ctx->userp);
    }
    line = next_line + 1;
  }

  if (line < end) {
    memmove(ctx->buffer, line, end - line);
    ctx->len = end - line;
  } else {
    ctx->len = 0;
  }

  return real_size;
}

bool http_sse(const request_t* request,
              void (*callback)(const response_t* event, void* userp),
              void* userp) {
  log(INFO, "Sending HTTP SSE request to %s", request->url);

  CURL* curl = curl_easy_init();
  if (!curl) return false;

  struct curl_slist* headers = NULL;
  for (size_t i = 0; i < request->n_header; ++i) {
    headers = curl_slist_append(headers, request->headers[i]);
  }

  struct sse_context ctx = {
      .len = 0,
      .cap = 0,
      .buffer = NULL,
      .callback = callback,
      .userp = userp,
  };

  curl_easy_setopt(curl, CURLOPT_URL, request->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_sse_callback);

  CURLcode res = curl_easy_perform(curl);
  if (ctx.buffer) free(ctx.buffer);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    curl_easy_cleanup(curl);
    log(ERROR, "curl error: %s", curl_easy_strerror(res));
    return false;
  }
  long status;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (status != 200) {
    log(ERROR, "HTTP request failed with status %ld", status);
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_cleanup(curl);
  return true;
}
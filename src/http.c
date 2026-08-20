#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "log.h"
#include "http.h"

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
  size_t realsize = size * nmemb;
  response_t* resp = (response_t*)userp;

  unsigned char* ptr = realloc(resp->data, resp->data_size + realsize + 1);
  if (!ptr) {
    return 0;  // Out of memory
  }

  resp->data = ptr;
  memcpy(&(resp->data[resp->data_size]), contents, realsize);
  resp->data_size += realsize;
  resp->data[resp->data_size] = '\0';

  return realsize;
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
  if (res != CURLE_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    log(ERROR, "curl error: %s", curl_easy_strerror(res));
    response_delete(resp);
    return NULL;
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);

  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  return resp;
}
#ifndef DEEPSEEK_H
#define DEEPSEEK_H

#include "provider/provider.h"

err_t deepseek_update(void* context, const char* response, size_t len);

const provider_t* get_deepseek_provider();

#endif  // DEEPSEEK_H
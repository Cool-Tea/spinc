#ifndef DEEPSEEK_H
#define DEEPSEEK_H

#include "provider/provider.h"

err_t deepseek_create_context(void** context);
err_t deepseek_deserialize(const char* data, size_t len, void** context);

const provider_t* get_deepseek_provider();

#endif  // DEEPSEEK_H
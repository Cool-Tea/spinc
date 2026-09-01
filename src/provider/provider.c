#include "provider/provider.h"
#include "provider/openai.h"
#include "provider/anthropic.h"

typedef const provider_t* (*profact_t)();

static profact_t provider_factories[] = {
    [OPENAI_COMPATIBLE] = get_openai_compatible_provider,
    [ANTHROPIC_COMPATIBLE] = get_anthropic_compatible_provider,
};

const provider_t* provider_get(protyp_t type) {
  return provider_factories[type]();
}
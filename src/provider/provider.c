#include "provider/provider.h"
#include "provider/chat_completion.h"
#include "provider/responses.h"
#include "provider/anthropic.h"

typedef const provider_t* (*profact_t)();

static profact_t provider_factories[] = {
    [OPENAI_CHAT_COMPLETION] = get_openai_chat_completion_provider,
    [OPENAI_RESPONSES] = get_openai_responses_provider,
    [ANTHROPIC] = get_anthropic_provider,
};

const provider_t* provider_get(protyp_t type) {
  return provider_factories[type]();
}
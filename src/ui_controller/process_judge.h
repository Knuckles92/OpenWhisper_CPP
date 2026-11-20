#pragma once

#include <memory>

namespace fv {

class AppContext;
namespace llm {
class JudgeProvider;
}

// The process's one llm::JudgeProvider and the llm::JudgeEnvironment under it (routing.py,
// text_judge.py and the Actions page share one judge module per process): the provider already
// in `context` (a test's fake), else one built and provided with its environment. What it is
// built over (its own credential store, HTTP client and .env reader) is owned by the provider,
// so a judge still asked after the app stopped waiting keeps everything it reads alive for as
// long as it holds the provider. GUI thread, while the app is composed.
std::shared_ptr<llm::JudgeProvider> provideProcessJudge(AppContext& context);

} // namespace fv

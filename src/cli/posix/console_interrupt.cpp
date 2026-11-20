#include "cli/detail/console_interrupt.h"

#include <csignal>
#include <pthread.h>

namespace fv::cli::detail {
namespace {

sigset_t interrupt_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

} // namespace

void prepare_interrupt() {
    // Blocked in this thread and every thread it starts afterwards, so sigwait alone takes them.
    const sigset_t signals = interrupt_signals();
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
}

void wait_for_interrupt() {
    const sigset_t signals = interrupt_signals();
    int received = 0;
    sigwait(&signals, &received);
}

} // namespace fv::cli::detail

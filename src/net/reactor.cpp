#include "net/reactor.h"

#include <exception>
#include <string>
#include <utility>

#include "base/log.h"
#include "net/detail/platform.h"
#include "net/detail/reactor_impl.h"

namespace fv::net {

namespace detail {

namespace {

constexpr log::Logger kExecutorLog("base.executor");

void run_logged(const Task& task) {
    try {
        task();
    } catch (const std::exception& error) {
        kExecutorLog.error_with(error, "A task failed on the network reactor");
    } catch (...) {
        kExecutorLog.error("A task failed on the network reactor");
    }
}

} // namespace

void ReactorExecutor::post(Task task) {
    if (reactor_.stopped()) {
        return;
    }
    asio::post(reactor_.io(), [task = std::move(task)] { run_logged(task); });
}

TimerHandle ReactorExecutor::post_after(std::chrono::milliseconds delay, Task task) {
    if (reactor_.stopped()) {
        return {};
    }
    auto timer = std::make_shared<asio::steady_timer>(reactor_.io(), delay);
    // A cancel may come from any thread; the timer itself is touched only on the reactor. Only
    // the wait owns the timer: the handle may outlive the reactor, and a timer must not outlive
    // its io_context. Once the reactor is gone there is nothing left to cancel.
    auto state = std::make_shared<TimerState>(
        [weak_timer = std::weak_ptr(timer), weak = reactor_.weak_from_this()] {
            const auto reactor = weak.lock();
            if (reactor && !reactor->stopped()) {
                asio::post(reactor->io(), [weak_timer] {
                    if (const auto timer = weak_timer.lock()) {
                        timer->cancel();
                    }
                });
            }
        });
    timer->async_wait([timer, state, task = std::move(task)](const boost::system::error_code&) {
        if (state->fire()) {
            run_logged(task);
        }
    });
    return TimerHandle(state);
}

bool ReactorExecutor::is_current() const {
    return reactor_.running_in_this_thread();
}

ReactorImpl::ReactorImpl(int threads) : work_(asio::make_work_guard(io_)), executor_(*this) {
    const int count = threads < 1 ? 1 : threads;
    running_ = count;
    for (int index = 0; index < count; ++index) {
        threads_.emplace_back([this, index] { run(index); });
    }
}

ReactorImpl::~ReactorImpl() {
    stop(std::chrono::milliseconds(0));
}

void ReactorImpl::run(int index) {
    set_thread_name("fv-net-" + std::to_string(index));
    while (true) {
        try {
            io_.run();
            break;
        } catch (const std::exception& error) {
            // A handler let an exception out; the other connections keep running.
            kExecutorLog.error_with(error, "A handler failed on the network reactor");
        } catch (...) {
            kExecutorLog.error("A handler failed on the network reactor");
        }
    }
    {
        std::lock_guard lock(mutex_);
        --running_;
    }
    idle_.notify_all();
}

bool ReactorImpl::running_in_this_thread() {
    return io_.get_executor().running_in_this_thread();
}

void ReactorImpl::stop(std::chrono::milliseconds grace) {
    stopped_ = true;
    work_.reset();
    {
        std::unique_lock lock(mutex_);
        if (joined_) {
            return;
        }
        if (!running_in_this_thread()) {
            idle_.wait_for(lock, grace, [this] { return running_ == 0; });
        }
        joined_ = true;
    }
    io_.stop();
    for (auto& thread : threads_) {
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach(); // stopped from its own handler: it ends when the handler returns
        } else if (thread.joinable()) {
            thread.join();
        }
    }
}

void ReactorImpl::discard_pending() {
    if (!running_in_this_thread()) {
        io_.shutdown();
    }
}

} // namespace detail

Reactor::Reactor(int threads) : impl_(std::make_shared<detail::ReactorImpl>(threads)) {}

Reactor::~Reactor() {
    impl_->stop(std::chrono::milliseconds(0));
    impl_->discard_pending();
}

Executor& Reactor::executor() {
    return impl_->executor();
}

void Reactor::stop(std::chrono::milliseconds grace) {
    impl_->stop(grace);
}

} // namespace fv::net

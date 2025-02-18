#include "eventloop.hh"

#include "util.hh"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#include<iostream>

using namespace std;

unsigned int EventLoop::Rule::service_count() const {
    return direction == Direction::In ? fd.read_count() : fd.write_count();
}

//! \param[in] fd is the FileDescriptor to be polled
//! \param[in] direction indicates whether to poll for reading (Direction::In) or writing (Direction::Out)
//! \param[in] callback is called when `fd` is ready.
//! \param[in] interest is called by EventLoop::wait_next_event. If it returns `true`, `fd` will
//!                     be polled, otherwise `fd` will be ignored only for this execution of `wait_next_event.
//! \param[in] cancel is called when the rule is cancelled (e.g. on hangup, EOF, or closure).
void EventLoop::add_rule(const FileDescriptor &fd,
                         const Direction direction,
                         const CallbackT &callback,
                         const InterestT &interest,
                         const CallbackT &cancel) {
    _rules.push_back({fd.duplicate(), direction, callback, interest, cancel});
}

//! \param[in] timeout_ms is the timeout value passed to [poll(2)](\ref man2::poll); `wait_next_event`
//!                       returns Result::Timeout if no fd is ready after the timeout expires.
//! \returns Eventloop::Result indicating success, timeout, or no more Rule objects to poll.
//!
//! For each Rule, this function first calls Rule::interest; if `true`, Rule::fd is added to the
//! list of file descriptors to be polled for readability (if Rule::direction == Direction::In) or
//! writability (if Rule::direction == Direction::Out) unless Rule::fd has reached EOF, in which case
//! the Rule is canceled (i.e., deleted from EventLoop::_rules).
//!
//! Next, this function calls [poll(2)](\ref man2::poll) with timeout value `timeout_ms`.
//!
//! Then, for each ready file descriptor, this function calls Rule::callback. If fd reaches EOF or
//! if the Rule was registered using EventLoop::add_cancelable_rule and Rule::callback returns true,
//! this Rule is canceled.
//!
//! If an error occurs during polling, this function throws a std::runtime_error.
//!
//! If a [signal(7)](\ref man7::signal) was caught during polling or if EventLoop::_rules becomes empty,
//! this function returns Result::Exit.
//!
//! If a timeout occurred while polling (i.e., no fd became ready), this function returns Result::Timeout.
//!
//! Otherwise, this function returns Result::Success.
//!
//! \b IMPORTANT: every call to Rule::callback must read from or write to Rule::fd, or the `interest`
//! callback must stop returning true after the callback completes.
//! If none of these conditions occur, EventLoop::wait_next_event will throw std::runtime_error. This is
//! because [poll(2)](\ref man2::poll) is level triggered, so failing to act on a ready file descriptor
//! will result in a busy loop (poll returns on a ready file descriptor; file descriptor is not read or
//! written, so it is still ready; the next call to poll will immediately return).
//  1. make pollfds  2. poll() 监听事件  3. handleEvents
EventLoop::Result EventLoop::wait_next_event(const int timeout_ms) {
    vector<pollfd> pollfds{};
    pollfds.reserve(_rules.size());
    bool something_to_poll = false;

    //  根据user传入的_rules , 生成pollfds
    // set up the pollfd for each rule
    for (auto it = _rules.cbegin(); it != _rules.cend();) {  // NOTE: it gets erased or incremented in loop body
        const auto &this_rule = *it;
        //  如果事件为读事件 且 相关fd已经读完
        if (this_rule.direction == Direction::In && this_rule.fd.eof()) {
            // no more reading on this rule, it's reached eof
            this_rule.cancel();
            it = _rules.erase(it);
            continue;
        }

        //  如果相关fd已经关闭
        if (this_rule.fd.closed()) {
            this_rule.cancel();
            it = _rules.erase(it);
            continue;
        }
        //  如果该事件需要被poll
        if (this_rule.interest()) {
            pollfds.push_back({this_rule.fd.fd_num(), static_cast<short>(this_rule.direction), 0});
            something_to_poll = true;
        } else {
            pollfds.push_back({this_rule.fd.fd_num(), 0, 0});  // placeholder --- we still want errors
        }
        ++it;
    }

    //  没有事件可以监听 return Exit
    //  quit if there is nothing left to poll
    if (not something_to_poll) {
        return Result::Exit;
    }

    //  poll(fds); 监听事件. timeout_ms
    // call poll -- wait until one of the fds satisfies one of the rules (writeable/readable)
    try {
        if (0 == SystemCall("poll", ::poll(pollfds.data(), pollfds.size(), timeout_ms))) {
            return Result::Timeout;
        }
    } catch (unix_error const &e) {
        //  发生异常 / 事件不应被监听 return Exit
        if (e.code().value() == EINTR) {
            return Result::Exit;
        }
    }

    // go through the poll results
    //  遍历 , 处理活跃事件
    for (auto [it, idx] = make_pair(_rules.begin(), size_t(0)); it != _rules.end(); ++idx) {
        const auto &this_pollfd = pollfds[idx];

        const auto poll_error = static_cast<bool>(this_pollfd.revents & (POLLERR | POLLNVAL));
        if (poll_error) {
            throw runtime_error("EventLoop: error on polled file descriptor");
        }

        //  移除死掉的fd的上事件
        const auto &this_rule = *it;
        //  fd上是否发生关注事件
        const auto poll_ready = static_cast<bool>(this_pollfd.revents & this_pollfd.events);
        //  POLLHUP
        //       Hang  up  (only  returned  in  revents; ignored in events).  Note that when reading from a channel such as a pipe or a stream socket, this
        //       event merely indicates that the peer closed its end of the channel.  Subsequent reads from the channel will return 0 (end  of  file)  only
        //       after all outstanding data in the channel has been consumed.
        const auto poll_hup = static_cast<bool>(this_pollfd.revents & POLLHUP);
        if (poll_hup && this_pollfd.events && !poll_ready) {
            // if we asked for the status, and the _only_ condition was a hangup, this FD is defunct:
            //   - if it was POLLIN and nothing is readable, no more will ever be readable
            //   - if it was POLLOUT, it will not be writable again
            this_rule.cancel();
            it = _rules.erase(it);
            continue;
        }

        //  调用回调函数处理事件
        if (poll_ready) {
            // we only want to call callback if revents includes the event we asked for
            const auto count_before = this_rule.service_count();
            this_rule.callback();
            //  如果处理该活跃事件失败，那么立刻退出eventloop. 因为该poll是水平触发. 如果活跃事件处理失败的话会陷入死循环
            // only check for busy wait if we're not canceling or exiting
            if (count_before == this_rule.service_count() and this_rule.interest()) {
                throw runtime_error(
                    "EventLoop: busy wait detected: callback did not read/write fd and is still interested");
            }
        }

        ++it;  // if we got here, it means we didn't call _rules.erase()
    }

    //  监听并处理成功
    return Result::Success;
}

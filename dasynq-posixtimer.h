#include <vector>
#include <utility>

#include <sys/time.h>
#include <time.h>
#include <signal.h>

#include "dasynq-config.h"
#include "dasynq-timerbase.h"

namespace dasynq {

// Timer implementation based on POSIX create_timer et al.
// May require linking with -lrt

template <class Base> class PosixTimerEvents : public timer_base<Base>
{
    private:
    timer_queue_t real_timer_queue;
    timer_queue_t mono_timer_queue;

    timer_t real_timer;
    timer_t mono_timer;

    // Set the timeout to match the first timer in the queue (disable the timer if there are no
    // active timers).
    void set_timer_from_queue(timer_t &timer, timer_queue_t &timer_queue)
    {
        struct itimerspec newalarm;

        if (timer_queue.empty()) {
            newalarm.it_value = {0, 0};
            newalarm.it_interval = {0, 0};
            timer_settime(timer, TIMER_ABSTIME, &newalarm, nullptr);
            return;
        }

        newalarm.it_interval = {0, 0};
        newalarm.it_value = timer_queue.get_root_priority();
        timer_settime(timer, TIMER_ABSTIME, &newalarm, nullptr);
    }

    protected:

    using SigInfo = typename Base::SigInfo;

    template <typename T>
    bool receiveSignal(T & loop_mech, SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGALRM) {
            struct timespec curtime;

            if (! real_timer_queue.empty()) {
                clock_gettime(CLOCK_REALTIME, &curtime);
                timer_base<Base>::process_timer_queue(real_timer_queue, curtime);
                set_timer_from_queue(real_timer, real_timer_queue);
            }

            if (! mono_timer_queue.empty()) {
                clock_gettime(CLOCK_MONOTONIC, &curtime);
                timer_base<Base>::process_timer_queue(mono_timer_queue, curtime);
                set_timer_from_queue(mono_timer, mono_timer_queue);
            }

            // loop_mech.rearmSignalWatch_nolock(SIGALRM);
            return false; // don't disable signal watch
        }
        else {
            return Base::receiveSignal(loop_mech, siginfo, userdata);
        }
    }

    timer_queue_t &queue_for_clock(clock_type clock)
    {
        switch (clock) {
        case clock_type::MONOTONIC:
            return mono_timer_queue;
        case clock_type::SYSTEM:
            return real_timer_queue;
        default:
            DASYNQ_UNREACHABLE;
        }
    }

    timer_t &timer_for_clock(clock_type clock)
    {
        switch (clock) {
        case clock_type::MONOTONIC:
            return mono_timer;
        case clock_type::SYSTEM:
            return real_timer;
        default:
            DASYNQ_UNREACHABLE;
        }
    }

    public:

    template <typename T> void init(T *loop_mech)
    {
        sigset_t sigmask;
        sigprocmask(SIG_UNBLOCK, nullptr, &sigmask);
        sigaddset(&sigmask, SIGALRM);
        sigprocmask(SIG_SETMASK, &sigmask, nullptr);
        loop_mech->addSignalWatch(SIGALRM, nullptr);

        struct sigevent timer_sigevent;
        timer_sigevent.sigev_notify = SIGEV_SIGNAL;
        timer_sigevent.sigev_signo = SIGALRM;
        timer_sigevent.sigev_value.sival_int = 0;

        // DAV error check these:
        timer_create(CLOCK_REALTIME, &timer_sigevent, &real_timer);
        timer_create(CLOCK_MONOTONIC, &timer_sigevent, &mono_timer);

        Base::init(loop_mech);
    }

    void addTimer(timer_handle_t &h, void *userdata, clock_type clock = clock_type::MONOTONIC)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        queue_for_clock(clock).allocate(h, userdata);
    }

    void removeTimer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        removeTimer_nolock(timer_id, clock);
    }

    void removeTimer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timer_queue_t &timer_queue = queue_for_clock(clock);

        if (timer_queue.is_queued(timer_id)) {
            timer_queue.remove(timer_id);
        }
        timer_queue.deallocate(timer_id);
    }

    // starts (if not started) a timer to timeout at the given time. Resets the expiry count to 0.
    //   enable: specifies whether to enable reporting of timeouts/intervals
    void setTimer(timer_handle_t &timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);

        timer_queue_t &timer_queue = queue_for_clock(clock);
        timer_t &timer = timer_for_clock(clock);

        auto &ts = timer_queue.node_data(timer_id);
        ts.interval_time = interval;
        ts.expiry_count = 0;
        ts.enabled = enable;

        if (timer_queue.is_queued(timer_id)) {
            // Already queued; alter timeout
            if (timer_queue.set_priority(timer_id, timeout)) {
                set_timer_from_queue(timer, timer_queue);
            }
        }
        else {
            if (timer_queue.insert(timer_id, timeout)) {
                set_timer_from_queue(timer, timer_queue);
            }
        }
    }

    // Set timer relative to current time:
    void setTimerRel(timer_handle_t &timer_id, struct timespec &timeout, struct timespec &interval,
            bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        // TODO consider caching current time somehow; need to decide then when to update cached value.
        struct timespec curtime;
        int posix_clock_id = (clock == clock_type::MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        clock_gettime(posix_clock_id, &curtime);
        curtime.tv_sec += timeout.tv_sec;
        curtime.tv_nsec += timeout.tv_nsec;
        if (curtime.tv_nsec > 1000000000) {
            curtime.tv_nsec -= 1000000000;
            curtime.tv_sec++;
        }
        setTimer(timer_id, curtime, interval, enable, clock);
    }

    // Enables or disabling report of timeouts (does not stop timer)
    void enableTimer(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        enableTimer_nolock(timer_id, enable, clock);
    }

    void enableTimer_nolock(timer_handle_t &timer_id, bool enable, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timer_queue_t &timer_queue = queue_for_clock(clock);

        auto &node_data = timer_queue.node_data(timer_id);
        auto expiry_count = node_data.expiry_count;
        if (expiry_count != 0) {
            node_data.expiry_count = 0;
            Base::receiveTimerExpiry(timer_id, node_data.userdata, expiry_count);
        }
        else {
            timer_queue.node_data(timer_id).enabled = enable;
        }
    }

    void stop_timer(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        stop_timer_nolock(timer_id, clock);
    }

    void stop_timer_nolock(timer_handle_t &timer_id, clock_type clock = clock_type::MONOTONIC) noexcept
    {
        timer_queue_t &timer_queue = queue_for_clock(clock);
        timer_t &timer = timer_for_clock(clock);

        if (timer_queue.is_queued(timer_id)) {
            bool was_first = (&timer_queue.get_root()) == &timer_id;
            timer_queue.remove(timer_id);
            if (was_first) {
                set_timer_from_queue(timer, timer_queue);
            }
        }
    }

    ~PosixTimerEvents()
    {
        timer_delete(mono_timer);
        timer_delete(real_timer);
    }
};

}
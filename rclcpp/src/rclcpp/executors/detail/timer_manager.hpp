#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <map>

#include <rcl/timer.h>
#include <rclcpp/timer.hpp>

#include <inttypes.h>

namespace rclcpp::executors
{

class TimerQueue
{
    struct TimerData
    {
        std::shared_ptr<const rcl_timer_t> rcl_ref;
//         rclcpp::TimerBase::WeakPtr timer_weak_ptr;
        std::function<void()> timer_ready_callback;
    };

public:
    TimerQueue(rcl_clock_type_t timer_type) :
        timer_type(timer_type),
        used_clock_for_timers(timer_type),
        trigger_thread([this] () {
            timer_thread();
        })
    {
    };

    ~TimerQueue()
    {
        running = false;

        while(!thread_terminated.load())
        {
            thread_conditional.notify_one();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        trigger_thread.join();
    }

    void remove_timer (const rclcpp::TimerBase::SharedPtr &timer)
    {
        timer->clear_on_reset_callback();

        std::scoped_lock l(mutex);

        auto it = std::find_if(all_timers.begin(), all_timers.end(), [rcl_ref = timer->get_timer_handle()] (const std::unique_ptr<TimerData> &d)
        {
            return d->rcl_ref == rcl_ref;
        });

        if(it != all_timers.end())
        {
            const TimerData *data_ptr = it->get();


            auto it2 = std::find_if(running_timers.begin(), running_timers.end(), [data_ptr] (const auto &e) {
                return e.second == data_ptr;
            });

            running_timers.erase(it2);
            all_timers.erase(it);
        }

        thread_conditional.notify_all();
    }

    void add_timer ( const rclcpp::TimerBase::SharedPtr &timer, const std::function<void()> &timer_ready_callback)
    {
        rcl_clock_t *clock_type_of_timer;

        std::shared_ptr<const rcl_timer_t> handle = timer->get_timer_handle();

        if(rcl_timer_clock(const_cast<rcl_timer_t *>(handle.get()), &clock_type_of_timer) != RCL_RET_OK)
        {
            assert(false);
        };

        if(clock_type_of_timer->type != timer_type)
        {
            // timer is handled by another queue
            return;
        }

        RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::add_timer matching timer");


        std::unique_ptr<TimerData> data = std::make_unique<TimerData>();
        data->timer_ready_callback = std::move(timer_ready_callback);
//         data->timer_weak_ptr = timer;
        data->rcl_ref = std::move(handle);

        timer->set_on_reset_callback ( [data_ptr = data.get(), this] ( size_t ) {
            RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::timer reset lambda");
            add_timer_to_running_map(data_ptr);
        } );

        {
            std::scoped_lock l(mutex);
            add_timer_to_running_map(data.get());

            all_timers.emplace_back ( std::move(data) );
        }

        RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::waking clock");

        //wake up thread as new timer was added
        thread_conditional.notify_all();
    };

    void add_timer_to_running_map(const TimerData *timer_data)
    {
        if(timer_data->rcl_ref.unique())
        {
            // timer was deleted
            auto it = std::find_if(all_timers.begin(), all_timers.end(), [timer_data] (const std::unique_ptr<TimerData> &e) {
                return timer_data == e.get();
            }
            );

            if(it != all_timers.end())
            {
                all_timers.erase(it);
            }
            return;
        }

        rcl_ret_t ret = rcl_timer_call(const_cast<rcl_timer_t *>(timer_data->rcl_ref.get()));
        if(ret == RCL_RET_TIMER_CANCELED)
        {
            return;
        }

        int64_t next_call_time;

        ret = rcl_timer_get_next_call_time(timer_data->rcl_ref.get(), &next_call_time);

        if(ret == RCL_RET_OK)
        {
            running_timers.emplace(next_call_time, timer_data);
        }
    }

    /**
     * Returns the time when the next timer becomes ready
     */
    std::chrono::nanoseconds get_next_timer_ready_time() const
    {
        if(running_timers.empty())
        {
            // can't use std::chrono::nanoseconds::max, as wait_for
            // internally computes end time by using ::now() + timeout
            // as a workaround, we use some absurd high timeout
            return std::chrono::hours(10000);
        }
        return running_timers.begin()->first;
    }

    void call_ready_timer_callbacks()
    {
        auto readd_timer_to_running_map = [this] (TimerMap::node_type &&e)
        {
            const auto &timer_data = e.mapped();
            if(timer_data->rcl_ref.unique())
            {
                // timer was deleted
                auto it = std::find_if(all_timers.begin(), all_timers.end(), [timer_data] (const std::unique_ptr<TimerData> &e) {
                    return timer_data == e.get();
                }
                );

                if(it != all_timers.end())
                {
                    all_timers.erase(it);
                }
                return;
            }

            int64_t next_call_time;

            auto ret = rcl_timer_get_next_call_time(timer_data->rcl_ref.get(), &next_call_time);

            if(ret == RCL_RET_OK)
            {
                e.key() = std::chrono::nanoseconds(next_call_time);
                running_timers.insert(std::move(e));
            }
        };

        while(!running_timers.empty())
        {
            int64_t time_until_call;

            const rcl_timer_t *rcl_timer_ref = running_timers.begin()->second->rcl_ref.get();
            auto ret = rcl_timer_get_time_until_next_call(rcl_timer_ref, &time_until_call);
            if(ret == RCL_RET_TIMER_CANCELED)
            {
                running_timers.erase(running_timers.begin());
                continue;
            }

//             RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::time until call is  %+" PRId64 , time_until_call);

            if(time_until_call <= 0)
            {
                // advance next call time;
                rcl_ret_t ret = rcl_timer_call(const_cast<rcl_timer_t *>(rcl_timer_ref));
                if(ret == RCL_RET_TIMER_CANCELED)
                {
                    running_timers.erase(running_timers.begin());
                    continue;
                }

                running_timers.begin()->second->timer_ready_callback();
                readd_timer_to_running_map(running_timers.extract(running_timers.begin()));
                continue;
            }
            break;
        }
    }

    void timer_thread()
    {
        RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::timer_thread starting");
        while(running && rclcpp::ok ())
        {
            std::chrono::nanoseconds next_wakeup_time;
            {
                std::scoped_lock l(mutex);

                call_ready_timer_callbacks();

                next_wakeup_time = get_next_timer_ready_time();
            }
            try {
//             RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::timer_thread before sleep, next wakeup time %+" PRId64 , next_wakeup_time.count());
                used_clock_for_timers.sleep_until(rclcpp::Time(next_wakeup_time.count(), timer_type), thread_conditional, false);
            }
            catch(const std::runtime_error &)
            {
                //there is a race on shutdown, were the context may become invalid, while we call sleep_until
                running = false;
            }
//             RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::timer_thread after sleep");
/*
            if(!rclcpp::contexts::get_global_default_context()->shutdown_reason().empty())
            {
                break;
            }*/

        }
        thread_terminated = true;
        RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerQueue::timer_thread terminating");

    };

    void stop()
    {
        running = false;
        while(!thread_terminated.load())
        {
            thread_conditional.notify_one();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    rcl_clock_type_t timer_type;

    Context::SharedPtr clock_sleep_context;

    rclcpp::Clock used_clock_for_timers;

    std::mutex mutex;

    std::atomic_bool running = true;
    std::atomic_bool thread_terminated = false;

    std::vector<std::unique_ptr<TimerData>> all_timers;

    using TimerMap = std::multimap<std::chrono::nanoseconds, const TimerData *>;
    TimerMap running_timers;

    std::thread trigger_thread;

    std::condition_variable thread_conditional;
};

class TimerManager
{
    std::array<TimerQueue, 3> timer_queues;
public:

    TimerManager() :
        timer_queues{RCL_ROS_TIME, RCL_SYSTEM_TIME, RCL_STEADY_TIME}
    {

    };

    void remove_timer (const rclcpp::TimerBase::SharedPtr &timer)
    {
        for(TimerQueue &q : timer_queues)
        {
            q.remove_timer(timer);
        }
    }

    void add_timer ( const rclcpp::TimerBase::SharedPtr &timer, const std::function<void()> &timer_ready_callback)
    {
        RCUTILS_LOG_ERROR_NAMED("rclcpp", "TimerManager::add_timer");

        for(TimerQueue &q : timer_queues)
        {
            q.add_timer(timer, timer_ready_callback);
        }
    };

    void stop()
    {
        for(TimerQueue &q : timer_queues)
        {
            q.stop();
        }
    }
};
}

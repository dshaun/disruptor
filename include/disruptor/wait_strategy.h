#ifndef DISRUPTOR2_WAIT_STRATEGY_H_
#define DISRUPTOR2_WAIT_STRATEGY_H_

#include <sys/time.h>

#include <vector>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread_time.hpp>

#include <disruptor/exceptions.h>
#include <disruptor/interface.h>

namespace disruptor {

// Strategy options which are available to those waiting on a
// {@link RingBuffer}
enum WaitStrategyOption {
    // This strategy uses a condition variable inside a lock to block the
    // event procesor which saves CPU resource at the expense of lock
    // contention.
    kBlockingStrategy,
    // This strategy uses a progressive back off strategy by first spinning,
    // then yielding, then sleeping for 1ms period. This is a good strategy
    // for burst traffic then quiet periods when latency is not critical.
    kSleepingStrategy,
    // This strategy calls Thread.yield() in a loop as a waiting strategy
    // which reduces contention at the expense of CPU resource.
    kYieldingStrategy,
    // This strategy call spins in a loop as a waiting strategy which is
    // lowest and most consistent latency but ties up a CPU.
    kBusySpinStrategy
};

// Blocking strategy that uses a lock and condition variable for
// {@link Consumer}s waiting on a barrier.
// This strategy should be used when performance and low-latency are not as
// important as CPU resource.
class BlockingStrategy : public IWaitStrategy
{
public:
    BlockingStrategy() {}

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier)
    {
        int64_t available_sequence = 0;
        // We need to wait.
        if ((available_sequence = cursor.get()) < sequence) {
            // acquire lock
            boost::unique_lock<boost::recursive_mutex> ulock(mutex_);
            while ((available_sequence = cursor.get()) < sequence) {
                barrier.checkAlert();
                consumer_notify_condition_.wait(ulock);
            }
        } // unlock happens here, on ulock destruction.

        if (0 != dependents.size()) {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                barrier.checkAlert();
            }
        }

        return available_sequence;
    }

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier,
                            const boost::posix_time::time_duration& timeout)
    {
        int64_t available_sequence = 0;
        // We have to wait
        if ((available_sequence = cursor.get()) < sequence) {
            boost::unique_lock<boost::recursive_mutex> ulock(mutex_);
            while ((available_sequence = cursor.get()) < sequence) {
                barrier.checkAlert();
                if (!consumer_notify_condition_.timed_wait(ulock, timeout))
                    break;

            }
        } // unlock happens here, on ulock destruction

        if (0 != dependents.size()) {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                barrier.checkAlert();
            }
        }

        return available_sequence;
    }

    virtual void signalAllWhenBlocking()
    {
        boost::unique_lock<boost::recursive_mutex> ulock(mutex_);
        consumer_notify_condition_.notify_all();
    }

private:
    boost::recursive_mutex mutex_;
    boost::condition_variable_any consumer_notify_condition_;
};

// Sleeping strategy
class SleepingStrategy : public IWaitStrategy
{
public:
    SleepingStrategy(const boost::posix_time::time_duration& sleep_time)
        : sleep_time_(sleep_time)
    {
    }

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier)
    {
        int64_t available_sequence = 0;
        int counter = retries;

        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                counter = applyWaitMethod(barrier, counter);
            }
        }
        else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                counter = applyWaitMethod(barrier, counter);
            }
        }

        return available_sequence;
    }

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier,
                            const boost::posix_time::time_duration& timeout)
    {
        int64_t timeout_micros = timeout.total_microseconds();
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);
        int64_t start_micro = (int64_t)start_time.tv_sec*1000*1000 + start_time.tv_usec;

        int64_t available_sequence = 0;
        int counter = retries;

        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                counter = applyWaitMethod(barrier, counter);
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }
        else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                counter = applyWaitMethod(barrier, counter);
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }

        return available_sequence;
    }

    virtual void signalAllWhenBlocking() {}

    static const int retries = 10;

private:

    int applyWaitMethod(const ISequenceBarrier& barrier, int counter)
    {
        barrier.checkAlert();
        if (counter > 0) {
            counter--;
        }
        else {
            // NOTE: boost::this_thread::sleep uses nanosleep, which has normal
            // resolution and can not sleep on microsecond precision,
            // to sleep more accurately, consider changing to clock_nanosleep,
            // however, I see no difference on redhat 6 with tsc clock source,
            // so I'll keep it like this for now.
            boost::this_thread::sleep(sleep_time_);
        }

        return counter;
    }

    boost::posix_time::time_duration sleep_time_;
};

// Yielding strategy that uses a sleep(0) for {@link EventProcessor}s waiting
// on a barrier. This strategy is a good compromise between performance and
// CPU resource.
class YieldingStrategy : public IWaitStrategy
{
public:
    YieldingStrategy() {}

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier)
    {
        int64_t available_sequence = 0;
        int counter = retries;

        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                counter = applyWaitMethod(barrier, counter);
            }
        }
        else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                counter = applyWaitMethod(barrier, counter);
            }
        }

        return available_sequence;
    }

    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier,
                            const boost::posix_time::time_duration& timeout)
    {
        int64_t timeout_micros = timeout.total_microseconds();
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);
        int64_t start_micro = (int64_t)start_time.tv_sec*1000*1000 + start_time.tv_usec;

        int64_t available_sequence = 0;
        int counter = retries;

        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                counter = applyWaitMethod(barrier, counter);
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }
        else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                counter = applyWaitMethod(barrier, counter);
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }

        return available_sequence;
    }

    virtual void signalAllWhenBlocking() {}

    static const int retries = 10;

private:
    int applyWaitMethod(const ISequenceBarrier& barrier, int counter)
    {
        barrier.checkAlert();
        if (counter == 0) {
            boost::this_thread::yield();
        }
        else {
            counter--;
        }

        return counter;
    }
};

// Busy Spin strategy that uses a busy spin loop for {@link EventProcessor}s
// waiting on a barrier.
// This strategy will use CPU resource to avoid syscalls which can introduce
// latency jitter.  It is best used when threads can be bound to specific
// CPU cores.
class BusySpinStrategy : public IWaitStrategy
{
public:
    BusySpinStrategy() {}

    virtual int64_t waitFor(const int64_t& sequence,
            const Sequence& cursor,
            const DependentSequences& dependents,
            const ISequenceBarrier& barrier)
    {
        int64_t available_sequence = 0;
        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                barrier.checkAlert();
            }
        } else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                barrier.checkAlert();
            }
        }

        return available_sequence;
    }


    virtual int64_t waitFor(const int64_t& sequence,
                            const Sequence& cursor,
                            const DependentSequences& dependents,
                            const ISequenceBarrier& barrier,
                            const boost::posix_time::time_duration& timeout)
    {
        int64_t timeout_micros = timeout.total_microseconds();
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);
        int64_t start_micro = (int64_t)start_time.tv_sec*1000*1000 + start_time.tv_usec;
        int64_t available_sequence = 0;

        if (0 == dependents.size()) {
            while ((available_sequence = cursor.get()) < sequence) {
                barrier.checkAlert();
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }
        else {
            while ((available_sequence = GetMinimumSequence(dependents)) < sequence) {
                barrier.checkAlert();
                gettimeofday(&end_time, NULL);
                int64_t end_micro = (int64_t)end_time.tv_sec*1000*1000 + end_time.tv_usec;
                if (timeout_micros < (end_micro - start_micro))
                    break;
            }
        }

        return available_sequence;
    }


    virtual void signalAllWhenBlocking() {}
};


inline WaitStrategyPtr CreateWaitStrategy(WaitStrategyOption wait_option,
                                          const TimeConfig& timeConfig)
{
    switch (wait_option) {
        case kBlockingStrategy:
            return boost::make_shared<BlockingStrategy>();
        case kSleepingStrategy:
            return boost::make_shared<SleepingStrategy>(
                    GetTimeConfig(timeConfig, kSleep,
                        boost::posix_time::milliseconds(1)));
        case kYieldingStrategy:
            return boost::make_shared<YieldingStrategy>();
        case kBusySpinStrategy:
            return boost::make_shared<BusySpinStrategy>();
        default:
            return WaitStrategyPtr();
    }
}


};  // namespace disruptor

#endif
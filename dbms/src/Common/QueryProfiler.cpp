#include "QueryProfiler.h"

#include <random>
#include <pcg_random.hpp>
#include <common/Pipe.h>
#include <common/phdr_cache.h>
#include <common/config_common.h>
#include <common/StackTrace.h>
#include <common/StringRef.h>
#include <common/logger_useful.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/randomSeed.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteBufferFromFileDescriptor.h>


namespace ProfileEvents
{
    extern const Event QueryProfilerCannotWriteTrace;
    extern const Event QueryProfilerSignalOverruns;
}

namespace DB
{

extern LazyPipe trace_pipe;

namespace
{
    /** Write to file descriptor but drop the data if write would block or fail.
      * To use within signal handler. Motivating example: a signal handler invoked during execution of malloc
      *  should not block because some mutex (or even worse - a spinlock) may be held.
      */
    class WriteBufferDiscardOnFailure : public WriteBufferFromFileDescriptor
    {
    protected:
        void nextImpl() override
        {
            size_t bytes_written = 0;
            while (bytes_written != offset())
            {
                ssize_t res = ::write(fd, working_buffer.begin() + bytes_written, offset() - bytes_written);

                if ((-1 == res || 0 == res) && errno != EINTR)
                {
                    ProfileEvents::increment(ProfileEvents::QueryProfilerCannotWriteTrace);
                    break;  /// Discard
                }

                if (res > 0)
                    bytes_written += res;
            }
        }

    public:
        using WriteBufferFromFileDescriptor::WriteBufferFromFileDescriptor;
        ~WriteBufferDiscardOnFailure() override {}
    };

    /// Normally query_id is a UUID (string with a fixed length) but user can provide custom query_id.
    /// Thus upper bound on query_id length should be introduced to avoid buffer overflow in signal handler.
    constexpr size_t QUERY_ID_MAX_LEN = 1024;

    thread_local size_t write_trace_iteration = 0;
    thread_local pcg64 rng{randomSeed()};

    void writeTraceInfo(TimerType timer_type, int /* sig */, siginfo_t * info, void * context)
    {
        /// Quickly drop if signal handler is called too frequently.
        /// Otherwise we may end up infinitelly processing signals instead of doing any useful work.
        ++write_trace_iteration;
        if (info && info->si_overrun > 0)
        {
            /// But pass with some frequency to avoid drop of all traces.
            if (write_trace_iteration % info->si_overrun == 0)
            {
                ProfileEvents::increment(ProfileEvents::QueryProfilerSignalOverruns, info->si_overrun);
            }
            else
            {
                ProfileEvents::increment(ProfileEvents::QueryProfilerSignalOverruns, info->si_overrun + 1);
                return;
            }
        }

        constexpr size_t buf_size = sizeof(char) + // TraceCollector stop flag
                                    8 * sizeof(char) + // maximum VarUInt length for string size
                                    QUERY_ID_MAX_LEN * sizeof(char) + // maximum query_id length
                                    sizeof(StackTrace) + // collected stack trace
                                    sizeof(TimerType) + // timer type
                                    sizeof(UInt32); // thread_number
        char buffer[buf_size];
        WriteBufferDiscardOnFailure out(trace_pipe.fds_rw[1], buf_size, buffer);

        StringRef query_id = CurrentThread::getQueryId();
        query_id.size = std::min(query_id.size, QUERY_ID_MAX_LEN);

        UInt32 thread_number = CurrentThread::get().thread_number;

        const auto signal_context = *reinterpret_cast<ucontext_t *>(context);
        const StackTrace stack_trace(signal_context);

        writeChar(false, out);
        writeStringBinary(query_id, out);
        writePODBinary(stack_trace, out);
        writePODBinary(timer_type, out);
        writePODBinary(thread_number, out);
        out.next();
    }

    const UInt32 TIMER_PRECISION = 1e9;
}

namespace ErrorCodes
{
    extern const int CANNOT_MANIPULATE_SIGSET;
    extern const int CANNOT_SET_SIGNAL_HANDLER;
    extern const int CANNOT_CREATE_TIMER;
    extern const int CANNOT_SET_TIMER_PERIOD;
    extern const int CANNOT_DELETE_TIMER;
}

template <typename ProfilerImpl>
QueryProfilerBase<ProfilerImpl>::QueryProfilerBase(const Int32 thread_id, const int clock_type, UInt32 period, const int pause_signal)
    : log(&Logger::get("QueryProfiler"))
    , pause_signal(pause_signal)
{
#if USE_INTERNAL_UNWIND_LIBRARY
    /// Sanity check.
    if (!hasPHDRCache())
        throw Exception("QueryProfiler cannot be used without PHDR cache, that is not available for TSan build", ErrorCodes::NOT_IMPLEMENTED);

    /// Too high frequency can introduce infinite busy loop of signal handlers. We will limit maximum frequency (with 1000 signals per second).
    if (period < 1000000)
        period = 1000000;

    struct sigaction sa{};
    sa.sa_sigaction = ProfilerImpl::signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigemptyset(&sa.sa_mask))
        throwFromErrno("Failed to clean signal mask for query profiler", ErrorCodes::CANNOT_MANIPULATE_SIGSET);

    if (sigaddset(&sa.sa_mask, pause_signal))
        throwFromErrno("Failed to add signal to mask for query profiler", ErrorCodes::CANNOT_MANIPULATE_SIGSET);

    if (sigaction(pause_signal, &sa, previous_handler))
        throwFromErrno("Failed to setup signal handler for query profiler", ErrorCodes::CANNOT_SET_SIGNAL_HANDLER);

    try
    {
        struct sigevent sev;
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = pause_signal;
        sev._sigev_un._tid = thread_id;
        if (timer_create(clock_type, &sev, &timer_id))
            throwFromErrno("Failed to create thread timer", ErrorCodes::CANNOT_CREATE_TIMER);

        /// Randomize offset as uniform random value from 0 to period - 1.
        /// It will allow to sample short queries even if timer period is large.
        /// (For example, with period of 1 second, query with 50 ms duration will be sampled with 1 / 20 probability).
        /// It also helps to avoid interference (moire).
        UInt32 period_rand = std::uniform_int_distribution<UInt32>(0, period)(rng);

        struct timespec interval{.tv_sec = period / TIMER_PRECISION, .tv_nsec = period % TIMER_PRECISION};
        struct timespec offset{.tv_sec = period_rand / TIMER_PRECISION, .tv_nsec = period_rand % TIMER_PRECISION};

        struct itimerspec timer_spec = {.it_interval = interval, .it_value = offset};
        if (timer_settime(timer_id, 0, &timer_spec, nullptr))
            throwFromErrno("Failed to set thread timer period", ErrorCodes::CANNOT_SET_TIMER_PERIOD);
    }
    catch (...)
    {
        tryCleanup();
        throw;
    }
#else
    UNUSED(thread_id, clock_type, period, pause_signal);
    throw Exception("QueryProfiler cannot work with stock libunwind", ErrorCodes::NOT_IMPLEMENTED);
#endif
}

template <typename ProfilerImpl>
QueryProfilerBase<ProfilerImpl>::~QueryProfilerBase()
{
    tryCleanup();
}

template <typename ProfilerImpl>
void QueryProfilerBase<ProfilerImpl>::tryCleanup()
{
    if (timer_id != nullptr && timer_delete(timer_id))
        LOG_ERROR(log, "Failed to delete query profiler timer " + errnoToString(ErrorCodes::CANNOT_DELETE_TIMER));

    if (previous_handler != nullptr && sigaction(pause_signal, previous_handler, nullptr))
        LOG_ERROR(log, "Failed to restore signal handler after query profiler " + errnoToString(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER));
}

template class QueryProfilerBase<QueryProfilerReal>;
template class QueryProfilerBase<QueryProfilerCpu>;

QueryProfilerReal::QueryProfilerReal(const Int32 thread_id, const UInt32 period)
    : QueryProfilerBase(thread_id, CLOCK_REALTIME, period, SIGUSR1)
{}

void QueryProfilerReal::signalHandler(int sig, siginfo_t * info, void * context)
{
    writeTraceInfo(TimerType::Real, sig, info, context);
}

QueryProfilerCpu::QueryProfilerCpu(const Int32 thread_id, const UInt32 period)
    : QueryProfilerBase(thread_id, CLOCK_THREAD_CPUTIME_ID, period, SIGUSR2)
{}

void QueryProfilerCpu::signalHandler(int sig, siginfo_t * info, void * context)
{
    writeTraceInfo(TimerType::Cpu, sig, info, context);
}

}

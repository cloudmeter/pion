// ---------------------------------------------------------------------
// pion:  a Boost C++ framework for building lightweight HTTP interfaces
// ---------------------------------------------------------------------
// Copyright (C) 2007-2014 Splunk Inc.  (https://github.com/splunk/pion)
//
// Distributed under the Boost Software License, Version 1.0.
// See http://www.boost.org/LICENSE_1_0.txt
//

#ifndef __PION_SCHEDULER_HEADER__
#define __PION_SCHEDULER_HEADER__

#include <vector>
#include <boost/assert.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/xtime.hpp>
#include <pion/config.hpp>
#include <pion/logger.hpp>
#include <pion/stdx/cstdint.hpp>
#include <pion/stdx/asio.hpp>
#include <pion/stdx/mutex.hpp>
#include <pion/stdx/condition_variable.hpp>
#include <pion/stdx/thread.hpp>
#include <pion/stdx/functional.hpp>
#include <pion/stdx/memory.hpp>

namespace pion {    // begin namespace pion

///
/// scheduler: combines Boost.ASIO with a managed thread pool for scheduling
/// 
class PION_API scheduler :
    private boost::noncopyable
{
public:

    /// constructs a new scheduler
    scheduler(void)
        : m_logger(PION_GET_LOGGER("pion.scheduler")),
        m_num_threads(DEFAULT_NUM_THREADS), m_active_users(0), m_is_running(false)
    {}
    
    /// virtual destructor
    virtual ~scheduler() {}

    /// Starts the thread scheduler (this is called automatically when necessary)
    virtual void startup(void) {}
    
    /// Stops the thread scheduler (this is called automatically when the program exits)
    virtual void shutdown(void);

    /// the calling thread will sleep until the scheduler has stopped
    void join(void);
    
    /// registers an active user with the thread scheduler.  Shutdown of the
    /// scheduler is deferred until there are no more active users.  This
    /// ensures that any work queued will not reference destructed objects
    void add_active_user(void);

    /// unregisters an active user with the thread scheduler
    void remove_active_user(void);
    
    /// returns true if the scheduler is running
    inline bool is_running(void) const { return m_is_running; }
    
    /// sets the number of threads to be used (these are shared by all servers)
    inline void set_num_threads(const stdx::uint32_t n) { m_num_threads = n; }
    
    /// returns the number of threads currently in use
    inline stdx::uint32_t get_num_threads(void) const { return m_num_threads; }

    /// sets the logger to be used
    inline void set_logger(logger log_ptr) { m_logger = log_ptr; }

    /// returns the logger currently in use
    inline logger get_logger(void) { return m_logger; }
    
    /// returns an async I/O service used to schedule work
    virtual stdx::asio::io_service& get_io_service(void) = 0;
    
    /**
     * schedules work to be performed by one of the pooled threads
     *
     * @param work_func work function to be executed
     */
     virtual void post(stdx::function<void()> work_func) {
        get_io_service().post(work_func);
    }
    
    /**
     * thread function used to keep the io_service running
     *
     * @param my_service IO service used to re-schedule keep_running()
     * @param my_timer deadline timer used to keep the IO service active while running
     */
    void keep_running(stdx::asio::io_service& my_service,
                     stdx::asio::deadline_timer& my_timer);
    
    /**
     * puts the current thread to sleep for a specific period of time
     *
     * @param sleep_sec number of entire seconds to sleep for
     * @param sleep_nsec number of nanoseconds to sleep for (10^-9 in 1 second)
     */
    inline static void sleep(stdx::uint32_t sleep_sec, stdx::uint32_t sleep_nsec) {
#if defined(PION_HAVE_CXX11)
        auto duration = std::chrono::nanoseconds(sleep_nsec);
        duration += std::chrono::seconds(sleep_sec);
        stdx::this_thread::sleep_for(duration);
#else
        boost::system_time wakeup_time(get_wakeup_time(sleep_sec, sleep_nsec));
        boost::thread::sleep(wakeup_time);
#endif
    }

    /**
     * puts the current thread to sleep for a specific period of time, or until
     * a wakeup condition is signaled
     *
     * @param wakeup_condition if signaled, the condition will wakeup the thread early
     * @param wakeup_lock scoped lock protecting the wakeup condition
     * @param sleep_sec number of entire seconds to sleep for
     * @param sleep_nsec number of nanoseconds to sleep for (10^-9 in 1 second)
     */
    template <typename ConditionType, typename LockType>
    inline static void sleep(ConditionType& wakeup_condition, LockType& wakeup_lock,
                             stdx::uint32_t sleep_sec, stdx::uint32_t sleep_nsec)
    {
#if defined(PION_HAVE_CXX11)
        auto duration = std::chrono::nanoseconds(sleep_nsec);
        duration += std::chrono::seconds(sleep_sec);
        wakeup_condition.wait_for(wakeup_lock, duration);
#else
        boost::system_time wakeup_time(get_wakeup_time(sleep_sec, sleep_nsec));
        wakeup_condition.timed_wait(wakeup_lock, wakeup_time);
#endif
    }
    
    
    /// processes work passed to the asio service & handles uncaught exceptions
    void process_service_work(stdx::asio::io_service& service);


protected:

    /**
     * calculates a wakeup time in boost::system_time format
     *
     * @param sleep_sec number of seconds to sleep for
     * @param sleep_nsec number of nanoseconds to sleep for
     *
     * @return boost::system_time time to wake up from sleep
     */
    static boost::system_time get_wakeup_time(stdx::uint32_t sleep_sec,
        stdx::uint32_t sleep_nsec);

    /// stops all services used to schedule work
    virtual void stop_services(void) {}
    
    /// stops all threads used to perform work
    virtual void stop_threads(void) {}

    /// finishes all services used to schedule work
    virtual void finish_services(void) {}

    /// finishes all threads used to perform work
    virtual void finish_threads(void) {}
    
    
    /// default number of worker threads in the thread pool
    static const stdx::uint32_t    DEFAULT_NUM_THREADS;

    /// number of nanoseconds in one full second (10 ^ 9)
    static const stdx::uint32_t    NSEC_IN_SECOND;

    /// number of microseconds in one full second (10 ^ 6)
    static const stdx::uint32_t    MICROSEC_IN_SECOND;
    
    /// number of seconds a timer should wait for to keep the IO services running
    static const stdx::uint32_t    KEEP_RUNNING_TIMER_SECONDS;


    /// mutex to make class thread-safe
    stdx::mutex                    m_mutex;
    
    /// primary logging interface used by this class
    logger                          m_logger;

    /// condition triggered when there are no more active users
    stdx::condition_variable                m_no_more_active_users;

    /// condition triggered when the scheduler has stopped
    stdx::condition_variable                m_scheduler_has_stopped;

    /// total number of worker threads in the pool
    stdx::uint32_t                 m_num_threads;

    /// the scheduler will not shutdown until there are no more active users
    stdx::uint32_t                 m_active_users;

    /// true if the thread scheduler is running
    bool                            m_is_running;
};

    
///
/// multi_thread_scheduler: uses a pool of threads to perform work
/// 
class PION_API multi_thread_scheduler :
    public scheduler
{
public:
    
    /// constructs a new multi_thread_scheduler
    multi_thread_scheduler(void) {}
    
    /// virtual destructor
    virtual ~multi_thread_scheduler() {}

    
protected:
    
    /// stops all threads used to perform work
    virtual void stop_threads(void) {
        if (! m_thread_pool.empty()) {
            PION_LOG_DEBUG(m_logger, "Waiting for threads to shutdown");
            
            // wait until all threads in the pool have stopped
#if !defined(PION_HAVE_CXX11)
            stdx::thread current_thread;
#endif
            for (ThreadPool::iterator i = m_thread_pool.begin();
                 i != m_thread_pool.end(); ++i)
            {
                // make sure we do not call join() for the current thread,
                // since this may yield "undefined behavior"
#if defined(PION_HAVE_CXX11)
                if ((*i)->get_id() != stdx::this_thread::get_id()) (*i)->join();
#else
                if (**i != current_thread) (*i)->join();
#endif
            }
        }
    }
    
    /// finishes all threads used to perform work
    virtual void finish_threads(void) { m_thread_pool.clear(); }

    
    /// typedef for a pool of worker threads
    typedef std::vector<stdx::shared_ptr<stdx::thread> >  ThreadPool;
    
    
    /// pool of threads used to perform work
    ThreadPool              m_thread_pool;
};
    
    
///
/// single_service_scheduler: uses a single IO service to schedule work
/// 
class PION_API single_service_scheduler :
    public multi_thread_scheduler
{
public:
    
    /// constructs a new single_service_scheduler
    single_service_scheduler(void)
        : m_service(), m_timer(m_service)
    {}
    
    /// virtual destructor
    virtual ~single_service_scheduler() { shutdown(); }
    
    /// returns an async I/O service used to schedule work
    virtual stdx::asio::io_service& get_io_service(void) { return m_service; }
    
    /// Starts the thread scheduler (this is called automatically when necessary)
    virtual void startup(void);
        
    
protected:
    
    /// stops all services used to schedule work
    virtual void stop_services(void) { m_service.stop(); }
    
    /// finishes all services used to schedule work
    virtual void finish_services(void) { m_service.reset(); }

    
    /// service used to manage async I/O events
    stdx::asio::io_service         m_service;
    
    /// timer used to periodically check for shutdown
    stdx::asio::deadline_timer     m_timer;
};
    

///
/// one_to_one_scheduler: uses a single IO service for each thread
/// 
class PION_API one_to_one_scheduler :
    public multi_thread_scheduler
{
public:
    
    /// constructs a new one_to_one_scheduler
    one_to_one_scheduler(void)
        : m_service_pool(), m_next_service(0)
    {}
    
    /// virtual destructor
    virtual ~one_to_one_scheduler() { shutdown(); }
    
    /// returns an async I/O service used to schedule work
    virtual stdx::asio::io_service& get_io_service(void) {
        stdx::lock_guard<stdx::mutex> scheduler_lock(m_mutex);
        while (m_service_pool.size() < m_num_threads) {
            stdx::shared_ptr<service_pair_type>  service_ptr(new service_pair_type());
            m_service_pool.push_back(service_ptr);
        }
        if (++m_next_service >= m_num_threads)
            m_next_service = 0;
        BOOST_ASSERT(m_next_service < m_num_threads);
        return m_service_pool[m_next_service]->first;
    }
    
    /**
     * returns an async I/O service used to schedule work (provides direct
     * access to avoid locking when possible)
     *
     * @param n integer number representing the service object
     */
    virtual stdx::asio::io_service& get_io_service(stdx::uint32_t n) {
        BOOST_ASSERT(n < m_num_threads);
        BOOST_ASSERT(n < m_service_pool.size());
        return m_service_pool[n]->first;
    }

    /// Starts the thread scheduler (this is called automatically when necessary)
    virtual void startup(void);
    
    
protected:
    
    /// stops all services used to schedule work
    virtual void stop_services(void) {
        for (service_pool_type::iterator i = m_service_pool.begin(); i != m_service_pool.end(); ++i) {
            (*i)->first.stop();
        }
    }
        
    /// finishes all services used to schedule work
    virtual void finish_services(void) { m_service_pool.clear(); }
    

    /// typedef for a pair object where first is an IO service and second is a deadline timer
    struct service_pair_type {
        service_pair_type(void) : first(), second(first) {}
        stdx::asio::io_service         first;
        stdx::asio::deadline_timer     second;
    };
    
    /// typedef for a pool of IO services
    typedef std::vector<stdx::shared_ptr<service_pair_type> >        service_pool_type;

    
    /// pool of IO services used to schedule work
    service_pool_type   m_service_pool;

    /// the next service to use for scheduling work
    stdx::uint32_t     m_next_service;
};
    
    
}   // end namespace pion

#endif

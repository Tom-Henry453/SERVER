// jOB  holds the data that we wish to pass to the thread so tha i knows about the data which i need to operate on
/*
since we need a quitel gneric job struct we need to put inside a container that can hold any type of truct
so  we package data insodea  closure and call the function that we want to execute my thread inside that clousure
*/
#include<thread>
#include<mutex>
#include<condition_variable>
#include<queue>
#include<functional>
#include<vector>
#include<atomic>
#include<cstdint>
#include<utility>
#define MAX_THREADS 200
/*
std::move_only_function is C++23 and newer libc++/libstdc++ only.
__cpp_lib_move_only_function is the standard feature-test macro: the library
headers define it when they ship the feature, so the preprocessor picks the
best available wrapper. Fallback std::function requires the stored callable to
be COPY-constructible (it must be able to clone its contents) — fine here
because our closures capture only copyables (int, sockaddr_in, reference_wrapper).
It would NOT work for closures capturing unique_ptr etc.
*/
#if defined(__cpp_lib_move_only_function)
using closure_t = std::move_only_function<void()>;
#else
using closure_t = std::function<void()>;
#endif

struct task{
    closure_t closure;
    task() = default;                 // keep default constructible
    template<typename F>       // accept any callable, forward  it into closure var
    task(F&& f) : closure(std::forward<F>(f)) {}
    void operator()() { closure(); } // let thread_pool treat task like a calable
};


// Item must support item() - task wraps an arbitrary closure and calls it;
// LogEntry (Logger.hh) wraps fixed log data and calls add_log directly.
// Same pool for both, no type erasure forced on the plain-data case.
template<typename Item>
concept Callable = requires(Item& item){
    item();
};  // requires-expression is an EXPRESSION — the concept declaration needs this semicolon

template<typename Item> requires Callable<Item>  // without `requires`, Item is unconstrained
class thread_pool{
    std::mutex mtx; // mutex for synchronizing access to the task queue
    std::condition_variable cv; // cv for notifying worker threads of events
    std::queue<Item> queue;
    std::atomic<bool>  stop{false}; // flag to notify worker threads to stop processing tasks ie gracefull shutdown
    size_t max_queue_size{0};          // 0 = unbounded (worker_pool's default)
    std::atomic<uint64_t> dropped{0};  // tasks refused because the queue was full
    // put worker at the end so that worker do not outlive the varible in threadpool ( which is undefine behaviour if they access the destroyed things)
    std::vector<std::jthread> workers;
    void worker_loop(){
        while(true){
            // check if there is any work to do sitting in the queue (since queue is mutual need mutex locked to check it state)
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait( lock,[&]{ return !queue.empty() || stop.load(std::memory_order_acquire) ;} ); // sleep as long as there is nothing to do or stop requested
            if( queue.empty() && stop.load() ){
                return ;   // nothing to do and stop requested go exit
            }

            Item item = std::move(queue.front()) ;// pop the item
            queue.pop();
            lock.unlock();
            // start doing the work we got
            item();
        }
    }
    public:
    ~thread_pool() {
        stop.store(true,std::memory_order_release);
        cv.notify_all();
    }
    thread_pool(size_t count, size_t max_queue = 0){
        // create worker threads
        if( count  > MAX_THREADS  || count < 1){
            throw std::runtime_error("Invalid Number of Threads");
        }
        max_queue_size = max_queue;
        for(size_t i=0;i<count ;i++){
            workers.emplace_back( [this]{worker_loop();} );
        }
    }
    void enqueue(Item item){
        // lock the mutex to push task in the queue
        {
            std::unique_lock<std::mutex> lock(mtx);
            // bounded + drop: refuse instead of growing forever. Correct for
            // work where losing an item is cheaper than blocking the producer
            if(max_queue_size > 0 && queue.size() >= max_queue_size){
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue.push(std::move(item)) ;
        } // lock went out of scpore released automatically
        // now notify one sleaping thred
        cv.notify_one();
    }
    uint64_t dropped_count() const {
         return dropped.load(std::memory_order_relaxed);
    }
};

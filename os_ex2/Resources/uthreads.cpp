#include <csetjmp>
#include "uthreads.h"
#include <queue>
#include <cstdio>
#include <vector>
#include <sys/time.h>
#include <signal.h>
#include <cstdlib>
#include <unordered_map>
#include <iostream>
#include <unistd.h>
#include <algorithm>

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g"(ret)
                 : "0"(addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
                 : "=g"(ret)
                 : "0"(addr));
    return ret;
}

#endif

// Constants
#define SECOND 1000000
#define STACK_SIZE 4096
#define SYSTEM_ERROR_MSG_PREFIX "system error: %s\n"
#define THREAD_ERROR_MSG_PREFIX "thread library error: %s\n"
#define NON_POSITIVE_QUANTUM_MSG "quantum_usecs must be positive"
#define BAD_ALLOCATION_MSG "couldn't allocate memory\n"
#define ERROR_CODE (-1)
#define SUCCESS_CODE (0)
#define SIGNAL_SET_CONFIG_ERROR_MSG "failed to configure signal set\n"
#define SIGNAL_MASK_CONFIG_ERROR_MSG "failed to configure signal mask\n"
#define SIGACTION_FAILURE_MSG "sigaction failed\n"
#define ENTRY_POINT_ERROR_MSG "entry_point is null\n"
#define OVERFLOW_THREADS_ERROR_MSG "max number of threads reached\n"
#define INVALID_TID_MSG "invalid thread ID\n"
#define SIGPROCMASK_FAILURE_MSG "sigprocmask failed"
#define QUANTUMS_ERROR_MSG "quantum_usecs must be non negative\n"
#define MAIN_THREAD_ERROR_MSG "main thread can't be asleep\n"

enum thread_states
{
    READY,
    RUNNING,
    BLOCKED,
    SLEEPING
};

/*
 * Thread Control Block (TCB) structure
 * Holds information about each thread, including its ID, status, environment, stack, and quantum count.
 */
struct TCB
{
    size_t id;
    thread_states status;
    sigjmp_buf env;
    char *stack;
    size_t quantums;

    explicit TCB(int id) : id(id), status(READY), stack(nullptr), quantums(0) {}

    // runs a thread
    void run_thread()
    {
        quantums++;
        status = RUNNING;
    }
};

// Struct to hold sleeping threads
struct SleepingThread
{
    size_t tid;
    size_t sleep_quantums;
    SleepingThread(int id, int sleep_quantums) : tid(id), sleep_quantums(sleep_quantums)
    {
    }
};

// Global Vars
std::vector<TCB *> threads_vec(MAX_THREAD_NUM, nullptr);
std::vector<SleepingThread> sleeping_threads_vec;
std::queue<int> ready_threads_queue;
struct itimerval timer;
struct sigaction sa;
size_t total_quantums = 1; // Total quantums across threads
sigset_t signals_set;
size_t current_running_tid;

/**
 * @brief Unblocks signals specified in the set using sigprocmask.
 */
void block_signals()
{
    if (sigprocmask(SIG_BLOCK, &signals_set, nullptr) < 0)
    {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGPROCMASK_FAILURE_MSG);
        exit(1);
    }
}

/**
 * @brief Unblocks signals specified in the set using sigprocmask.
 */
void unblock_signals()
{
    if (sigprocmask(SIG_UNBLOCK, &signals_set, nullptr) < 0)
    {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGPROCMASK_FAILURE_MSG);
        exit(1);
    }
}

/**
 * @brief wakes up sleeping threads that have exceeded their quantum limit.
 */
void wake_sleeping_threads()
{
    std::vector<SleepingThread> still_sleeping;

    for (auto &thread : sleeping_threads_vec)
    {
        size_t tid = thread.tid;
        if (threads_vec[tid] == nullptr)
        {
            // Thread has been terminated
            continue;
        }
        if (total_quantums >= thread.sleep_quantums)
        {
            if (threads_vec[tid]->status == SLEEPING)
            {
                threads_vec[tid]->status = READY;
                ready_threads_queue.push(tid);
            }
        }
        else
        {
            still_sleeping.push_back(thread);
        }
    }
    sleeping_threads_vec = std::move(still_sleeping);
}

/**
 * @brief Configures the timer for the specified quantum in microseconds.
 *
 * @param quantum_usecs The quantum time in microseconds.
 */
void configure_timer(size_t quantum_usecs)
{
    timer.it_value.tv_sec = quantum_usecs / SECOND;
    timer.it_value.tv_usec = quantum_usecs % SECOND;
    timer.it_interval.tv_sec = quantum_usecs / SECOND;
    timer.it_interval.tv_usec = quantum_usecs % SECOND;
}

/**
 * @brief Use Round Robin scheduling to switch between threads.
 */
void round_robin()
{
    block_signals();
    total_quantums++;

    if (ready_threads_queue.empty())
    {
        threads_vec[current_running_tid]->run_thread();
    }
    else
    {
        size_t next_tid = ready_threads_queue.front();
        ready_threads_queue.pop();

        // Ensure next thread exists (could have been terminated)
        if (threads_vec[next_tid] != nullptr) {
            current_running_tid = next_tid;
            TCB *next_thread = threads_vec[next_tid];
            next_thread->run_thread();
            unblock_signals();
            siglongjmp(next_thread->env, 1);
        }
    }
    unblock_signals();
}

/**
 * @brief Moves the current running thread to the ready state if it is currently running.
 */
void move_current_running_thread_to_ready()
{
    if (threads_vec[current_running_tid]->status == RUNNING)
    {
        threads_vec[current_running_tid]->status = READY;
        ready_threads_queue.push(current_running_tid);
    }
}

/**
 * @brief The signal handler for SIGVTALRM.
 * @param signal required by sa.sa_handler
 */
void timer_handler(int signal)
{
    wake_sleeping_threads();
    move_current_running_thread_to_ready();

    round_robin();
}

/**
 * @brief Frees all allocated resources.
 */
void free()
{
    for (const auto t : threads_vec)
    {
        if (t != nullptr)
        {
            delete[] t->stack;
            delete t;
        }
    }
    for (const auto t : sleeping_threads_vec)
    {
    }
}

/**
 * @brief Sets up the SIGVTALRM signal handler.
 *
 * This function creates a signal set with SIGVTALRM and sets up the signal handler to call timer_handler
 * when SIGVTALRM is raised.
 */
void setup_SIGVTALRM_handler()
{
    // create signal set with SIGVTALRM in it
    if (sigemptyset(&signals_set) == -1 || sigaddset(&signals_set, SIGVTALRM) == -1)
    {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGNAL_SET_CONFIG_ERROR_MSG);
        free();
        exit(ERROR_CODE);
    }

    sa.sa_handler = &timer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGVTALRM, &sa, nullptr) < 0)
    {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGACTION_FAILURE_MSG);
        free();
        exit(ERROR_CODE);
    }
}

/**
 * @brief Allocates a new TCB for the thread with the given ID.
 *
 * @param tid The thread ID.
 * @return A pointer to the newly allocated TCB.
 */
TCB *allocate_new_tcb(size_t tid)
{
    try
    {
        threads_vec[tid] = new TCB(tid);
        return threads_vec[tid];
    }
    catch (std::bad_alloc &_)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, BAD_ALLOCATION_MSG);
        free();
        exit(ERROR_CODE);
    }
}

/**
 * @brief Allocates a new stack for the given thread.
 *
 * @param thread The thread for which to allocate a new stack.
 */
void allocate_new_stack(TCB *thread)
{
    try
    {
        thread->stack = new char[STACK_SIZE];
    }
    catch (std::bad_alloc &_)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, BAD_ALLOCATION_MSG);
        free();
        exit(ERROR_CODE);
    }
}

/**
 * @brief Finds the lowest available thread ID.
 *
 * @return The lowest available thread ID, or -1 if no IDs are available.
 */
int find_lowest_tid()
{
    for (size_t i = 0; i < MAX_THREAD_NUM; ++i)
    {
        if (threads_vec[i] == nullptr)
        {
            return i;
        }
    }
    return ERROR_CODE;
}

/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_init(int quantum_usecs)
{
    if (quantum_usecs <= 0)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, NON_POSITIVE_QUANTUM_MSG);
        return ERROR_CODE;
    }

    current_running_tid = 0;
    allocate_new_tcb(current_running_tid);

    threads_vec[0]->run_thread();

    sigsetjmp(threads_vec[0]->env, 1);
    sigemptyset(&(threads_vec[0]->env->__saved_mask));
    sigemptyset(&signals_set);

    setup_SIGVTALRM_handler();
    configure_timer(quantum_usecs);

    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) < 0)
    {
        fprintf(stderr, "thread library error: setitimer error\n");
        free();
        return ERROR_CODE;
    }

    return SUCCESS_CODE;
}

/**
 * @brief Initializes the thread context for the given thread.
 *
 * @param thread The thread to initialize.
 * @param entry_point The entry point function for the thread.
 */
void init_thread_context(TCB *thread, thread_entry_point entry_point)
{
    address_t sp = (address_t)thread->stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t)entry_point;
    sigsetjmp((thread->env), 1);
    (thread->env)->__jmpbuf[JB_SP] = translate_address(sp);
    (thread->env)->__jmpbuf[JB_PC] = translate_address(pc);
    sigemptyset(&(thread->env)->__saved_mask);
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
 */
int uthread_spawn(thread_entry_point entry_point)
{
    block_signals();

    if (entry_point == nullptr)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, ENTRY_POINT_ERROR_MSG);
        unblock_signals();
        return ERROR_CODE;
    }

    int tid = find_lowest_tid();
    if (tid == -1)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, OVERFLOW_THREADS_ERROR_MSG);
        unblock_signals();
        return ERROR_CODE;
    }

    TCB *new_thread = allocate_new_tcb(tid);
    allocate_new_stack(new_thread);

    init_thread_context(new_thread, entry_point);
    ready_threads_queue.push(tid);
    unblock_signals();
    return tid;
}

/**
 * @brief Removes the thread with the given ID from the ready queue.
 *
 * @param tid The thread ID to remove.
 */
void remove_thread_from_ready_queue(int tid)
{
    std::queue<int> temp_queue;
    while (!ready_threads_queue.empty())
    {
        int ready_tid = ready_threads_queue.front();
        ready_threads_queue.pop();
        if (ready_tid != tid)
        {
            temp_queue.push(ready_tid);
        }
    }
    ready_threads_queue = temp_queue;
}

/**
 * @brief Removes the thread with the given ID from the sleeping threads vector.
 *
 * @param tid The thread ID to remove.
 */
void remove_thread_from_sleeping_vec(int tid)
{
    auto it = std::remove_if(sleeping_threads_vec.begin(), sleeping_threads_vec.end(),
                             [tid](const SleepingThread &st)
                             { return st.tid == tid; });
    sleeping_threads_vec.erase(it, sleeping_threads_vec.end());
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
 */
int uthread_terminate(int tid)
{
    block_signals();

    if (tid < 0 || tid >= MAX_THREAD_NUM || threads_vec[tid] == nullptr)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, INVALID_TID_MSG);
        return ERROR_CODE;
    }

    if (tid == 0)
    {
        free();
        exit(SUCCESS_CODE);
    }

    bool is_running_thread = (current_running_tid == tid);

    if (threads_vec[tid]->status == READY)
    {
        remove_thread_from_ready_queue(tid);
    }
    if (threads_vec[tid]->status == SLEEPING)
    {
        remove_thread_from_sleeping_vec(tid);
    }
    delete[] threads_vec[tid]->stack;
    delete threads_vec[tid];
    threads_vec[tid] = nullptr;

    if (is_running_thread)
    {
        round_robin();
    }
    unblock_signals();
    return SUCCESS_CODE;
}

/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_block(int tid)
{
    block_signals();

    if (tid < 0 || tid >= MAX_THREAD_NUM || threads_vec[tid] == nullptr || tid == 0)
    {
        unblock_signals();
        return ERROR_CODE;
    }

    if (threads_vec[tid]->status == BLOCKED)
    {
        unblock_signals();
        return SUCCESS_CODE;
    }
    if (threads_vec[tid]->status == READY)
    {
        remove_thread_from_ready_queue(tid);
    }

    threads_vec[tid]->status = BLOCKED;
    if (tid == current_running_tid)
    {
        if (sigsetjmp(threads_vec[current_running_tid]->env, 1) == 0)
        {
            round_robin();
        }
    }

    unblock_signals();
    return SUCCESS_CODE;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_resume(int tid)
{
    block_signals();

    if (tid < 0 || tid >= MAX_THREAD_NUM || threads_vec[tid] == nullptr)
    {
        unblock_signals();
        return ERROR_CODE;
    }

    if (threads_vec[tid]->status != BLOCKED)
    {
        unblock_signals();
        return SUCCESS_CODE;
    }

    threads_vec[tid]->status = READY;
    ready_threads_queue.push(tid);
    unblock_signals();
    return SUCCESS_CODE;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_sleep(int num_quantums)
{
    block_signals();
    if (num_quantums < 0)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, QUANTUMS_ERROR_MSG);
        unblock_signals();
        return ERROR_CODE;
    }
    if (current_running_tid == 0)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, MAIN_THREAD_ERROR_MSG);
        unblock_signals();
        return ERROR_CODE;
    }
    if (num_quantums == 0)
    {
        unblock_signals();
        return SUCCESS_CODE;
    }
    threads_vec[current_running_tid]->status = SLEEPING;
    sleeping_threads_vec.push_back(SleepingThread(current_running_tid, total_quantums + num_quantums));
    if (sigsetjmp(threads_vec[current_running_tid]->env, 1) == 0)
    {
        round_robin();
    }
    unblock_signals();
    return SUCCESS_CODE;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
 */
int uthread_get_tid()
{
    return current_running_tid;
}

/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
 */
int uthread_get_total_quantums()
{
    return total_quantums;
}

/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
 */
int uthread_get_quantums(int tid)
{
    block_signals();
    if (tid < 0 || tid >= MAX_THREAD_NUM || threads_vec[tid] == nullptr)
    {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, INVALID_TID_MSG);
        unblock_signals();
        return ERROR_CODE;
    }
    int quantums = threads_vec[tid]->quantums;
    unblock_signals();
    return quantums;
}
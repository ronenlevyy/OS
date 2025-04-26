//
// Created by RONEN LEVY on 23/04/2025.
//
#include <iostream>
#include <csetjmp>

#include "uthreads.h"
#include <csignal>
#include <unistd.h>
#include <sys/time.h>


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
                 : "=g" (ret)
                 : "0" (addr));
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
    : "=g" (ret)
    : "0" (addr));
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

enum thread_states { READY, RUNNING, BLOCKED };

struct TCB {
    size_t id;
    thread_states status;
    sigjmp_buf env;
    char *stack;
    int quantums;

    explicit TCB(int id) : id(id), status(READY), stack(nullptr), quantums(0) {
        //    todo - this is for d_bug
        std::cout << "new thread was made with id=" << this->id << std::endl;
    }

    void run_thread() {
        quantums++;
        status = RUNNING;
    }
};

struct SleepingThread {
    int tid;
    size_t sleep_quantums;
};

// Global Vars
std::vector<TCB *> threads_vec(MAX_THREAD_NUM, nullptr);
std::vector<SleepingThread> sleeping_threads;
std::queue<int> ready_threads_queue;
itimerval timer;
struct sigaction sa;
int total_quantums = 1; // Total quantums across threads
sigset_t signals_set;
int current_running_tid;

int get_and_update_next_tid_to_run() {
    current_running_tid = ready_threads_queue.front();
    ready_threads_queue.pop();
    return current_running_tid;
}

void block_signals() {
    if (sigprocmask(SIG_BLOCK, &signals_set, nullptr) < 0) {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, "sigprocmask failed");
        exit(1);
    }
}

void wake_sleeping_threads() {
    std::vector<SleepingThread> still_sleeping;

    for (auto &thread: sleeping_threads) {
        int tid = thread.tid;
        thread.sleep_quantums--;
        if (thread.sleep_quantums <= 0) {
            if (threads_vec[tid]->status != BLOCKED) {
                threads_vec[tid]->status = READY;
                ready_threads_queue.push(tid);
            }
        } else {
            still_sleeping.push_back(thread);
        }
    }
    sleeping_threads = std::move(still_sleeping);
}

void round_robin() {
    block_signals();
    wake_sleeping_threads();

    if (ready_threads_queue.empty()) {
        threads_vec[current_running_tid]->run_thread();
    } else {
        TCB *current_thread = threads_vec[get_and_update_next_tid_to_run()];
        current_thread->run_thread();
        siglongjmp(current_thread->env, 1);
    }
}

void timer_handler(int signal) {
    //maybe rename
    round_robin();
}

void free_and_exit(int exit_code) {
    for (const auto t: threads_vec) {
        if (t != nullptr) {
            delete [] t->stack;
            delete t;
        }
    }
    exit(exit_code);
}

void configure_timer(int quantum_usecs) {
    timer.it_value.tv_sec = quantum_usecs / 1000000;
    timer.it_value.tv_usec = quantum_usecs % 1000000;
    timer.it_interval = timer.it_value;
    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) == -1) {
        std::cerr << "system error: setitimer failed\n";
        exit(1);
    }
}

void signal_handler_init() {
    if (sigemptyset(&signals_set) == -1 || sigaddset(&signals_set, SIGVTALRM) == -1) {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGNAL_SET_CONFIG_ERROR_MSG);
        free_and_exit(ERROR_CODE);
    }

    sa.sa_handler = &timer_handler; // timer_handler will be called when SIGVTALARAM is raised

    // if (sigemptyset(&sa.sa_mask) < 0 || sigaddset(&sa.sa_mask, SIGVTALRM) < 0) {
    //     fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGNAL_MASK_CONFIG_ERROR_MSG);
    //     free_and_exit(ERROR_CODE);
    // }

    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
        fprintf(stderr, SYSTEM_ERROR_MSG_PREFIX, SIGACTION_FAILURE_MSG);
        free_and_exit(ERROR_CODE);
    }
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
int uthread_init(int quantum_usecs) {
    if (quantum_usecs <= 0) {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, NON_POSITIVE_QUANTUM_MSG);
        return ERROR_CODE;
    }
    signal_handler_init();
    configure_timer(quantum_usecs);

    try {
        threads_vec[0] = new TCB(0);
    } catch (std::bad_alloc &_) {
        fprintf(stderr, THREAD_ERROR_MSG_PREFIX, BAD_ALLOCATION_MSG);
        free_and_exit(1);
    }
    threads_vec[0]->run_thread();
    current_running_tid = 0;
    if (sigsetjmp(threads_vec[0]->env, 1) != 0) {
        return SUCCESS_CODE;
    }
    return SUCCESS_CODE;
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
int uthread_spawn(thread_entry_point entry_point);


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
int uthread_terminate(int tid);


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid);


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid);


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
int uthread_sleep(int num_quantums);


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
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
int uthread_get_total_quantums() {
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
int uthread_get_quantums(int tid) {
    return threads_vec[tid]->quantums;
}

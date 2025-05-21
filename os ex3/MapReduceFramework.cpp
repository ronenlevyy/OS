#include "MapReduceFramework.h"
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <iostream>
#include "Barrier.h"
#define SYSTEM_ERROR_PREFIX "system error: "
#define ERROR_JOIN_FAILED "thread join failed with error code"
#define EMIT3_ERROR "problem at the emit3 func"
#define ERROR "error"
#define PHASE_ERROR "there is a phase problem"
#define EXIT_FAIL 1

/**
 * @enum PhaseType
 * @brief Represents the current execution phase in the MapReduce process.
 *
 * This enum is used to indicate whether a thread is performing the Map phase
 * or the Reduce phase. It helps control the behavior of the `phase()` function.
 */
enum PhaseType {
    MAP_PHASE,
    REDUCE_PHASE
};


struct JobContext;
/**
 * @struct ThreadContext
 * @brief Represents the context of a single thread in the MapReduce job.
 *
 * Each thread participating in the MapReduce process gets its own
 * ThreadContext, which includes an ID, intermediate key-value pairs collected
 * during the map phase, and a pointer to the shared JobContext.
 */
struct ThreadContext {
    int id;
    IntermediateVec intermediateData;
    JobContext* context;

    ThreadContext(int id_, JobContext* ctx)
    {
      id = id_;
      context = ctx;
    }
};

/**
 * @struct JobContext
 * @brief Represents the shared context for a full MapReduce job.
 *
 * This struct holds all the shared data and synchronization primitives needed
 * by multiple threads to execute a MapReduce job in parallel. It includes the
 * input/output data, counters, thread states, synchronization tools (mutexes,
 * barrier), and a job state tracker.
 */
struct JobContext {
    int multiThreadLevel;
    int sum_for_testing = 0;
    int for_testing = 0;
    const InputVec& inputVec;
    OutputVec& outputVec;
    const MapReduceClient& client;

    std::vector<std::thread> threadsVec;
    std::vector<ThreadContext*> threadCtx;


    std::atomic<uint64_t> inputVecIndAtomic;
    std::atomic<uint64_t> shuffledVecsAtomic;
    std::atomic<uint32_t> intermediatePairsAtomicNum;

    std::atomic<uint64_t> jobStateAtomic;
    std::mutex outputMutex;
    std::mutex stateMutex;

    Barrier* my_barrier;

    std::vector<IntermediateVec> intermediateVectors;
    std::vector<IntermediateVec> shuffleQueue;

    std::atomic<bool> hasWaitedAtomic;
    std::mutex intermediateMutex;



    /**
     * @brief Constructor for JobContext.
     * Initializes members, resizes thread containers, and creates the barrier.
     */
    JobContext(int multiThreadLevel,
               const InputVec& inputVec,
               OutputVec& outputVec,
               const MapReduceClient& client)
        : multiThreadLevel(multiThreadLevel),
          inputVec(inputVec),
          outputVec(outputVec),
          client(client),
          jobStateAtomic(0),
          my_barrier(new Barrier(multiThreadLevel)),
          hasWaitedAtomic(false)
    {
      threadsVec.resize(multiThreadLevel);
      threadCtx.resize(multiThreadLevel);
    }

    /**
     * @brief Destructor for JobContext.
     * Joins any unjoined threads, deletes the barrier, and cleans up thread contexts.
     */
    ~JobContext() {
      if (!hasWaitedAtomic.load()) {
        for (std::thread& t : threadsVec) {
          if (t.joinable()) {
            try {
              t.join();
            } catch (const std::system_error& e) {
              std::cerr <<  SYSTEM_ERROR_PREFIX <<  ERROR_JOIN_FAILED <<
                        std::endl;
              exit(EXIT_FAIL);
            }
          }
        }
      }
      delete my_barrier;
      for (ThreadContext* ctx : threadCtx) {
        delete ctx;
      }
    }
};

/**
 * Encodes job stage, number of tasks done, and total tasks into a single 64-bit integer.
 *
 * Bits layout:
 * [63–62]: stage, [61–31]: total, [30–0]: done
 */
uint64_t encodeJobState(stage_t stage, uint32_t done, uint32_t
total) {
  return (static_cast<uint64_t>(stage) << 62) |
         (static_cast<uint64_t>(total) << 31) |
         static_cast<uint64_t>(done);
}

/**
 * Decodes a 64-bit job state into stage, done, and total.
 *
 * Extracts:
 * - stage from bits 63–62
 * - total from bits 61–31
 * - done from bits 30–0
 */
void decodeState(uint64_t encodedState, stage_t &stage, uint32_t &done,
                 uint32_t &total) {
  stage = static_cast<stage_t>((encodedState >> 62) & 0x3);
  total = static_cast<uint32_t>((encodedState >> 31) & 0x7FFFFFFF);
  done = static_cast<uint32_t>(encodedState & 0x7FFFFFFF);
}



int dosSort(ThreadContext *);
int phase(ThreadContext* , PhaseType);
int doShuffle(JobContext *, bool);
/**
 * Executes the full lifecycle of a worker thread:
 * 1. Performs the Map phase.
 * 2. Sorts intermediate data.
 * 3. Waits at a barrier.
 * 4. If thread ID is 0, runs Shuffle and updates stage.
 * 5. Performs the Reduce phase.
 *
 * Exits on phase failure.
 */
void threadLifeCycle(ThreadContext* threadContext) {
  JobContext* jobCtx = threadContext->context;
  if (!phase(threadContext, MAP_PHASE)){
    std::cerr << SYSTEM_ERROR_PREFIX << PHASE_ERROR << std::endl;
    exit (EXIT_FAIL);
  }
  dosSort(threadContext);
  jobCtx->my_barrier->barrier();
  if (threadContext->id == 0) {
    bool no_more = false;
    jobCtx->jobStateAtomic.store(encodeJobState(SHUFFLE_STAGE, 0,
                                                jobCtx->intermediatePairsAtomicNum.load()));
    doShuffle(jobCtx, no_more);
    jobCtx->jobStateAtomic.store(encodeJobState(REDUCE_STAGE, 0,
                                                jobCtx->shuffleQueue.size()));
  }
  if (!phase(threadContext, REDUCE_PHASE)){
    std::cerr << SYSTEM_ERROR_PREFIX << PHASE_ERROR << std::endl;
    exit (EXIT_FAIL);
  }
}

/**
 * Called during the Map phase to collect intermediate key-value pairs.
 * Appends the pair to the thread's intermediate vector and increments the global counter.
 */
void emit2(K2* key, V2* value, void* context) {
  auto* threadCtx = static_cast<ThreadContext*>(context);
  threadCtx->intermediateData.emplace_back(key, value);
  threadCtx->context->intermediatePairsAtomicNum.fetch_add(1);
}

/**
 * Called during the Reduce phase to emit output key-value pairs.
 * Adds the pair to the final output vector under mutex lock.
 * Exits if mutex fails.
 */
void emit3(K3* key, V3* value, void* context) {
  auto* ctx = static_cast<ThreadContext*>(context);
  JobContext* jobCtx = ctx->context;

  try {
    std::lock_guard<std::mutex> lock(jobCtx->outputMutex);
    jobCtx->outputVec.emplace_back(key, value);
  } catch (const std::system_error& e) {
    std::cerr << SYSTEM_ERROR_PREFIX << EMIT3_ERROR << std::endl;
    delete jobCtx;
    exit(EXIT_FAIL);
  }
}

/**
 * Initializes the job context and spawns worker threads.
 * Sets the initial MAP stage in job state.
 * Returns a JobHandle to be used for control functions.
 */
JobHandle startMapReduceJob(const MapReduceClient& client,
                            const InputVec& inputVec,
                            OutputVec& outputVec,
                            int multiThreadLevel) {
  auto* jobCtx = new JobContext(multiThreadLevel, inputVec, outputVec, client);
  uint32_t totalMapPairs = static_cast<uint32_t>(inputVec.size());
  jobCtx->jobStateAtomic.store(encodeJobState(MAP_STAGE, 0, totalMapPairs));

  for (int i = 0; i < multiThreadLevel; ++i){
    jobCtx->threadCtx[i] = new ThreadContext(i, jobCtx);

    try {
      jobCtx->threadsVec[i] = std::thread(&threadLifeCycle,
                                        jobCtx->threadCtx[i]);
    }
    catch (const std::system_error& e) {
      std::cerr << SYSTEM_ERROR_PREFIX  << ERROR << std::endl;
      exit(EXIT_FAIL);
    }
  }
  return static_cast<JobHandle>(jobCtx);
}


/**
 * Sorts the thread's intermediate data by key and adds it to the global intermediate vector.
 */
int dosSort(ThreadContext *threadContext)
{
  std::sort(threadContext->intermediateData.begin(), threadContext->intermediateData.end(),
            [](const IntermediatePair &x, const IntermediatePair &y) {
                return *x.first < *y.first;
            });

  {
    std::lock_guard<std::mutex> lock(threadContext->context->intermediateMutex);
    threadContext->context->intermediateVectors.push_back(threadContext->intermediateData);
  }
  return 1;
}

/**
 * Executes either the Map or Reduce phase by iterating over assigned work units.
 * Uses atomic counters to divide work across threads.
 */
int phase(ThreadContext* threadContext, PhaseType type) {
  JobContext* ctx = threadContext->context;
  if(!ctx){
    return 0;
  }
  if (type == REDUCE_PHASE) {
    ctx->my_barrier->barrier();
  }

  std::atomic<uint64_t>* counter;
  size_t total;

  if (type == MAP_PHASE) {
    counter = &ctx->inputVecIndAtomic;
    total = ctx->inputVec.size();
  } else {
    counter = &ctx->shuffledVecsAtomic;
    total = ctx->shuffleQueue.size();
  }

  uint64_t index;
  while ((index = counter->fetch_add(1)) < total) {
    if (type == MAP_PHASE) {
      const auto& pair = ctx->inputVec[index];
      ctx->client.map(pair.first, pair.second, threadContext);
    } else {
      auto& vec = ctx->shuffleQueue[index];
      ctx->client.reduce(&vec, threadContext);
    }
    ctx->jobStateAtomic.fetch_add(1);
  }
  return 1;
}

/**
 * Waits for all threads to finish. Ensures this runs only once using atomic flag.
 */
void waitForJob(JobHandle job) {
  auto* jobCtx = static_cast<JobContext*>(job);
  bool expected = false;
  if (jobCtx->hasWaitedAtomic.compare_exchange_strong(expected, true)) {
    for (std::thread &t : jobCtx->threadsVec) {
      if (t.joinable()) {
        try {
          t.join();
        } catch (const std::system_error& e) {
          std::cerr << SYSTEM_ERROR_PREFIX << ERROR_JOIN_FAILED << std::endl;
          delete jobCtx;
          exit(EXIT_FAIL);
        }
      }
    }
  }
}

/**
 * Retrieves the current job stage and completion percentage.
 */
void getJobState(JobHandle job, JobState* state) {
  auto* ctx = static_cast<JobContext*>(job);
  uint64_t encodedState = ctx->jobStateAtomic.load();

  uint32_t done = 0;
  uint32_t total = 0;
  stage_t stage = UNDEFINED_STAGE;
  float percent = 0.0f;

  decodeState(encodedState, stage, done, total);

  if (total > 0) {
    float val = (static_cast<float>(done) / static_cast<float>(total)) ;
    percent = val * 100.0f;
  }

  state->percentage = percent;
  state->stage = stage;
}

/**
 * Final cleanup: waits for threads and deletes job context.
 */
void closeJobHandle(JobHandle job) {
  auto* ctx = static_cast<JobContext*>(job);
  waitForJob(job);
  delete ctx;
}

/**
 * Returns the largest key from all non-empty intermediate vectors.
 */
K2* getMax(const std::vector<IntermediateVec>& sortedVecs)
{
  K2* Keymax = nullptr;
  for (auto& intermediateVec : sortedVecs) {
    if (!intermediateVec.empty()) {
      auto& pair = intermediateVec.back();
      K2* currKey = pair.first;
      if (Keymax == nullptr || *Keymax < *currKey) {
        Keymax = currKey;
      }
    }
  }
  return Keymax;
}

/**
 * Compares two keys using operator< to determine if they are equal.
 */
 int checkEqualKeys(K2* key1, K2* key2) {
  if (*key1 < *key2 || *key2 < *key1) {
    return 0;
  }
  return 1;
}

/**
 * Groups all intermediate pairs by key across all threads.
 * Builds the shuffle queue for the Reduce phase.
 */
int doShuffle(JobContext *jobCtx, bool no_more) {
  {
    jobCtx->for_testing++;
    jobCtx->sum_for_testing = jobCtx->for_testing;
    std::vector<IntermediateVec> shuffledVecs;
    while (!no_more)
    {
      if (jobCtx->sum_for_testing < -1)
      {
        return 0;
      }
      jobCtx->for_testing++;

      K2 *maxKey = getMax
          (jobCtx->intermediateVectors);
      if (!maxKey)
      {
        jobCtx->for_testing++;
        jobCtx->sum_for_testing = jobCtx->for_testing;
        break;
      }else{
        IntermediateVec newVec;
        for (auto &intermediateVec: jobCtx->intermediateVectors)
        {
          while (!intermediateVec.empty ())
          {
            if (!checkEqualKeys (intermediateVec.back ().first, maxKey))
            {
              break;
            }
            jobCtx->for_testing++;
            newVec.push_back (std::move (intermediateVec.back ()));
            intermediateVec.pop_back ();
            jobCtx->jobStateAtomic.fetch_add (1);
            jobCtx->sum_for_testing = jobCtx->for_testing;
          }
        }
        jobCtx->shuffleQueue.push_back (std::move (newVec));
        jobCtx->for_testing++;
      }
    }
  }
  return 1;
}
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>
#include <windows.h>

typedef struct ThreadPool ThreadPool;

/* Callback type for thread pool tasks.
 * Called with the user-provided arg and task index. */
typedef void (*thread_func_t)(void *arg, int task_index);

/* Create a thread pool with N worker threads */
ThreadPool* threadpool_create(int num_threads);

/* Submit tasks to the pool. Blocks until all tasks complete.
 * If one task fails, other tasks may be cancelled.
 * Returns 0 on success, -1 if any task failed. */
int threadpool_run(ThreadPool *pool, thread_func_t func, void *arg, int num_tasks);

/* Destroy the thread pool */
void threadpool_destroy(ThreadPool *pool);

#endif

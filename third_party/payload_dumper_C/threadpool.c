/**
 * threadpool.c -- Simple thread pool for Windows
 */
#include "threadpool.h"
#include <stdlib.h>
#include <windows.h>

typedef struct {
    thread_func_t func;
    void *arg;
    int num_tasks;
    volatile LONG next_task;
    volatile LONG failed;
} TaskQueue;

typedef struct {
    int dummy;
    int num_threads;
    HANDLE *threads;
    TaskQueue queue;
} PoolImpl;

static DWORD WINAPI worker_thread(LPVOID param) {
    PoolImpl *pool = (PoolImpl*)param;
    TaskQueue *q = &pool->queue;
    
    while (1) {
        LONG idx = InterlockedIncrement(&q->next_task) - 1;
        if (idx >= q->num_tasks) break;
        if (q->failed) break; /* early exit on failure */
        q->func(q->arg, (int)idx);
    }
    return 0;
}

ThreadPool* threadpool_create(int num_threads) {
    if (num_threads < 1) num_threads = 1;
    PoolImpl *pool = calloc(1, sizeof(PoolImpl));
    if (!pool) return NULL;
    pool->num_threads = num_threads;
    pool->threads = calloc(num_threads, sizeof(HANDLE));
    if (!pool->threads) { free(pool); return NULL; }
    return (ThreadPool*)pool;
}

int threadpool_run(ThreadPool *tp, thread_func_t func, void *arg, int num_tasks) {
    PoolImpl *pool = (PoolImpl*)tp;
    if (!pool || num_tasks <= 0) return -1;
    
    pool->queue.func = func;
    pool->queue.arg = arg;
    pool->queue.num_tasks = num_tasks;
    pool->queue.next_task = 0;
    pool->queue.failed = 0;
    
    int actual_threads = pool->num_threads;
    if (actual_threads > num_tasks) actual_threads = num_tasks;
    
    int created_threads = 0;
    for (int i = 0; i < actual_threads; i++) {
        pool->threads[i] = CreateThread(NULL, 0, worker_thread, pool, 0, NULL);
        if (!pool->threads[i]) {
            for (int j = i; j < actual_threads; j++) pool->threads[j] = NULL;
            break;
        }
        created_threads++;
    }
    
    /* Wait for all threads */
    for (int i = 0; i < actual_threads; i++) {
        if (pool->threads[i]) {
            WaitForSingleObject(pool->threads[i], INFINITE);
            CloseHandle(pool->threads[i]);
            pool->threads[i] = NULL;
        }
    }
    
    /* If no worker could be created, execute the queue exactly once here. */
    if (created_threads == 0) {
        for (int i = 0; i < num_tasks; i++) {
            if (pool->queue.failed) break;
            func(arg, i);
        }
    }
    
    return pool->queue.failed ? -1 : 0;
}

void threadpool_destroy(ThreadPool *tp) {
    if (!tp) return;
    PoolImpl *pool = (PoolImpl*)tp;
    free(pool->threads);
    free(pool);
}

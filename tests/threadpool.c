#define _GNU_SOURCE
#include "threadpool.h"
#include "list.h"
#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

struct thread_pool *thread_pool_new(int nthreads);
void thread_pool_shutdown_and_destroy(struct thread_pool *);
struct future *thread_pool_submit(struct thread_pool *pool,
                                  fork_join_task_t task, void *data);
void *future_get(struct future *);
void future_free(struct future *);
// struct future * steal(struct worker *worker);
static _Thread_local struct worker *local = NULL;

typedef struct thread_pool
{
    pthread_mutex_t lock; // mutex lock
    pthread_cond_t cond;  // condition variable
    struct list workers;  // list of workers to manipulate
    int threadcount;      // the number of threads
    struct list tasks;    // list of tasks
    // pthread_t * tasks; // tasks array instead of a list, thats what they used in dutchblitz
    bool shutdown; // boolean on whether the pool is to shut down
    // sem_t sema; //potential semaphore
} *thread_pool;

typedef struct worker
{
    struct thread_pool *pool; // thread pool that the worker holds
    struct list task;         // the list of tasks for the worker
    pthread_cond_t cond;      // condition variable
    pthread_mutex_t lock;     // mutex lock
    struct list_elem elem;    // element from list
    pthread_t thread;         // thread associated with worker
    bool mugger;              // whether or not the worker is trying to steal a task
    int id;                   // the id number, used for checking since comparing list elem mmmmm

} *worker;

typedef struct future
{
    struct thread_pool *pool; // thread pool that the future worker holds
    fork_join_task_t task;    // typedef of a function pointer type that you will execute
    void *args;               // the data from thread_pool_submit
    void *result;             // will store task result once it completes execution
    // sem_t sema; // semaphore
    struct list_elem elem; // element from list
    // bool getB4Free; //checks to see if get was called before free
    int state;            // the state of the future
    pthread_cond_t cond;  // condition variable
    pthread_mutex_t lock; // mutex lock

} *future;

//function for work stealing
//it takes in the worker and then loops through the pool's worker list to 
//find a suitible oldLady victim to steal the task from. If successful,
//it returns the future set to run the task.
static struct future *steal(struct worker *worker)
{

    // create a new future task and lock the worker lock
    struct future *newt = NULL;
    pthread_mutex_lock(&worker->lock);
    worker->mugger = true; // if it is the one stealing the tasl
    pthread_mutex_unlock(&worker->lock);
    
    struct list_elem *poor;
    for (poor = list_begin(&(worker->pool)->workers);
         poor != list_end(&(worker->pool)->workers);
         poor = list_next(poor))
    {
        // if (poor = list_begin(&(worker->pool)->workers)) { 
            //maybe some element of opt by looping less through things?
        //     continue;
        // }
        // if iterator is not what we are, pick a pool worker, lock their lock and steal from their worker queue 
        struct worker *oldLady = list_entry(poor, struct worker, elem);
        
        if (!(oldLady->mugger)) //make sure its not the one already trying to steal
        {
            pthread_mutex_lock(&oldLady->lock);
            if (!list_empty(&oldLady->task))
            {
                //pops the task for the element to do
                newt = list_entry(list_pop_back(&oldLady->task), struct future, elem);
                pthread_mutex_unlock(&oldLady->lock);
                return newt;
            }
            pthread_mutex_unlock(&oldLady->lock);
        }

    }

    return newt;


}

// helper function for pthread_create. 
// this establishes a local thread and while the thread runs
// it makes sure that the pool does not shut down
// It checks the global queue, if its empty then it waits,
// if its not then it pops a task to run from the global queue.
// otherwise if none of them, then it work steals
static void *thread_function(void *worker)
{
    local = (struct worker *)worker;
    struct thread_pool *tpool = (struct thread_pool *)local->pool;

    assert(tpool != NULL);
    while (true)
    {

        pthread_mutex_lock(&tpool->lock);

        if (list_empty(&tpool->tasks))
        {
            if (!tpool->shutdown)
            {
                pthread_cond_wait(&tpool->cond, &tpool->lock);
            }
        }
        if (tpool->shutdown)
        {
            pthread_mutex_unlock(&tpool->lock);
            break;
            // pthread_exit(NULL);
            // break;
        }
        // lock?
        // if local queue //front
        // global else if not while //back
        // else if not either, then steal from another worker //back
        if (!list_empty(&tpool->tasks))
        {
            
            struct list_elem *elem = list_pop_front(&tpool->tasks); ////

            struct future *future = list_entry(elem, struct future, elem);

            pthread_mutex_unlock(&tpool->lock);

            pthread_mutex_lock(&future->lock);
            future->state = 1;
            pthread_mutex_unlock(&future->lock);
            future->result = future->task(future->pool, future->args); //// ///
  
            pthread_mutex_lock(&future->lock);
            future->state = 2;

            pthread_cond_signal(&future->cond);
            pthread_mutex_unlock(&future->lock);
        }
        else
        {
            pthread_mutex_unlock(&tpool->lock);
            struct future *fut = steal(local);

            if (fut != NULL)
            {
                fut->result = fut->task(tpool, fut->args);
                pthread_mutex_lock(&fut->lock);
                fut->state = 2;
                pthread_cond_signal(&fut->cond);
                pthread_mutex_unlock(&fut->lock);
            }
            
        }
    }
    pthread_exit(NULL);
    return NULL;
}


// function to create a new threadpool
// initializes a pool and all its fields, 
// then it starts to create all the threads and the workers,
// performing a pthread_create for all the workers 
// and pushing them into the pool's list
struct thread_pool *thread_pool_new(int nthreads)
{
    // create the thread pool
    struct thread_pool *tpool = (struct thread_pool *)malloc(sizeof(struct thread_pool));
    if (tpool == NULL)
    {
        fprintf(stderr, "Error creating pool");
        return NULL;
    }
    // initialize worker threads
    list_init(&tpool->workers);
    tpool->threadcount = nthreads;

    list_init(&tpool->tasks); // they use a array in dutch blitz so maybe its easier to call array instead?
    pthread_mutex_init(&tpool->lock, NULL);
    pthread_cond_init(&tpool->cond, NULL);

    tpool->shutdown = false;
    // create loop

    for (int i = 0; i < nthreads; i++)
    {
        struct worker *worker = (struct worker *)malloc(sizeof(struct worker));
        if (worker == NULL)
        {
            perror("Error creating worker");
            exit(1);
        }
        list_init(&worker->task);
        worker->pool = tpool;
        worker->mugger = false;
        worker->id = i + 1;
        pthread_mutex_init(&worker->lock, NULL);
        pthread_cond_init(&worker->cond, NULL);
        list_push_back(&tpool->workers, &worker->elem);
        int rc = pthread_create(&worker->thread, NULL, &thread_function, (void *)worker);
        if (rc != 0)
        {
            errno = rc;
            perror("Error creating thread");
        }
    }
    return tpool;
}

// shut down and destroy function
// it goes through and marks the pool to be shut down
// them it goes into the workers for the pool and closes the threads
// by joining them all, waiting for them to finish executing.
// the it pops out all the workers in the pool and frees them
// then it frees the pool
void thread_pool_shutdown_and_destroy(struct thread_pool *tpool)
{

    // makes sure the pool isnt null
    assert(tpool != NULL);
    struct worker *worker;
    pthread_mutex_lock(&tpool->lock);
    tpool->shutdown = true;
    pthread_cond_broadcast(&tpool->cond);
    pthread_mutex_unlock(&tpool->lock);
    // void * stat;

    for (struct list_elem *welem = list_begin(&tpool->workers);
         welem != list_end(&tpool->workers);
         welem = list_next(welem))
    {
        worker = list_entry(welem, struct worker, elem);
        int rc = pthread_join(worker->thread, NULL);
        if (rc != 0)
        {
            errno = rc;
            perror("Error joining thread");
        }
    }
    while (!list_empty(&tpool->workers))
    {
        worker = list_entry((list_pop_front(&tpool->workers)), struct worker, elem);
        pthread_mutex_destroy(&worker->lock);
        pthread_cond_destroy(&worker->cond);
        free(worker);
    }
    pthread_cond_destroy(&tpool->cond);
    pthread_mutex_destroy(&tpool->lock);
    // free(tpool->tasks);
    free(tpool);
}

// functon to create new future function and submit it to the pool
// It creates and initializes a future and all its fields.
// Then, it checks to see if its an internal, and if it is, 
// it pushes it onto the locals list
// Otherwise, if its not local, it pushes it onto the global task queue
struct future *thread_pool_submit(struct thread_pool *pool, fork_join_task_t task, void *data)
{
    // inititalize the future and its fields
    struct future *fut = (struct future *)malloc(sizeof(struct future));
    if (fut == NULL)
    {
        perror("Error creating future");
    }
    fut->task = task;
    fut->args = data;

    fut->state = 0;
    fut->pool = pool;
    fut->result = NULL;

    pthread_cond_init(&fut->cond, NULL);
    pthread_mutex_init(&fut->lock, NULL);

    if (local != NULL)
    {
        pthread_mutex_lock(&local->lock);
        list_push_back(&local->task, &fut->elem);
        //pthread_cond_signal(&pool->cond); // talk to ta
        pthread_mutex_unlock(&local->lock);
    }
    else
    {
        pthread_mutex_lock(&(fut->pool)->lock);
        list_push_back(&(fut->pool)->tasks, &fut->elem);
        // //signal and unlock
        pthread_cond_signal(&(fut->pool)->cond);
        pthread_mutex_unlock(&(fut->pool)->lock);
    }


    return fut;
}

// getter function for the future
// checks to see if internal or external
// if internal, then it starts a task from the workers task list
// then it sets the state to 1 for running, and then 2 for finished. 
// it waits if its not finished.
// if its external. then it waits for the task to finish
// and then it returns the result of the future running the task
void *future_get(struct future *fut) {
    // // internal vs external (this thread vs outside thread)
    // // 0 task hasnt started. 1task is in progress, 2 task is completed

    if (local != NULL)
    {
        assert(fut != NULL);

        pthread_mutex_lock(&fut->lock);

        pthread_mutex_lock(&local->lock);
        if (fut->state == 0)
        {

            list_remove(&fut->elem);
            pthread_mutex_unlock(&local->lock);
            fut->state = 1;
            pthread_mutex_unlock(&fut->lock);
            fut->result = fut->task(fut->pool, fut->args);
            pthread_mutex_lock(&fut->lock);
            // internal, execute the thread/task
            fut->state = 2;
        }
        else
        {
            pthread_mutex_unlock(&local->lock);
        }
        while (fut->state == 1 || fut->state == 0)
        {
            //  internal, wait for future to complete
            pthread_cond_wait(&fut->cond, &fut->lock);
        }
        void *futRes = fut->result;
        pthread_mutex_unlock(&fut->lock);
        return futRes;

    }
    else
    {
        pthread_mutex_lock(&fut->lock);
        while (fut->state != 2)
        {
            // external, wait for future to complete
            pthread_cond_wait(&fut->cond, &fut->lock);
        }
        pthread_mutex_unlock(&fut->lock);

        void *futRes = fut->result;
        
        
        return futRes;
    }
}


// free method for the future
// it destroys the futures lock an condition
// and then it frees the future
void future_free(struct future *fut)
{
    // check to see if not null
    if (fut != NULL)
    {
        pthread_cond_destroy(&fut->cond);
        pthread_mutex_destroy(&fut->lock);
        free(fut);
    }
    else
    {
        return;
    }
}

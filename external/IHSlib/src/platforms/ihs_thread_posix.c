/*
 * POSIX/pthread implementation of the IHS thread/mutex/cond primitives.
 *
 * Replaces the SDL backend on UNIX so ihslib does not depend on SDL at all —
 * an SDL3 host application would otherwise clash with ihslib's SDL2 usage
 * (both SDL2 and SDL3 export the same SDL_* symbols into one process).
 *
 * Mutexes are recursive to match SDL2's SDL_mutex semantics, which ihslib's
 * locking relies on.
 */
#define _GNU_SOURCE
#include "ihs_thread.h"

#include <pthread.h>
#include <stdlib.h>

struct IHS_Thread {
    pthread_t tid;
    IHS_ThreadFunction *function;
    void *context;
};
struct IHS_Mutex { pthread_mutex_t mutex; };
struct IHS_Cond { pthread_cond_t cond; };

static void *ThreadEntry(void *arg) {
    IHS_Thread *t = arg;
    t->function(t->context);
    return NULL;
}

IHS_Thread *IHS_ThreadCreate(IHS_ThreadFunction *function, const char *name, void *context) {
    (void) name;
    IHS_Thread *t = calloc(1, sizeof(IHS_Thread));
    t->function = function;
    t->context = context;
    if (pthread_create(&t->tid, NULL, ThreadEntry, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void IHS_ThreadJoin(IHS_Thread *thread) {
    if (thread == NULL) return;
    pthread_join(thread->tid, NULL);
    free(thread);
}

IHS_Mutex *IHS_MutexCreate() {
    IHS_Mutex *m = calloc(1, sizeof(IHS_Mutex));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return m;
}

void IHS_MutexDestroy(IHS_Mutex *mutex) {
    if (mutex == NULL) return;
    pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
}

bool IHS_MutexLock(IHS_Mutex *mutex) {
    return pthread_mutex_lock(&mutex->mutex) == 0;
}

bool IHS_MutexUnlock(IHS_Mutex *mutex) {
    return pthread_mutex_unlock(&mutex->mutex) == 0;
}

IHS_Cond *IHS_CondCreate() {
    IHS_Cond *c = calloc(1, sizeof(IHS_Cond));
    pthread_cond_init(&c->cond, NULL);
    return c;
}

void IHS_CondDestroy(IHS_Cond *cond) {
    if (cond == NULL) return;
    pthread_cond_destroy(&cond->cond);
    free(cond);
}

void IHS_CondSignal(IHS_Cond *cond) {
    pthread_cond_signal(&cond->cond);
}

bool IHS_CondWait(IHS_Cond *cond, IHS_Mutex *mutex) {
    return pthread_cond_wait(&cond->cond, &mutex->mutex) == 0;
}

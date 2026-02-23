#include "rkav/bqueue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int bq_init(BQueue *q,size_t capacity)
{
    if(!q || capacity == 0) return -1;

    memset(q,0,sizeof(*q));

    q->items = (void **)calloc(capacity, sizeof(void*));
    if(!q->items) return -1;
    
    q->capacity = capacity;
    q->size = 0;
    q->head = 0;
    q->tail = 0;
    q->closed = 0;

    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);

    return 0;
}

void bq_close(BQueue *q)
{
    if(!q) return;

    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

void bq_destroy(BQueue *q)
{
    if(!q) return;

    pthread_mutex_lock(&q->mtx);
    void **items = q->items;
    q->items = NULL;
    pthread_mutex_unlock(&q->mtx);

    if(items) free(items);

    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    memset(q,0,sizeof(*q));
}

int bq_push(BQueue *q,void *item)
{
    if(!q) return -1;
    pthread_mutex_lock(&q->mtx);

    while(!q->closed && q->size == q->capacity){
        pthread_mutex_unlock(&q->mtx);
        return -1; /* 队列满，直接返回失败 */
    }

    if(q->closed){
        pthread_mutex_unlock(&q->mtx);
        return -1; /* 队列已关闭，不能再 push */
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}


int bq_try_push(BQueue *q, void *item)
{
    if (!q) return -1;
    pthread_mutex_lock(&q->mtx);

    if (q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    if (q->size == q->capacity) {
        pthread_mutex_unlock(&q->mtx);
        return 1;
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int bq_pop(BQueue *q,void **out)
{
    if (!q || !out) return -1;
    pthread_mutex_lock(&q->mtx);

    while (!q->closed && q->size == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }

    if (q->size == 0 && q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }

    void *item = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->size--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);

    *out = item;
    return 1;
}

size_t bq_size(BQueue *q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->mtx);
    size_t s = q->size;
    pthread_mutex_unlock(&q->mtx);
    return s;
}

size_t bq_capacity(BQueue *q)
{
    if (!q) return 0;
    return q->capacity;
}

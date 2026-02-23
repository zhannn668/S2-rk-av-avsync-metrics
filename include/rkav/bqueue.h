#pragma once

#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void **items;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    int closed;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} BQueue;

// 返回值约定：
//  - push: 0=成功, 1=队列满(try_push), -1=队列已关闭
//  - pop:  1=成功取到元素, 0=队列已关闭且已空, -1=错误

int    bq_init(BQueue *q, size_t capacity);
void   bq_close(BQueue *q);
void   bq_destroy(BQueue *q);

int    bq_push(BQueue *q, void *item);      // 阻塞直到有空间 / 或 close
int    bq_try_push(BQueue *q, void *item);  // 不阻塞
int    bq_pop(BQueue *q, void **out);       // 阻塞直到有元素 / 或 close

size_t bq_size(BQueue *q);
size_t bq_capacity(BQueue *q);


#ifdef __cplusplus
}
#endif
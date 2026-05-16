#include "perf_internal.h"
#include <string.h>

void ring_init(ring_buffer_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
    pthread_mutex_init(&rb->mu, NULL);
}

perf_status_t ring_push(ring_buffer_t *rb, const perf_sample_t *s)
{
    pthread_mutex_lock(&rb->mu);

    size_t next = (rb->head + 1) % RING_CAPACITY;
    if (next == rb->tail) {
        /* Buffer lleno: descartamos la muestra más antigua avanzando tail */
        rb->tail = (rb->tail + 1) % RING_CAPACITY;
    }
    rb->slots[rb->head] = *s;
    rb->head = next;

    pthread_mutex_unlock(&rb->mu);
    return PERF_OK;
}

/*
 * Drena hasta max muestras del ring buffer hacia dst.
 * Retorna el número de muestras copiadas.
 */
size_t ring_drain(ring_buffer_t *rb, perf_sample_t *dst, size_t max)
{
    pthread_mutex_lock(&rb->mu);

    size_t count = 0;
    while (rb->tail != rb->head && count < max) {
        dst[count++] = rb->slots[rb->tail];
        rb->tail = (rb->tail + 1) % RING_CAPACITY;
    }

    pthread_mutex_unlock(&rb->mu);
    return count;
}

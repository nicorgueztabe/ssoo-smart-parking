#ifndef HEADER_FILE
#define HEADER_FILE

#include <pthread.h>
#include <stdlib.h>

// Structure representing a vehicle (queue element)
struct car {
  char plate[20];
  int gate;
  int priority;
};

// Queue struct.
typedef struct queue_t {
  struct car *data;
  int head;
  int tail;
  int count;
  int size;
  pthread_mutex_t mutex;
  pthread_cond_t cond_not_full;
  pthread_cond_t cond_not_empty;
  int terminado; //anadido como flag para comprobar si ha terminado la cola
} queue;

queue *queue_init(int size);
int queue_destroy(queue *q);
int queue_put(queue *q, struct car *elem);
struct car *queue_get(queue *q);
int queue_empty(queue *q);
int queue_full(queue *q);

#endif

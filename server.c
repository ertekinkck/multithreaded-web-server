#include "blg312e.h"
#include "request.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>


pthread_mutex_t mutex;
sem_t empty, full;
int *buffer;
int buffer_size = 0;
int buffer_index_in = 0;
int buffer_index_out = 0;

void getargs(int *port, int *num_threads, int *buffer_size, int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <port> <threads> <buffers>\n", argv[0]);
        exit(1);
    }
    *port = atoi(argv[1]);
    *num_threads = atoi(argv[2]);
    *buffer_size = atoi(argv[3]);
}

void push_to_buffer(int connfd) {
    sem_wait(&empty);
    pthread_mutex_lock(&mutex);

    buffer[buffer_index_in] = connfd;
    buffer_index_in = (buffer_index_in + 1) % buffer_size;

    pthread_mutex_unlock(&mutex);
    sem_post(&full);
}

int pop_from_buffer() {
    sem_wait(&full);
    pthread_mutex_lock(&mutex);

    int connfd = buffer[buffer_index_out];
    buffer_index_out = (buffer_index_out + 1) % buffer_size;

    pthread_mutex_unlock(&mutex);
    sem_post(&empty);

    return connfd;
}

void* client_handler(void* arg) {
    while (1) {
        int connfd = pop_from_buffer();
        requestHandle(connfd);
        //Close(connfd);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int port, num_threads;
    pthread_t *thread_pool;

    getargs(&port, &num_threads, &buffer_size, argc, argv);

    pthread_mutex_init(&mutex, NULL);
    sem_init(&empty, 0, buffer_size);
    sem_init(&full, 0, 0);
    
    buffer = (int *)malloc(sizeof(int) * buffer_size);
    if (buffer == NULL) {
        fprintf(stderr, "Error: malloc failed\n");
        exit(1);
    }

    thread_pool = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    if (thread_pool == NULL) {
        fprintf(stderr, "Error: malloc failed\n");
        exit(1);
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&thread_pool[i], NULL, client_handler, NULL) != 0) {
            fprintf(stderr, "Error: pthread_create failed\n");
            exit(1);
        }
    }

    int listenfd = Open_listenfd(port);
    while (1) {
        struct sockaddr_in clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        push_to_buffer(connfd);
    }

    Close(listenfd);
    free(thread_pool);
    pthread_mutex_destroy(&mutex);
    sem_destroy(&empty);
    sem_destroy(&full);

    return 0;
}
#include "blg312e.h"
#include "request.h"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_BUFFER_SIZE 100

typedef struct {
    int connfd;
    char filename[MAXLINE];
    struct timespec mod_time;
    off_t file_size;
} request_t;

pthread_mutex_t mutex;
sem_t empty, full;
request_t buffer[MAX_BUFFER_SIZE];
int buffer_size = MAX_BUFFER_SIZE;
int buffer_index = 0;
char *sched_policy;

void getargs(int *port, int *num_threads, int *buffer_size, char **sched_policy, int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <port> <num_threads> <buffer_size> <sched_policy>\n", argv[0]);
        exit(1);
    }
    *port = atoi(argv[1]);
    *num_threads = atoi(argv[2]);
    *buffer_size = atoi(argv[3]);
    *sched_policy = argv[4];
}

void push_to_buffer(request_t req) {
    sem_wait(&empty);
    pthread_mutex_lock(&mutex);
    
    buffer[buffer_index] = req;
    buffer_index = (buffer_index + 1) % buffer_size;
    
    pthread_mutex_unlock(&mutex);
    sem_post(&full);
}

request_t pop_from_buffer() {
    sem_wait(&full);
    pthread_mutex_lock(&mutex);

    request_t req;
    int index = -1;
    if (strcmp(sched_policy, "FIFO") == 0) { // FIFO
        index = (buffer_index - 1 + buffer_size) % buffer_size;
    } else if (strcmp(sched_policy, "RFF") == 0) { // RFF
        struct timespec latest_mod_time = {0, 0};
        for (int i = 0; i < buffer_size; i++) {
            if (buffer[i].connfd != -1 && 
                (latest_mod_time.tv_sec < buffer[i].mod_time.tv_sec || 
                 (latest_mod_time.tv_sec == buffer[i].mod_time.tv_sec && 
                  latest_mod_time.tv_nsec < buffer[i].mod_time.tv_nsec))) {
                latest_mod_time = buffer[i].mod_time;
                index = i;
            }
        }
    } else if (strcmp(sched_policy, "SFF") == 0) { // SFF
        off_t smallest_size = __LONG_MAX__;
        for (int i = 0; i < buffer_size; i++) {
            if (buffer[i].connfd != -1 && buffer[i].file_size < smallest_size) {
                smallest_size = buffer[i].file_size;
                index = i;
            }
        }
    }

    if (index != -1) {
        req = buffer[index];
        buffer[index].connfd = -1; // Mark as handled
    }

    pthread_mutex_unlock(&mutex);
    sem_post(&empty);

    return req;
}

void *client_handler(void *arg) {
    while (1) {
        request_t req = pop_from_buffer();
        requestHandle(req.connfd);
        Close(req.connfd);
    }
    return NULL;
}

void extract_filename(char *request, char *filename) {
    sscanf(request, "GET %s HTTP/1.1", filename);
}

void handle_client(int connfd) {
    char buf[MAXLINE], filename[MAXLINE];
    rio_t rio;
    struct stat file_stat;

    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);

    extract_filename(buf, filename);
    
    if (stat(filename, &file_stat) < 0) {
        fprintf(stderr, "Error: stat failed for %s\n", filename);
        Close(connfd);
        return;
    }

    request_t req;
    req.connfd = connfd;
    strncpy(req.filename, filename, MAXLINE);
    req.mod_time = file_stat.st_mtim;
    req.file_size = file_stat.st_size;

    push_to_buffer(req);
}

int main(int argc, char *argv[]) {
    int port, num_threads;
    pthread_t *thread_pool;
    struct sockaddr_in clientaddr;
    socklen_t clientlen;

    getargs(&port, &num_threads, &buffer_size, &sched_policy, argc, argv);

    pthread_mutex_init(&mutex, NULL);
    sem_init(&empty, 0, buffer_size);
    sem_init(&full, 0, 0);

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
        clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        handle_client(connfd);
    }

    Close(listenfd);
    free(thread_pool);
    pthread_mutex_destroy(&mutex);
    sem_destroy(&empty);
    sem_destroy(&full);

    return 0;
}

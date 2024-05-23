#include "blg312e.h"
#include "request.h"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_BUFFER_SIZE 10

typedef struct {
    int connfd;
    char filename[MAXLINE];
    struct timespec mod_time;
    time_t send_time;
    off_t file_size;
} request_t;

// Initializing Mutex Lock in main function
pthread_mutex_t mutex;

sem_t empty, full, thread_mutex;

int buffer_size=0;
request_t *buffer = NULL;

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
    if(*buffer_size > 10){
        *buffer_size = MAX_BUFFER_SIZE;
    }
    *sched_policy = argv[4];
}

void push_to_buffer(request_t* req) {
    sem_wait(&empty);
    pthread_mutex_lock(&mutex);

    // Check if the buffer is full
    if (buffer_index >= buffer_size) {
        // Buffer is full, handle the error (e.g., drop the request or wait)
        fprintf(stderr, "Error: Buffer is full. Dropping the request...\n");
        pthread_mutex_unlock(&mutex);
        sem_post(&empty);
        free(req);
        return;
    }
    
    buffer[buffer_index++] = *req;
    
    pthread_mutex_unlock(&mutex);
    sem_post(&full);

    printf("Pushed to buffer: %s\n", req->filename); // Debugging statement
}

request_t *pop_from_buffer() {
    sem_wait(&full);
    pthread_mutex_lock(&mutex);

    request_t *req = malloc(sizeof(request_t)); // Allocate memory for req

    int index = -1;


    if (strcmp(sched_policy, "FIFO") == 0) { // FIFO
        index = buffer_index;
        buffer_index++;
    } else if (strcmp(sched_policy, "RFF") == 0) { // RFF
        struct timespec latest_mod_time = {0, 0};
        for (int i = 0; i < buffer_size; i++) {
            int idx = buffer_index+i;
            if (buffer[idx].connfd != -1 && 
                (latest_mod_time.tv_sec < buffer[idx].mod_time.tv_sec || 
                 (latest_mod_time.tv_sec == buffer[idx].mod_time.tv_sec && 
                  latest_mod_time.tv_nsec < buffer[idx].mod_time.tv_nsec))) {
                latest_mod_time = buffer[idx].mod_time;
                index = idx;
            }
        }
    } else if (strcmp(sched_policy, "SFF") == 0) { // SFF
        off_t smallest_size = __LONG_MAX__;
        for (int i = 0; i < buffer_size; i++) {
            int idx = buffer_index+i;
            if (buffer[idx].connfd != -1 && buffer[idx].file_size < smallest_size) {
                smallest_size = buffer[idx].file_size;
                index = idx;
            }
        }
    }

    if (index != -1) {
        *req = buffer[index];
        buffer[index].connfd = -1; // Mark as handled
    }

    pthread_mutex_unlock(&mutex);
    sem_post(&empty);

    printf("Popped from buffer: %s\n", req->filename); // Debugging statement

    return req;
}



void *client_handler(void *arg) {
    while (1) {
        request_t *req = pop_from_buffer();
        printf("Handling request: %s\n", req->filename); // Debugging statement
        printf("Handling request: %d\n", req->connfd); // Debugging statement

        requestHandle(req->connfd);
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
    struct tm *tm_info;
    char mod_time_str[64];

    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);

    extract_filename(buf, filename);    
    printf("Received request line: %s\n", buf); // Debugging statement

    if (stat(filename, &file_stat) < 0) {
        fprintf(stderr, "Error: stat failed for %s\n", filename);
        Close(connfd);
        return;
    }

    // Convert the modification time to a readable format
    tm_info = localtime(&file_stat.st_mtime);
    strftime(mod_time_str, sizeof(mod_time_str), "%H:%M:%S", tm_info);
    

    request_t *req = malloc(sizeof(request_t));
    if (req == NULL) {
        fprintf(stderr, "Error: malloc failed\n");
        Close(connfd);
        return;
    }

    req->connfd = connfd;
    printf("\nCONNFD: %d\n", connfd);
    strncpy(req->filename, filename, MAXLINE);
    req->mod_time = file_stat.st_mtim;
    req->file_size = file_stat.st_size;

    time_t raw_time;
    struct tm *local_time;
    char send_time_str[64]; // Buffer to store the formatted time
    time(&raw_time); // Get current time
    local_time = localtime(&raw_time);
    req->send_time = raw_time;
    strftime(send_time_str, sizeof(send_time_str), "%H:%M:%S", local_time);


    printf("Received request: %s (size: %ld, modified: %s, sending time: %s)\n", filename, file_stat.st_size, mod_time_str, send_time_str);
    push_to_buffer(req);
}

int main(int argc, char *argv[]) {
    int port, num_threads;
    pthread_t *thread_pool;
    struct sockaddr_in clientaddr;
    socklen_t clientlen;

    getargs(&port, &num_threads, &buffer_size, &sched_policy, argc, argv);

    // initialize mutex lock
    pthread_mutex_init(&mutex, NULL);
    sem_init(&empty, 0, buffer_size);
    sem_init(&full, 0, 0);
    sem_init(&thread_mutex, 0, num_threads);

    buffer = (request_t*)malloc(sizeof(request_t) * (buffer_size));
    thread_pool = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);

    if (buffer == NULL) {
        fprintf(stderr, "Error: buffer malloc failed\n");
        exit(1);
    }
    if (thread_pool == NULL) {
        fprintf(stderr, "Error: malloc failed\n");
        exit(1);
    }

    for (int i = 0; i < num_threads; i++) {
        sem_wait(&thread_mutex);
        if (pthread_create(&thread_pool[i], NULL, client_handler, NULL) != 0) {
            fprintf(stderr, "Error: pthread_create failed\n");
            exit(1);
        }
    }

    int listenfd = Open_listenfd(port);
    while (1) {
        clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, (socklen_t *)&clientlen);
        printf("Accepted connection from client\n"); // Debugging statement
        handle_client(connfd);
    }

    //Close(listenfd);
    free(thread_pool);
    pthread_mutex_destroy(&mutex);
    sem_destroy(&empty);
    sem_destroy(&full);
    sem_destroy(&thread_mutex);

    return 0;
}

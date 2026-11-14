#ifndef HEADERS_H
#define HEADERS_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>

#define SHKEY 300
#define PROCESS_MSG_KEY 400
#define MAX_RESOURCES_PER_PROCESS 10
#define MAX_RESOURCE_NAME_LEN 32
#define MAX_PROCESSES 1000

typedef enum
{
    ALGO_SJF = 1,
    ALGO_RR = 2,
    ALGO_HPF = 3,
    ALGO_HPF_DEMOTION = 4
} SchedulingAlgorithm;

typedef enum
{
    STATE_READY,
    STATE_RUNNING,
    STATE_BLOCKED,
    STATE_STOPPED,
    STATE_FINISHED
} ProcessState;

typedef struct
{
    char resource_id[MAX_RESOURCE_NAME_LEN];
    int capture_time;
    int release_time;
} ResourceReq;

typedef struct
{
    int id;
    int arrival_time;
    int runtime;
    int priority;
    int resource_count;
    ResourceReq resources[MAX_RESOURCES_PER_PROCESS];
} ProcessData;

typedef struct
{
    long mtype;
    ProcessData process;
} ProcessMessage;

typedef struct
{
    ProcessData data;
    pid_t pid;
    ProcessState state;

    int remaining_time;
    int execution_time;
    int wait_time;

    int start_time;
    int finish_time;

    int original_priority;
    int inherited_priority;
    int effective_priority;
    int consecutive_ready_time;

    int current_resource_idx;
    bool holds_resource;
    char held_resource_id[MAX_RESOURCE_NAME_LEN];
} PCB;

int *shmaddr;

static inline int getClk(void)
{
    return *shmaddr;
}

static inline void initClk(void)
{
    int shmid = shmget(SHKEY, 4, 0444);
    while ((int)shmid == -1)
    {
        usleep(10000);
        shmid = shmget(SHKEY, 4, 0444);
    }
    shmaddr = (int *)shmat(shmid, (void *)0, 0);
}

static inline void destroyClk(bool terminateAll)
{
    shmdt(shmaddr);
    if (terminateAll)
    {
        killpg(getpgrp(), SIGINT);
    }
}

#endif

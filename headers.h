#pragma once
#ifndef HEADERS_H
#define HEADERS_H

#include <stdio.h>      // if you don't use scanf/printf change this include
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <math.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
// C23 provides bool, true, and false natively
#elif defined(__bool_true_false_are_defined) || defined(__cplusplus)
// Already defined
#else
#include <stdbool.h>
#endif

#define SHKEY 300
#define PROCESS_MSG_KEY 400

#define MAX_RESOURCES_PER_PROCESS 10
#define MAX_RESOURCE_NAME_LEN 32
#define MAX_PROCESSES 1000

// ==========================================
// Scheduling Algorithms
// ==========================================
typedef enum {
    ALGO_SJF = 1,
    ALGO_RR = 2,
    ALGO_HPF = 3,
    ALGO_HPF_DEMOTION = 4
} SchedulingAlgorithm;

// ==========================================
// Process States
// ==========================================
typedef enum {
    STATE_READY,
    STATE_RUNNING,
    STATE_BLOCKED,
    STATE_STOPPED,
    STATE_FINISHED
} ProcessState;

// ==========================================
// Resource Requirement for a Process
// ==========================================
typedef struct {
    char resource_id[MAX_RESOURCE_NAME_LEN];
    int capture_time; // measured in process executed time
    int release_time; // measured in process executed time
} ResourceReq;

// ==========================================
// Process Data (Input / IPC Data)
// ==========================================
typedef struct {
    int id;
    int arrival_time;
    int runtime;
    int priority;
    int resource_count;
    ResourceReq resources[MAX_RESOURCES_PER_PROCESS];
} ProcessData;

// ==========================================
// IPC Message Structure (Generator -> Scheduler)
// ==========================================
typedef struct {
    long mtype;
    ProcessData process;
} ProcessMessage;

// ==========================================
// Process Control Block (PCB)
// ==========================================
typedef struct {
    ProcessData data;
    pid_t pid;
    ProcessState state;

    int remaining_time;
    int execution_time;
    int wait_time;

    int start_time;
    int finish_time;

    // Priority tracking
    int original_priority;
    int inherited_priority;     // Highest priority among waiters; -1 if none
    int effective_priority;     // Used in bonus demotion/aging
    int consecutive_ready_time; // Counter 'w' for bonus aging

    // Resource tracking
    int current_resource_idx;   // Index of current/next resource entry
    bool holds_resource;
    char held_resource_id[MAX_RESOURCE_NAME_LEN];
} PCB;

///==============================
//don't mess with this variable//
int * shmaddr;                 //
//===============================

static inline int getClk()
{
    return *shmaddr;
}

/*
 * All processes call this function at the beginning to establish communication between them and the clock module.
 * Again, remember that the clock is only emulation!
*/
static inline void initClk()
{
    int shmid = shmget(SHKEY, 4, 0444);
    while ((int)shmid == -1)
    {
        // Make sure that the clock exists
        usleep(10000); // 10ms
        shmid = shmget(SHKEY, 4, 0444);
    }
    shmaddr = (int *) shmat(shmid, (void *)0, 0);
}

/*
 * All processes call this function at the end to release the communication
 * resources between them and the clock module.
 * Again, Remember that the clock is only emulation!
 * Input: terminateAll: a flag to indicate whether that this is the end of simulation.
 *                      It terminates the whole system and releases resources.
*/
static inline void destroyClk(bool terminateAll)
{
    shmdt(shmaddr);
    if (terminateAll)
    {
        killpg(getpgrp(), SIGINT);
    }
}

#endif

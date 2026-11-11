#include "headers.h"

// ========================================================
// Global State & Simulation Variables
// ========================================================
int chosen_algo = 1;
int total_processes = 0;
int completed_processes = 0;
int quantum = 3;
int context_switch_time = 1;
int msg_queue_id = -1;

FILE *log_file = NULL;

// Process Storage
PCB all_pcbs[MAX_PROCESSES];
int all_pcb_count = 0;

// Ready Queue
PCB *ready_queue[MAX_PROCESSES];
int ready_count = 0;

// Blocked Queue (for HPF resource waiting)
PCB *blocked_queue[MAX_PROCESSES];
int blocked_count = 0;

// CPU & Context Switch State
PCB *running_pcb = NULL;
PCB *switching_to_pcb = NULL;
int cs_remaining = 0;
int quantum_remaining = 0;
bool cpu_was_idle = true;

// System Resources
typedef struct {
    char id[MAX_RESOURCE_NAME_LEN];
    bool is_held;
    int holder_id; // Process ID of holder, -1 if free
} SysResource;

SysResource system_resources[MAX_PROCESSES * MAX_RESOURCES_PER_PROCESS];
int system_resource_count = 0;

// ========================================================
// Helper Functions: Resources
// ========================================================
SysResource *getOrCreateResource(const char *res_id)
{
    for (int i = 0; i < system_resource_count; i++)
    {
        if (strcmp(system_resources[i].id, res_id) == 0)
            return &system_resources[i];
    }
    SysResource *r = &system_resources[system_resource_count++];
    strncpy(r->id, res_id, MAX_RESOURCE_NAME_LEN - 1);
    r->id[MAX_RESOURCE_NAME_LEN - 1] = '\0';
    r->is_held = false;
    r->holder_id = -1;
    return r;
}

// ========================================================
// Helper Functions: Priority & Aging
// ========================================================
int getEffectivePriority(PCB *p)
{
    if (chosen_algo == ALGO_HPF_DEMOTION)
    {
        int w = p->consecutive_ready_time;
        int bonus = (w < 20) ? 0 : ((w - 20) / 5) + 1;
        int eff = p->original_priority - bonus;
        if (eff < 0) eff = 0;
        return eff;
    }
    return p->original_priority;
}

// For comparison: returns the active priority of a process
int getActivePriority(PCB *p)
{
    int base_prio = (chosen_algo == ALGO_HPF_DEMOTION) ? getEffectivePriority(p) : p->original_priority;

    // If holding a resource with priority inheritance active, use the better (smaller)
    if (p->holds_resource && p->inherited_priority != -1)
    {
        if (p->inherited_priority < base_prio)
            return p->inherited_priority;
    }
    return base_prio;
}

// Recalculate inherited priority for a resource holder based on waiting blocked processes
void updateInheritedPriority(PCB *holder)
{
    if (!holder || !holder->holds_resource)
        return;

    int best_inherited = -1;
    for (int i = 0; i < blocked_count; i++)
    {
        PCB *b = blocked_queue[i];
        int r_idx = b->current_resource_idx;
        if (r_idx < b->data.resource_count &&
            strcmp(b->data.resources[r_idx].resource_id, holder->held_resource_id) == 0)
        {
            int prio = b->original_priority;
            if (best_inherited == -1 || prio < best_inherited)
            {
                best_inherited = prio;
            }
        }
    }
    holder->inherited_priority = best_inherited;
}

// ========================================================
// Helper Functions: Ready Queue Management
// ========================================================
void readyQueuePush(PCB *p)
{
    p->state = STATE_READY;
    ready_queue[ready_count++] = p;
}

PCB *readyQueuePopAt(int index)
{
    if (index < 0 || index >= ready_count)
        return NULL;
    PCB *p = ready_queue[index];
    for (int i = index; i < ready_count - 1; i++)
    {
        ready_queue[i] = ready_queue[i + 1];
    }
    ready_count--;
    return p;
}

// Select next process based on the scheduling algorithm
int selectNextProcessIndex()
{
    if (ready_count == 0)
        return -1;

    if (chosen_algo == ALGO_SJF)
    {
        // Smallest runtime. Ties: earlier arrival, then smaller ID
        int best_idx = 0;
        for (int i = 1; i < ready_count; i++)
        {
            PCB *curr = ready_queue[i];
            PCB *best = ready_queue[best_idx];

            if (curr->data.runtime < best->data.runtime)
                best_idx = i;
            else if (curr->data.runtime == best->data.runtime)
            {
                if (curr->data.arrival_time < best->data.arrival_time)
                    best_idx = i;
                else if (curr->data.arrival_time == best->data.arrival_time && curr->data.id < best->data.id)
                    best_idx = i;
            }
        }
        return best_idx;
    }
    else if (chosen_algo == ALGO_RR)
    {
        // First In First Out
        return 0;
    }
    else // ALGO_HPF or ALGO_HPF_DEMOTION
    {
        // Highest priority (smallest number). Ties: smaller ID
        int best_idx = 0;
        for (int i = 1; i < ready_count; i++)
        {
            PCB *curr = ready_queue[i];
            PCB *best = ready_queue[best_idx];

            int curr_prio = getActivePriority(curr);
            int best_prio = getActivePriority(best);

            if (curr_prio < best_prio)
                best_idx = i;
            else if (curr_prio == best_prio)
            {
                if (curr->data.id < best->data.id)
                    best_idx = i;
            }
        }
        return best_idx;
    }
}

// ========================================================
// Helper Functions: Logging
// ========================================================
void logProcessEvent(int current_time, PCB *p, const char *event_type)
{
    int wait = current_time - p->data.arrival_time - p->execution_time;
    p->wait_time = wait;

    if (strcmp(event_type, "finished") == 0)
    {
        int ta = current_time - p->data.arrival_time;
        double wta = (double)ta / p->data.runtime;
        fprintf(log_file, "At time %d process %d %s arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
                current_time, p->data.id, event_type, p->data.arrival_time,
                p->data.runtime, p->remaining_time, wait, ta, wta);
    }
    else
    {
        fprintf(log_file, "At time %d process %d %s arr %d total %d remain %d wait %d\n",
                current_time, p->data.id, event_type, p->data.arrival_time,
                p->data.runtime, p->remaining_time, wait);
    }
    fflush(log_file);
}

// ========================================================
// Dispatching & Process Control
// ========================================================
void dispatchProcess(PCB *p, int current_time)
{
    p->consecutive_ready_time = 0; // Reset aging counter 'w' upon dispatch
    p->state = STATE_RUNNING;
    running_pcb = p;

    if (chosen_algo == ALGO_RR)
        quantum_remaining = quantum;

    if (p->pid == -1)
    {
        // First time starting process
        p->start_time = current_time;
        logProcessEvent(current_time, p, "started");

        pid_t pid = fork();
        if (pid == 0)
        {
            char runtime_str[16];
            sprintf(runtime_str, "%d", p->data.runtime);
            execl("./process.out", "process.out", runtime_str, NULL);
            perror("Error executing process.out");
            exit(-1);
        }
        p->pid = pid;
    }
    else
    {
        // Resuming previously stopped process
        logProcessEvent(current_time, p, "resumed");
        kill(p->pid, SIGCONT);
    }
}

void stopProcess(PCB *p, int current_time)
{
    if (p && p->state == STATE_RUNNING)
    {
        p->state = STATE_STOPPED;
        logProcessEvent(current_time, p, "stopped");
        kill(p->pid, SIGSTOP);
    }
}

// ========================================================
// Performance Statistics Generation
// ========================================================
void generatePerfReport(int finish_time)
{
    FILE *perf_file = fopen("scheduler.perf", "w");
    if (!perf_file)
    {
        perror("Error creating scheduler.perf");
        return;
    }

    int total_runtime = 0;
    double sum_wta = 0.0;
    int sum_wait = 0;

    for (int i = 0; i < all_pcb_count; i++)
    {
        PCB *p = &all_pcbs[i];
        total_runtime += p->data.runtime;

        int ta = p->finish_time - p->data.arrival_time;
        double wta = (double)ta / p->data.runtime;
        sum_wta += wta;

        int wait = p->finish_time - p->data.arrival_time - p->data.runtime;
        sum_wait += wait;
    }

    double cpu_util = ((double)total_runtime / finish_time) * 100.0;
    double avg_wta = sum_wta / all_pcb_count;
    double avg_wait = (double)sum_wait / all_pcb_count;

    // Population standard deviation: divide by N, not N - 1
    double variance = 0.0;
    for (int i = 0; i < all_pcb_count; i++)
    {
        PCB *p = &all_pcbs[i];
        int ta = p->finish_time - p->data.arrival_time;
        double wta = (double)ta / p->data.runtime;
        variance += (wta - avg_wta) * (wta - avg_wta);
    }
    variance /= all_pcb_count;
    double std_wta = sqrt(variance);

    fprintf(perf_file, "CPU utilization = %d%%\n", (int)floor(cpu_util));
    fprintf(perf_file, "Avg WTA = %.2f\n", avg_wta);
    fprintf(perf_file, "Avg Waiting = %d\n", (int)floor(avg_wait));
    fprintf(perf_file, "Std WTA = %.2f\n", std_wta);

    fclose(perf_file);
}

// ========================================================
// Main Scheduler Loop
// ========================================================
int main(int argc, char *argv[])
{
    if (argc >= 5)
    {
        chosen_algo = atoi(argv[1]);
        total_processes = atoi(argv[2]);
        quantum = atoi(argv[3]);
        context_switch_time = atoi(argv[4]);
    }

    initClk();

    msg_queue_id = msgget(PROCESS_MSG_KEY, 0666);
    if (msg_queue_id == -1)
    {
        perror("Error attaching to message queue in scheduler");
        exit(-1);
    }

    log_file = fopen("scheduler.log", "w");
    if (!log_file)
    {
        perror("Error creating scheduler.log");
        exit(-1);
    }

    int sim_time = 0;
    int last_finish_time = 0;

    // Simulation runs until all processes have finished
    while (completed_processes < total_processes)
    {
        int current_time = getClk();
        if (current_time <= sim_time)
        {
            usleep(20000); // 20ms pause
            continue;
        }

        // Advance simulation second-by-second
        while (sim_time < current_time && completed_processes < total_processes)
        {
            sim_time++;
            usleep(30000); // 30ms delay to guarantee generator has sent all arrivals for sim_time

            // ----------------------------------------------------
            // If currently in a context switch:
            // ----------------------------------------------------
            if (cs_remaining > 0)
            {
                // Age ready processes during context switch
                for (int i = 0; i < ready_count; i++)
                    ready_queue[i]->consecutive_ready_time++;

                cs_remaining--;
                if (cs_remaining == 0 && switching_to_pcb != NULL)
                {
                    // Context switch has completed: dispatch target process
                    PCB *next = switching_to_pcb;
                    switching_to_pcb = NULL;
                    dispatchProcess(next, sim_time);
                    cpu_was_idle = false;
                }
            }
            else if (running_pcb != NULL)
            {
                // Process executed for 1 second of CPU time
                running_pcb->execution_time++;
                running_pcb->remaining_time--;
                if (chosen_algo == ALGO_RR)
                    quantum_remaining--;

                // Age ready processes while CPU is busy
                for (int i = 0; i < ready_count; i++)
                    ready_queue[i]->consecutive_ready_time++;
            }
            else
            {
                // CPU is idle: age ready processes if any
                for (int i = 0; i < ready_count; i++)
                    ready_queue[i]->consecutive_ready_time++;
            }

            // ====================================================
            // Phase 1: Running Process's Own Events
            // Order: Release resource -> Capture resource -> Finish
            // ====================================================
            if (running_pcb != NULL && cs_remaining == 0)
            {
                PCB *curr = running_pcb;

                // 1. Check Resource Release
                if (curr->holds_resource)
                {
                    int r_idx = curr->current_resource_idx;
                    if (r_idx < curr->data.resource_count &&
                        curr->execution_time == curr->data.resources[r_idx].release_time)
                    {
                        // Release resource
                        SysResource *res = getOrCreateResource(curr->held_resource_id);
                        res->is_held = false;
                        res->holder_id = -1;

                        curr->holds_resource = false;
                        curr->held_resource_id[0] = '\0';
                        curr->inherited_priority = -1;
                        curr->current_resource_idx++;

                        // Direct transfer to highest-priority waiting blocked process
                        int best_blocked_idx = -1;
                        for (int i = 0; i < blocked_count; i++)
                        {
                            PCB *b = blocked_queue[i];
                            int br_idx = b->current_resource_idx;
                            if (br_idx < b->data.resource_count &&
                                strcmp(b->data.resources[br_idx].resource_id, res->id) == 0)
                            {
                                if (best_blocked_idx == -1)
                                    best_blocked_idx = i;
                                else
                                {
                                    int p1 = b->original_priority;
                                    int p2 = blocked_queue[best_blocked_idx]->original_priority;
                                    if (p1 < p2 || (p1 == p2 && b->data.id < blocked_queue[best_blocked_idx]->data.id))
                                        best_blocked_idx = i;
                                }
                            }
                        }

                        if (best_blocked_idx != -1)
                        {
                            PCB *unblocked = blocked_queue[best_blocked_idx];
                            // Remove from blocked
                            for (int i = best_blocked_idx; i < blocked_count - 1; i++)
                                blocked_queue[i] = blocked_queue[i + 1];
                            blocked_count--;

                            // Transfer resource
                            res->is_held = true;
                            res->holder_id = unblocked->data.id;
                            unblocked->holds_resource = true;
                            strncpy(unblocked->held_resource_id, res->id, MAX_RESOURCE_NAME_LEN - 1);
                            updateInheritedPriority(unblocked);

                            // Unblocked process becomes Ready
                            readyQueuePush(unblocked);
                        }
                    }
                }

                // 2. Check Resource Capture
                if (!curr->holds_resource && curr->current_resource_idx < curr->data.resource_count)
                {
                    int r_idx = curr->current_resource_idx;
                    if (curr->execution_time == curr->data.resources[r_idx].capture_time)
                    {
                        SysResource *res = getOrCreateResource(curr->data.resources[r_idx].resource_id);
                        if (!res->is_held)
                        {
                            // Capture immediately
                            res->is_held = true;
                            res->holder_id = curr->data.id;
                            curr->holds_resource = true;
                            strncpy(curr->held_resource_id, res->id, MAX_RESOURCE_NAME_LEN - 1);
                            updateInheritedPriority(curr);
                        }
                        else
                        {
                            // Resource busy: process blocks
                            stopProcess(curr, sim_time);
                            curr->state = STATE_BLOCKED;
                            blocked_queue[blocked_count++] = curr;
                            running_pcb = NULL;

                            // Update priority inheritance of the resource holder
                            for (int i = 0; i < all_pcb_count; i++)
                            {
                                if (all_pcbs[i].data.id == res->holder_id)
                                {
                                    updateInheritedPriority(&all_pcbs[i]);
                                    break;
                                }
                            }
                        }
                    }
                }

                // 3. Check Finish
                if (curr->remaining_time == 0)
                {
                    int status;
                    waitpid(curr->pid, &status, 0);

                    curr->state = STATE_FINISHED;
                    curr->finish_time = sim_time;
                    last_finish_time = sim_time;
                    logProcessEvent(sim_time, curr, "finished");
                    completed_processes++;

                    if (curr->holds_resource)
                    {
                        SysResource *res = getOrCreateResource(curr->held_resource_id);
                        res->is_held = false;
                        res->holder_id = -1;
                        curr->holds_resource = false;
                    }

                    running_pcb = NULL;
                }
            }

            // ====================================================
            // Phase 2: Arrivals from Process Generator (in ID order)
            // ====================================================
            ProcessMessage msg;
            while (msgrcv(msg_queue_id, &msg, sizeof(ProcessData), 1, IPC_NOWAIT) != -1)
            {
                PCB *p = &all_pcbs[all_pcb_count++];
                p->data = msg.process;
                p->pid = -1;
                p->state = STATE_READY;
                p->remaining_time = msg.process.runtime;
                p->execution_time = 0;
                p->wait_time = 0;
                p->start_time = -1;
                p->finish_time = -1;
                p->original_priority = msg.process.priority;
                p->inherited_priority = -1;
                p->effective_priority = msg.process.priority;
                p->consecutive_ready_time = 0;
                p->current_resource_idx = 0;
                p->holds_resource = false;
                p->held_resource_id[0] = '\0';

                readyQueuePush(p);
            }

            // ====================================================
            // Phase 3: Preemption Checks, Quantum Expiry & Next Process
            // ====================================================
            if (cs_remaining > 0)
            {
                // Still switching, do not schedule
                continue;
            }

            if (chosen_algo == ALGO_SJF)
            {
                // Non-preemptive: if a process is already running, let it continue
                if (running_pcb == NULL && ready_count > 0)
                {
                    int next_idx = selectNextProcessIndex();
                    PCB *next = readyQueuePopAt(next_idx);

                    if (cpu_was_idle)
                    {
                        // 0 overhead on idle CPU
                        dispatchProcess(next, sim_time);
                        cpu_was_idle = false;
                    }
                    else
                    {
                        // 1 sec context switch overhead
                        switching_to_pcb = next;
                        cs_remaining = context_switch_time;
                    }
                }
                else if (running_pcb == NULL && ready_count == 0)
                {
                    cpu_was_idle = true;
                }
            }
            else if (chosen_algo == ALGO_RR)
            {
                if (running_pcb != NULL)
                {
                    if (quantum_remaining <= 0)
                    {
                        // Quantum expired
                        if (ready_count > 0)
                        {
                            PCB *preempted = running_pcb;
                            stopProcess(preempted, sim_time);
                            readyQueuePush(preempted); // placed at end of queue
                            running_pcb = NULL;

                            PCB *next = readyQueuePopAt(selectNextProcessIndex());
                            switching_to_pcb = next;
                            cs_remaining = context_switch_time;
                        }
                        else
                        {
                            // Ready queue empty: continue for another quantum with no stop/switch/log
                            quantum_remaining = quantum;
                        }
                    }
                }
                else if (ready_count > 0)
                {
                    PCB *next = readyQueuePopAt(selectNextProcessIndex());
                    if (cpu_was_idle)
                    {
                        dispatchProcess(next, sim_time);
                        cpu_was_idle = false;
                    }
                    else
                    {
                        switching_to_pcb = next;
                        cs_remaining = context_switch_time;
                    }
                }
                else
                {
                    cpu_was_idle = true;
                }
            }
            else // ALGO_HPF or ALGO_HPF_DEMOTION
            {
                if (running_pcb != NULL)
                {
                    // Check strictly higher priority preemption
                    if (ready_count > 0)
                    {
                        int next_idx = selectNextProcessIndex();
                        PCB *best_ready = ready_queue[next_idx];

                        int ready_prio = getActivePriority(best_ready);
                        int running_prio = getActivePriority(running_pcb);

                        if (ready_prio < running_prio)
                        {
                            // Strictly higher priority preempts running process
                            PCB *preempted = running_pcb;
                            stopProcess(preempted, sim_time);
                            readyQueuePush(preempted);
                            running_pcb = NULL;

                            PCB *next = readyQueuePopAt(selectNextProcessIndex());
                            switching_to_pcb = next;
                            cs_remaining = context_switch_time;
                        }
                    }
                }
                else if (ready_count > 0)
                {
                    PCB *next = readyQueuePopAt(selectNextProcessIndex());
                    if (cpu_was_idle)
                    {
                        dispatchProcess(next, sim_time);
                        cpu_was_idle = false;
                    }
                    else
                    {
                        switching_to_pcb = next;
                        cs_remaining = context_switch_time;
                    }
                }
                else
                {
                    cpu_was_idle = true;
                }
            }
        }
    }

    fclose(log_file);
    generatePerfReport(last_finish_time);
    destroyClk(false);
    return 0;
}

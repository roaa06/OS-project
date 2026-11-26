#include "headers.h"

int chosen_algo = 1;
int total_processes = 0;
int completed_processes = 0;
int quantum = 3;
int context_switch_time = 1;
int msg_queue_id = -1;

FILE *log_file = NULL;

PCB all_pcbs[MAX_PROCESSES];
int all_pcb_count = 0;

typedef struct QueueNode
{
    PCB *pcb;
    struct QueueNode *next;
} QueueNode;

QueueNode *ready_head = NULL;
QueueNode *ready_tail = NULL;
int ready_count = 0;

PCB *blocked_queue[MAX_PROCESSES];
int blocked_count = 0;

PCB *running_pcb = NULL;
PCB *switching_to_pcb = NULL;
int cs_remaining = 0;
int quantum_remaining = 0;
bool cpu_was_idle = true;

// Signal notification counter for child termination sanity check
volatile sig_atomic_t termination_signals_received = 0;

void handleChildTermination(int sig)
{
    (void)sig;
    termination_signals_received++;
}

void clearReadyQueue(void)
{
    while (ready_head != NULL)
    {
        QueueNode *temp = ready_head;
        ready_head = ready_head->next;
        free(temp);
    }
    ready_tail = NULL;
    ready_count = 0;
}

void fatalError(const char *msg)
{
    fprintf(stderr, "Fatal Error: %s\n", msg);
    clearReadyQueue();
    destroyClk(true);
    exit(1);
}

typedef struct
{
    char id[MAX_RESOURCE_NAME_LEN];
    bool is_held;
    int holder_id;
} SysResource;

SysResource system_resources[MAX_PROCESSES * MAX_RESOURCES_PER_PROCESS];
int system_resource_count = 0;

SysResource *getOrCreateResource(const char *res_id)
{
    if (!res_id || res_id[0] == '\0')
    {
        return NULL;
    }
    for (int i = 0; i < system_resource_count; i++)
    {
        if (strcmp(system_resources[i].id, res_id) == 0)
        {
            return &system_resources[i];
        }
    }
    if (system_resource_count >= MAX_PROCESSES * MAX_RESOURCES_PER_PROCESS)
    {
        fatalError("system_resources capacity exceeded.");
    }
    SysResource *r = &system_resources[system_resource_count++];
    strncpy(r->id, res_id, MAX_RESOURCE_NAME_LEN - 1);
    r->id[MAX_RESOURCE_NAME_LEN - 1] = '\0';
    r->is_held = false;
    r->holder_id = -1;
    return r;
}

int getEffectivePriority(PCB *p)
{
    if (chosen_algo == ALGO_HPF_DEMOTION)
    {
        int w = p->consecutive_ready_time;
        int bonus = 0;
        if (w >= 20)
        {
            bonus = ((w - 20) / 5) + 1;
        }
        int eff = p->original_priority - bonus;
        if (eff < 0)
        {
            eff = 0;
        }
        return eff;
    }
    return p->original_priority;
}

int getActivePriority(PCB *p)
{
    int base_prio = (chosen_algo == ALGO_HPF_DEMOTION) ? getEffectivePriority(p) : p->original_priority;
    p->effective_priority = base_prio;

    if (p->holds_resource && p->inherited_priority != -1)
    {
        if (p->inherited_priority < base_prio)
        {
            return p->inherited_priority;
        }
    }
    return base_prio;
}

void updateInheritedPriority(PCB *holder)
{
    if (!holder || !holder->holds_resource)
    {
        return;
    }

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

void readyQueuePush(PCB *p)
{
    QueueNode *node = (QueueNode *)malloc(sizeof(QueueNode));
    if (!node)
    {
        fatalError("Memory allocation failed for readyQueuePush.");
    }
    node->pcb = p;
    node->next = NULL;
    p->state = STATE_READY;

    if (ready_tail == NULL)
    {
        ready_head = ready_tail = node;
    }
    else
    {
        ready_tail->next = node;
        ready_tail = node;
    }
    ready_count++;
}

PCB *readyQueuePeekBest(void)
{
    if (ready_head == NULL)
    {
        return NULL;
    }
    if (chosen_algo == ALGO_RR)
    {
        return ready_head->pcb;
    }

    QueueNode *best_node = ready_head;
    QueueNode *curr = ready_head->next;

    while (curr != NULL)
    {
        bool is_better = false;
        if (chosen_algo == ALGO_SJF)
        {
            if (curr->pcb->data.runtime < best_node->pcb->data.runtime)
            {
                is_better = true;
            }
            else if (curr->pcb->data.runtime == best_node->pcb->data.runtime)
            {
                if (curr->pcb->data.arrival_time < best_node->pcb->data.arrival_time)
                {
                    is_better = true;
                }
                else if (curr->pcb->data.arrival_time == best_node->pcb->data.arrival_time &&
                         curr->pcb->data.id < best_node->pcb->data.id)
                {
                    is_better = true;
                }
            }
        }
        else // HPF or HPF Demotion
        {
            int curr_prio = getActivePriority(curr->pcb);
            int best_prio = getActivePriority(best_node->pcb);

            if (curr_prio < best_prio)
            {
                is_better = true;
            }
            else if (curr_prio == best_prio)
            {
                if (curr->pcb->data.id < best_node->pcb->data.id)
                {
                    is_better = true;
                }
            }
        }

        if (is_better)
        {
            best_node = curr;
        }
        curr = curr->next;
    }
    return best_node->pcb;
}

PCB *readyQueuePopBest(void)
{
    if (ready_head == NULL)
    {
        return NULL;
    }

    if (chosen_algo == ALGO_RR)
    {
        QueueNode *temp = ready_head;
        PCB *p = temp->pcb;
        ready_head = ready_head->next;
        if (ready_head == NULL)
        {
            ready_tail = NULL;
        }
        free(temp);
        ready_count--;
        return p;
    }

    QueueNode *best_node = ready_head;
    QueueNode *best_prev = NULL;
    QueueNode *curr = ready_head->next;
    QueueNode *prev = ready_head;

    while (curr != NULL)
    {
        bool is_better = false;
        if (chosen_algo == ALGO_SJF)
        {
            if (curr->pcb->data.runtime < best_node->pcb->data.runtime)
            {
                is_better = true;
            }
            else if (curr->pcb->data.runtime == best_node->pcb->data.runtime)
            {
                if (curr->pcb->data.arrival_time < best_node->pcb->data.arrival_time)
                {
                    is_better = true;
                }
                else if (curr->pcb->data.arrival_time == best_node->pcb->data.arrival_time &&
                         curr->pcb->data.id < best_node->pcb->data.id)
                {
                    is_better = true;
                }
            }
        }
        else // HPF or HPF Demotion
        {
            int curr_prio = getActivePriority(curr->pcb);
            int best_prio = getActivePriority(best_node->pcb);

            if (curr_prio < best_prio)
            {
                is_better = true;
            }
            else if (curr_prio == best_prio)
            {
                if (curr->pcb->data.id < best_node->pcb->data.id)
                {
                    is_better = true;
                }
            }
        }

        if (is_better)
        {
            best_node = curr;
            best_prev = prev;
        }

        prev = curr;
        curr = curr->next;
    }

    if (best_prev == NULL)
    {
        ready_head = best_node->next;
    }
    else
    {
        best_prev->next = best_node->next;
    }

    if (best_node == ready_tail)
    {
        ready_tail = best_prev;
    }

    PCB *p = best_node->pcb;
    free(best_node);
    ready_count--;
    return p;
}

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

void dispatchProcess(PCB *p, int current_time)
{
    p->consecutive_ready_time = 0;
    p->state = STATE_RUNNING;
    running_pcb = p;

    if (chosen_algo == ALGO_RR)
    {
        quantum_remaining = quantum;
    }

    if (p->pid == -1)
    {
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

void scheduleNext(PCB *next, int current_time)
{
    if (context_switch_time == 0 || cpu_was_idle)
    {
        dispatchProcess(next, current_time);
        cpu_was_idle = false;
    }
    else
    {
        switching_to_pcb = next;
        cs_remaining = context_switch_time;
    }
}

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

    int cpu_util = (finish_time > 0) ? ((total_runtime * 100) / finish_time) : 0;
    double avg_wta = (all_pcb_count > 0) ? (sum_wta / all_pcb_count) : 0.0;
    int avg_wait = (all_pcb_count > 0) ? (sum_wait / all_pcb_count) : 0;

    double variance = 0.0;
    for (int i = 0; i < all_pcb_count; i++)
    {
        PCB *p = &all_pcbs[i];
        int ta = p->finish_time - p->data.arrival_time;
        double wta = (double)ta / p->data.runtime;
        variance += (wta - avg_wta) * (wta - avg_wta);
    }
    if (all_pcb_count > 0)
    {
        variance /= all_pcb_count;
    }
    double std_wta = sqrt(variance);

    fprintf(perf_file, "CPU utilization = %d%%\n", cpu_util);
    fprintf(perf_file, "Avg WTA = %.2f\n", avg_wta);
    fprintf(perf_file, "Avg Waiting = %d\n", avg_wait);
    fprintf(perf_file, "Std WTA = %.2f\n", std_wta);

    fclose(perf_file);
}

/* ========================================================
 * Modular Phase Handlers
 * ======================================================== */

// Phase 1: Tick accounting (advances execution, quantum, and aging)
void tickAccounting(int sim_time)
{
    if (cs_remaining > 0)
    {
        cs_remaining--;
        if (cs_remaining == 0 && switching_to_pcb != NULL)
        {
            PCB *next = switching_to_pcb;
            switching_to_pcb = NULL;
            dispatchProcess(next, sim_time);
        }
    }
    else if (running_pcb != NULL)
    {
        running_pcb->execution_time++;
        running_pcb->remaining_time--;
        if (chosen_algo == ALGO_RR)
        {
            quantum_remaining--;
        }
    }

    // Consolidated single loop: advance waiting aging for all processes in ready queue
    for (QueueNode *curr = ready_head; curr != NULL; curr = curr->next)
    {
        curr->pcb->consecutive_ready_time++;
    }
}

void releaseHeldResource(PCB *curr)
{
    if (!curr || !curr->holds_resource)
    {
        return;
    }

    SysResource *res = getOrCreateResource(curr->held_resource_id);
    if (res)
    {
        res->is_held = false;
        res->holder_id = -1;
    }

    curr->holds_resource = false;
    curr->held_resource_id[0] = '\0';
    curr->inherited_priority = -1;

    int best_blocked_idx = -1;
    for (int i = 0; i < blocked_count; i++)
    {
        PCB *b = blocked_queue[i];
        int br_idx = b->current_resource_idx;
        if (br_idx < b->data.resource_count && res &&
            strcmp(b->data.resources[br_idx].resource_id, res->id) == 0)
        {
            if (best_blocked_idx == -1)
            {
                best_blocked_idx = i;
            }
            else
            {
                int p1 = b->original_priority;
                int p2 = blocked_queue[best_blocked_idx]->original_priority;
                if (p1 < p2 || (p1 == p2 && b->data.id < blocked_queue[best_blocked_idx]->data.id))
                {
                    best_blocked_idx = i;
                }
            }
        }
    }

    if (best_blocked_idx != -1)
    {
        PCB *unblocked = blocked_queue[best_blocked_idx];

        for (int i = best_blocked_idx; i < blocked_count - 1; i++)
        {
            blocked_queue[i] = blocked_queue[i + 1];
        }
        blocked_count--;
        blocked_queue[blocked_count] = NULL;

        if (res)
        {
            res->is_held = true;
            res->holder_id = unblocked->data.id;
            unblocked->holds_resource = true;
            strncpy(unblocked->held_resource_id, res->id, MAX_RESOURCE_NAME_LEN - 1);
            updateInheritedPriority(unblocked);
        }

        readyQueuePush(unblocked);
    }
}

// Phase 2: HPF resource release & capture handling
void handleResources(int sim_time)
{
    if (chosen_algo < ALGO_HPF || running_pcb == NULL || cs_remaining > 0)
    {
        return;
    }

    PCB *curr = running_pcb;

    // Resource release
    if (curr->holds_resource)
    {
        int r_idx = curr->current_resource_idx;
        if (r_idx < curr->data.resource_count &&
            curr->execution_time == curr->data.resources[r_idx].release_time)
        {
            releaseHeldResource(curr);
            curr->current_resource_idx++;
        }
    }

    // Resource capture
    if (!curr->holds_resource && curr->current_resource_idx < curr->data.resource_count)
    {
        int r_idx = curr->current_resource_idx;
        if (curr->execution_time == curr->data.resources[r_idx].capture_time)
        {
            SysResource *res = getOrCreateResource(curr->data.resources[r_idx].resource_id);
            if (res && !res->is_held)
            {
                res->is_held = true;
                res->holder_id = curr->data.id;
                curr->holds_resource = true;
                strncpy(curr->held_resource_id, res->id, MAX_RESOURCE_NAME_LEN - 1);
                updateInheritedPriority(curr);
            }
            else if (res)
            {
                stopProcess(curr, sim_time);
                curr->state = STATE_BLOCKED;
                if (blocked_count >= MAX_PROCESSES)
                {
                    fatalError("Blocked queue capacity exceeded (MAX_PROCESSES).");
                }
                blocked_queue[blocked_count++] = curr;
                running_pcb = NULL;

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
}

// Phase 3: Process finish handling
void handleFinish(int sim_time, int *last_finish_time)
{
    if (running_pcb == NULL || cs_remaining > 0)
    {
        return;
    }

    PCB *curr = running_pcb;
    if (curr->remaining_time == 0)
    {
        int status;
        waitpid(curr->pid, &status, 0);

        curr->state = STATE_FINISHED;
        curr->finish_time = sim_time;
        *last_finish_time = sim_time;
        logProcessEvent(sim_time, curr, "finished");
        completed_processes++;

        if (chosen_algo >= ALGO_HPF && curr->holds_resource)
        {
            releaseHeldResource(curr);
        }

        running_pcb = NULL;
    }
}

static bool has_pending_arrival = false;
static ProcessMessage pending_arrival_msg;

// Phase 4: Receive arriving processes from IPC message queue (guarding against early clock arrivals)
void receiveArrivals(int sim_time)
{
    if (has_pending_arrival)
    {
        if (pending_arrival_msg.process.arrival_time <= sim_time)
        {
            if (all_pcb_count >= MAX_PROCESSES)
            {
                fatalError("Process table capacity exceeded (MAX_PROCESSES).");
            }
            PCB *p = &all_pcbs[all_pcb_count++];
            p->data = pending_arrival_msg.process;
            p->pid = -1;
            p->state = STATE_READY;
            p->remaining_time = pending_arrival_msg.process.runtime;
            p->execution_time = 0;
            p->wait_time = 0;
            p->start_time = -1;
            p->finish_time = -1;
            p->original_priority = pending_arrival_msg.process.priority;
            p->inherited_priority = -1;
            p->effective_priority = pending_arrival_msg.process.priority;
            p->consecutive_ready_time = 0;
            p->current_resource_idx = 0;
            p->holds_resource = false;
            p->held_resource_id[0] = '\0';

            readyQueuePush(p);
            has_pending_arrival = false;
        }
        else
        {
            return;
        }
    }

    ProcessMessage msg;
    while (msgrcv(msg_queue_id, &msg, sizeof(ProcessData), 1, IPC_NOWAIT) != -1)
    {
        if (msg.process.arrival_time > sim_time)
        {
            pending_arrival_msg = msg;
            has_pending_arrival = true;
            break;
        }

        if (all_pcb_count >= MAX_PROCESSES)
        {
            fatalError("Process table capacity exceeded (MAX_PROCESSES).");
        }
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
}

// Phase 5: Algorithm scheduling dispatch and preemption
void scheduleProcesses(int sim_time)
{
    if (cs_remaining > 0)
    {
        return;
    }

    if (chosen_algo == ALGO_SJF)
    {
        if (running_pcb == NULL && ready_count > 0)
        {
            PCB *next = readyQueuePopBest();
            scheduleNext(next, sim_time);
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
                if (ready_count > 0)
                {
                    PCB *preempted = running_pcb;
                    stopProcess(preempted, sim_time);
                    readyQueuePush(preempted);
                    running_pcb = NULL;

                    PCB *next = readyQueuePopBest();
                    scheduleNext(next, sim_time);
                }
                else
                {
                    quantum_remaining = quantum;
                }
            }
        }
        else if (ready_count > 0)
        {
            PCB *next = readyQueuePopBest();
            scheduleNext(next, sim_time);
        }
        else
        {
            cpu_was_idle = true;
        }
    }
    else // HPF or HPF Bonus Demotion
    {
        if (running_pcb != NULL)
        {
            if (ready_count > 0)
            {
                PCB *best_ready = readyQueuePeekBest();

                int ready_prio = getActivePriority(best_ready);
                int running_prio = getActivePriority(running_pcb);

                if (ready_prio < running_prio)
                {
                    PCB *preempted = running_pcb;
                    stopProcess(preempted, sim_time);
                    readyQueuePush(preempted);
                    running_pcb = NULL;

                    PCB *next = readyQueuePopBest();
                    scheduleNext(next, sim_time);
                }
            }
        }
        else if (ready_count > 0)
        {
            PCB *next = readyQueuePopBest();
            scheduleNext(next, sim_time);
        }
        else
        {
            cpu_was_idle = true;
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc >= 5)
    {
        chosen_algo = atoi(argv[1]);
        total_processes = atoi(argv[2]);
        quantum = atoi(argv[3]);
        context_switch_time = atoi(argv[4]);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleChildTermination;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

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

    while (completed_processes < total_processes)
    {
        int current_time = getClk();
        if (current_time <= sim_time)
        {
            usleep(20000);
            continue;
        }

        while (sim_time < current_time && completed_processes < total_processes)
        {
            sim_time++;
            usleep(30000);

            tickAccounting(sim_time);
            handleResources(sim_time);
            handleFinish(sim_time, &last_finish_time);
            receiveArrivals(sim_time);
            scheduleProcesses(sim_time);
        }
    }

    if (termination_signals_received < total_processes)
    {
        usleep(10000);
    }

    fclose(log_file);
    generatePerfReport(last_finish_time);
    clearReadyQueue();
    destroyClk(false);
    return 0;
}

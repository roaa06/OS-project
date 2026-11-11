#include "headers.h"

int msg_queue_id = -1;
pid_t clk_pid = -1;
pid_t scheduler_pid = -1;

void clearResources(int signum)
{
    if (msg_queue_id != -1)
    {
        msgctl(msg_queue_id, IPC_RMID, NULL);
        msg_queue_id = -1;
    }
    destroyClk(true);
    exit(0);
}


int compareProcesses(const void *a, const void *b)
{
    ProcessData *p1 = (ProcessData *)a;
    ProcessData *p2 = (ProcessData *)b;
    if (p1->arrival_time != p2->arrival_time)
        return p1->arrival_time - p2->arrival_time;
    return p1->id - p2->id;
}


int readProcesses(const char *filename, ProcessData processes[])
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        printf("Error: Could not open %s\n", filename);
        return 0;
    }

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), file))
    {
        
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        int id, arrival, runtime, priority;
        if (sscanf(line, "%d %d %d %d", &id, &arrival, &runtime, &priority) == 4)
        {
            processes[count].id = id;
            processes[count].arrival_time = arrival;
            processes[count].runtime = runtime;
            processes[count].priority = priority;
            processes[count].resource_count = 0;
            count++;
        }
    }

    fclose(file);

    
    qsort(processes, count, sizeof(ProcessData), compareProcesses);
    return count;
}


void readResources(const char *filename, ProcessData processes[], int count)
{
    FILE *file = fopen(filename, "r");
    if (!file)
        return; 

    char line[256];
    while (fgets(line, sizeof(line), file))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        int pid, capture, release;
        char resource_id[MAX_RESOURCE_NAME_LEN];

        if (sscanf(line, "%d %s %d %d", &pid, resource_id, &capture, &release) == 4)
        {
            for (int i = 0; i < count; i++)
            {
                if (processes[i].id == pid)
                {
                    int r_idx = processes[i].resource_count;
                    if (r_idx < MAX_RESOURCES_PER_PROCESS)
                    {
                        strncpy(processes[i].resources[r_idx].resource_id, resource_id, MAX_RESOURCE_NAME_LEN - 1);
                        processes[i].resources[r_idx].resource_id[MAX_RESOURCE_NAME_LEN - 1] = '\0';
                        processes[i].resources[r_idx].capture_time = capture;
                        processes[i].resources[r_idx].release_time = release;
                        processes[i].resource_count++;
                    }
                    break;
                }
            }
        }
    }

    fclose(file);
}

int main(int argc, char * argv[])
{
    signal(SIGINT, clearResources);

    
    ProcessData processes[MAX_PROCESSES];
    int total_processes = readProcesses("processes.txt", processes);
    if (total_processes <= 0)
    {
        printf("No processes found in processes.txt!\n");
        return 1;
    }

    readResources("resources.txt", processes, total_processes);
    printf("Successfully loaded %d processes.\n", total_processes);

    
    int chosen_algo = 1;
    int quantum = 3;
    int context_switch_time = 1;

    printf("\n=========================================\n");
    printf("Select Scheduling Algorithm:\n");
    printf("  1. Shortest Job First (SJF)\n");
    printf("  2. Round Robin (RR)\n");
    printf("  3. Preemptive HPF with Priority Inheritance\n");
    printf("  4. (Bonus) Preemptive HPF with Priority Demotion\n");
    printf("=========================================\n");
    printf("Enter choice [1-4]: ");
    if (scanf("%d", &chosen_algo) != 1 || chosen_algo < 1 || chosen_algo > 4)
    {
        chosen_algo = 1;
        printf("Invalid input. Defaulting to 1 (SJF).\n");
    }

    if (chosen_algo == ALGO_RR)
    {
        printf("Enter Round Robin Quantum (e.g. 3): ");
        if (scanf("%d", &quantum) != 1 || quantum <= 0)
        {
            quantum = 3;
            printf("Invalid quantum. Defaulting to 3.\n");
        }

        printf("Enter Context Switch Time (default 1): ");
        if (scanf("%d", &context_switch_time) != 1 || context_switch_time < 0)
        {
            context_switch_time = 1;
            printf("Invalid context switch time. Defaulting to 1.\n");
        }
    }
    else
    {
        
        context_switch_time = 1;
    }

    
    
    int existing_qid = msgget(PROCESS_MSG_KEY, 0666);
    if (existing_qid != -1)
        msgctl(existing_qid, IPC_RMID, NULL);

    int existing_shmid = shmget(SHKEY, 4, 0666);
    if (existing_shmid != -1)
        shmctl(existing_shmid, IPC_RMID, NULL);

    msg_queue_id = msgget(PROCESS_MSG_KEY, IPC_CREAT | 0666);
    if (msg_queue_id == -1)
    {
        perror("Error creating message queue in process_generator");
        exit(-1);
    }

    
    clk_pid = fork();
    if (clk_pid == 0)
    {
        execl("./clk.out", "clk.out", NULL);
        perror("Error executing clk.out");
        exit(-1);
    }

    
    initClk();

    scheduler_pid = fork();
    if (scheduler_pid == 0)
    {
        char algo_str[16], count_str[16], quantum_str[16], cs_str[16];
        sprintf(algo_str, "%d", chosen_algo);
        sprintf(count_str, "%d", total_processes);
        sprintf(quantum_str, "%d", quantum);
        sprintf(cs_str, "%d", context_switch_time);

        execl("./scheduler.out", "scheduler.out", algo_str, count_str, quantum_str, cs_str, NULL);
        perror("Error executing scheduler.out");
        exit(-1);
    }

    
    int sent_count = 0;
    while (sent_count < total_processes)
    {
        int current_time = getClk();

        while (sent_count < total_processes && processes[sent_count].arrival_time <= current_time)
        {
            ProcessMessage msg;
            msg.mtype = 1;
            msg.process = processes[sent_count];

            if (msgsnd(msg_queue_id, &msg, sizeof(ProcessData), 0) == -1)
            {
                perror("Error sending process via msgsnd");
            }
            sent_count++;
        }

        usleep(2000); 
    }

    
    int status;
    waitpid(scheduler_pid, &status, 0);

    
    clearResources(0);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_RESOURCES_PER_PROCESS 3

int main(int argc, char *argv[])
{
    int numProcesses, numResources;

    printf("Number of processes: ");
    if (scanf("%d", &numProcesses) != 1 || numProcesses < 1)
    {
        printf("Error: the number of processes must be at least 1.\n");
        return 1;
    }
    printf("Maximum number of distinct resources (0 for none): ");
    if (scanf("%d", &numResources) != 1 || numResources < 0)
        numResources = 0;

    FILE *pFile = fopen("processes.txt", "w");
    FILE *rFile = fopen("resources.txt", "w");
    if (pFile == NULL || rFile == NULL)
    {
        printf("Error: could not create the output files.\n");
        return 1;
    }

    // optional seed: ./generator 42 repeats the same test case
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : (unsigned)time(NULL));

    fprintf(pFile, "#id\tarrival\truntime\tpriority\n");
    fprintf(rFile, "#pid\tresourceid\tcapturetime\treleasetime\n");

    int arrival = 1; // the clock starts at time 1
    for (int id = 1; id <= numProcesses; id++)
    {
        if (id > 1)
            arrival += rand() % 11;    // sorted by arrival, ties allowed
        int runtime = rand() % 30 + 1; // [1, 30]
        int priority = rand() % 11;    // [0, 10], 0 is the highest

        fprintf(pFile, "%d\t%d\t%d\t%d\n", id, arrival, runtime, priority);

        if (numResources == 0)
            continue;

        // Deadlock guard: intervals of one process are generated one after another,
        // and each capture is >= the previous release. So a process never holds two
        // resources at once (no hold-and-wait), and deadlock cannot occur.
        int wanted = rand() % (MAX_RESOURCES_PER_PROCESS + 1);
        int cursor = 1; // earliest allowed next capture
        for (int s = 0; s < wanted && cursor <= runtime - 1; s++)
        {
            int capture = cursor + rand() % (runtime - cursor);       // [cursor, runtime-1]
            int release = capture + 1 + rand() % (runtime - capture); // [capture+1, runtime]
            int resource = rand() % numResources + 1;

            fprintf(rFile, "%d\tr%d\t%d\t%d\n", id, resource, capture, release);
            cursor = release; // next capture may equal this release
        }
    }

    fclose(pFile);
    fclose(rFile);
    return 0;
}
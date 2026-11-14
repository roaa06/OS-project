#include "headers.h"

int remainingtime;
volatile sig_atomic_t last_clk;

void handle_cont(int sig)
{
    (void)sig;
    last_clk = getClk();
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <runtime>\n", argv[0]);
        exit(-1);
    }

    remainingtime = atoi(argv[1]);
    signal(SIGCONT, handle_cont);

    initClk();
    last_clk = getClk();

    while (remainingtime > 0)
    {
        int current_clk = getClk();
        if (current_clk > last_clk)
        {
            remainingtime -= (current_clk - last_clk);
            last_clk = current_clk;
        }
        usleep(50000);
    }

    destroyClk(false);
    return 0;
}

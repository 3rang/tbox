#include <stdio.h>
#include "cli.h"

void tbox_cli(int argc, char *argv[])
{
    // Implement CLI handling logic here
    if(argc < 2)
    {
        printf("Usage: %s <arguments>\n", argv[0]);
        return;
    }

    printf("Arguments provided:\n");
    for(int i = 1; i < argc; i++)
    {
        printf("  %s\n", argv[i]);
    }

}
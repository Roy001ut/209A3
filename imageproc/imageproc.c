#include <stdio.h>
#include <stdlib.h>

#include "parent.h"

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s <input.ppm> <output.ppm> [k] [tiles]\n",
                argv[0]);
        return 1;
    }

    const char *infile  = argv[1];
    const char *outfile = argv[2];
    int k     = argc >= 4 ? atoi(argv[3]) : 3;
    int tiles = argc >= 5 ? atoi(argv[4]) : 2;

    if (k < 1 || k > 8) {
        fprintf(stderr, "k must be 1-8\n");
        return 1;
    }
    if (tiles < 1 || tiles > 8) {
        fprintf(stderr, "tiles must be 1-8\n");
        return 1;
    }

    return run_parent(infile, outfile, k, tiles);
}

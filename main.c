#include <stdio.h>
#include <stdlib.h>


int main(int argc, char *argv[]) {
    if (argc < 4) {                 //usage
        fprintf(stderr, "usage: %s k total_frames q [optionally max_refs]\n", argv[0]);
        return 1;
    }

    int k= atoi(argv[1]);
    int total_frames= atoi(argv[2]);
    int q= atoi(argv[3]);
    long max_refs= (argc >= 5) ? atol(argv[4]) : -1; //the max_refs= -1 means no limit in references

    char *trace_file1 ="bzip_.txt"; //trace files
    char *trace_file2 ="gcc_.txt";

    return 0;
}
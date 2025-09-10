#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <errno.h>

typedef struct {
    long page;
    char op;      //"R" or "W
    int proc_id;  // 1 =PM1, 2 = PM2
    int valid;    // flag: 1=υπάρχει αίτημα, 0=κενό
} Request;

#define SHM_NAME "/shared_mem"       // POSIX shared memory object
#define PM_SEM   "/pm_sem"       //semaphore for PMs (producers)
#define MM_SEM   "/mm_sem"       //semaphore for MM (consumer)

//this function is executed by each PM process
void pm_process(char* trace_file, int proc_id, long max_refs, sem_t* PM_sem, sem_t* MM_sem, Request* req) {
    FILE* f = fopen(trace_file, "r");
    if (!f) { perror(trace_file); exit(1); }

    char line[64];
    long refs = 0;

    while ((max_refs == -1 || refs < max_refs) && fgets(line, sizeof(line), f)) { 
        unsigned long addr;
        char op;
        if (sscanf(line, "%lx %c", &addr, &op) != 2) continue;

        long page = addr /4096;

        sem_wait(PM_sem);       //περιμένει άδεια να γράψει
        req->page = page;
        req->op = op;
        req->proc_id = proc_id;
        req->valid = 1;
        sem_post(MM_sem);       //notify MM
        refs++;
    }

    fclose(f);
    exit(0);  //end of Pm
}

int main(int argc, char *argv[]) {
    if (argc < 4) {                 //usage
        fprintf(stderr, "usage: %s k total_frames q (optionally max_refs)\n", argv[0]);
        return 1;
    }

    int k= atoi(argv[1]);                                 //number of page faults
    int total_frames= atoi(argv[2]);                     //total number of frames
    int q= atoi(argv[3]);                               //number of references between traces
    long max_refs= (argc >= 5) ? atol(argv[4]) : -1;   //the max_refs= -1 means no limit in references

    char *trace_file1 ="bzip_.txt"; //trace files
    char *trace_file2 ="gcc_.txt";

    int frames1 = total_frames /2;
    int frames2 = total_frames - frames1;

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);  //create shared memory
    if (shm_fd == -1) { perror("shm_open");
        exit(1);
    }
    ftruncate(shm_fd, sizeof(Request));

    Request *req = mmap(0, sizeof(Request), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (req == MAP_FAILED) { perror("mmap");
        exit(1);
    }
    req->valid = 0; //initialize request as empty/0


    sem_t *PM_sem = sem_open(PM_SEM, O_CREAT, 0666, 1); //startins as 1: PMs can read/write
    sem_t *MM_sem = sem_open(MM_SEM, O_CREAT, 0666, 0); //starting as 0: MM waits for PMs to reach k+1 page faults
    if (PM_sem == SEM_FAILED || MM_sem == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    pid_t pm1 = fork();      //fork() for PM1  PM2 processes
    if (pm1 == 0) {
        pm_process(trace_file1, 1, max_refs, PM_sem, MM_sem, req);
    }

    pid_t pm2 = fork();
    if (pm2 == 0) {
        pm_process(trace_file2, 2, max_refs, PM_sem, MM_sem, req);
    }

    long page_faults = 0, disk_reads = 0, disk_writes = 0, processed = 0;

    while (1) {
        sem_wait(MM_sem);       
        if (req->valid) { //true if valid== 1
            //FWF (Flush When Full)
            //....
            //....
            processed++;

            req->valid = 0;
            sem_post(PM_sem);   //PMs can send their next requests
        }

        // Τερματισμός όταν τα παιδιά τελειώσουν
        int status;
        if (waitpid(pm1, &status, WNOHANG) > 0 &&
            waitpid(pm2, &status, WNOHANG) > 0) {
            break;
        }
    }
    return 0;
}
#define _XOPEN_SOURCE 700 //for shm_open

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

#define PAGE_SIZE 4096
#define MAX_QUEUE 2000002

/*
Request struct for communication between PM and MM
holds page number, operation type, and the PM id
*/
typedef struct {
    long page;
    char op;  //'R(ead)', 'W(rite)', 'E', 'B': Ε is for EOF, B is for Batch end
    int pmid;
} Request;

/*
Shared Queue struct for circular buffer
between PM and MM processes, each PM has its own queue.

*/
typedef struct {
    Request buf[MAX_QUEUE];
    size_t head;
    size_t tail;
} SharedQueue;

/*
each frame holds a page number, dirty bit if page was written, and last used timestamp
*/
typedef struct {
    long page;
    int dirty;
    unsigned long last_used;
} Frame;

/*
Proc struct holds all data for a process's memory management
including frames, hash table for quick lookup, and the stats for the process
*/
typedef struct {
    Frame *frames;
    int nframes;
    int *hash;
    int hashsize;

    unsigned long page_faults; //total page faults
    unsigned long disk_reads;  //total disk reads
    unsigned long disk_writes; //total disk writes
    unsigned long block_pf;    //page faults in current block/banch
    unsigned long flush_events; //number of flush events tirggered
    unsigned long batches;     //number of batches processed
    unsigned long processed; //total references processed
} Proc;

//hash functions 
static inline int hfunc(long page, int size) { //hash function to map page to hash table number
    return (int)(page % size);
}

int ht_lookup(Proc *p, long page) { //returns index in frames or -1
    int h = hfunc(page, p->hashsize);
    while (p->hash[h] != -1) {
        int idx = p->hash[h];
        if (p->frames[idx].page == page) return idx;
        h = (h + 1) % p->hashsize;
    }
    return -1;
}

void ht_insert(Proc *p, long page, int idx) { //inserts page with index idx in frames
    int h = hfunc(page, p->hashsize);
    while (p->hash[h] != -1) h = (h + 1) % p->hashsize;
    p->hash[h] = idx;
}

void ht_remove(Proc *p, long page) { //removes page from hash table
    int h = hfunc(page, p->hashsize);
    while (p->hash[h] != -1) {
        int idx = p->hash[h];
        if (p->frames[idx].page == page) {
            p->hash[h] = -1;
            return;
        }
        h = (h + 1) % p->hashsize;
    }
}

//initialize proc struct
void init_proc(Proc *p, int frames) {
    p->nframes = frames;
    p->frames = malloc(sizeof(Frame)*frames);
    if(!p->frames) { 
        perror("malloc frames");
        exit(1);
    }

    for(int i=0;i<frames;i++) {
        p->frames[i].page=-1;
        p->frames[i].dirty=0;
        p->frames[i].last_used=0;
    }

    p->hashsize = frames*2;
    p->hash = malloc(sizeof(int)*p->hashsize);
    if(!p->hash) {
        perror("malloc hash");
        exit(1);
    }

    for(int i=0;i<p->hashsize;i++) p->hash[i]=-1;
    p->page_faults= 0;
    p->disk_reads= 0;
    p->disk_writes= 0;
    p->block_pf= 0;
    p->flush_events= 0;
    p->batches= 0;
    p->processed= 0;
}

/*
handle_request processes a memory request for PM1/2
it checks if the page is in memory, and updates stats accordingly
if not it handles the page fault, flushes if needed (block_pf > k), and updates frames   
*/
void handle_request(Proc *p, Request r, int k){
    long page = r.page;
    char op = r.op;

    int idx = ht_lookup(p,page); //check if page is in memory
    if(idx!=-1) { 
        if(op=='W') p->frames[idx].dirty=1;
        p->frames[idx].last_used=p->processed;
    } else { //if idx== -1, page fault -> check if flush needed
        p->page_faults++;
        p->block_pf++;
        if(p->block_pf>k) { //flush all frames of the prcess dedicated frames 
            for(int i=0;i<p->nframes;i++){
                if(p->frames[i].page!=-1){
                    if(p->frames[i].dirty) {
                        p->disk_writes++;
                    }
                    ht_remove(p,p->frames[i].page);
                    p->frames[i].page=-1;
                    p->frames[i].dirty=0;
                }
            }
            p->block_pf=1;
            p->flush_events++;
            printf("flush triggered after PF > k\n");
        }

        idx=-1;
        for(int i=0; i< p->nframes; i++) { //find empty frame
            if (p->frames[i].page== -1) { 
                idx= i;
                break;
            }
        }
        if(idx== -1) {
            unsigned long oldest=(unsigned long)-1;
            for (int i=0; i< p->nframes; i++) {
                if (p->frames[i].last_used < oldest) {
                    oldest=p->frames[i].last_used; idx=i;
                }
            }
            if(p->frames[idx].dirty) p->disk_writes++;
            ht_remove(p,p->frames[idx].page);
        }

        p->frames[idx].page = page;
        p->frames[idx].dirty = (op=='W')?1:0; //set dirty if operation is Write
        p->frames[idx].last_used = p->processed;
        ht_insert(p,page,idx);
        p->disk_reads++;
    }

    p->processed++;
}

/*
pm_process reads the trace file and sends requests to MM via shared queue
it sends requests in batches of size q, and marks the end of each batch with a special request 'B" 
it also sends termination "request" -E when done.
*/
void pm_process(char* trace_file, int pmid, long max_refs, int q, sem_t* empty, sem_t* full, sem_t* mutex, SharedQueue* queue) {

    FILE* f = fopen(trace_file,"r");
    if (!f) {
        perror(trace_file);
        exit(1);
    }
    char line[64];
    long refs=0;

    while(max_refs==-1 || refs<max_refs) {
        int sent=0;
        while(sent<q && (max_refs==-1 || refs<max_refs) && fgets(line,sizeof(line),f)) {
            unsigned long addr;
            char op;

            if(sscanf(line,"%lx %c",&addr,&op)!=2) {
                continue;
            }

            long page = addr / PAGE_SIZE;
            sem_wait(empty);
            sem_wait(mutex);

            queue->buf[queue->tail].page = page;    //add request to queue
            queue->buf[queue->tail].op   = op;      // R or W
            queue->buf[queue->tail].pmid = pmid;   // PM id (from a version that i used a single queue)
            queue->tail = (queue->tail+1) % MAX_QUEUE; //circular buffer

            sem_post(mutex); 
            sem_post(full);

            sent++; refs++;
        }

        if(sent==0) break;

        // batch end marker
        sem_wait(empty);
        sem_wait(mutex);
        queue->buf[queue->tail].page = -2; // -2  for page _> batch end
        queue->buf[queue->tail].op = 'B';
        queue->buf[queue->tail].pmid = pmid;
        queue->tail = (queue->tail+1) % MAX_QUEUE;
        sem_post(mutex);
        sem_post(full);

        printf("PM%d completed batch %lu\n", pmid, (refs+q-1)/q);
        fflush(stdout);
    }

    // termination marker
    sem_wait(empty);
    sem_wait(mutex);
    queue->buf[queue->tail].page = -1;
    queue->buf[queue->tail].op = 'E';
    queue->buf[queue->tail].pmid = pmid;
    queue->tail = (queue->tail+1) % MAX_QUEUE;
    sem_post(mutex);
    sem_post(full);

    fclose(f);
    exit(0);
}

//prints stats for each PM process when theu finish sending traces
void print_proc_stats(char *label, char *file, Proc *p){
    int used_frames=0;
    for(int i=0;i<p->nframes;i++) if(p->frames[i].page!=-1) used_frames++;

    double pf_rate = (p->processed>0)?((double)p->page_faults/p->processed*100.0):0.0;

    printf("Process: %s (%s)\n", label, file);
    printf(" - References Processed:  %lu\n", p->processed);
    printf(" - Page Faults:           %lu\n", p->page_faults);
    printf(" - Flush Events:          %lu\n", p->flush_events);
    printf(" - Disk Reads (Loads):    %lu\n", p->disk_reads);
    printf(" - Disk Writes (Saves): %lu\n", p->disk_writes);
    printf(" - Batches Completed:     %lu\n", p->batches);
    printf(" - Page Fault Rate:       %.2f%%\n", pf_rate);
    printf(" - Final Occupied Frames: %d / %d\n", used_frames, p->nframes);
    printf("--------------------------------------------------------\n");
}

//main function that also works as MM and calling handle requests function
int main(int argc,char* argv[]){
    if(argc<4){
        fprintf(stderr,"Usage: %s k total_frames q [max_refs]\n",argv[0]);
        return 1;
    }
    sem_unlink("/sem_empty1"); sem_unlink("/sem_full1"); sem_unlink("/sem_mutex1");
    sem_unlink("/sem_empty2"); sem_unlink("/sem_full2"); sem_unlink("/sem_mutex2");
    shm_unlink("/shm_queue_pm1"); shm_unlink("/shm_queue_pm2");

    int k = atoi(argv[1]);
    int total_frames = atoi(argv[2]);
    int q = atoi(argv[3]);
    long max_refs = (argc>=5)?atol(argv[4]):-1;

    char* trace1 = "bzip_.txt";
    char* trace2 = "gcc_.txt";

    Proc p1,p2;
    init_proc(&p1, total_frames/2);
    init_proc(&p2,total_frames/2);
 
    // --- Shared memory ---
    int shm_fd1 = shm_open("/shm_queue_pm1",O_CREAT|O_RDWR,0666);
    int shm_fd2 = shm_open("/shm_queue_pm2",O_CREAT|O_RDWR,0666);
    if(shm_fd1<0 || shm_fd2<0) {
        perror("shm_open");
        exit(1);
    }
    if(ftruncate(shm_fd1,sizeof(SharedQueue))< 0 || ftruncate(shm_fd2,sizeof(SharedQueue))< 0) {
        perror("ftruncate");
        exit(1);
    }

    SharedQueue* queue1 = mmap(0,sizeof(SharedQueue), PROT_READ| PROT_WRITE, MAP_SHARED,shm_fd1, 0);
    SharedQueue* queue2 = mmap(0,sizeof(SharedQueue), PROT_READ| PROT_WRITE, MAP_SHARED,shm_fd2, 0);

    if(queue1==MAP_FAILED || queue2==MAP_FAILED){ perror("mmap"); exit(1); }
    queue1->head = queue1->tail = 0;
    queue2->head = queue2->tail = 0;

    // --- Semaphores ---
    sem_t* empty1 = sem_open("/sem_empty1", O_CREAT,0666, MAX_QUEUE);
    sem_t* full1  = sem_open("/sem_full1", O_CREAT,0666, 0);
    sem_t* mutex1 = sem_open("/sem_mutex1", O_CREAT,0666, 1);

    sem_t* empty2 = sem_open("/sem_empty2", O_CREAT,0666,MAX_QUEUE);
    sem_t* full2  = sem_open("/sem_full2", O_CREAT,0666,0);
    sem_t* mutex2 = sem_open("/sem_mutex2", O_CREAT,0666,1);

    //fork to create PM1 and PM2 processes
    pid_t pm1 = fork();
    if(pm1==0) {
        pm_process(trace1, 1, max_refs, q, empty1, full1, mutex1, queue1);
    }

    pid_t pm2 = fork();
    if(pm2==0) {
        pm_process(trace2, 2, max_refs, q, empty2, full2, mutex2, queue2);
    }

    ////////////// MM loop
    int finished1=0, finished2=0; //flags to check if PM1 and PM2 have sent all requests and exited

    while(!(finished1 && finished2)){
        // PM1 queue
        if(!finished1) {
            int done_batch = 0;
            while(!done_batch && !finished1) {
                sem_wait(full1);
                sem_wait(mutex1);
                Request r = queue1->buf[queue1->head];
                queue1->head = (queue1->head+1)%MAX_QUEUE;
                sem_post(mutex1);
                sem_post(empty1);

                if(r.op=='E') {
                    finished1=1;
                    break;
                }
                if(r.op=='B') {
                    done_batch=1;
                    p1.batches++;
                    p1.block_pf = 0;  // reset PF counter για το νέο batch
                    printf("MM processed batch %lu for PM1\n", p1.batches);
                    continue;
                }
                handle_request(&p1,r,k);
            }
        }
        // PM2 queue
        if(!finished2) {
            int done_batch = 0;
            while(!done_batch && !finished2) {
                sem_wait(full2);
                sem_wait(mutex2);
                Request r = queue2->buf[queue2->head];
                queue2->head= (queue2->head+1) % MAX_QUEUE;
                sem_post(mutex2);
                sem_post(empty2);

                if(r.op=='E') {
                    finished2=1;
                    break;
                }
                if(r.op=='B') {
                    done_batch=1;
                    p2.batches++;
                    p2.block_pf = 0;
                    printf("MM processed batch %lu for PM2\n", p2.batches);
                    continue;
                }

                handle_request(&p2,r,k);
            }
        }
    }

    wait(NULL);
    wait(NULL);

    printf("\n=========== Final Statistics ============\n");
    print_proc_stats("PM1", trace1, &p1);
    print_proc_stats("PM2", trace2, &p2);

    unsigned long tot_refs = p1.processed+ p2.processed;
    unsigned long tot_pf   = p1.page_faults+ p2.page_faults;
    unsigned long tot_rd   = p1.disk_reads+ p2.disk_reads;
    unsigned long tot_wr   = p1.disk_writes+ p2.disk_writes;
    double fault_rate = (tot_refs>0) ?((double)tot_pf/tot_refs*100.0):0.0;

    printf("System Totals:\n");
    printf(" - Total References:   %lu\n", tot_refs);
    printf(" - Total Page Faults:  %lu\n", tot_pf);
    printf(" - Total Disk Reads:   %lu\n", tot_rd);
    printf(" - Total Disk Writes:  %lu\n", tot_wr);
    printf(" - Overall Fault Rate: %.2f%%\n", fault_rate);
    printf("==========================================\n\n");

    sem_unlink("/sem_empty1");
    sem_unlink("/sem_full1");
    sem_unlink("/sem_mutex1");
    sem_unlink("/sem_empty2");
    sem_unlink("/sem_full2");
    sem_unlink("/sem_mutex2");
    shm_unlink("/shm_queue_pm1");
    shm_unlink("/shm_queue_pm2");

    free(p1.frames);
    free(p1.hash);
    free(p2.frames);
    free(p2.hash);

    return 0;
}

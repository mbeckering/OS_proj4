/* 
 * File:   main.c
 * Author: Michael Beckering
 * Project 4
 * Spring 2018 CS-4760-E01
 * Created on March 15, 2018, 10:35 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

#define BILLION 1000000000 //dont want to type the wrong # of zeroes
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_pct 4020014
#define BUFF_SZ sizeof (unsigned int)


/********************** Function prototypes **********************************/

void makePCB(int pidnum, int isRealTime); //initializes a new PCB
void initBitVector(int); //initilize bit vector
void allocateSharedMemory(); //allocate shared memory
void clearIPC(); //clear shared memory and message queues
void initQueue(int[], int); //initialize queues, takes queue name and size

/************************ Global variables ***********************************/

int bitVector[18]; // Bit vector indicating used PCB's
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct; //shared memory id holder for process control table
static unsigned int *simClock_secs; //pointer to shm sim clock (seconds)
static unsigned int *simClock_ns; //pointer to shm sim clock (nanoseconds)
int queue0[18]; //Round Robin queue for realtime processes
int queue1[18]; //high-priority queue
int queue2[18]; //medium-priority queue
int queue3[18]; //low-priority queue

struct pcb { // Process control block struct
    unsigned int totalCPUtime_secs;
    unsigned int totalCPUtime_ns;
    unsigned int totalLIFEtime_secs;
    unsigned int totalLIFEtime_ns;
    unsigned int timeUsedLastBurst_ns;
    int localPID;
    int isRealTimeClass; // 1 = realtime class process
};

struct pcb * pct; //pct = process control table (array of 18 process
                  //control blocks)

/*************************** MAIN ********************************************/

int main(int argc, char** argv) {
    pid_t childpid;
    char str_pct_id[20];
    
    allocateSharedMemory();
    initBitVector(18);
    initQueue(queue0, 18);
    initQueue(queue1, 18);
    initQueue(queue2, 18);
    initQueue(queue3, 18);
    
    makePCB(1, 1); //pid1, realtime = yes
    
    *simClock_secs = 1;
    *simClock_ns = 12345;
    
    if ( (childpid = fork()) < 0 ){ //terminate code
                perror("Error forking consumer");
                return 1;
            }
        if (childpid == 0) { //child code
            sprintf(str_pct_id, "%d", shmid_pct); //build arg1 string
            execlp("./user", "./user", str_pct_id, (char *)NULL);
            perror("execl() failure"); //report & exit if exec fails
            return 0;
        }
    
    sleep(1);
    
    unsigned int local_secs = *simClock_secs;
    unsigned int local_ns = *simClock_ns;
    printf("OSS: seconds: %u, ns: %u \n", local_secs, local_ns);
    
    clearIPC();
    printf("OSS: Normal exit\n");

    return (EXIT_SUCCESS);
}

/************************** END MAIN *****************************************/

//makePCB initializes a new process control block and sets bit vector
//accepts simulated pid number and whether or not it's a realtime class process
void makePCB(int pidnum, int isRealTime) {
    pct[pidnum].totalCPUtime_secs = 0;
    pct[pidnum].totalCPUtime_ns = 0;
    pct[pidnum].totalLIFEtime_secs = 0;
    pct[pidnum].totalLIFEtime_ns = 0;
    pct[pidnum].timeUsedLastBurst_ns = 0;
    pct[pidnum].localPID = pidnum; //pids will be 1-18, not 0-17
    pct[pidnum].isRealTimeClass = isRealTime;
    bitVector[pidnum] = 1; //mark this pcb taken in bit vector
    printf("OSS: Generated process id %d, isRealTime = %d\n", 
        pct[pidnum].localPID, pct[pidnum].isRealTimeClass);
}

//initialize bit vector based on specified size
void initBitVector(int n) {
    printf("OSS: Initializing bit vector\n");
    int i;
    for (i=0; i<n; i++) {
        bitVector[i] = 0;
    }
}

// Function to set up shared memory
void allocateSharedMemory() {
    printf("OSS: Allocating shared memory\n");
    //process control table
    shmid_pct = shmget(SHMKEY_pct, 18*sizeof(struct pcb), 0777 | IPC_CREAT);
     if (shmid_pct == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_pct");
            exit(1);
        }
    
    pct = (struct pcb *)shmat(shmid_pct, 0, 0);
    if ( pct == (struct pcb *)(-1) ) {
        perror("OSS: error in shmat pct");
        exit(1);
    }
    
    //sim clock seconds
    shmid_sim_secs = shmget(SHMKEY_sim_s, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_secs == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_secs");
            exit(1);
        }
    simClock_secs = (unsigned int*) shmat(shmid_sim_secs, 0, 0);
    //sim clock nanoseconds
    shmid_sim_ns = shmget(SHMKEY_sim_ns, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_ns == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_ns");
            exit(1);
        }
    simClock_ns = (unsigned int*) shmat(shmid_sim_ns, 0, 0);
}

// Function to clear shared memory
void clearIPC() {
    printf("OSS: Clearing IPC resources...\n");
    //shared memory
    if ( shmctl(shmid_sim_secs, IPC_RMID, NULL) == -1) {
        perror("error removing shared memory");
    }
    if ( shmctl(shmid_sim_ns, IPC_RMID, NULL) == -1) {
        perror("error removing shared memory");
    }
    //process control table
    if ( shmctl(shmid_pct, IPC_RMID, NULL) == -1) {
        perror("error removing shared memory");
    }
    /*
    if ( msgctl(mutex_qid, IPC_RMID, NULL) == -1 ) {
        perror("Master: Error removing mutex_qid");
        exit(0);
    }
    if ( msgctl(comms_qid, IPC_RMID, NULL) == -1 ) {
        perror("Master: Error removing comms_qid");
        exit(0);
    }
    */
}

void initQueue(int q[], int size) {
    int i;
    for(i=0; i<size; i++) {
        q[i] = 0;
    }
}




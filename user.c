/* 
 * File:   user.c
 * Author: Michael Beckering
 * Project 4
 * Spring 2018 CS-4760-E01
 * Created on March 15, 2018, 11:35 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define BILLION 1000000000 //dont want to type the wrong # of zeroes
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_pct 4020014
#define BUFF_SZ sizeof (unsigned int)

/****************************FUNCTION PROTOTYPES *****************************/
void getSharedMemory();

/***************************** GLOBALS ***************************************/
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct;
static unsigned int *simClock_secs; 
static unsigned int *simClock_ns; //pointers to shm sim clock values

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

/********************************* MAIN **************************************/
int main(int argc, char** argv) {
    shmid_pct = atoi(argv[1]);
    getSharedMemory();
    printf("User: seconds: %u nanoseconds: %u\n", *simClock_secs, *simClock_ns);
    printf("User: process id %d, isRealTime = %d\n", 
        pct[1].localPID, pct[1].isRealTimeClass);
    printf("User: process id %d, isRealTime = %d\n", 
        pct[17].localPID, pct[17].isRealTimeClass);

    return (EXIT_SUCCESS);
}

void getSharedMemory() {
    //process control table
    pct = (struct pcb *)shmat(shmid_pct, 0, 0);
    if ( pct == (struct pcb *)(-1) ) {
        perror("User: error in shmat pct");
        exit(1);
    }
    
    printf("User: getting shared memory\n");
    //sim clock: seconds (READ ONLY)
    shmid_sim_secs = shmget(SHMKEY_sim_s, BUFF_SZ, 0444);
        if (shmid_sim_secs == -1) { //terminate if shmget failed
            perror("User: error in shmget shmid_sim_secs");
            exit(1);
        }
    simClock_secs = (unsigned int*) shmat(shmid_sim_secs, 0, 0);
    //sim clock: nanoseconds (READ ONLY)
    shmid_sim_ns = shmget(SHMKEY_sim_ns, BUFF_SZ, 0444);
        if (shmid_sim_ns == -1) { //terminate if shmget failed
            perror("User: error in shmget shmid_sim_ns");
            exit(1);
        }
    simClock_ns = (unsigned int*) shmat(shmid_sim_ns, 0, 0);
}


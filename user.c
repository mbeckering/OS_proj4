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
#include <sys/msg.h>
#include <sys/types.h>
#include <signal.h>

#define BILLION 1000000000 //dont want to type the wrong # of zeroes
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_pct 4020014
#define BUFF_SZ sizeof (unsigned int)
#define MSGQKEY_oss 4020069

/****************************FUNCTION PROTOTYPES *****************************/
void getIPC(); //Attach shared memory (allocated by OSS) to local vars
static void siginthandler(int); //SIGINT handler

/***************************** GLOBALS ***************************************/
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct; //shared memory id holder for process control table
int oss_qid; //message queue ID for OSS communications
static unsigned int *simClock_secs; 
static unsigned int *simClock_ns; //pointers to shm sim clock values
unsigned int myStartTimeSecs, myStartTimeNS; //to deduce my LIFEtime later

struct pcb { // Process control block struct
    unsigned int totalCPUtime_secs;
    unsigned int totalCPUtime_ns;
    unsigned int totalLIFEtime_secs;
    unsigned int totalLIFEtime_ns;
    unsigned int timeUsedLastBurst_ns;
    int blocked; // 1=blocked, 0= not blocked
    unsigned int blockedUntilSecs;
    unsigned int blockedUntilNS;
    int localPID;
    int isRealTimeClass; // 1 = realtime class process
};

struct pcb * pct; //pct = process control table (array of process
                  //control blocks)

//struct for communications message queue
struct commsbuf {
    long msgtype;
    unsigned int ossTimeSliceGivenNS; //from oss. time slice given to run
    int userTerminatingFlag; //from user. 1=terminating, 0=not terminating
    int userUsedFullTimeSliceFlag; //fromuser. 1=used full time slice
};

/********************************* MAIN **************************************/
int main(int argc, char** argv) {
    shmid_pct = atoi(argv[1]);
    struct commsbuf myinfo;
    
    // Set up interrupt handler
    signal (SIGINT, siginthandler);
    
    getIPC();
    myStartTimeSecs = *simClock_secs; 
    myStartTimeNS = *simClock_ns;
    
    if ( msgrcv(oss_qid, &myinfo, sizeof(myinfo), 1, 1) == -1 ) {
            perror("User: error in msgrcv");
            exit(0);
        }
    
    printf("User: msgtype %ld, timeslice %u\n",  myinfo.msgtype, myinfo.ossTimeSliceGivenNS);
    
    

    return (EXIT_SUCCESS);
}

void getIPC() {
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
    
    //message queue
    if ( (oss_qid = msgget(MSGQKEY_oss, 0777)) == -1 ) {
        perror("Error generating communication message queue");
        exit(0);
    }
}

//SIGINT handler
static void siginthandler(int sig_num) {
    printf("Slave(pid %ld) Terminating: Interrupted.\n", getpid());
    exit(0);
}

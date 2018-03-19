/* 
 * File:   user.c
 * Author: Michael Beckering
 * Project 4
 * Spring 2018 CS-4760-E01
 * Created on March 15, 2018, 11:35 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
int roll100(); //returns an int in range 1-100

/***************************** GLOBALS ***************************************/
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct; //shared memory id holder for process control table
int oss_qid; //message queue ID for OSS communications
static unsigned int *simClock_secs; 
static unsigned int *simClock_ns; //pointers to shm sim clock values
unsigned int myStartTimeSecs, myStartTimeNS; //to deduce my LIFEtime later
int my_sim_pid; //this user's simulated pid (1-18)
int seed;

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
    long msgtyp;
    int user_sim_pid;
    unsigned int ossTimeSliceGivenNS; //from oss. time slice given to run
    int userTerminatingFlag; //from user. 1=terminating, 0=not terminating
    int userUsedFullTimeSliceFlag; //fromuser. 1=used full time slice
};

/********************************* MAIN **************************************/
int main(int argc, char** argv) {
    struct commsbuf myinfo;
    shmid_pct = atoi(argv[1]);
    my_sim_pid = atoi(argv[2]);
    printf("User my_sim_pid %d spawned.\n", my_sim_pid);
    
    // Set up interrupt handler
    signal (SIGINT, siginthandler);
    
    //get IPC info and read clock for my start time
    getIPC();
    myStartTimeSecs = *simClock_secs; 
    myStartTimeNS = *simClock_ns;
    
    while(1) {
        printf("User my_sim_pid %d: waiting for message...\n", my_sim_pid);
        if ( msgrcv(oss_qid, &myinfo, sizeof(myinfo), 1, my_sim_pid) == -1 ) {
            perror("User: error in msgrcv");
            exit(0);
        }
    
        printf("User: Message received: msgtyp %ld, timeslice %u\n",  myinfo.msgtyp, myinfo.ossTimeSliceGivenNS);
    
        myinfo.userTerminatingFlag = 0;
        myinfo.userUsedFullTimeSliceFlag = 1;
        myinfo.user_sim_pid = my_sim_pid;
        myinfo.msgtyp = 99;
    
        sleep(1);
        
        printf("User: sending message to oss...\n");
        if ( msgsnd(oss_qid, &myinfo, sizeof(myinfo), 0) == -1 ) {
            perror("User: error sending msg to oss");
            exit(0);
        }
    }
    
    

    return (EXIT_SUCCESS);
}

int roll100() {
    int return_val;
    return_val = rand_r(&seed) % (100 + 1);
    return return_val;
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

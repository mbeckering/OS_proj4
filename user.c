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
int roll1000(); //returns an int in range 1-100
void reportFinishedTimeSlice();
void reportTermination();
void reportBlocked();
unsigned int randomPortionOfTimeSlice();

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
    int currentQueue;
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
    int userBlockedFlag; //from user. 1=blocked
    unsigned int userTimeUsedLastBurst; //from user, time in ns that it ran
};

struct commsbuf myinfo;

/********************************* MAIN **************************************/
int main(int argc, char** argv) {
    unsigned int localsec, localns;
    shmid_pct = atoi(argv[1]);
    my_sim_pid = atoi(argv[2]);
    printf("User my_sim_pid %d spawned.\n", my_sim_pid);
    
    // Set up interrupt handler
    signal (SIGINT, siginthandler);
    
    //get IPC info and read clock for my start time
    getIPC();
    myStartTimeSecs = *simClock_secs; 
    myStartTimeNS = *simClock_ns;
    
    /*********************USER OPERATIONS ALGORITHM ***************************/
    while(1) {
        if ( msgrcv(oss_qid, &myinfo, sizeof(myinfo), my_sim_pid, 0) == -1 ) {
            perror("User: error in msgrcv");
            exit(0);
        }
        //printf("User %d recieved msgtyp %ld\n", my_sim_pid, myinfo.msgtyp);
        
        //roll to terminate
        if (roll1000() < 10) {
            //printf("user %d rolled to terminate!\n", myinfo.user_sim_pid);
            //roll a portion of timeslice to use before terminating
            myinfo.userTimeUsedLastBurst = randomPortionOfTimeSlice();
            reportTermination();
            exit(1);
        }
        //roll to get blocked
        else if (roll1000() < 10) {
            //read sim clock
            localsec = *simClock_secs; 
            localns = *simClock_ns;
            //set a time 0-5:0-1000 to be unblocked in process control block
            pct[my_sim_pid].blockedUntilSecs = localsec + (rand_r(&seed) % 5 + 1);
            pct[my_sim_pid].blockedUntilNS = localns + (rand_r(&seed) % 1000 + 1);
            //printf("user %d rolled to GET BLOCKED until %ld:%09ld\n", myinfo.user_sim_pid,
                    //pct[my_sim_pid].blockedUntilSecs, pct[my_sim_pid].blockedUntilNS);
            reportBlocked();
        }
        else {
            reportFinishedTimeSlice();
        }

    }
    
    

    return (EXIT_SUCCESS);
}

/************************************* FUNCTIONS ******************************/

//packs appropriate information into struct and sends vis message queue
//if this user has finished all of its given timeslice for this burst
void reportFinishedTimeSlice() {
    myinfo.userBlockedFlag = 0;
    myinfo.userTerminatingFlag = 0;
    myinfo.userUsedFullTimeSliceFlag = 1;
    myinfo.userTimeUsedLastBurst = myinfo.ossTimeSliceGivenNS;
    myinfo.user_sim_pid = my_sim_pid;
    myinfo.msgtyp = 99;
    if ( msgsnd(oss_qid, &myinfo, sizeof(myinfo), 0) == -1 ) {
        perror("User: error sending msg to oss");
        exit(0);
    }
}

//packs appropriate information into struct and sends vis message queue
//if this user has rolled to terminate during this given timeslice
void reportTermination() {
    myinfo.userBlockedFlag = 0;
    myinfo.userTerminatingFlag = 1;
    myinfo.userUsedFullTimeSliceFlag = 0;
    myinfo.user_sim_pid = my_sim_pid;
    myinfo.msgtyp = 99;
    
    if ( msgsnd(oss_qid, &myinfo, sizeof(myinfo), 0) == -1 ) {
        perror("User: error sending msg to oss");
        exit(0);
    }
}

//returns time in nanoseconds of a random portion of the given timeslice
unsigned int randomPortionOfTimeSlice() {
    unsigned int return_val;
    return_val = (rand_r(&seed) % (myinfo.ossTimeSliceGivenNS) + 1);
    return return_val;
}

//pack appropriate info into message struct and report to OSS
void reportBlocked() {
    myinfo.msgtyp = 99;
    myinfo.userUsedFullTimeSliceFlag = 0;
    //use 1-99ns before getting preempted by blocking event
    myinfo.userTimeUsedLastBurst = rand_r(&seed) % 99 + 1;
    myinfo.userBlockedFlag = 1;
    if ( msgsnd(oss_qid, &myinfo, sizeof(myinfo), 0) == -1 ) {
        perror("User: error sending msg to oss");
        exit(0);
    }
}

//rolls and returns an int from 1-1000
int roll1000() {
    int return_val;
    return_val = rand_r(&seed) % (1000 + 1);
    return return_val;
}

void getIPC() {
    //process control table
    pct = (struct pcb *)shmat(shmid_pct, 0, 0);
    if ( pct == (struct pcb *)(-1) ) {
        perror("User: error in shmat pct");
        exit(1);
    }
    
    //printf("User: getting shared memory\n");
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
    printf("User realpid:%ld Terminating: Interrupted.\n", getpid());
    exit(0);
}

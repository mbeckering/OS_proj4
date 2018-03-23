/* 
 * File:   main.c
 * Author: Michael Beckering
 * Project 4
 * Spring 2018 CS-4760-E01
 * Created on March 15, 2018, 10:35 AM
 */

/******************* INCLUDES & DEFINITIONS ***********************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>

#define BILLION 1000000000 //dont want to type the wrong # of zeroes
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_pct 4020014
#define MSGQKEY_oss 4020069
#define BUFF_SZ sizeof (unsigned int)

/*********************** Function prototypes **********************************/

void makePCB(int pidnum, int isRealTime); //initializes a new PCB
void initBitVector(int); //initilize bit vector
void allocateIPC(); //allocate shared memory
void clearIPC(); //clear shared memory and message queues
void initQueue(int[], int); //initialize queues, takes queue name and size
static void siginthandler(int sig_num); //SIGINT handler
void setTimeToNextProc(); //rolls AND stores sim clock time for next user exec
int isTimeToSpawnProc(); //checks to see if it's time to spawn another process
int getOpenBitVector(); //finds open spot in bit vector, returns -1 if full
int roll1000(); //returns an into (1-100 range)
void incrementSimClockAfterMessageReceipt(int);
void spawnNewUser();
void terminateUser(int);
void blockUser(int);
int checkBlockedQueue(); //returns # of users unblocked
void awakenUser(int);
int isBlockedQueueEmpty();
int incrementIdleTime();

void printarrays();

void initQueue(int[], int);
int getNextOccupiedQueue();
int getNextProcFromQueue(int[]); //array name, returns proc_num of next process in queue, -1 if queue is empty
int addProcToQueue(int[], int, int); //array name, arrsize, proc_num, returns 1 if successful, else -1
int removeProcFromQueue(int[], int, int); //array name, arrsize, proc_num, returns 1 if successful, else -1

/************************* Global variables ***********************************/

static FILE *mlog; // log file pointer
pid_t childpids[20]; // Real-world pid values for killing children if necessary
int bitVector[19]; // Bit vector indicating used PCB's
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct; //shared memory id holder for process control table
int oss_qid; //message queue ID for OSS communications
int loglines = 0; //keeps track of lines in the log file
int numProcsSpawned = 0;
int allowNewUsers = 1;
int numCurrentUsers = 0;
int numProcsUnblocked;
static unsigned int *simClock_secs; //pointer to shm sim clock (seconds)
static unsigned int *simClock_ns; //pointer to shm sim clock (nanoseconds)
int arraysize = 19; //so I can use local sim pids 1-18 without confusion
int queue0[19]; //Round Robin queue for realtime processes
int queue1[19]; //high-priority queue
int queue2[19]; //medium-priority queue
int queue3[19]; //low-priority queue
unsigned int idleTime_secs = 0;
unsigned int idleTime_ns = 0;

int blocked[19]; //blocked queue
int nextProcToRun, nextOccupiedQueue;
unsigned int maxTimeBetweenProcsNS, maxTimeBetweenProcsSecs;
unsigned int timeToNextProcNS, timeToNextProcSecs, seed;
unsigned int spawnNextProcNS, spawnNextProcSecs;
unsigned int quantum_0 = 2000000; //time quantums: 2ms, 4ms, 8ms, 16ms
unsigned int quantum_1 = 4000000; 
unsigned int quantum_2 = 8000000;
unsigned int quantum_3 = 16000000;

struct pcb { // Process control block struct
    unsigned int totalCPUtime_secs;
    unsigned int totalCPUtime_ns;
    unsigned int totalLIFEtime_secs;
    unsigned int totalLIFEtime_ns;
    unsigned int timeUsedLastBurst_ns;
    int blocked;  // 1=blocked, 0= not blocked
    unsigned int blockedUntilSecs;
    unsigned int blockedUntilNS;
    int localPID;
    int isRealTimeClass; // 1 = realtime class process
    int currentQueue;
};

//struct for communications message queue
struct commsbuf {
    long msgtyp;
    int user_sim_pid;
    pid_t user_sys_pid;
    unsigned int ossTimeSliceGivenNS; //from oss. time slice given to run
    int userTerminatingFlag; //from user. 1=terminating, 0=not terminating
    int userUsedFullTimeSliceFlag; //from user. 1=used full time slice
    int userBlockedFlag; //from user. 1=blocked
    unsigned int userTimeUsedLastBurst; //from user, time in ns that it ran
};

struct commsbuf infostruct;

struct pcb * pct; //pct = process control table (array of 18 process
                  //control blocks)

/************************************ MAIN ************************************/

int main(int argc, char** argv) {
    int finishThem = 0;
    int i; //the great iterator
    double realTimeElapsed = 0;
    double runtimeAllowed = 3; // Seconds before timeout interrupt & termination
    maxTimeBetweenProcsNS = 999999998;
    maxTimeBetweenProcsSecs = 0;
    seed = (unsigned int) getpid(); //use my pid as first rand seed
    pid_t childpid; //holder for childpid, used when determining child fork
    char str_pct_id[20]; //string argument sent to users, holds shmid for pct
    char str_user_sim_pid[4]; // string arg for user's simulated pid (1-18)
    int user_sim_pid;
    int firstblocked;
    
    signal (SIGINT, siginthandler); // Set up interrupt handling
    time_t start = time(NULL); //start the clock
    
    // Set up logging
    mlog = fopen("master.log", "w");
    if (mlog == NULL) {
        perror("OSS: error opening log file");
        return -1;
    }
    
    fprintf(mlog, "OSS: Launched\n");
    
    //Set up shared memory and initialize bitvector and queues
    allocateIPC();
    initBitVector(arraysize);
    initQueue(queue0, arraysize);
    initQueue(queue1, arraysize);
    initQueue(queue2, arraysize);
    initQueue(queue3, arraysize);
    initQueue(blocked, arraysize);
    
    //schedule a time for the first process to spawn
    setTimeToNextProc();
    printf("First user will spawn at %u:%u\n", 
    spawnNextProcSecs, spawnNextProcNS);
    
    /********************* SCHEDULING ALGORITHM *******************************/
    while (1) {
        //update timer and turn off user generation if needed
        if (realTimeElapsed < runtimeAllowed) {
            realTimeElapsed = (double)(time(NULL) - start);
            if (realTimeElapsed >= runtimeAllowed) {
                printf("OSS: Real-time limit of %f has elapsed. No new users"
                        "will be generated\n", runtimeAllowed);
                allowNewUsers = 0;
            }
        }
        //if no new users are allowed and no users are left in the system, break
        if (allowNewUsers == 0 && numCurrentUsers == 0) {
            printf("OSS: User generation is disallowed and "
                    "all users have terminated.\n");
            fprintf(mlog, "OSS: User generation is disallowed and "
                    "all users have terminated.\n");
            break;
        }
        //if any users are blocked, parse the blocked queue and awaken 
        //  users if their time has come, placing them in the proper queue
        numProcsUnblocked = 0;
        if (isBlockedQueueEmpty() == 0) { 
            numProcsUnblocked = checkBlockedQueue();
        }

        //get highest priority occupied queue number
        nextOccupiedQueue = getNextOccupiedQueue();
        //if non-blocked queues are empty AND new user generation is turned off
        //then only blocked processes remain, need to change strategy.
        //set time to next proc spawn, advance sim clock that far
        if ( (nextOccupiedQueue == -1) && (allowNewUsers == 0) ) {
            setTimeToNextProc();
            fprintf(mlog, "OSS: No processes ready to run, incrementing sim "
                    "clock from %u:%09u to ", *simClock_secs, *simClock_ns);
            incrementIdleTime();
            *simClock_secs = spawnNextProcSecs;
            *simClock_ns = spawnNextProcNS;
            fprintf(mlog, "%u:%09u\n", *simClock_secs, *simClock_ns);
            fflush(mlog);
            //if a user was not unblocked, restart algo and see if it's time
            if (numProcsUnblocked == 0) {
                finishThem = 1;
                continue;
            }
        }
        //if active queues are empty, and new user spawns are allowed, 
        //advance system clock to next spawn time, spawn a user, and schedule it
        else if ( nextOccupiedQueue == -1 && allowNewUsers == 1 ) {
            fprintf(mlog, "OSS: Current live users = %d\n", numCurrentUsers);
            fprintf(mlog, "OSS: No processes ready to run, incrementing sim "
                    "clock from %u:%09u to ", *simClock_secs, *simClock_ns);
            incrementIdleTime();
            *simClock_secs = spawnNextProcSecs;
            *simClock_ns = spawnNextProcNS;
            fprintf(mlog, "%u:%09u\n", *simClock_secs, *simClock_ns);
            fflush(mlog);
            if ( (allowNewUsers == 1) && (getOpenBitVector() != -1) ) {
                spawnNewUser(); //also sets new time for next proc spawn and
                                //packs proper info into infostruct
                numCurrentUsers++;
                fprintf(mlog, "OSS: Dispatching process PID %d from queue %d at "
                    "time %u:%09u\n", infostruct.user_sim_pid ,
                        pct[infostruct.user_sim_pid].currentQueue,
                        *simClock_secs, *simClock_ns);
                if ( msgsnd(oss_qid, &infostruct, sizeof(infostruct), 0) == -1 ) {
                    perror("OSS: error sending init msg");
                    clearIPC();
                    exit(0);
                }
            }
        }
        //this block of code exists to prevent a hang that was occuring
        //after a process awoke from the blocked queue under the conditions
        //that only blocked processes remain in the system and no new
        //process generation is allowed. I should have structured my entire
        //algorightm differently to prevent this, but i'm OUT OF TIME!
        else if ( finishThem == 1 && nextOccupiedQueue != -1) {
            finishThem = 0;
                if (nextOccupiedQueue == 0) {
                    nextProcToRun = getNextProcFromQueue(queue0);
                    infostruct.ossTimeSliceGivenNS = quantum_0;
                    removeProcFromQueue(queue0, arraysize, nextProcToRun);
                    addProcToQueue(queue0, arraysize, nextProcToRun);
                }
                else if (nextOccupiedQueue == 1) {
                    nextProcToRun = getNextProcFromQueue(queue1);
                    infostruct.ossTimeSliceGivenNS = quantum_1;
                    removeProcFromQueue(queue1, arraysize, nextProcToRun);
                    addProcToQueue(queue1, arraysize, nextProcToRun);
                }
                //else continue;
                if (nextProcToRun != -1) {
                    infostruct.msgtyp = (long) nextProcToRun;
                    infostruct.userTerminatingFlag = 0;
                    infostruct.userUsedFullTimeSliceFlag = 0;
                    infostruct.userBlockedFlag = 0;
                    infostruct.user_sim_pid = nextProcToRun;
                    fprintf(mlog, "OSS: Dispatching process PID %d from queue %d at "
                    "time %u:%09u\n", nextProcToRun, 
                    pct[nextProcToRun].currentQueue, 
                    *simClock_secs, *simClock_ns);
                    if ( msgsnd(oss_qid, &infostruct, sizeof(infostruct), 0) == -1 ) {
                        perror("OSS: error sending user msg");
                        clearIPC();
                        exit(0);
                    }
                }
        }
        
        
        firstblocked = blocked[1]; //ghetto bug fix
        //wait for a message from any user (Always msgtyp 99)
        if ( msgrcv(oss_qid, &infostruct, sizeof(infostruct), 99, 0) == -1 ) {
            perror("User: error in msgrcv");
            clearIPC();
            exit(0);
        }
        blocked[1] = firstblocked; //ghetto bug fix
        
        //store CPU time used in appropriate process control block
        pct[infostruct.user_sim_pid].timeUsedLastBurst_ns = 
                infostruct.userTimeUsedLastBurst;
        //increment the sim clock accordingly
        incrementSimClockAfterMessageReceipt(infostruct.user_sim_pid);
        
        //store & clear info, and remove from queue, if user reports termination
        if (infostruct.userTerminatingFlag == 1) {
            fprintf(mlog, "OSS: Receiving that process PID %d ran for %u"
                    " nanoseconds and "
                    "then terminated\n", infostruct.user_sim_pid,
                    infostruct.userTimeUsedLastBurst);
            terminateUser(infostruct.user_sim_pid);
        }
        //if the user was blocked on some event
        else if (infostruct.userBlockedFlag == 1) {
            fprintf(mlog, "OSS: Receiving that process PID %d ran for %u "
                    "nanoseconds and "
                    "then was blocked by an event. Moving to blocked queue\n", 
                    infostruct.user_sim_pid, infostruct.userTimeUsedLastBurst);
            blockUser(infostruct.user_sim_pid);
        }
        //if the process didn't finish timeslice but didn't get blocked
        else if (infostruct.userUsedFullTimeSliceFlag == 0) {
            fprintf(mlog, "OSS: Receiving that process PID %d ran for %u "
                    "nanoseconds, not "
                    "using its entire quantum", infostruct.user_sim_pid,
                    infostruct.userTimeUsedLastBurst);
            //if in queue2 or 3, move to queue 1
            if (pct[infostruct.user_sim_pid].currentQueue == 2) {
                fprintf(mlog, ", moving to queue 1\n");
                removeProcFromQueue(queue2, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue1, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 1;
            }
            else if (pct[infostruct.user_sim_pid].currentQueue == 3) {
                fprintf(mlog, ", moving to queue 1\n");
                removeProcFromQueue(queue3, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue1, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 1;
            }
            else {
                fprintf(mlog, "\n");
            }
        }
        //if full time slice was used
        else if (infostruct.userUsedFullTimeSliceFlag == 1) {
            fprintf(mlog, "OSS: Receiving that process PID %d ran for %u "
                    "nanoseconds", 
                    infostruct.user_sim_pid,
                    infostruct.userTimeUsedLastBurst);
            //if in queue1, move down to queue2
            if (pct[infostruct.user_sim_pid].currentQueue == 1) {
                fprintf(mlog, ", moving to queue 2\n");
                removeProcFromQueue(queue1, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue2, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 2;
            }
            //if in queue2, move down to queue3
            else if (pct[infostruct.user_sim_pid].currentQueue == 2) {
                fprintf(mlog, ", moving to queue 3\n");
                removeProcFromQueue(queue2, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue3, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 3;
            }
            else {
                fprintf(mlog, "\n");
            }
        }
        
        //if it's time, and it's allowed, spawn a new user
        if (allowNewUsers == 1) {
            if (isTimeToSpawnProc()) {
                if (getOpenBitVector() != -1) {
                    spawnNewUser();
                    numCurrentUsers++;
                }
                else {
                    fprintf(mlog, "OSS: New process spawn denied: "
                            "process control table is full.\n");
                    setTimeToNextProc();
                }
            }
        }

        //obtain highest priority occupied queue, get pid of first process
        //in that queue, attach appropriate time quantum, and move the
        //process to the back of the queue it's in, and schedule it
        nextOccupiedQueue = getNextOccupiedQueue();
        if (nextOccupiedQueue == 0) {
            nextProcToRun = getNextProcFromQueue(queue0);
            infostruct.ossTimeSliceGivenNS = quantum_0;
            removeProcFromQueue(queue0, arraysize, nextProcToRun);
            addProcToQueue(queue0, arraysize, nextProcToRun);
        }
        else if (nextOccupiedQueue == 1) {
            nextProcToRun = getNextProcFromQueue(queue1);
            infostruct.ossTimeSliceGivenNS = quantum_1;
            removeProcFromQueue(queue1, arraysize, nextProcToRun);
            addProcToQueue(queue1, arraysize, nextProcToRun);
        }
        else if (nextOccupiedQueue == 2) {
            nextProcToRun = getNextProcFromQueue(queue2);
            infostruct.ossTimeSliceGivenNS = quantum_2;
            removeProcFromQueue(queue2, arraysize, nextProcToRun);
            addProcToQueue(queue2, arraysize, nextProcToRun);
        }
        else if (nextOccupiedQueue == 3) {
            nextProcToRun = getNextProcFromQueue(queue3);
            infostruct.ossTimeSliceGivenNS = quantum_3;
            removeProcFromQueue(queue3, arraysize, nextProcToRun);
            addProcToQueue(queue3, arraysize, nextProcToRun);
        }
        //if no active queues have any processes, loop back to the start
        //this can occur if all processes in the system are blocked
        //start of algo will then jump to time for next user spawn
        else {
            continue;
        }
        //schedule next user
        if (nextProcToRun != -1) {
            infostruct.msgtyp = (long) nextProcToRun;
            infostruct.userTerminatingFlag = 0;
            infostruct.userUsedFullTimeSliceFlag = 0;
            infostruct.userBlockedFlag = 0;
            infostruct.user_sim_pid = nextProcToRun;
            fprintf(mlog, "OSS: Dispatching process PID %d from queue %d at "
                    "time %u:%09u\n", nextProcToRun, 
                    pct[nextProcToRun].currentQueue, 
                    *simClock_secs, *simClock_ns);
            if ( msgsnd(oss_qid, &infostruct, sizeof(infostruct), 0) == -1 ) {
                perror("OSS: error sending init msg");
                clearIPC();
                exit(0);
            }
        }
    
    }
    
    fprintf(mlog, "OSS: Terminated: Normal.\n");
    fflush(mlog);
    clearIPC();
    printf("OSS: Normal exit\n");

    return (EXIT_SUCCESS);
}

/*************************** END MAIN *****************************************/

/*************************** FUNCTIONS ****************************************/

int incrementIdleTime(){
    unsigned int temp, localsec, localns, localsim_s, localsim_ns;
    localsec = 0;
    localsim_s = *simClock_secs;
    localsim_ns = *simClock_ns;
    if (localsim_s == spawnNextProcSecs) {
        localns = (spawnNextProcNS - localsim_ns);
    }
    else {
        localsec = (spawnNextProcSecs - localsim_s);
        localns = spawnNextProcNS + (BILLION - localsim_ns);
        localsec--;
    }
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    idleTime_secs = idleTime_secs + localsec; 
    idleTime_ns = idleTime_ns + localns;
    if (idleTime_ns >= BILLION) {
        idleTime_secs++;
        temp = idleTime_ns - BILLION;
        idleTime_ns = temp;
    }
    return 1;
}

int isBlockedQueueEmpty() {
    if (blocked[1] != 0) {
        return 0;
    }
    return 1;
}

//compares sim clock with "blocked-until" times of each occupied
//slot in the blocked queue and wakes them up if it's time
int checkBlockedQueue() {
    int returnval = 0;
    int j;
    //read current simclock
    unsigned int localsec = *simClock_secs;
    unsigned int localns = *simClock_ns;
    //for each slot in the blocked queue
    for (j=1; j<arraysize; j++) {
        //if the blocked array slot is occupied by a process
        if (blocked[j] != 0) {
            //and if the time has passed for it to be awoken
            if ( (localsec > pct[blocked[j]].blockedUntilSecs) || 
            ( (localsec >= pct[blocked[j]].blockedUntilSecs) && 
                    (localns >= pct[blocked[j]].blockedUntilNS) ) ) {
                //then AWAKEN HIIIIIIIIIIM!!!!
                //printf("OSS: Calling awakenUser(%d) ... ", blocked[j]);
                awakenUser(blocked[j]);
                returnval++;
            }
        }
    }
    return returnval;
}

void awakenUser(int wakepid) {
    unsigned int localsec, localns, temp;
    fprintf(mlog, "OSS: Waking up user %d ", wakepid);
    //remove this user from the blocked queue
    removeProcFromQueue(blocked, arraysize, wakepid);
    //update the process control block
    pct[wakepid].blocked = 0;
    pct[wakepid].blockedUntilNS = 0;
    pct[wakepid].blockedUntilSecs = 0; 
    //if it's realtime, put it back in realitime queue
    if (pct[wakepid].isRealTimeClass == 1) {
        addProcToQueue(queue0, arraysize, wakepid);
        pct[wakepid].currentQueue = 0;
        fprintf(mlog, "and putting into queue 0\n");
    }
    //otherwise add it back into queue 1 (high-priority)
    else {
        addProcToQueue(queue1, arraysize, wakepid);
        pct[wakepid].currentQueue = 1;
        fprintf(mlog, "and putting into queue 1\n");
    }
    //increment sim clock to represent moving process to blocked queue
    localsec = *simClock_secs;
    localns = *simClock_ns;
    temp = roll1000();
    if (temp < 100) temp = 10000;
    else temp = temp * 100;
    localns = localns + temp; //increment 1000-100,000ns for oss operations
    fprintf(mlog, "OSS: Time used to awaken user from blocked state: %u "
            "nanoseconds\n", temp);
}

void blockUser(int blockpid) {
    int temp;
    unsigned int localsec, localns;
    //remove from current queue
    pct[blockpid].blocked = 1;
    if (pct[blockpid].currentQueue == 0)
        {removeProcFromQueue(queue0, arraysize, blockpid);}
    else if (pct[blockpid].currentQueue == 1)
        {removeProcFromQueue(queue1, arraysize, blockpid);}
    else if (pct[blockpid].currentQueue == 2)
        {removeProcFromQueue(queue2, arraysize, blockpid);}
    else if (pct[blockpid].currentQueue == 3)
        {removeProcFromQueue(queue3, arraysize, blockpid);}
    //and add it to the blocked queue
    addProcToQueue(blocked, arraysize, blockpid);
    //increment sim clock to represent moving process to blocked queue
    localsec = *simClock_secs;
    localns = *simClock_ns;
    temp = roll1000();
    if (temp < 100) temp = 10000;
    else temp = temp * 100;
    localns = localns + temp; //increment 1000-100,000ns for oss operations
    fprintf(mlog, "OSS: Time used to move user to blocked queue: %u "
            "nanoseconds\n", temp);
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    //update the sim clock in shared memory
    *simClock_secs = localsec;
    *simClock_ns = localns;
}

void terminateUser(int termpid) {
    int status;
    waitpid(infostruct.user_sys_pid, &status, 0);
    if (pct[termpid].currentQueue == 0) {
        removeProcFromQueue(queue0, arraysize, termpid);
    }
    else if (pct[termpid].currentQueue == 1) {
        removeProcFromQueue(queue1, arraysize, termpid);
    }
    else if (pct[termpid].currentQueue == 2) {
        removeProcFromQueue(queue2, arraysize, termpid);
    }
    else if (pct[termpid].currentQueue == 3) {
        removeProcFromQueue(queue3, arraysize, termpid);
    }
    bitVector[termpid] = 0;
    numCurrentUsers--;
    printf("OSS: User %d has terminated. Users alive: %d\n",
        infostruct.user_sim_pid, numCurrentUsers);
}

int getNextOccupiedQueue() {
    if (queue0[1] != 0) {
        return 0;
    }
    else if (queue1[1] != 0) {
        return 1;
    }
    else if (queue2[1] != 0) {
        return 2;
    }
    else if (queue3[1] != 0) {
        return 3;
    }
    else return -1;
}

int addProcToQueue (int q[], int arrsize, int proc_num) {
    int i;
    for (i=1; i<arrsize; i++) {
        if (q[i] == 0) { //empty queue spot found
            q[i] = proc_num;
            return 1;
        }
    }
    return -1; //no empty spot found
}

int removeProcFromQueue(int q[], int arrsize, int proc_num) {
    //do something with pct
    int i;
    for (i=1; i<arrsize; i++) {
        if (q[i] == proc_num) { //found the process to remove from queue
            while(i+1 < arrsize) { //shift next in queue down 1, repeat
                q[i] = q[i+1];
                i++;
            }
            q[18] = 0; //once 18 is moved to 17, clear it up by setting to 0
            return 1;
        }
    }
    return -1;
}

int getNextProcFromQueue(int q[]) {
    if (q[1] == 0) { //queue is empty
        return -1;
    }
    else return q[1];
}

//also sets time for next process
void spawnNewUser() {
    char str_pct_id[20]; //string argument sent to users, holds shmid for pct
    char str_user_sim_pid[4]; // string arg for user's simulated pid (1-18)
    int user_sim_pid, roll;
    pid_t childpid;
    setTimeToNextProc(); //decide when the next user process will be spawned
    user_sim_pid = getOpenBitVector();
    if (user_sim_pid == -1) {
        printf("OSS: Error in spawnNewUser: no open bitvector\n");
        clearIPC();
        exit(0);
    }
    numProcsSpawned++;
    //flag for no new processes if we have spawned 100
    if (numProcsSpawned >= 100) {
        printf("OSS: 100 total users spawned. No new users will be "
                "generated after this one.\n");
        fprintf(mlog, "OSS: 100 total users spawned. "
                "No new users will be generated after this one.\n");
        allowNewUsers = 0;
    }
    infostruct.msgtyp = user_sim_pid;
    infostruct.userTerminatingFlag = 0;
    infostruct.userUsedFullTimeSliceFlag = 0;
    infostruct.user_sim_pid = user_sim_pid;
    //chance it's a real-time process
    roll = roll1000();
    if (roll < 45) {
        printf("OSS: Rolled a real-time class user process.\n");
        infostruct.ossTimeSliceGivenNS = quantum_0;
        makePCB(user_sim_pid, 1);
        addProcToQueue(queue0, arraysize, user_sim_pid);
        pct[user_sim_pid].currentQueue = 0;
    }
    //user process
    else {
        infostruct.ossTimeSliceGivenNS = quantum_1;
        makePCB(user_sim_pid, 0);
        addProcToQueue(queue1, arraysize, user_sim_pid);
        pct[user_sim_pid].currentQueue = 1;
    }
    fprintf(mlog, "OSS: Generating process PID %d and putting it in queue "
            "%d at time %u:%09u, total spawned: %d\n", 
        pct[user_sim_pid].localPID, pct[user_sim_pid].currentQueue, 
        *simClock_secs, *simClock_ns, numProcsSpawned);
    fflush(mlog);
    if ( (childpid = fork()) < 0 ){ //terminate code
        perror("OSS: Error forking user");
        fprintf(mlog, "Fork error\n");
        clearIPC();
        exit(0);
    }
    if (childpid == 0) { //child code
        sprintf(str_pct_id, "%d", shmid_pct); //build arg1 string
        sprintf(str_user_sim_pid, "%d", user_sim_pid);
        execlp("./user", "./user", str_pct_id, str_user_sim_pid, (char *)NULL);
        perror("OSS: execl() failure"); //report & exit if exec fails
        exit(0);
    }
    childpids[user_sim_pid] = childpid;
}

void incrementSimClockAfterMessageReceipt(int userpid) {
    unsigned int localsec = *simClock_secs;
    unsigned int localns = *simClock_ns;
    unsigned int temp;
    temp = roll1000();
    if (temp < 100) temp = 100;
    else temp = temp * 10;
    localns = localns + temp; //increment 100-10,000ns for oss operations
    fprintf(mlog, "OSS: Time spent this dispatch: %ld nanoseconds\n", temp);
    //increment last user burst
    localns = localns + pct[userpid].timeUsedLastBurst_ns;
    
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    //update the sim clock in shared memory
    *simClock_secs = localsec;
    *simClock_ns = localns;
}

//sets length of sim time from now until next child process spawn
//AND sets variables to indicate when that time will be on the sim clock
void setTimeToNextProc() {
    unsigned int temp;
    unsigned int localsecs = *simClock_secs;
    unsigned int localns = *simClock_ns;
    timeToNextProcSecs = rand_r(&seed) % (maxTimeBetweenProcsSecs + 1);
    timeToNextProcNS = rand_r(&seed) % (maxTimeBetweenProcsNS + 1);
    spawnNextProcSecs = localsecs + timeToNextProcSecs;
    spawnNextProcNS = localns + timeToNextProcNS;
    if (spawnNextProcNS >= BILLION) { //roll ns to s if > bill
        spawnNextProcSecs++;
        temp = spawnNextProcNS - BILLION;
        spawnNextProcNS = temp;
    }
    
    fprintf(mlog, "OSS: Next user spawn scheduled for %ld:%09ld\n", 
        spawnNextProcSecs, spawnNextProcNS );
}

int roll1000() {
    int return_val;
    return_val = rand_r(&seed) % (1000 + 1);
    return return_val;
}

int isTimeToSpawnProc() {
    int return_val = 0;
    unsigned int localsec = *simClock_secs;
    unsigned int localns = *simClock_ns;
    if ( (localsec > spawnNextProcSecs) || 
            ( (localsec >= spawnNextProcSecs) && (localns >= spawnNextProcNS) ) ) {
        return_val = 1;
    }
    //printf("OSS SPAWN CHECK FUNCTION: time: %ld:%ld next: %ld:%ld isTimeToSpawnProc = %d\n", localsec, localns, spawnNextProcSecs, spawnNextProcNS, return_val);
    return return_val;
}

int getOpenBitVector() {
    int i;
    int return_val = -1;
    for (i=1; i<19; i++) {
        if (bitVector[i] == 0) {
            return_val = i;
            break;
        }
    }
    return return_val;
}

//makePCB initializes a new process control block and sets bit vector
//accepts simulated pid number and whether or not it's a realtime class process
void makePCB(int pidnum, int isRealTime) {
    unsigned int localsec = *simClock_secs;
    unsigned int localns = *simClock_ns;
    pct[pidnum].totalCPUtime_secs = 0;
    pct[pidnum].totalCPUtime_ns = 0;
    pct[pidnum].totalLIFEtime_secs = 0;
    pct[pidnum].totalLIFEtime_ns = 0;
    pct[pidnum].timeUsedLastBurst_ns = 0;
    pct[pidnum].blocked = 0;
    pct[pidnum].blockedUntilSecs = 0;
    pct[pidnum].blockedUntilNS = 0;
    pct[pidnum].localPID = pidnum; //pids will be 1-18
    pct[pidnum].isRealTimeClass = isRealTime;
    //place into round-robin queue if realtime, else queue1
    if (isRealTime == 1) {
        pct[pidnum].currentQueue = 0;
    }
    else {
        pct[pidnum].currentQueue = 1;
    }
    bitVector[pidnum] = 1; //mark this pcb taken in bit vector
}

//initialize bit vector based on specified size
void initBitVector(int n) {
    int i;
    for (i=0; i<n; i++) {
        bitVector[i] = 0;
    }
}

// Function to set up shared memory
void allocateIPC() {
    printf("OSS: Allocating shared memory\n");
    //process control table
    shmid_pct = shmget(SHMKEY_pct, 19*sizeof(struct pcb), 0777 | IPC_CREAT);
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
    
    //message queue
    if ( (oss_qid = msgget(MSGQKEY_oss, 0777 | IPC_CREAT)) == -1 ) {
        perror("Error generating communication message queue");
        exit(0);
    }
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

    if ( msgctl(oss_qid, IPC_RMID, NULL) == -1 ) {
        perror("OSS: Error removing ODD message queue");
        exit(0);
    }
    //log file
    if (mlog != NULL) {
        fprintf(mlog, "OSS: Total CPU idle time: %u:%09u\n", 
                idleTime_secs, idleTime_ns);
        fflush(mlog);
        fclose(mlog);
    }
}

void initQueue(int q[], int size) {
    int i;
    for(i=0; i<size; i++) {
        q[i] = 0;
    }
}

void printarrays() {
    //printing shit for testing
    fprintf(mlog, "Users allegedly alive: %d\n", numCurrentUsers);
    int i;
        fprintf(mlog, "Q0-");
        for (i=1; i<arraysize; i++) {
            fprintf(mlog,"%d.", queue0[i]);
        }
        fprintf(mlog," Q1-");
        for (i=1; i<arraysize; i++) {
            fprintf(mlog,"%d.", queue1[i]);
        }
        fprintf(mlog," Q2-");
        for (i=1; i<arraysize; i++) {
            fprintf(mlog,"%d.", queue2[i]);
        }
        fprintf(mlog," Q3-");
        for (i=1; i<arraysize; i++) {
            fprintf(mlog,"%d.", queue3[i]);
        }
        fprintf(mlog," BL-");
        for (i=1; i<arraysize; i++) {
            fprintf(mlog,"%d.", blocked[i]);
        }
        fprintf(mlog,"\n");
}

/********************* INTERRUPT HANDLING *************************************/

//SIGINT handler
static void siginthandler(int sig_num) {
    printf("OSS: Ctrl+C interrupt detected! signo = %d\n", sig_num);
    
    //killchildren();
    clearIPC();
    
    if (mlog != NULL) {
        fprintf(mlog, "OSS: Terminated: Interrupted\n");
        fflush(mlog);
        //fclose(mlog);
    }
    
    printf("OSS: Terminated: Interrupted\n");
    exit(0);
}


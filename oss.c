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
static int setperiodic(double); //timed interrupt handler
static int setinterrupt(); //SIGALRM handler
static void interrupt(int signo, siginfo_t *info, void *context); //actions
static void siginthandler(int sig_num); //SIGINT handler
void setTimeToNextProc(); //rolls AND stores sim clock time for next user exec
int isTimeToSpawnProc(); //checks to see if it's time to spawn another process
int getOpenBitVector(); //finds open spot in bit vector, returns -1 if full
int roll1000(); //returns an into (1-100 range)
void incrementSimClockAfterMessageReceipt(int);
void spawnNewUser();
void terminateUser(int);
int getNextOccupiedQueue();

void initQueue(int[], int);
int getNextProcFromQueue(int[]); //array name, returns proc_num of next process in queue, -1 if queue is empty
int addProcToQueue(int[], int, int); //array name, arrsize, proc_num, returns 1 if successful, else -1
int removeProcFromQueue(int[], int, int); //array name, arrsize, proc_num, returns 1 if successful, else -1

/************************* Global variables ***********************************/

pid_t childpids[20]; // Real-world pid values for killing children if necessary
int bitVector[19]; // Bit vector indicating used PCB's
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_pct; //shared memory id holder for process control table
int oss_qid; //message queue ID for OSS communications
static unsigned int *simClock_secs; //pointer to shm sim clock (seconds)
static unsigned int *simClock_ns; //pointer to shm sim clock (nanoseconds)
int arraysize = 19; //so I can use local sim pids 1-18 without confusion
int queue0[19]; //Round Robin queue for realtime processes
int queue1[19]; //high-priority queue
int queue2[19]; //medium-priority queue
int queue3[19]; //low-priority queue
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
    unsigned int ossTimeSliceGivenNS; //from oss. time slice given to run
    int userTerminatingFlag; //from user. 1=terminating, 0=not terminating
    int userUsedFullTimeSliceFlag; //from user. 1=used full time slice
    unsigned int userTimeUsedLastBurst; //from user, time in ns that it ran
};

struct commsbuf infostruct;

struct pcb * pct; //pct = process control table (array of 18 process
                  //control blocks)

/************************************ MAIN ************************************/

int main(int argc, char** argv) {
    int i; //the great iterator
    maxTimeBetweenProcsNS = 999999998;
    maxTimeBetweenProcsSecs = 0;
    seed = (unsigned int) getpid(); //use my pid as first rand seed
    double runtime = 10; // Seconds before timeout interrupt & termination
    pid_t childpid; //holder for childpid, used when determining child fork
    char str_pct_id[20]; //string argument sent to users, holds shmid for pct
    char str_user_sim_pid[4]; // string arg for user's simulated pid (1-18)
    int user_sim_pid;
    
    // Set up ctrl^c interrupt handling
    signal (SIGINT, siginthandler);
    //set up interrupt timer
    if (setinterrupt() == -1) {
        perror("Failed to set up SIGALRM handler");
        return 1;
    }
    // Set up periodic timer
    if (setperiodic(runtime) == -1) {
        perror("Failed to setup periodic interrupt");
        return 1;
    }
    
    //Set up shared memory and initialize bitvector and queues
    allocateIPC();
    initBitVector(arraysize);
    initQueue(queue0, arraysize);
    initQueue(queue1, arraysize);
    initQueue(queue2, arraysize);
    initQueue(queue3, arraysize);
    
    //schedule a time for the first process to spawn
    setTimeToNextProc();
    printf("First user will spawn at %u:%u\n", 
    spawnNextProcSecs, spawnNextProcNS);
    
    /********************* SCHEDULING ALGORITHM *******************************/
    while (1) {
        //get highest priority occupied queue number
        nextOccupiedQueue = getNextOccupiedQueue();
        //if all queues are empty, advance system clock to next spawn time,
        //spawn a user, and send schedule it
        if (nextOccupiedQueue == -1) {
            *simClock_secs = spawnNextProcSecs;
            *simClock_ns = spawnNextProcNS;
            spawnNewUser(); //also sets new time for next proc spawn and
                            //packs proper info into infostruct
            if ( msgsnd(oss_qid, &infostruct, sizeof(infostruct), 0) == -1 ) {
                perror("OSS: error sending init msg");
                clearIPC();
                exit(0);
            }
        }
        //wait for a message from any user (Always msgtyp 99)
        if ( msgrcv(oss_qid, &infostruct, sizeof(infostruct), 99, 0) == -1 ) {
            perror("User: error in msgrcv");
            clearIPC();
            exit(0);
        }
        //store CPU time used in appropriate process control block
        pct[infostruct.user_sim_pid].timeUsedLastBurst_ns = 
                infostruct.userTimeUsedLastBurst;
        //increment the sim clock accordingly
        incrementSimClockAfterMessageReceipt(infostruct.user_sim_pid);
        
        printf("OSS: Message received from user %d, ran %u, clock now %ld:%ld\n", 
                infostruct.user_sim_pid, infostruct.userTimeUsedLastBurst, 
                *simClock_secs, *simClock_ns);
        
        //store & clear info, and remove from queue, if user reports termination
        if (infostruct.userTerminatingFlag == 1) {
            printf("OSS: User reported termination!\n");
            sleep(1);
            terminateUser(infostruct.user_sim_pid);
        }
        //if full time slice was used
        else if (infostruct.userUsedFullTimeSliceFlag == 1) {
            //if in queue1, move down to queue2
            if (pct[infostruct.user_sim_pid].currentQueue == 1) {
                removeProcFromQueue(queue1, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue2, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 2;
            }
            //if in queue2, move down to queue3
            else if (pct[infostruct.user_sim_pid].currentQueue == 2) {
                removeProcFromQueue(queue2, arraysize, infostruct.user_sim_pid);
                addProcToQueue(queue3, arraysize, infostruct.user_sim_pid);
                pct[infostruct.user_sim_pid].currentQueue = 3;
            }
        }
        
        //spawn a user process if it's time AND there's an open spot
        if (isTimeToSpawnProc() && (getOpenBitVector() != -1) ) {
            spawnNewUser();
        }
        //if it's time but there's no open space, only set a new time to spawn
        else if (isTimeToSpawnProc() && (getOpenBitVector() == -1) ) {
            setTimeToNextProc();
        }
        //obtain highest priority occupied queue, get pid of first process
        //in that queue, attach appropriate time quantum, and move the
        //process to the back of the queue it's in
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
        else {
            printf("OSS: No processes in queue\n");
            continue;
        }
        if (nextProcToRun != -1) {
            infostruct.msgtyp = (long) nextProcToRun;
            infostruct.userTerminatingFlag = 0;
            infostruct.userUsedFullTimeSliceFlag = 0;
            infostruct.user_sim_pid = nextProcToRun;
            printf("OSS: Scheduling user %d now\n", nextProcToRun);
            if ( msgsnd(oss_qid, &infostruct, sizeof(infostruct), 0) == -1 ) {
                perror("OSS: error sending init msg");
                clearIPC();
                exit(0);
            }
        }
        else printf("OSS: no processes in queue\n");
        
        printf("Q0:");
        for (i=1; i<arraysize; i++) {
            printf("%d.", queue0[i]);
        }
        printf(" Q1:");
        for (i=1; i<arraysize; i++) {
            printf("%d.", queue1[i]);
        }
        printf(" Q2:");
        for (i=1; i<arraysize; i++) {
            printf("%d.", queue2[i]);
        }
        printf(" Q3:");
        for (i=1; i<arraysize; i++) {
            printf("%d.", queue3[i]);
        }
        printf("\n");
    
    }
    
    clearIPC();
    printf("OSS: Normal exit\n");

    return (EXIT_SUCCESS);
}

/*************************** END MAIN *****************************************/

/*************************** FUNCTIONS ****************************************/

void terminateUser(int termpid) {
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
            //do something with pct
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
    int user_sim_pid;
    pid_t childpid;
    setTimeToNextProc(); //decide when the next user process will be spawned
    user_sim_pid = getOpenBitVector();
    if (user_sim_pid == -1) {
        printf("OSS: Error in spawnNewUser: no open bitvector\n");
        clearIPC();
        exit(0);
    }
    infostruct.msgtyp = user_sim_pid;
    infostruct.userTerminatingFlag = 0;
    infostruct.userUsedFullTimeSliceFlag = 0;
    infostruct.user_sim_pid = user_sim_pid;
    // 10% chance it's a real-time process
    if (roll1000() < 10) {
        infostruct.ossTimeSliceGivenNS = quantum_0;
        makePCB(user_sim_pid, 1);
        addProcToQueue(queue0, arraysize, user_sim_pid);
    }
    else {
        infostruct.ossTimeSliceGivenNS = quantum_0;
        makePCB(user_sim_pid, 0);
        addProcToQueue(queue1, arraysize, user_sim_pid);
    }
    if ( (childpid = fork()) < 0 ){ //terminate code
        perror("OSS: Error forking user");
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
    localns = localns + 100; //increment 100ns for oss operations
    //increment last user burst
    localns = localns + pct[userpid].timeUsedLastBurst_ns;
    
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    
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
    
    printf("OSS SET NEXT PROC FUNCTION: Next user will spawn at %ld:%ld\n", spawnNextProcSecs, spawnNextProcNS );
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
    printf("BITVECTOR FUNCTION: Open bit vector result: %d\n", return_val);
    return return_val;
}

//makePCB initializes a new process control block and sets bit vector
//accepts simulated pid number and whether or not it's a realtime class process
void makePCB(int pidnum, int isRealTime) {
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
}

void initQueue(int q[], int size) {
    int i;
    for(i=0; i<size; i++) {
        q[i] = 0;
    }
}

/********************* INTERRUPT HANDLING *************************************/

//this function taken from UNIX text
static int setperiodic(double sec) {
    timer_t timerid;
    struct itimerspec value;
    
    if (timer_create(CLOCK_REALTIME, NULL, &timerid) == -1)
        return -1;
    value.it_interval.tv_sec = (long)sec;
    value.it_interval.tv_nsec = (sec - value.it_interval.tv_sec)*BILLION;
    if (value.it_interval.tv_nsec >= BILLION) {
        value.it_interval.tv_sec++;
        value.it_interval.tv_nsec -= BILLION;
    }
    value.it_value = value.it_interval;
    return timer_settime(timerid, 0, &value, NULL);
}

//this function taken from UNIX text
static int setinterrupt() {
    struct sigaction act;
    
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = interrupt;
    if ((sigemptyset(&act.sa_mask) == -1) ||
            (sigaction(SIGALRM, &act, NULL) == -1))
        return -1;
    return 0;
}

//action taken after timed interrupt is detected
static void interrupt(int signo, siginfo_t *info, void *context) {
    printf("OSS: Timer Interrupt Detected! signo = %d\n", signo);
    //killchildren();
    clearIPC();
    //close log file
    //fprintf(mlog, "OSS: Terminated: Timed Out\n");
    //fclose(mlog);
    printf("OSS: Terminated: Timed Out\n");
    exit(0);
}

//SIGINT handler
static void siginthandler(int sig_num) {
    printf("OSS: Ctrl+C interrupt detected! signo = %d\n", sig_num);
    
    //killchildren();
    clearIPC();
    
    //fprintf(mlog, "OSS: Terminated: Interrupted\n");
    //fclose(mlog);
    
    printf("OSS: Terminated: Interrupted\n");
    exit(0);
}


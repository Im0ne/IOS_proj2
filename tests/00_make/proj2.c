#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdbool.h> 

#define MAX_ACTIVITY 3                                                              // maximum number of activities
#define ARGS_COUNT 6                                                                // number of arguments
#define ERR_ARGS 42                                                                 // error code for invalid arguments

#define is_num(x) (x >= '0' && x <= '9')                                            // check if char is number
#define RANDOM_INTERVAL(x) (rand() % x )                                            // generate random number in interval <0,n)
#define init(name,value)(sem_open(name, O_CREAT | O_WRONLY, 0666, value))           // initialize semaphore
#define RANDOM_ACTIVITY (RANDOM_INTERVAL(MAX_ACTIVITY) + 1)                         // generate random activity number

#define CLERK_CREATE(id) ((clerk){id, 0})                                           // create clerk struct
#define CUSTOMER_CREATE(id) ((customer){id, 0})                                     // create customer struct
#define CLERK_PROCESS_CREATE(id, clerk) ((clerk_process){id, clerk})                // create clerk process struct
#define CUSTOMER_PROCESS_CREATE(id, customer) ((customer_process){id, customer})    // create customer process struct

#define CLERK_CREATED 20
#define CUSTOMER_CREATED 21
#define CUSTOMER_QUEUED 22
#define CUSTOMER_CALLED 23
#define CLERK_BUSY 24
#define CLERK_FREE 25
#define CUSTOMER_DONE 26
#define CLERK_BREAK 27
#define CLERK_UNBREAK 28
#define CLERK_DONE 29
#define POST_CLOSED 30

typedef struct {
    int idZ;                    // unique customer ID
    int activity;               // activity number
} customer;

typedef struct {
    int idU;                    // unique clerk ID
    int serving_activity;       // activity number of customer being served
} clerk;

typedef struct {    
    pid_t id;                   // process ID
    customer customer;          // customer struct
} customer_process;

typedef struct {  
    pid_t id;                   // process ID
    clerk clerk;                // clerk struct
} clerk_process;

typedef struct
{
    unsigned int NZ;            // number of customers
    unsigned int NU;            // number of clerks
    unsigned int TZ;            // maximum time of customer service
    unsigned int TU;            // maximum time of clerk service
    unsigned int F;             // maximum number of customers in queue
} args;

typedef struct {
    int number_line;            // number of line
    key_t shmid;                // shared memory id
    bool mail_open;             // whether mail is open or not
    int queue[MAX_ACTIVITY];    // queue of customers
} ipc_t;

typedef struct {
    sem_t *semid_queue1;        // semaphore for queue of letters
    sem_t *semid_queue2;        // semaphore for queue of packages
    sem_t *semid_queue3;        // semaphore for queue of money services
    sem_t *semid_output;        // semaphore for printing output
    sem_t *semid_mutex;         // semaphore for mutual exclusion of shared memory
    sem_t *semid_closing;       // semaphore for closing post office
} sem;

void err(int err_id){
    if(err_id == 0){    
        return; 
    }

    switch(err_id){
        case ERR_ARGS: 
            fprintf(stderr,"ERROR: Invalud input arguments\n");
            break;
        default:
            fprintf(stderr,"ERROR: Unknown error id\n");
            break;
    }

    exit(EXIT_FAILURE);
}

int read_args(args *arguments, int argc, char **argv)
{

    if (argc != ARGS_COUNT)                                 // check number of arguments
    {
        return ERR_ARGS; 
    }

    for (int i = 1; i < argc; i++) 
    {
        if (argv[i][0] == '\0')                             // check if argument is empty
        {
            return ERR_ARGS;
        }

        for (int j = 0; argv[i][j]; j++)                    // check if argument is number
            if (!is_num(argv[i][j]))
                return ERR_ARGS;

        unsigned int arg_value = (unsigned)atoi(argv[i]);   // convert argument to unsigned int

        switch (i)                                          // save argument to struct
        {
        case 1:
            arguments->NZ = arg_value;
            break;
        case 2:
            if (arg_value < 1)
            {
                return ERR_ARGS;
            }
            arguments->NU = arg_value;
            break;
        case 3:
            if (arg_value > 10000)
            {
                return ERR_ARGS;
            }
            arguments->TZ = arg_value;
            break;
        case 4:
            if (arg_value > 100)
            {
                return ERR_ARGS;
            }
            arguments->TU = arg_value;
            break;
        case 5:
            if (arg_value > 10000)
            {
                return ERR_ARGS;
            }
            arguments->F = arg_value;
            break;        
        }        
    }

    return EXIT_SUCCESS;
}

ipc_t* ipc_init(){                                                  // initialize shared memory
    key_t key = ftok("proj2.c",'c');                                // generate key
    int ipc_key = shmget(key, sizeof(ipc_t), 0666 | IPC_CREAT);     // create shared memory
    ipc_t* ipc = shmat(ipc_key, NULL, 0);                           // attach shared memory
    ipc->shmid = ipc_key;                                           // save shared memory id
    return ipc;                                                     // return shared memory
}

void ipc_destroy(ipc_t*ipc){        // destroy shared memory
    key_t key = ipc->shmid;         // get shared memory id
    shmctl(key, IPC_RMID, NULL);    // remove shared memory
    shmdt(ipc);                     // detach shared memory
}

void sem_create(sem*sem){   // create semaphores
    if ((sem->semid_queue1 = init("sem_queue1",0)) == SEM_FAILED){
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    if ((sem->semid_queue2 = init("sem_queue2",0)) == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
    
    if ((sem->semid_queue3 = init("sem_queue3",0)) == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    if ((sem->semid_output = init("sem_output",1)) == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }  

    if ((sem->semid_mutex = init("sem_mutex",1)) == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
    if ((sem->semid_closing = init("sem_closing",1)) == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    sem_unlink("sem_queue1");
    sem_unlink("sem_queue2");
    sem_unlink("sem_queue3");
    sem_unlink("sem_output");
    sem_unlink("sem_mutex");
    sem_unlink("sem_closing");
}

void sem_clear(sem*sem){ 
    sem_close(sem->semid_queue1);
    sem_close(sem->semid_queue2);
    sem_close(sem->semid_queue3);
    sem_close(sem->semid_output);
    sem_close(sem->semid_mutex);
    sem_close(sem->semid_closing);
}

clerk_process fork_clerk(unsigned int n){                               // fork clerks
    for (id_t id = 1; id <= n; id++) 
        if (fork() == 0) 
            return CLERK_PROCESS_CREATE(getpid(), CLERK_CREATE(id));    // create clerk process
    return (clerk_process){0};                                          // if didn't complete return empty process
}

customer_process fork_customer(unsigned int n){                             // fork customers
    if (n == 0)
    {
        return (customer_process){0};                                       // if no customers return empty process
    }
    
    for (id_t id = 1; id <= n; id++) 
        if (fork() == 0)
            return CUSTOMER_PROCESS_CREATE(getpid(), CUSTOMER_CREATE(id));  // create customer process
    return (customer_process){0};                                           // if didn't complete return empty process
}

void message_clerk(const int act_id, clerk_process *process, ipc_t *ipc){   // print clerk message
    FILE *out = fopen("./proj2.out", "a");                                  // open output file
    ++ipc->number_line;                                                     // increment number of line
    
    switch (act_id)
    {
        case CLERK_CREATED:
            fprintf(out, "%d: U %d: started\n", ipc->number_line, process->clerk.idU);
        break;
        case CLERK_BUSY:
            fprintf(out, "%d: U %d: serving a service of type %d\n", ipc->number_line, process->clerk.idU, process->clerk.serving_activity);
        break;
        case CLERK_FREE:
            fprintf(out, "%d: U %d: service finished\n", ipc->number_line, process->clerk.idU);
        break;
        case CLERK_BREAK:
            fprintf(out, "%d: U %d: taking break\n", ipc->number_line, process->clerk.idU);
        break;
        case CLERK_UNBREAK:
            fprintf(out, "%d: U %d: break finished\n", ipc->number_line, process->clerk.idU);
        break;
        case CLERK_DONE:
            fprintf(out, "%d: U %d: going home\n", ipc->number_line, process->clerk.idU);
        break;
        default:
        break;
    }

    fclose(out);    // close output file
}

void message_customer(const int act_id, customer_process *process, ipc_t *ipc){     // print customer message
    FILE *out = fopen("./proj2.out", "a");                                          // open output file
    ++ipc->number_line;                                                             // increment number of line
    
    switch (act_id)
    {
        case CUSTOMER_CREATED:
            fprintf(out, "%d: Z %d: started\n", ipc->number_line, process->customer.idZ);
        break;    
        case CUSTOMER_QUEUED:
            fprintf(out, "%d: Z %d: entering office for a service %d\n", ipc->number_line, process->customer.idZ, process->customer.activity);
        break; 
        case CUSTOMER_CALLED:
            fprintf(out, "%d: Z %d: called by office worker\n", ipc->number_line, process->customer.idZ);
        break;
        case CUSTOMER_DONE:
            fprintf(out, "%d: Z %d: going home\n", ipc->number_line, process->customer.idZ);
        break;
        default:
        break;
    }

    fclose(out); // close output file
}

int main(int argc, char *argv[]) {
    remove("./proj2.out");                  // remove previous output file
    
    args arguments;                         // arguments
    sem sem;                                // semaphores
    ipc_t *ipc = NULL;                      // shared memory
    clerk_process clerk_process;            // clerk process
    customer_process customer_process;      // customer process

    err(read_args(&arguments, argc, argv)); // read arguments
    sem_create(&sem);                       // create semaphores
    ipc = ipc_init();                       // create shared memory
    
    pid_t main_process_id = getpid();       // main process id
    ipc->mail_open = true;                  // set mail open to true

    if (main_process_id == getpid())
    {       
        clerk_process = fork_clerk(arguments.NU);           // fork clerks     
    }
    
    if (main_process_id == getpid())
    {
        customer_process = fork_customer(arguments.NZ);     // fork customers
    }
    
    if (main_process_id == getpid()) 
    {
        srand(getpid());    // set random seed

        usleep(arguments.F > 0 ? ((rand() % (arguments.F - arguments.F/2 + 1)) + arguments.F/2) * 1000 : 0);  // wait for random time between F/2 and F

        sem_wait(sem.semid_closing);
        sem_wait(sem.semid_output);                         // lock output semaphore 
        FILE *out = fopen("./proj2.out", "a");              // open output file
        ++ipc->number_line;                                 // increment number of line
        fprintf(out, "%d: closing\n", ipc->number_line);    // print closing message
        fclose(out);                                        // close output file
        sem_post(sem.semid_output);                         // unlock output semaphore
        ipc->mail_open = false;                             // close mail 
        sem_post(sem.semid_closing);
    }

    if(main_process_id != getpid()){
        
        srand(getpid());    // set random seed
        
        sem_wait(sem.semid_output);
        if (clerk_process.id != 0)
        {
            message_clerk(CLERK_CREATED, &clerk_process, ipc);              // print clerk message about creation
        }
        
        else if (customer_process.id != 0)
        {
            message_customer(CUSTOMER_CREATED, &customer_process, ipc);     // print customer message about creation
        }
        sem_post(sem.semid_output);
        
        if (clerk_process.id != 0)                  // if process is clerk
        {
            while (1) 
            {               
                int queues = MAX_ACTIVITY;          // number of not empty queues                

                sem_wait(sem.semid_mutex);          // lock mutex semaphore
                for(int i=0; i<MAX_ACTIVITY; i++)   // count not empty queues
                {
                    if(ipc->queue[i]==0)            // if queue is empty
                    {
                        queues--;                   // decrement number of not empty queues
                    }
                }
                sem_post(sem.semid_mutex);          // unlock mutex semaphore

                if(queues==0)                       // if mail is closed and all queues are empty
                {                                      

                    if(ipc->mail_open==false)       // if mail is closed
                    {                        
                        sem_wait(sem.semid_output);
                        message_clerk(CLERK_DONE, &clerk_process, ipc);         // print clerk message about finishing
                        sem_post(sem.semid_output);
                        exit(EXIT_SUCCESS);                                     // exit process
                    }                   
                    
                    else{
                        sem_wait(sem.semid_output); 
                        message_clerk(CLERK_BREAK, &clerk_process, ipc);        // print clerk message about break
                        sem_post(sem.semid_output);

                        usleep(arguments.TU > 0 ? RANDOM_INTERVAL(arguments.TU) * 1000 : 0);          // wait for random time
                        
                        sem_wait(sem.semid_output);
                        message_clerk(CLERK_UNBREAK, &clerk_process, ipc);      // print clerk message about unbreak
                        sem_post(sem.semid_output);
                    }
                }

                else if (queues != 0)   // if there is some not empty queues
                {                   
                    clerk_process.clerk.serving_activity = RANDOM_ACTIVITY;                 // set serving activity
                    
                    sem_wait(sem.semid_mutex); 
                    while (ipc->queue[clerk_process.clerk.serving_activity - 1] == 0)       // while serving queue is empty
                    {
                        int tmp = clerk_process.clerk.serving_activity;                     // temporary variable
                        clerk_process.clerk.serving_activity = tmp++ % MAX_ACTIVITY + 1;    // increment serving activity
                    }
                    sem_post(sem.semid_mutex);

                    sem_wait(sem.semid_mutex);
                    if(clerk_process.clerk.serving_activity == 1)       // if serving activity is 1 
                    {
                        sem_post(sem.semid_queue1);                     // increment semaphore of queue 1
                    }                   
                    else if(clerk_process.clerk.serving_activity == 2)  // if serving activity is 2
                    {
                        sem_post(sem.semid_queue2);                     // increment semaphore of queue 2
                    }                    
                    else if(clerk_process.clerk.serving_activity == 3)  // if serving activity is 3
                    {
                        sem_post(sem.semid_queue3);                     // increment semaphore of queue 3 
                    }
                    sem_post(sem.semid_mutex); 

                    sem_wait(sem.semid_output);
                    message_clerk(CLERK_BUSY, &clerk_process, ipc);     // print clerk message about serving
                    sem_post(sem.semid_output);
                    
                    usleep(RANDOM_INTERVAL(10) * 1000);                 // wait for random time
                    
                    sem_wait(sem.semid_output);
                    message_clerk(CLERK_FREE, &clerk_process, ipc);     // print clerk message about finishing serving
                    sem_post(sem.semid_output);
                }
            }
        }
        
        else if (customer_process.id != 0)                          // if process is customer
        {
            usleep(arguments.TZ > 0 ? RANDOM_INTERVAL(arguments.TZ) * 1000 : 0);           // wait for random time 
            
            customer_process.customer.activity = RANDOM_ACTIVITY;   // set activity
            
            sem_wait(sem.semid_closing);
            if(ipc->mail_open==true)                                // if mail is open
            {                   
                sem_wait(sem.semid_output);
                message_customer(CUSTOMER_QUEUED, &customer_process, ipc);  // print customer message about queuing
                sem_post(sem.semid_output);

                if(customer_process.customer.activity == 1)         // if activity is 1
                {
                    sem_wait(sem.semid_mutex);
                    ipc->queue[0]++;                                // increment queue 1
                    sem_post(sem.semid_mutex);
                    sem_post(sem.semid_closing);    
                    sem_wait(sem.semid_queue1);                     // wait for semaphore of queue 1
                }               
                else if(customer_process.customer.activity == 2)    // if activity is 2
                {
                    sem_wait(sem.semid_mutex);
                    ipc->queue[1]++;                                // increment queue 2
                    sem_post(sem.semid_mutex);
                    sem_post(sem.semid_closing);    
                    sem_wait(sem.semid_queue2);                     // wait for semaphore of queue 2
                }                
                else if(customer_process.customer.activity == 3)    // if activity is 3
                {            
                    sem_wait(sem.semid_mutex);
                    ipc->queue[2]++;                                // increment queue 3
                    sem_post(sem.semid_mutex);
                    sem_post(sem.semid_closing);
                    sem_wait(sem.semid_queue3);                     // wait for semaphore of queue 3
                }
                
                sem_wait(sem.semid_mutex);
                ipc->queue[customer_process.customer.activity - 1]--;       // decrement queue
                sem_post(sem.semid_mutex);

                sem_wait(sem.semid_output);
                message_customer(CUSTOMER_CALLED, &customer_process, ipc);  // print customer message about being called
                sem_post(sem.semid_output);
                
                usleep(RANDOM_INTERVAL(10) * 1000);                         // wait for random time  
                
                sem_wait(sem.semid_output);
                message_customer(CUSTOMER_DONE, &customer_process, ipc);    // print customer message about finishing
                sem_post(sem.semid_output);
                exit(EXIT_SUCCESS);
            }              
            else{
                sem_post(sem.semid_closing);
                sem_wait(sem.semid_output);
                message_customer(CUSTOMER_DONE, &customer_process, ipc);    // print customer message about finishing
                sem_post(sem.semid_output);
                exit(EXIT_SUCCESS);
            }
        }  
    }  
    
    while (wait(NULL) > 0);     // wait for all processes to finish 
    
    sem_clear(&sem);            // clear semaphores  
    ipc_destroy(ipc);           // destroy shared memory
    
    return EXIT_SUCCESS;        // finish program
}
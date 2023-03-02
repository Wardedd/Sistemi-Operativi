#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include "utility.h"

#define USER_FILENAME "./utente"
#define NODE_FILENAME "./nodo"

#define CONF_NUM_ATTRIBUTES 4

#define MAX_PROC_BALANCE_PRINT 32
#define MAX_TMP_STR_SIZE 50
#define MAX_PRINT_PROC_BALANCE_INT_ARGS 10
#define MAX_PRINT_PROC_BALANCE_STR_SIZE 100

/* starting functions (called in main) */
int start_processes(); /*also sets users_i, nodes_i */
int start_routine(int SO_BUDGET_INIT);
int send_setup_message();

/* process life cycle functions */
void signal_handler(int signum);
void clean_exit(int status); /* closes ipc objects, manages ending of child processes (if any), quits with exit() */

/* other */
void calc_processes_balance();
int print_processes_balance(int print_max_num);

void print_termination_info(termination_status term_status,int budget_init,
    	int *users,int users_i, int *nodes, int nodes_i, 
    	libro_mastro* shm_lm_data,int *nodes_tp_remaining,int curr_alive_users);

int setup_args(exec_args *args_user,exec_args *args_node,int mutex_lm_id);

int SO_USERS_NUM=-1,
    SO_NODES_NUM=-1,
    SO_BUDGET_INIT=-1,
    SO_SIM_SEC=-1;
char *attribute_keys[]={
    "SO_USERS_NUM",
    "SO_NODES_NUM",
    "SO_BUDGET_INIT",
    "SO_SIM_SEC"
};

int msq_trans=-1,
    msq_feedback=-1,
    msq_setup=-1;
int shm_lm=-1; /* id shared memory libro mastro */
libro_mastro *shm_lm_data=NULL;
int mutex_lm_id=-1;

pid_t *users=NULL, *nodes=NULL;
int *users_balance=NULL,*nodes_balance=NULL;
int users_i=0,nodes_i=0;

termination_status term_status=ERROR;
int running_sec=0;
int curr_alive_users=0;

int main(int argc, char** argv){
    int i;
    struct sigaction sa;    
    exec_args args_user,args_node;
    int *attribute_pointers[CONF_NUM_ATTRIBUTES];
    
    srand(getpid());
#ifdef DEBUG
    fprintf(stderr,"Master's pid: %d\n",getpid());
    fprintf(stderr,"trans size: %d\nlong size: %d\nMSG_SETUP_LEN: %d\nMSG_MAIN_LEN: %d\n",sizeof(transaction),sizeof(long),MSG_SETUP_LEN,MSG_MAIN_LEN);
#endif   
    /* LETTURA config */
    attribute_pointers[0]=&SO_USERS_NUM;
    attribute_pointers[1]=&SO_NODES_NUM;
    attribute_pointers[2]=&SO_BUDGET_INIT;
    attribute_pointers[3]=&SO_SIM_SEC;

    if(read_config_file(CONF_FILENAME,CONF_NUM_ATTRIBUTES,attribute_keys,attribute_pointers)==-1)
        clean_exit(EXIT_FAILURE);

    /* Setto sighandler per SIGALRM, SIGINT */
    bzero(&sa,sizeof(sa));
    sa.sa_handler=signal_handler;

    sa.sa_flags=0;
    sigaction(SIGALRM,&sa,NULL);
    sigaction(SIGINT,&sa,NULL);

    /* crea ipc */
    msq_trans=msgget(IPC_PRIVATE,0600);
    msq_feedback=msgget(IPC_PRIVATE,0600);
    msq_setup=msgget(IPC_PRIVATE,0600);
    shm_lm=shmget(IPC_PRIVATE,sizeof(libro_mastro),0600);
    mutex_lm_id=semget(IPC_PRIVATE,1,0600);
    
    if(msq_trans==-1 || msq_feedback==-1 || msq_setup==-1 || shm_lm==-1 || mutex_lm_id==-1){
        TEST_ERRNO;
        clean_exit(EXIT_FAILURE);
    }

    /* attacco la memoria condivisa allo spazio degli indirizzi del processo 
        (rendo la mem. condivisa accessibile in questo processo) e la inizializzo */
    if((shm_lm_data=shmat(shm_lm,NULL,0))==SHMAT_ERR){
        TEST_ERRNO;
        clean_exit(EXIT_FAILURE);
    }
    shm_lm_data->index=0;  

    /* setto mutex a 1 */
    semctl(mutex_lm_id, 0, SETVAL, 1);

    /* alloco memoria */
    users=malloc(sizeof(pid_t)*SO_USERS_NUM);
    users_balance=malloc(sizeof(int)*SO_USERS_NUM);
    nodes=malloc(sizeof(pid_t)*SO_NODES_NUM);
    nodes_balance=malloc(sizeof(int)*SO_NODES_NUM);

    if(users==NULL || users_balance==NULL || nodes==NULL || nodes_balance==NULL)
        clean_exit(EXIT_FAILURE);

    if(start_processes()==-1) 
        clean_exit(EXIT_FAILURE);
    
    printf("Preparazione simulazione...\n"); 
    /* =======SENDING START MESSAGES======= */
    if(send_setup_message()==-1) /*con 1000 nodi e 1000 utenti può durare anche una decina di  secondi */
        clean_exit(EXIT_FAILURE);

    printf("Inizio simulazione\n");
    printf("-----------------------------------\n");
    fflush(stdout);

    alarm(1);
    if(start_routine(SO_BUDGET_INIT)==-1)
        clean_exit(EXIT_FAILURE);
    clean_exit(EXIT_SUCCESS);
}

/* ========== starting functions ========== */
int start_processes(){ 
    int tmp;
    exec_args args_user,args_node;

    exec_args_start(&args_user,8);
    exec_args_start(&args_node,8);
    setup_args(&args_user,&args_node,mutex_lm_id);
    
    /* apro SO_USERS_NUM programmi user */
    users_i=start_programs(SO_USERS_NUM,USER_FILENAME,args_user,users);
    curr_alive_users=users_i;

    /* apro SO_NODES_NUM programmi node */
    nodes_i=start_programs(SO_NODES_NUM,NODE_FILENAME,args_node,nodes);
    
    exec_args_free(&args_user);
    exec_args_free(&args_node);
    
    if(nodes_i!=SO_NODES_NUM || users_i!=SO_USERS_NUM)
            return -1;
    return 0;  
}

int start_routine(int SO_BUDGET_INIT){
    int i;
    int wait_val,status;

    while(curr_alive_users>0 && running_sec<SO_SIM_SEC && shm_lm_data->index<SO_REGISTRY_SIZE){
        wait_val=wait(&status); /* only users expected here */

        if(wait_val>0){
            for(i=0;i<users_i;i++){
                if(users[i]==wait_val){
                    curr_alive_users--;
                    break;
                }
            }
#ifdef DEBUG
            for(i=0;i<nodes_i;i++)
                if(nodes[i]==wait_val){
                    fprintf(stderr,"Nodo %d terminato inaspettatamente\n",wait_val);
                    break;
                }
#endif
        }else{
            if(errno!=EINTR){
                TEST_ERRNO;
                return -1;
            }
        }
    }
    alarm(0); /* cancel pending alarm */
    
    if(term_status!=SIG_INT){
        if(running_sec==SO_SIM_SEC)
            term_status=TIME;
        else if(curr_alive_users==0)
            term_status=USERS_TERMINATED;
        else if(shm_lm_data->index==SO_REGISTRY_SIZE)
            term_status=LM_FULL;
        else
            term_status=ERROR;
    }
    
    return 0;
}

int send_setup_message(){
    /* send msgs to user */
    if(send_setup_message_set(msq_setup,users,users_i,1,users,users_i)==-1
        || send_setup_message_set(msq_setup,users,users_i,2,nodes,nodes_i)==-1
        || send_setup_message_set(msq_setup,users,users_i,0,NULL,0)==-1)
        return -1;
    /* send msgs to nodes */
    if(send_setup_message_set(msq_setup,nodes,nodes_i,2,nodes,nodes_i)==-1
        || send_setup_message_set(msq_setup,nodes,nodes_i,0,NULL,0)==-1)
        return -1;

    return 0;
}

/* ========== process life cycle functions ========== */
void signal_handler(int signum){
    int i;
    int old_errno=errno;

    switch(signum){
        case SIGALRM:
            running_sec++;
            if(running_sec<SO_SIM_SEC){
                as_safe_printf("Alive user processes: %d\n",curr_alive_users);
                print_processes_balance(MAX_PROC_BALANCE_PRINT);
                as_safe_printf("--------------------\n");
                alarm(1);
            }
            break;

        case SIGINT:
            term_status=SIG_INT;

            if(users!=NULL)
                for(i=0;i<users_i;i++)
                    kill(users[i],SIGINT);
            break;
    }
    
    errno=old_errno;
}

void clean_exit(int status){ 
    int i;
    int *nodes_tp_remaining;
    int users_on_ending;
    msg_setup msg_s;
    
    users_on_ending=curr_alive_users;

    /* start closing users */
    for(i=0;i<users_i;i++){

        kill(users[i],SIGINT);
#ifdef DEBUG
        errno=0;
        waitpid(users[i],NULL,0);
        if(errno==ECHILD)
            fprintf(stderr,"Utente %d terminato prima della clean_exit\n",users[i]);
        else
            fprintf(stderr,"Utente %d terminato in clean_exit\n",users[i]);
#else
        waitpid(users[i],NULL,0);
#endif
        curr_alive_users--;
    }

    /* start closing nodes */
    nodes_tp_remaining=malloc(nodes_i*sizeof(int));

    if(nodes_tp_remaining!=NULL)
        for(i=0;i<nodes_i;i++)
            nodes_tp_remaining[i]=-1;

    for(i=0;i<nodes_i;i++)
        kill(nodes[i],SIGINT);

    semctl(mutex_lm_id,0,IPC_RMID);
    msgctl(msq_trans,IPC_RMID,NULL);
    msgctl(msq_feedback,IPC_RMID,NULL);

    for(i=0;i<nodes_i;i++){
        if(waitpid(nodes[i],NULL,0)>0){
#ifdef DEBUG
            fprintf(stderr,"Nodo %d terminato in clean_exit\n",nodes[i]);
#endif
        }
        if(nodes_tp_remaining!=NULL)
            if(msgrcv(msq_setup,&msg_s,MSG_SETUP_LEN,nodes[i],IPC_NOWAIT)==MSG_SETUP_LEN)
                nodes_tp_remaining[i]=msg_s.val;
            
    }

    msgctl(msq_setup,IPC_RMID,NULL);

    print_termination_info(term_status,SO_BUDGET_INIT,
                users,users_i,nodes,nodes_i,
                shm_lm_data,nodes_tp_remaining,users_on_ending);

    shmdt(shm_lm_data);
    shmctl(shm_lm,IPC_RMID,NULL);
    
    free(nodes_tp_remaining);
    free(users);
    free(users_balance);
    free(nodes);
    free(nodes_balance);

    printf("Simulazione finita\n");
    exit(status);
}

/* ========== other ========== */
void calc_processes_balance(){
    int i,j,z;
    int max_block_num;
    pid_t sender,receiver;
    int qty,reward;
    
    for(i=0;i<users_i;i++){
        users_balance[i]=SO_BUDGET_INIT;
    }
    for(i=0;i<nodes_i;i++){
        nodes_balance[i]=0;
    }
    max_block_num=shm_lm_data->index;

    for(i=0;i<max_block_num;i++){
        for(j=0;j<SO_BLOCK_SIZE;j++){
            sender=shm_lm_data->transactions[i][j].sender;
            receiver=shm_lm_data->transactions[i][j].receiver;
            qty=shm_lm_data->transactions[i][j].qty;
            reward=shm_lm_data->transactions[i][j].reward;

            if(sender==-1){ 
                /* reward node transaction */
                for(z=0;z<nodes_i;z++){
                    if(nodes[z]==receiver){
                        nodes_balance[z]+=qty;
                        break;
                    }
                }
            }else{
                /* normal users transaction */
                int found_sender=0;
                int found_receiver=0;
                
                for(z=0;z<users_i && (!found_sender || !found_receiver);z++){
                    if(sender==users[z]){
                        users_balance[z]-=qty+reward;
                        found_sender=1;
                    }
                    if(receiver==users[z]){
                        users_balance[z]+=qty;
                        found_receiver=1;
                    }
                }
            }
        }
    }
}

int print_processes_balance(int print_max_num){
    int i,j,z;
    int max_block_num;
    int sender,receiver,qty;
    char tmpstr[MAX_PRINT_PROC_BALANCE_STR_SIZE];
    int args[MAX_PRINT_PROC_BALANCE_INT_ARGS];
    int str_size,max_str_size=MAX_PRINT_PROC_BALANCE_STR_SIZE;
    
    calc_processes_balance();

    if(print_max_num!=-1 && users_i+nodes_i>print_max_num){
        /* print processes with min balance and max balance */
        
        int min_users_i=0,max_users_i=0;
        int min_nodes_i=0,max_nodes_i=0;

        /* search through users  */
        for(i=1;i<users_i;i++){
            if(users_balance[i]<users_balance[min_users_i])
                min_users_i=i;
            if(users_balance[i]>users_balance[max_users_i])
                max_users_i=i;
        }
        /* search through nodes */
        for(i=1;i<nodes_i;i++){
            if(nodes_balance[i]<nodes_balance[min_nodes_i])
                min_nodes_i=i;
            if(nodes_balance[i]>nodes_balance[max_nodes_i])
                max_nodes_i=i;
        }
        
        as_safe_printf("Note: there may be more processes of the same type with the same balance not noted\n");
        /* min balance */

        if(users_balance[min_users_i]<nodes_balance[min_nodes_i])
            as_safe_printf("Process with lowest balance is the user with pid:%d and balance:%d\n",
                users[min_users_i],users_balance[min_users_i]);
        else if(users_balance[min_users_i]==nodes_balance[min_nodes_i])
            as_safe_printf("Both user pid: %d and node pid:%d have the lowest balance of:%d\n",
                users[min_users_i],nodes[min_nodes_i],users_balance[min_users_i]);
        else
            as_safe_printf("Process with lowest balance is the node with pid:%d and balance:%d\n",
                nodes[min_nodes_i],nodes_balance[min_nodes_i]);
        
        /* max balance */
        if(users_balance[max_users_i]>nodes_balance[max_nodes_i])
            as_safe_printf("Process with highest balance is the user with pid:%d and balance:%d\n",
                users[max_users_i],users_balance[max_users_i]);
        else if(users_balance[max_users_i]==nodes_balance[max_nodes_i])
            as_safe_printf("Both user pid: %d and node pid:%d have the highest balance of:%d\n",
                users[max_users_i],nodes[max_nodes_i],users_balance[max_users_i]);
        else
            as_safe_printf("Process with highest balance is the node with pid:%d and balance:%d\n",
                nodes[max_nodes_i],nodes_balance[max_nodes_i]);
        
    }else{
        for(i=0;i<users_i;i++)
            as_safe_printf("User pid:%d balance %d\n",users[i],users_balance[i]);
        for(i=0;i<nodes_i;i++)
            as_safe_printf("Node pid:%d balance %d\n",nodes[i],nodes_balance[i]);
    }
    return 0;
}

void print_termination_info(termination_status term_status,int budget_init,
    int *users,int users_i, int *nodes, int nodes_i, 
    libro_mastro* shm_lm_data,int *nodes_tp_remaining,int curr_alive_users
    ){
    
    int i;
    printf("Terminazione causata da: ");
    
    switch(term_status){
        case TIME:
            printf("fine tempo");
            break;
        case LM_FULL:
            printf("libro mastro pieno");
            break;
        case USERS_TERMINATED:
            printf("terminazione utenti");
            break;
        case SIG_INT:
            printf("SIGINT ricevuto");
            break;
        case ERROR:
            printf("Errore");
            break;
    }
    printf("\n");

    fflush(stdout);  
    print_processes_balance(-1);
    printf("Numero di processi terminati prematuramente: %d\n",users_i-curr_alive_users);
    
    if(shm_lm_data!=NULL)
        printf("Numero di blocchi nel libro mastro %d\n",shm_lm_data->index);
    else
        printf("Numero di blocchi nel libro mastro non disponibile (errore)\n");

    if(nodes_tp_remaining!=NULL){
        printf("Numero di transazioni rimaste nella transaction pool dei nodi a fine esecuzione:\n");
        for(i=0;i<nodes_i;i++){
            printf("Nodo %d transazioni rimaste nella tp %d\n",nodes[i],nodes_tp_remaining[i]);
        }
    }else
        printf("Numero di transazioni rimaste nelle transaction pool dei nodi non disponibile (errore)\n");
    
}

int setup_args(exec_args *args_user,exec_args *args_node,int mutex_lm_id){
    /* convention: argv[0]=filename */    
    exec_args_add(args_user,USER_FILENAME,0);
    exec_args_add(args_node,NODE_FILENAME,0);
    /* send msq_setup id to child programs via argv */
    exec_args_add_int(args_user,msq_setup,1);
    exec_args_add_int(args_node,msq_setup,1);
    /* send msq_trans id to child programs via argv */
    exec_args_add_int(args_user,msq_trans,2);
    exec_args_add_int(args_node,msq_trans,2);
    /* send msq_feedback id to child programs via argv */
    exec_args_add_int(args_user,msq_feedback,3);
    exec_args_add_int(args_node,msq_feedback,3);
    /* send sharedmemory_lm id to child programs via argv */
    exec_args_add_int(args_user,shm_lm,4);
    exec_args_add_int(args_node,shm_lm,4);         
    /* send mutex_lm_id id to child programs via argv */
    exec_args_add_int(args_user,mutex_lm_id,5);
    exec_args_add_int(args_node,mutex_lm_id,5); 
    return 0;
}
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include "utility.h"

#define SIG_MASK sigprocmask(SIG_BLOCK,&sigint_mask,NULL);
#define SIG_UNMASK sigprocmask(SIG_UNBLOCK,&sigint_mask,NULL);

#define CONF_NUM_ATTRIBUTES 4

/* starting functions */
int receive_setup(int msq_setup);
int start_routine(int SO_TP_SIZE,int SO_MIN_TRANS_PROC_NSEC,int SO_MAX_TRANS_PROC_NSEC,int msq_trans,int msq_feedback,int msq_setup, int mutex_lm_id);

/* routine functions */
int handle_trans_requests(int SO_TP_SIZE,int msq_trans,int msq_feedback);
void write_trans_block(transaction *trans_block);
int notify_trans_success(int msq_feedback,transaction* trans_block);

/* process life cycle functions */
void signal_handler(int signum);
void clean_exit(int status);

int msq_setup;
pid_t *nodes;
transaction *tp;
int nodes_i,tp_i;
int shm_lm,run;
libro_mastro *shm_lm_data;
sigset_t sigint_mask;

#ifdef DEBUG
int num_bad_feedback=0;
int num_pos_feedback=0;
#endif

int main(int argc,char *argv[]){
    int i,tmp;
    int SO_NODES_NUM,
        SO_TP_SIZE,
        SO_MIN_TRANS_PROC_NSEC,
        SO_MAX_TRANS_PROC_NSEC;
        
    FILE *fp;
    msg_main msg_m;
    msg_setup msg_s;

    int msq_trans,msq_feedback;
    int mutex_lm_id;
    struct sigaction sa;  

    int *attribute_pointers[CONF_NUM_ATTRIBUTES];
    char *attribute_keys[]={
        "SO_NODES_NUM",
        "SO_TP_SIZE",
        "SO_MIN_TRANS_PROC_NSEC",
        "SO_MAX_TRANS_PROC_NSEC"
    };

    /* inizializzazione */

    attribute_pointers[0]=&SO_NODES_NUM;    
    attribute_pointers[1]=&SO_TP_SIZE;
    attribute_pointers[2]=&SO_MIN_TRANS_PROC_NSEC;
    attribute_pointers[3]=&SO_MAX_TRANS_PROC_NSEC;
    
    nodes=NULL;
    nodes_i=0;
    tp=NULL;
    tp_i=0;
    msq_trans=-1;
    msq_feedback=-1;
    msq_setup=-1;
    shm_lm=-1;
    shm_lm_data=NULL;
    mutex_lm_id=-1;
    run=1;

    sigemptyset(&sigint_mask);
    sigaddset(&sigint_mask,SIGINT);

    bzero(&sa,sizeof(sa));
    sa.sa_handler=signal_handler;
    sa.sa_flags=0;
    sigaction(SIGINT,&sa,NULL);

    srand(getpid());
    
    if(argc<6){
        fprintf(stderr,"%d) missing arguments, need atleast 6\n",getpid());
        clean_exit(EXIT_FAILURE);
    }

    /* LETTURA config */
    if(read_config_file(CONF_FILENAME,CONF_NUM_ATTRIBUTES,attribute_keys,attribute_pointers)==-1)
        clean_exit(EXIT_FAILURE);
    
    /* malloc */
    nodes=malloc(sizeof(pid_t)*SO_NODES_NUM);
    tp=malloc(sizeof(transaction)*SO_TP_SIZE);
    if(nodes==NULL || tp==NULL){
        TEST_ERRNO;
        clean_exit(EXIT_FAILURE);
    }

#ifdef DEBUG
    fprintf(stderr,"Node nodes pnt: %p,tp pnt: %p\n",(void*)nodes,(void*)tp);
#endif

    msq_setup=atoi(argv[1]);   
    msq_trans=atoi(argv[2]);
    msq_feedback=atoi(argv[3]);
    shm_lm=atoi(argv[4]);
    mutex_lm_id=atoi(argv[5]);
    
    /* attach shm_lm mem to current process */
    if((shm_lm_data=shmat(shm_lm,NULL,0))==SHMAT_ERR){
        TEST_ERRNO;
        clean_exit(EXIT_FAILURE);
    }

    /* receive starting user list,node list via msq_setup */
    if(receive_setup(msq_setup)==-1)
        clean_exit(EXIT_FAILURE);

    if(nodes_i!=SO_NODES_NUM-1)
        clean_exit(EXIT_FAILURE);
    
    
    if(start_routine(SO_TP_SIZE,
        SO_MIN_TRANS_PROC_NSEC,
        SO_MAX_TRANS_PROC_NSEC,
        msq_trans,
        msq_feedback,
        msq_setup,
        mutex_lm_id)==-1)
        clean_exit(EXIT_FAILURE);
    clean_exit(EXIT_SUCCESS);
}

/* ========== starting functions ========== */
int receive_setup(int msq_setup){
    msg_setup msg_s;
    int tmp=1;

    while(tmp){

        if(msgrcv(msq_setup,&msg_s,MSG_SETUP_LEN,getpid(),0)!=MSG_SETUP_LEN){
            TEST_ERRNO;
            return -1;
        }

        switch(msg_s.type){
            case 0: /* ricevuta sentinella fine */
                tmp=0;
                break;
            case 2: /* ricevuto nodo */
                if(msg_s.val!=getpid())
                     nodes[nodes_i++]=msg_s.val;
                break; 
        }
    }
    return 0;
}

int start_routine(
    int SO_TP_SIZE,
    int SO_MIN_TRANS_PROC_NSEC,
    int SO_MAX_TRANS_PROC_NSEC,
    int msq_trans,
    int msq_feedback,
    int msq_setup,
    int mutex_lm_id){
    int i;
    transaction trans_block[SO_BLOCK_SIZE];
    transaction reward_trans;
    reward_trans.sender=REWARD_SENDER;
    reward_trans.receiver=getpid();
    reward_trans.reward=0;

    while(run){
        if(handle_trans_requests(SO_TP_SIZE,msq_trans,msq_feedback)==-1) /* makes tp_i >= SO_BLOCK_SIZE-1 */
            return -1;     
#ifdef DEBUG
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
        fprintf(stderr,"Nodo %d) ottenute abbastanza trans. tp_i:%d\n",getpid(),tp_i);
#endif
#endif
        SIG_MASK;
        if(tp_i>=SO_BLOCK_SIZE-1){
            /* crea blocco */
            reward_trans.qty=0;
            clock_gettime(CLOCK_REALTIME,&(reward_trans.time));

            for(i=0;i<SO_BLOCK_SIZE-1;i++){ /* TP FIFO */
                trans_block[i]=tp[i];
                reward_trans.qty+=tp[i].reward;
            }
            trans_block[SO_BLOCK_SIZE-1]=reward_trans;

            /* finta elaborazione di transazioni */
            if(nanosleep_nsec_between(SO_MIN_TRANS_PROC_NSEC,SO_MAX_TRANS_PROC_NSEC)==-1)
                return -1;
            
            if(lock_mutex(mutex_lm_id)==-1) {
                TEST_ERRNO;
                return -1;
            }
            /* !!INIZIO SEZIONE CRITICA!! */
            if(shm_lm_data->index<SO_REGISTRY_SIZE)
                write_trans_block(trans_block);

            /* !!FINE SEZIONE CRITICA!! */
            if(unlock_mutex(mutex_lm_id)==-1){
                TEST_ERRNO;
                return -1;
            }
            /* notifica utenti del successo della loro transazione*/
            if(notify_trans_success(msq_feedback,trans_block)==-1)
                return -1;

            /* shift tp a sinistra di SO_BLOCK_SIZE-1 posizioni */
            for(i=SO_BLOCK_SIZE-1;i<tp_i;i++) 
                tp[i-(SO_BLOCK_SIZE-1)]=tp[i];
            tp_i-=SO_BLOCK_SIZE-1;
        }
        SIG_UNMASK;
    }
    return 0;
}

/* ========== routine functions ========== */
int handle_trans_requests(int SO_TP_SIZE,int msq_trans,int msq_feedback){
    int msgrcv_flags;
    msg_main m;

    /* se nodo ancora non può elaborare un blocco, aspetta in msgrcv 
        fin quando può effettuare un'elaborazione di un blocco */
    if(tp_i<SO_BLOCK_SIZE-1) 
        msgrcv_flags=0;
    else /* altrimenti, consuma le transazioni ricevute senza aspettare */
        msgrcv_flags=IPC_NOWAIT; 
    
    while(msgrcv(msq_trans,&m,MSG_MAIN_LEN,getpid(),msgrcv_flags)==MSG_MAIN_LEN){ 
#ifdef DEBUG
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
        fprintf(stderr,"Nodo %d) @tp_i:%d ricevuta trans. ",getpid(),tp_i);
        fprint_transaction(stderr,m.t);
#endif
#endif
        if(tp_i<SO_TP_SIZE){
            /* se c'è spazio in tp invia metti la transazione in tp */
            SIG_MASK;
            tp[tp_i++]=m.t;
            SIG_UNMASK;
            if(tp_i==SO_BLOCK_SIZE-1) /* se possiamo elaborare un blocco, smetti di aspettare altre transazioni */
                msgrcv_flags=IPC_NOWAIT;        
        }
        else{
            /* altrimenti notifica il mittente del fallimento della transazione */
            m.mtype=m.t.sender;
            m.val=-1; /* convenzione: transazione fallita */

            if(msgsnd(msq_feedback,&m,MSG_MAIN_LEN,0)==-1){ 
                TEST_ERRNO;
                return -1;
            }  
#ifdef DEBUG
            num_bad_feedback++;
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
            fprintf(stderr,"Nodo %d) @tp_i:%d mandato feedback negativo ",getpid(),tp_i);
            fprint_transaction(stderr,m.t);
#endif
#endif
        }
    }
    if(errno!=0 && errno!=ENOMSG){  
        TEST_ERRNO;
        return -1;
    }

    return 0;
}

void write_trans_block(transaction *trans_block){
    int i;
#ifdef DEBUG
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
    fprintf(stderr,"Nodo %d) scrittura blocco index:%d\n",getpid(),shm_lm_data->index);
#endif
#endif
    for(i=0;i<SO_BLOCK_SIZE;i++){
        shm_lm_data->transactions[shm_lm_data->index][i]=trans_block[i];
    }
    shm_lm_data->index++;
}

int notify_trans_success(int msq_feedback,transaction* trans_block){
    int i;
    msg_main m;

    for(i=0;i<SO_BLOCK_SIZE-1;i++){
        m.mtype=trans_block[i].sender;
        m.t=trans_block[i];
        m.val=0; /* convenzione: transazione riuscita */

        if(msgsnd(msq_feedback,&m,MSG_MAIN_LEN,0)==-1){
            TEST_ERRNO;
            return -1;
        }
#ifdef DEBUG
        num_pos_feedback++;
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
        fprintf(stderr,"Nodo %d) @tp_i:%d mandato feedback positivo ",getpid(),tp_i);
        fprint_transaction(stderr,m.t);
#endif
#endif
    }
    return 0;
}

/* ========== process life cycle functions ========== */
void signal_handler(int signum){
    switch(signum){
        case SIGINT:
            run=0;
            break;
    }
}

void clean_exit(int status){
    msg_setup msg_s;
    msg_s.mtype=getpid();
    msg_s.val=tp_i;
    SIG_MASK;
    errno=0;
    /* mando al master le transazioni rimanenti nella transaction pool */
    if(msq_setup!=-1)
        if(msgsnd(msq_setup,&msg_s,MSG_SETUP_LEN,0)==-1)
            TEST_ERRNO;
        
    shmdt(shm_lm_data);
    #ifdef DEBUG
    fprintf(stderr,"Node %d) Bye! Status:%d Num_bad_exit: %d Num_pos_exit:%d Num blocks written: %d\n",getpid(),status,num_bad_feedback,num_pos_feedback,num_pos_feedback/(SO_BLOCK_SIZE-1));
    #endif
    free(nodes);
    free(tp);

    exit(status);
}
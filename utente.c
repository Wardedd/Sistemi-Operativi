#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include "utility.h"

#define SIG_MASK sigprocmask(SIG_BLOCK,&sigusr1_mask,NULL);
#define SIG_UNMASK sigprocmask(SIG_UNBLOCK,&sigusr1_mask,NULL);

#define CONF_NUM_ATTRIBUTES 7

typedef struct{
    int num;
    transaction t;
}pending_transaction;

/* starting functions */
int receive_setup(int msq_setup);
int start_routine(int SO_REWARD,int SO_RETRY,int SO_BUDGET_INIT, int SO_MIN_TRANS_GEN_NSEC, int SO_MAX_TRANS_GEN_NSEC, int msq_trans,int msq_feedback);

/* routine functions*/
int check_pending_results(int SO_RETRY, int msq_feedback,
        int curr_trans_num,
        int *last_success_trans_num,
        pending_transaction *pending_trans,
        int *pending_trans_i,
        int max_pending_trans,
        int *balance);
void update_balance(int SO_BUDGET_INIT,int *balance,int *last_block_num);

/* process life cycle functions */
void signal_handler(int signum);
void clean_exit(int status);

/* other */
void add_pending_transaction(pending_transaction* pt_arr,int pt_arr_i,transaction t,int t_num);

pid_t* users,*nodes;
int users_i,nodes_i;
int shm_lm;
libro_mastro *shm_lm_data;
#ifdef DEBUG
int num_msq_trans_full=0;
#endif
int trans_request=0;
sigset_t sigusr1_mask;
int balance=-1; /* definita qui per debug*/

int main(int argc,char *argv[]){
    int i,tmp;
    int SO_USERS_NUM,
        SO_NODES_NUM,
        SO_BUDGET_INIT,
        SO_REWARD,
        SO_MIN_TRANS_GEN_NSEC,
        SO_MAX_TRANS_GEN_NSEC,
        SO_RETRY;
    struct sembuf sem_minusone;
    FILE *fp;
    transaction tmptrans;
    msg_main msg_m;
    msg_setup msg_s;
    int trans_num;
    int msq_trans,msq_feedback,msq_setup;
    int start_mutex_id;
    int *attribute_pointers[CONF_NUM_ATTRIBUTES];
    char *attribute_keys[]={
        "SO_USERS_NUM","SO_NODES_NUM","SO_BUDGET_INIT",
        "SO_REWARD","SO_MIN_TRANS_GEN_NSEC",
        "SO_MAX_TRANS_GEN_NSEC","SO_RETRY"
    };
    struct sigaction sa;
    /* inizializzazione */
    
    attribute_pointers[0]=&SO_USERS_NUM;
    attribute_pointers[1]=&SO_NODES_NUM;
    attribute_pointers[2]=&SO_BUDGET_INIT;
    attribute_pointers[3]=&SO_REWARD;
    attribute_pointers[4]=&SO_MIN_TRANS_GEN_NSEC;
    attribute_pointers[5]=&SO_MAX_TRANS_GEN_NSEC;
    attribute_pointers[6]=&SO_RETRY;
    
    sem_minusone.sem_flg=0; 
    sem_minusone.sem_op=-1;
    users=NULL;
    nodes=NULL;
    users_i=0;
    nodes_i=0;
    msq_trans=-1;
    msq_feedback=-1;
    msq_setup=-1;
    shm_lm=-1;
    shm_lm_data=NULL;

    sigemptyset(&sigusr1_mask);
    sigaddset(&sigusr1_mask,SIGUSR1);

    bzero(&sa,sizeof(sa));
    sa.sa_handler=signal_handler;
    sa.sa_flags=0;
    sigaction(SIGUSR1,&sa,NULL);

    srand(getpid());
    
    if(argc<6){
        fprintf(stderr,"%d) missing arguments, need atleast 6\n",getpid());
        clean_exit(EXIT_FAILURE);
    }

    /* LETTURA config */
    if(read_config_file(CONF_FILENAME,CONF_NUM_ATTRIBUTES,attribute_keys,attribute_pointers)==-1)
        clean_exit(EXIT_FAILURE);

    /* malloc */
    users=malloc(sizeof(pid_t)*(SO_USERS_NUM-1)); /* -1 perchè l'utente non conta se stesso nella lista */
    nodes=malloc(sizeof(pid_t)*SO_NODES_NUM);
    balance=SO_BUDGET_INIT;

    msq_setup=atoi(argv[1]);   
    msq_trans=atoi(argv[2]);
    msq_feedback=atoi(argv[3]);
    shm_lm=atoi(argv[4]);

    /* attach memoria condivisa shm_lm al processo corrente */
    if((shm_lm_data=shmat(shm_lm,NULL,SHM_RDONLY))==SHMAT_ERR){
        TEST_ERRNO;
        clean_exit(EXIT_FAILURE);
    }

    /* ricevi la lista degli utenti, nodi iniziale via msq_setup */
    if(receive_setup(msq_setup)==-1)
        clean_exit(EXIT_FAILURE);
    
    if(start_routine(SO_REWARD,SO_RETRY,SO_BUDGET_INIT,SO_MIN_TRANS_GEN_NSEC,SO_MAX_TRANS_GEN_NSEC,msq_trans,msq_feedback)==-1)
        clean_exit(EXIT_FAILURE);
    clean_exit(EXIT_SUCCESS);
}

/* ========== starting functions  ========== */
int receive_setup(int msq_setup){
    int tmp=1;
    msg_setup msg_m;

    while(tmp){
        if(msgrcv(msq_setup,&msg_m,MSG_SETUP_LEN,getpid(),0)==-1){
            TEST_ERRNO;
            return -1;
        }

        switch(msg_m.type){
            case 0: /* ricevuto sentinella fine */
                tmp=0;
                break;
            case 1: /* ricevuto utente */
                if(msg_m.val!=getpid())
                    users[users_i++]=msg_m.val;
                break;
            case 2: /* ricevuto nodo */
                nodes[nodes_i++]=msg_m.val;
                break; 
        }
    }
    return 0;
}

int start_routine(int SO_REWARD,int SO_RETRY,int SO_BUDGET_INIT, int SO_MIN_TRANS_GEN_NSEC, int SO_MAX_TRANS_GEN_NSEC, int msq_trans,int msq_feedback){

    int i,tmp;
    int curr_trans_num=0,
        last_success_trans_num=-1,
        pending_trans_i=0,
        max_pending_trans=SO_RETRY,
        last_block_num=0;
    struct timespec ts;
    pending_transaction *pending_trans;

    pending_trans=malloc(sizeof(pending_transaction)*SO_RETRY);

    while(curr_trans_num-last_success_trans_num<=SO_RETRY 
            || pending_trans_i>0){
        
        SIG_MASK;
        if(pending_trans_i>0){
            /* check_pending_results mette il processo in stato di waiting
                se l'utente deve aspettare un esito positivo di una transazione
                prima di mandarne altre */
            tmp=check_pending_results(SO_RETRY,
                msq_feedback,
                curr_trans_num,
                &last_success_trans_num,
                pending_trans,
                &pending_trans_i,
                max_pending_trans,
                &balance);
            if(tmp==-1)
                return -1;
        }

        if(curr_trans_num-last_success_trans_num<=SO_RETRY){
            update_balance(SO_BUDGET_INIT,&balance,&last_block_num);
            if(balance>=2){
                /* scegli nodo a cui inviare la transazione */
                pid_t node=nodes[rand()%nodes_i];
                /* genera transazione*/
                transaction t=generate_transaction(getpid(),balance,SO_REWARD,users,users_i);
                
                if(pending_trans_i>=SO_RETRY)
                    fprintf(stderr,"Utente %d) Errore: pending_trans_i: %d >= SO_RETRY : %d\n",getpid(),pending_trans_i,SO_RETRY);
                else{
                    /* manda transazione */
                    if(send_transaction(t,node,msq_trans,IPC_NOWAIT,0)==0){
#ifdef DEBUG
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
                            fprintf(stderr,"Utente %d) spedita trans. con nodo: %d new balance:",getpid(),node,balance-t.qty-t.reward);
                            fprint_transaction(stderr,t);
#endif
#endif
                        add_pending_transaction(pending_trans,pending_trans_i,t,curr_trans_num);
                        pending_trans_i++;
                        balance-=t.qty+t.reward;
                    }
#ifdef DEBUG
                    else{
                        if(errno==EAGAIN)
                            num_msq_trans_full++;
                    }
#endif
                }
            }
            /* aumenta contatore tentativo invio transazione */
            curr_trans_num++;
            /* dormi per nsec. tra SO_MIN_TRANS_GEN_NSEC e SO_MAX_TRANS_GEN_NSEC NSEC */
            tmp=SO_MIN_TRANS_GEN_NSEC+rand()%(SO_MAX_TRANS_GEN_NSEC-SO_MIN_TRANS_GEN_NSEC+1);
            ts.tv_sec=tmp/1000000000;
            ts.tv_nsec=tmp%1000000000;
            SIG_UNMASK;
            nanosleep(&ts,NULL);
        }   

        if(trans_request==1){
            /* gestisci richiesta di invio transazione tramite segnale SIGUSR1*/
            SIG_MASK;
            if(curr_trans_num-last_success_trans_num<=SO_RETRY){    
                pid_t node;
                node=nodes[rand()%nodes_i];
                if(balance>=2){
                    transaction t=generate_transaction(getpid(),balance,SO_REWARD,users,users_i);
                    if(send_transaction(t,node,msq_trans,IPC_NOWAIT,0)==0){
                        fprintf(stderr,"Utente %d) La transazione seguente è stata correttamente generata e inviata: ",getpid());
                        fprint_transaction(stderr,t);
                        balance-=t.qty+t.reward;
                    }else
                        fprintf(stderr,"Utente %d) Non è stato possibile inviare la transazione generata all'utente.\n",getpid());
                }else
                    as_safe_printf("Utente %d) la transazione non è stata generata, motivo: bilancio < 2\n",getpid());
            }else{
                fprintf(stderr,"Utente %d) Tentativi di generazione transazioni temporanemente terminati\n",getpid());
            } 
            trans_request=0;
        }
    }
    free(pending_trans);
    return 0;
}

/* ========== routine functions ========== */
int check_pending_results(int SO_RETRY, int msq_feedback,
        int curr_trans_num,
        int *last_success_trans_num,
        pending_transaction *pending_trans,
        int *pending_trans_i,
        int max_pending_trans,
        int *balance){

    int i,y,tmp_flags,
        init_pending_trans_i=*pending_trans_i,
        msgrcv_attempts=0;
    msg_main msg_buf;
    
    /* utente si mette ad aspettare l'esito delle transazioni ricevute 
        se non ne può inviarne altre, altrimenti consuma solo le gli 
        esiti presenti attualmente in msq_feedback */
    if(curr_trans_num-(*last_success_trans_num)>SO_RETRY) 
        tmp_flags=0;
    else
        tmp_flags=IPC_NOWAIT;    

    while(msgrcv_attempts<init_pending_trans_i){
        if(msgrcv(msq_feedback,&msg_buf,MSG_MAIN_LEN,getpid(),tmp_flags)==MSG_MAIN_LEN){
            int pt_found_i=-1;
#ifdef DEBUG
            fprintf(stderr,"Utente %d) balance: %d, pending_trans_i: %d, curr_trans_num: %d, last_success_trans_num %d, num_msq_trans_full:%d\n",
                getpid(),*balance,*pending_trans_i,curr_trans_num,*last_success_trans_num,num_msq_trans_full);
#endif

#ifdef DEBUG
#ifdef DEBUG_MSQ_TRANS_MOVEMENT
            
            if(is_trans_successfull(msg_buf))
                fprintf(stderr,"Utente %d) ricevuto positivo\n",getpid());
            else
                fprintf(stderr,"Utente %d) ricevuto negativo\n",getpid());
#endif
#endif
            /* controllo se esito si riferisce a transazione presente in array */
            for(i=0;i<*pending_trans_i;i++){
                if(trans_equals(pending_trans[i].t,msg_buf.t)){
                    pt_found_i=i;
                    break;
                }
            }

            if(is_trans_successfull(msg_buf)){
                if(pt_found_i!=-1){
                    /* aggiorno la var. che contiene il num dell'ultima transazione
                        correttamente inviata */
                    *last_success_trans_num=pending_trans[pt_found_i].num;
                    /* tolgo le transazioni + vecchie di last_success_trans_num */
                    for(i=pt_found_i+1;i<*pending_trans_i;i++){
                        pending_trans[i-(pt_found_i+1)]=pending_trans[i];
                    }
                    *pending_trans_i=(*pending_trans_i)-(pt_found_i+1);
                    tmp_flags=IPC_NOWAIT;
                }
                /* se pt_found_i==-1,
                    l'esito si riferisce a  una trans. + vecchia di last_success_trans_num,
                    quindi non c'è bisogno di aggiornare last_success_trans_num o pending_trans */
            }else{
                /* tolgo transazione da array se presente, 
                    ma ridò il budget della transazione fallita 
                    all'utente a prescindere */
                if(pt_found_i!=-1){
                    for(i=pt_found_i+1;i<*pending_trans_i;i++){
                        pending_trans[i-1]=pending_trans[i];
                    }
                    (*pending_trans_i)--;
                }
                (*balance)+=msg_buf.t.reward+msg_buf.t.qty;
            }
        }else{
            if(errno!=ENOMSG){
                TEST_ERRNO;
                return -1;
            }
        }
        msgrcv_attempts++;
    }
#ifdef DEBUG
    fprintf(stderr,"Utente %d) exit check feedback - balance: %d, pending_trans_i: %d, curr_trans_num: %d, last_success_trans_num %d, num_msq_trans_full:%d\n",
        getpid(),*balance,*pending_trans_i,curr_trans_num,*last_success_trans_num,num_msq_trans_full);
#endif
    return 0;
}

void update_balance(int SO_BUDGET_INIT,int *balance,int *last_block_num){
    int i,j;
    int max_block_num;
    transaction t;
    
    max_block_num=shm_lm_data->index;
    
    for(i=*last_block_num;i<max_block_num;i++){
        for(j=0;j<SO_BLOCK_SIZE;j++){
            t=shm_lm_data->transactions[i][j];
            if(t.receiver==getpid())
                balance+=t.qty;
            if(t.sender==getpid())
                balance-=t.qty+t.reward;
            
        }
    }
    *last_block_num=max_block_num;
}

/* ========== process life cycle functions ========== */
void signal_handler(int signum){
    switch (signum){
        case SIGUSR1:
            as_safe_printf("Utente %d) Ricevuto segnale SIGUSR1, tentativo generazione transazione\n",getpid());
            trans_request=1;
            break;
    }
}

void clean_exit(int status){
    shmdt(shm_lm_data);

    free(users);
    free(nodes);
#ifdef DEBUG
    fprintf(stderr,"Utente %d) Bye! Status:%d, num_msq_trans_full: %d,balance: %d\n",getpid(),status,num_msq_trans_full,balance);
#endif
    exit(status);
}

/* ========== other ========== */
void add_pending_transaction(pending_transaction* pt_arr,int pt_arr_i,transaction t,int t_num){
    pt_arr[pt_arr_i].t=t;
    pt_arr[pt_arr_i].num=t_num;  
}
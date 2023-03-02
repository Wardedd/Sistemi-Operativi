#include "utility.h"

#define UTIL_MAX_LINE_SIZE 50
#define TMP_STRING_SIZE 50
#define MAX_PRINT_PROC_BALANCE_STR_SIZE 100
#define MAX_PRINT_PROC_BALANCE_INT_ARGS 10

/* ========== process utility ========== */
int start_programs(int num,char* filename,exec_args args,pid_t* arr){
    int i,forkval;
    
    for(i=0;i<num;i++){
        switch(forkval=fork()){
            case -1: /* errore  */
                TEST_ERRNO;
                return -1;
                break;
            case 0: /* codice processo figlio: 
                        apro il programma degli utenti con execve,
                        se execve fallisce ritorno EXIT_FAILURE */
                exec_args_add(&args,NULL,args.argc-1); /* argc.argc-1 da cambiare */
                execve(filename,args.argv,NULL);  
                TEST_ERRNO;
                exit(EXIT_FAILURE);
            default: /* codice processo padre */
#ifdef SHOW_STARTING_PIDS
                fprintf(stderr,"Iniziato programma %s pid: %d\n",filename,forkval);
#endif
                if(arr!=NULL)
                    arr[i]=forkval;
                break;
        }     
    }
    return i;
}

/* ========== ipc utility ========== */
int lock_mutex(int mutex_id){
    struct sembuf sop;
    sop.sem_num=0;
    sop.sem_flg=0;
    sop.sem_op=-1;
    return semop(mutex_id,&sop,1);
}

int unlock_mutex(int mutex_id){
    struct sembuf sembuf;
    sembuf.sem_num=0;
    sembuf.sem_flg=0;
    sembuf.sem_op=1;
    return semop(mutex_id,&sembuf,1);
}

int send_transaction(transaction trans,pid_t node,int msq,int msgsnd_flags,int POKESIG){
    msg_main msg_m;
    msg_m.t=trans;
    msg_m.mtype=node;

    if(kill(node,0)==-1) /* check if pid doesn't exists */
        return -1; 
    if(msgsnd(msq,&msg_m,MSG_MAIN_LEN,msgsnd_flags)==-1)
        return -1;

    if(POKESIG>0)
        if(kill(node,POKESIG)==-1)
            return -1;
    return 0;
}

int send_setup_message_set(int msq_setup,int *receiver_list,int receiver_num,int type,int* val_list,int val_num){
    /* if type==0 then the message is empty (nothing inside the msg_setup.val attribute) */
    int i,j;
    msg_setup msg_s;
    msg_s.type=type;

    if(receiver_list==NULL || 
        (type!=0 && val_list==NULL))
        return -1;

    for(i=0;i<receiver_num;i++){
        msg_s.mtype=receiver_list[i];
        
        if(type==0){
            if(msgsnd(msq_setup,&msg_s,MSG_SETUP_LEN,0)==-1){ 
                    TEST_ERRNO; 
                    return -1;
            }
        }
        else{
            for(j=0;j<val_num;j++){
                msg_s.val=val_list[j];
                
                if(msgsnd(msq_setup,&msg_s,MSG_SETUP_LEN,0)==-1){ 
                    TEST_ERRNO; 
                    return -1;
                }
            }

        }
    }
    return 0;
}

/* ========== config utility ========== */
void fix_string_atoi(char* str_num){ /* just puts a 0 on \n, string has to be correct anyways*/
    int i;
    
    if(str_num==NULL)
        return;
    
    for(i=0;i<strlen(str_num);i++){
        if(str_num[i]=='\n'){
            str_num[i]='\0';
            return;
        }
    }
}

int read_config_from_file(FILE* file,int num_attributes,char** attribute_names,int **attribute_pointers){ /*returns num. of found attributes*/
    int num,i;
    int num_curr_attributes=0;
    char *name,*str_num;
    char str[UTIL_MAX_LINE_SIZE];
    
    if(file==NULL)
        return -1;

    while(fgets(str,UTIL_MAX_LINE_SIZE,file)!=NULL){
        name=strtok(str,";");
        str_num=strtok(NULL,";");
        if(name==NULL||str_num==NULL)
            return -1;
        
        fix_string_atoi(str_num);
        num=atoi(str_num);
    
        for(i=0;i<num_attributes;i++){
            if(strcmp(attribute_names[i],str)==0){
                *(attribute_pointers[i])=num;
                num_curr_attributes++;
                break;
            }
        }
    }
    return num_curr_attributes;
}

int read_config_file(char *filename,int num_attributes,char **attribute_keys,int **attribute_pointers){
    int tmp;
    FILE *fp;
    
    fp = fopen(filename,"r");

    if(fp==NULL){
        TEST_ERRNO;
        return -1;
    }

    tmp=read_config_from_file(fp,num_attributes,attribute_keys,attribute_pointers);
    fclose(fp);
    
    if(tmp!=num_attributes){
        fprintf(stderr,"error on reading config, read %d attributes\n",tmp);
        return -1;
    }
    return tmp;
}

/* ========== exec_args utility ========== */
void exec_args_start(exec_args *args,int argc){
    int i;

    args->argc=argc;
    args->argv=malloc(sizeof(char*)*argc);   
    for(i=0;i<argc;i++)
        args->argv[i]=NULL;
}

int exec_args_add(exec_args *args,char *str, int i){
    if(i>=args->argc)
        return -1;

    if(args->argv[i]!=NULL)
        free(args->argv[i]);

    if(str==NULL)
        args->argv[i]=NULL;
    else
        args->argv[i]=strdup(str);
    
    return 0;
}

void exec_args_free(exec_args *args){
    int i;
    for(i=0;i<args->argc;i++){
        free(args->argv[i]);
    }
    free(args->argv);
}

int exec_args_add_int(exec_args *args,int val, int i){
    char tmpstr[TMP_STRING_SIZE];
    sprintf(tmpstr,"%d",val);     
    exec_args_add(args,tmpstr,i);
}

/* ========== transaction, pending transaction utility ========== */
void print_transaction(transaction t){
    fprint_transaction(stdout,t);
}
void fprint_transaction(FILE* fp,transaction t){
    fprintf(fp,"sender: %d receiver: %d qty:%d reward: %d time.tv_sec:%d time.tv_nsec: %ld\n",
        t.sender,t.receiver,t.qty,t.reward,t.time.tv_sec,t.time.tv_nsec);
}

int trans_equals(transaction t1, transaction t2){
    if(t1.receiver==t2.receiver 
        && t1.sender==t2.sender
        && t1.time.tv_nsec==t2.time.tv_nsec
        && t1.time.tv_sec==t2.time.tv_sec)
        return 1;
    return 0;
}

int is_trans_successfull(msg_main msg_m){
    if(msg_m.val==-1)
        return 0;
    return 1;
}

transaction generate_transaction(int sender,int balance,int SO_REWARD,pid_t *users,int users_i){
    int cash_used;
    transaction t;
    t.sender=sender;
    t.receiver=users[rand()%users_i];
    cash_used=2+rand()%(balance-1);
    
    t.reward=cash_used*SO_REWARD/100+0.5; /*0.5 per arrotondare. da x.5 (incluso) in poi viene arrotondato per eccesso*/
    if(t.reward<1) 
        t.reward=1;
    
    t.qty=cash_used-t.reward;
    clock_gettime(CLOCK_REALTIME,&t.time);
    return t;
}

/* ========== miscellaneus ========== */
int nanosleep_nsec_between(int MIN_NSEC,int MAX_NSEC){
    struct timespec ts;
    int tmp=MIN_NSEC+rand()%(MAX_NSEC-MIN_NSEC+1);
    ts.tv_sec=tmp/1000000000;
    ts.tv_nsec=tmp%1000000000;
    return nanosleep(&ts,NULL);
}

int get_int_digits(int num){
    if(num==0)
        return 1;
    else{
        int result=0;
        while(num!=0){
            num=num/10;
            result++;
        }
        return result;
    }

}

int as_safe_vsnprintf(char *str,int size,char* format,va_list ap){ /*can't print int <0*/
    /*
        arguments: 
                size refers to total size of the argument str (including '\0')
        returns: if successfull->number of chars(bytes) written (including '\0')
                 if unsuccessfull->-1
    */
    int str_i,format_i;
    int int_args_i=0;
    int write_size=size-1;

    str_i=0;

    for(format_i=0;format[format_i]!='\0' && str_i<write_size;format_i++){
        if(format[format_i]=='%'){            
            if(format[format_i+1]=='d'){
                int y;
                int curr_int;
                int int_digits;
                curr_int = va_arg(ap,int);
                int_digits=get_int_digits(curr_int);

                if(str_i+int_digits-1>write_size-1)
                    return -1;
                
                for(y=0;y<int_digits;y++){
                    str[str_i+int_digits-1-y]=curr_int%10+'0';
                    curr_int/=10;
                }
                str_i+=int_digits;
                format_i++;
                continue;
           }
        }else if(format[format_i]=='\0')
            break;
        
        str[str_i++]=format[format_i];
    }
    str[str_i]='\0';

    return str_i;
}

int as_safe_vdprintf(int fd,char *format,va_list ap){
    char tmpstr[MAX_PRINT_PROC_BALANCE_STR_SIZE];
    int str_size;

    str_size=as_safe_vsnprintf(tmpstr,MAX_PRINT_PROC_BALANCE_STR_SIZE
        ,format,ap);

    return write(fd,tmpstr,str_size);
}

int as_safe_dprintf(int fd,char *format,...){
    int written_bytes;

    va_list ap;
    va_start(ap,format);
    written_bytes=as_safe_vdprintf(fd,format,ap);
    va_end(ap);

    return written_bytes;
}

int as_safe_printf(char *format,...){
    int written_bytes;

    va_list ap;
    va_start(ap,format);
    written_bytes=as_safe_vdprintf(1,format,ap);
    va_end(ap);

    return written_bytes;
}
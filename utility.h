#ifndef UTILITY_H
#define UTILITY_H

#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/sem.h>

#define SO_REGISTRY_SIZE 1000
#define SO_BLOCK_SIZE 10
#define REWARD_SENDER -1
#define CONF_FILENAME "conf.txt"

#define SHMAT_ERR (void*)-1
#define MSG_SETUP_LEN sizeof(msg_setup)-sizeof(long)
#define MSG_MAIN_LEN sizeof(msg_main)-sizeof(long)

/* TEST_ERRNO = TEST_ERROR del prof. Enrico Bini*/
#ifdef DEBUG
	#define TEST_ERRNO    { fprintf(stderr, \
					  "%s:%d: PID=%5d: Error %d (%s)\n", \
					  __FILE__,			\
					  __LINE__,			\
					  getpid(),			\
					  errno,			\
					  strerror(errno)); }
#else
	#define TEST_ERRNO
#endif

typedef struct{
    pid_t sender,receiver;
    int qty,reward;
	struct timespec time;
}transaction;

typedef struct{
	int index;
	transaction transactions[SO_REGISTRY_SIZE][SO_BLOCK_SIZE];
}libro_mastro;

typedef struct{
	long mtype;
	transaction t;
	int val; 
	/* val: keeps transaction result info (failed: -1, successfull: 0) in msq_feedback*/
}msg_main;

typedef struct{
	long mtype;
	int type;
	int val;
}msg_setup;

typedef struct{
    char **argv;
    int argc;
}exec_args;

typedef enum{
    TIME,LM_FULL,USERS_TERMINATED,SIG_INT,ERROR
}termination_status;

/* process utility */
int start_programs(int num,char* filename,exec_args args,pid_t* arr);
	/* if arr!=NULL pid gets saved in the array "pid_t *arr" */

/* ipc utility */
int lock_mutex(int mutex_id);
int unlock_mutex(int mutex_id);
int send_transaction(transaction trans,pid_t node,int msq,int msgsnd_flags,int POKESIG);
int send_setup_message_set(int msq_setup,int *receiver_list,int receiver_num,int type,int* val_list,int val_num);

/* config utiity */
int read_config_file(char *filename,int num_attributes,char **attribute_keys,int **attribute_pointers);

/* exec_args utility */
void exec_args_start(exec_args *args,int argc);
int exec_args_add(exec_args *args,char *str, int i);
void exec_args_free(exec_args *args);
int exec_args_add_int(exec_args *args,int val, int i);

/* transaction, pending transaction utility */
void print_transaction(transaction t);
void fprint_transaction(FILE* fp,transaction t);
int trans_equals(transaction t1, transaction t2);
transaction generate_transaction(int sender,int balance,int SO_REWARD,pid_t *users,int users_i);
int is_trans_successfull(msg_main msg_m);

/* miscellaneus */
int nanosleep_nsec_between(int SO_MIN_TRANS_GEN_NSEC,int SO_MAX_TRANS_GEN_NSEC);
int as_safe_vsnprintf(char *str,int size,char* format,va_list ap); /* does not call va_end, can use ap */
int as_safe_dprintf(int fd,char *format,...);
int as_safe_printf(char *format,...);
#endif
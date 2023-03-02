CFLAGS= -std=c89 -pedantic
TARGET= utente nodo master

all: OPTIONS=
all: utility.o master utente nodo

debug: OPTIONS=-DDEBUG -DSHOW_STARTING_PIDS
debug: utility.o master utente nodo

fdebug: OPTIONS=-DDEBUG -DSHOW_STARTING_PIDS -DDEBUG_MSQ_TRANS_MOVEMENT
fdebug: utility.o master utente nodo

master: master.c utility.o
	gcc $(CFLAGS) $(OPTIONS) master.c utility.o -o master

utente: utente.c utility.o
	gcc $(CFLAGS) $(OPTIONS) utente.c utility.o -o utente

nodo: nodo.c utility.o
	gcc $(CFLAGS) $(OPTIONS) nodo.c utility.o -o nodo

utility.o: ./utility.c ./utility.h
	gcc $(CFLAGS) $(OPTIONS) -c utility.c

clean: #presa da esempio makefile durante lezione del prof. Enrico Bini
	rm -f *.o $(TARGET) *~

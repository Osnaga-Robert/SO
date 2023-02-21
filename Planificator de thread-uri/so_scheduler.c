#include "utils/so_scheduler.h"
#include "utils/structures.h"

#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

static void check_scheduler(void);
static void enqueue(thread *t);
static void dequeue(void);
static void *thread_start(void *args);

static Scheduler scheduler;

int so_init(unsigned int time_quantum, unsigned int io) //verificam datele de intrare si alocam dinamic
{
	if (scheduler.init != 0 || time_quantum < 1 || io > SO_MAX_NUM_EVENTS)
		return -1;
	scheduler.init = 1;
	scheduler.io_events = io;
	scheduler.threads_no = 0;
	scheduler.queue_size = 0;
	scheduler.quantum = time_quantum;
	scheduler.first_thread = 0;

	scheduler.threads = malloc(sizeof(thread *) * 1000);
	scheduler.queue = malloc(sizeof(thread *) * 1000);

	return 0;
}

void so_end(void)
{//verificam daca end-ul este facut inainte de init
    if (scheduler.init != 1)
        return;//lasam thread-urile ramase sa isi termine executia
    for (int i = 0; i < scheduler.threads_no; i++)
        pthread_join(scheduler.threads[i]->tid, NULL);
    //distrugem thread-urile create si le eliberam
    for (int i = 0 ; i < scheduler.threads_no ; i++) {
        pthread_mutex_destroy(&scheduler.threads[i]->th_sem);
        free(scheduler.threads[i]);
    }
    //eliberam vectorul de prioritati
    for(int i = 0 ; i < scheduler.queue_size ; i++)
        dequeue();

    free(scheduler.threads);
    free(scheduler.queue);

    scheduler.init = 0;
}

void so_exec(void)
{
	scheduler.current_thread->time --;
    //in cazul in care isi termina cuanta, ii vom aloca din nou timp si vom pune alt thread in functiune
    if (scheduler.current_thread->time == 0) {
        scheduler.current_thread->time = scheduler.quantum;
        enqueue(scheduler.current_thread);
        check_scheduler();
        pthread_mutex_lock(&scheduler.current_thread->th_sem);
    }
}

int so_wait(unsigned int io)
{
    if (io > scheduler.io_events)
        return -1;
    //in cazul in care thread-ul curent isi termina timpul, ii vom aloca timp si vom pune alt thread in functiune
    scheduler.current_thread->time--;
    if (scheduler.current_thread->time == 0) {
        scheduler.current_thread->time = scheduler.quantum;
        enqueue(scheduler.current_thread);
        check_scheduler();
        pthread_mutex_lock(&scheduler.current_thread->th_sem);
    }
    //in caz contrar il vom pune pe coada si vom lasa sa se execute alt thread
    else {
        enqueue(scheduler.current_thread);
        check_scheduler();
        pthread_mutex_lock(&scheduler.current_thread->th_sem);
    }
    return 0;
}
//acelasi lucru de la so_wait il vom face si in so_signal
int so_signal(unsigned int io)
{
    if (io > scheduler.io_events)
        return -1;

    scheduler.current_thread->time--;
    if (scheduler.current_thread->time == 0) {
        scheduler.current_thread->time = scheduler.quantum;
        enqueue(scheduler.current_thread);
        check_scheduler();
        pthread_mutex_lock(&scheduler.current_thread->th_sem);
    } else {
        enqueue(scheduler.current_thread);
        check_scheduler();
        pthread_mutex_lock(&scheduler.current_thread->th_sem);
    }
    return 0;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
    //vom verifica parametrii
	if (func == NULL || priority > SO_MAX_PRIO)
		return 0;
    //vom creea un nou thread
	thread *new_thread  =malloc(sizeof(thread));

	new_thread->prio = priority;
	new_thread->time = scheduler.quantum;
    new_thread->handler = func;

    scheduler.threads[scheduler.threads_no] = new_thread;
    scheduler.threads_no = scheduler.threads_no + 1;
    //initializam si cream in sistem thread-ul
    pthread_mutex_init(&new_thread->th_sem,NULL);
	pthread_create(&new_thread->tid, NULL, &thread_start,(void *)new_thread);
    //adaugam in coada
    enqueue(new_thread);
    //in cazul in care este primul thread(cel pe care nu trebuie sa il bagam in coada)
    //vom adauga in coada doar thread-ul pe care l-am creat
    if (scheduler.first_thread == 0) {
        check_scheduler();
        scheduler.first_thread = 1;
    } else{
    //in caz contrat ne vom lua dupa cerintele din round_robin care se modelezeaza pe situatia noastra
    //in cazul de fata fie thread-ul creat are prioritate mai mare, fie nu am are timp pe procesor
        if (scheduler.current_thread->prio < new_thread->prio) {
                check_scheduler();
                enqueue(scheduler.current_thread);
            } else if(scheduler.current_thread->time == 0) {
                scheduler.current_thread->time = scheduler.quantum;
                enqueue(scheduler.current_thread);
                check_scheduler();
            }
    }
	return new_thread->tid;
}
//conform cerintei, vom bloca thread-ul si vom apela handler-ul
static void *thread_start(void *args)
{
	thread *t = (thread *)args;

	pthread_mutex_lock(&t->th_sem);
	t->handler(t->prio);
	return NULL;
}

static void check_scheduler(void)
{
	//vom lua urmatorul thread si thread-ul curent
	thread *next = scheduler.queue[scheduler.queue_size - 1];
	thread *aux =scheduler.current_thread;

	scheduler.current_thread = next;
    scheduler.current_thread->time = scheduler.quantum;
	//vom scoate thread-ul din coada dupa care vom adauga thread-ul curent
    dequeue();
	pthread_mutex_unlock(&scheduler.current_thread->th_sem);
	//il vom debloca pentru a putea lucra cu el

}

void enqueue(thread *cur)
{//functie pentru a a adauga intr-o lista cu prioritati
    if (scheduler.queue_size == 0) {
		scheduler.queue[scheduler.queue_size] = cur;
		scheduler.queue_size = scheduler.queue_size + 1;
		return;
	}
    scheduler.queue[scheduler.queue_size] = cur;
	scheduler.queue_size = scheduler.queue_size + 1;
    for (int i = 0; i < scheduler.queue_size; i++) {
      for (int j = i + 1; j < scheduler.queue_size; j++) {
         if (scheduler.queue[i]->prio > scheduler.queue[j]->prio){
            thread *a = scheduler.queue[i];

            scheduler.queue[i] = scheduler.queue[j];
            scheduler.queue[j] = a;
         }
      }
   }
}

void dequeue(void)
{//functie pentru a scoate din lista de prioritati
    scheduler.queue[scheduler.queue_size - 1] = NULL;
	scheduler.queue_size --;
}

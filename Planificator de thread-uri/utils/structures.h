typedef struct {
	tid_t tid;
	int prio;
	int time;
    so_handler *handler;
    pthread_mutex_t th_sem;
} thread; //structura pentru datele thread-urilor

typedef struct {
	int init;
	unsigned int io_events;
	unsigned int quantum;
	int threads_no;
	int first_thread;
	int queue_size;

	thread *current_thread;
	thread **threads;
	thread **queue;

} Scheduler;//structura pentru scheduler
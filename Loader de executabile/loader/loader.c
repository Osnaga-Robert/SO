/*
 * Loader Implementation
 *
 * 2022, Operating Systems
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "exec_parser.h"

static so_exec_t *exec;
static int fd;

void segv_handler(int signum, siginfo_t *info, void *context)
{
	struct sigaction previous;
	//in cazul in care SIGSEGV detecteaza alta eroare inafara de nemaparea in memorie, vom rula handler-ul default
	if (info->si_code != SEGV_MAPERR) 
		signal(SIGSEGV, NULL);
	//luam fiecare segment in parte
	for (int i = 0 ; i < exec->segments_no ; i++) {
		//verificam daca adresa la care se produce seg_fault-ul se afla in segmentele noastre
		//in caz contrar rulam handler-ul default
		if (exec->segments[i].vaddr <= (int)info->si_addr && (int)info->si_addr <= exec->segments[i].vaddr + exec->segments[i].mem_size) {
			//aflam in ce pagina se produce seg_fault-ul
			int page_fault = ((int)info->si_addr - exec->segments[i].vaddr) / getpagesize();
			//aflam la ce distanta fata de o pagina se afla PAGE_FAULT-ul
			int distance = (int)info->si_addr % getpagesize();
			//aliniem dimensiunea unui segment la nivel de pagina
			off_t pa_offset = exec->segments[i].offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
			//alocam memorie virtuala in cadrul procesului
			char *mmap_ret = mmap((exec->segments[i].vaddr + page_fault * getpagesize()), getpagesize() - pa_offset + exec->segments[i].offset, PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fd, pa_offset);
			//citim din file descriptor elementele in mmap_ret(deoarece nu mi-a reusit din mmap, asta a fost o alternativa)
			read(fd, mmap_ret, getpagesize());
			//daca page-fault se afla in ultima pagina(adica in pagina in care trebuie sa se afle si elementele prorpiu-zise si zerouri)
			if (page_fault  == exec->segments[i].file_size / getpagesize()) {
				int size = exec->segments[i].file_size - page_fault * getpagesize() + distance;//calculam dimensiunea de unde ar trebui sa punem zerouri

				memset(mmap_ret + size, 0, getpagesize() - size);//punem zero acolo unde trebuie
			}	else if (page_fault  > exec->segments[i].file_size / getpagesize())//in cazul in care se afla intr-o pagina in care ar trebui sa fie doar zero
				memset(mmap_ret, 0, getpagesize());//punem zero pe toata pagina

			mprotect(mmap_ret, getpagesize(), exec->segments[i].perm);//punem permisiune

			return;
		}
	}
	signal(SIGSEGV, NULL);// in cazul in care page_fault-ul nu se afla in segmentele noastre, atunci apelam handler-ul default
}
int so_init_loader(void)
{
	int rc;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	rc = sigaction(SIGSEGV, &sa, NULL);
	if (rc < 0) {
		perror("sigaction");
		return -1;
	}
	return 0;
}

int so_execute(char *path, char *argv[])
{
	fd = open(path, O_RDONLY);

	exec = so_parse_exec(path);
	if (!exec)
		return -1;

	so_start_exec(exec, argv);

	close(fd);

	return -1;
}

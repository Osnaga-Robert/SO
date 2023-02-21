/*
 * epoll-based echo server. Uses epoll(7) to multiplex connections.
 *
 * TODO:
 *  - block data receiving when receive buffer is full (use circular buffers)
 *  - do not copy receive buffer into send buffer when send buffer data is
 *      still valid
 *
 * 2011-2017, Operating Systems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <libaio.h>
#include <sys/eventfd.h>

#include "./util/util.h"
#include "./util/debug.h"
#include "./util/lin/sock_util.h"
#include "./util/lin/w_epoll.h"
#include "./util/aws.h"
#include "./util/http-parser/http_parser.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

/* Parser used for requests */
static http_parser request_parser;

/* Storage for request_path */
static char request_path[BUFSIZ];

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	int fd;
	char pathname[BUFSIZ];
	char buffer[BUFSIZ];
	int bytes_read;
	int bytes_sent;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	enum connection_state state;

	int efd;
	io_context_t io;
	struct iocb *iocb;
	struct iocb **piocb;

};

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);

	conn->efd = eventfd(0, 0);
	conn->iocb = (struct iocb*) malloc(1* sizeof(*(conn->iocb)));
	conn->piocb = (struct iocb **)malloc(1 * sizeof(*(conn->piocb)));

	return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;
	

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	//setam socket-ul ca fiind non-blocant(initial fiind TCP)

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}
	//citim din socket pe parti
	bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->bytes_read, BUFSIZ, 0);

	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}
	//adaugam cati biti am citit si ii dam valoarea 0 ultimului bit
	conn->bytes_read += bytes_recv;
	conn->recv_buffer[conn->bytes_read] = 0;

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	printf("--\n%s--\n", conn->recv_buffer);

	conn->recv_len = strlen(conn->recv_buffer);
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}
	//in cazul fisierelor statice
	if (strstr(conn->pathname,"static") != NULL ){
		//vom proceda la fel ca la recv, dar de acesta data vom scrie pe parti
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->bytes_sent, conn->send_len - conn->bytes_sent, 0);
		fprintf(stderr,"%s\n",conn->send_buffer);
		conn->bytes_sent += bytes_sent;

		//in cazul in care mai este de citit, ii vom cere request de input

		if (conn->bytes_sent < conn->send_len){
			rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(rc < 0, "w_epoll_add_ptr_inout");
			return STATE_DATA_SENT;
		}

		//acum scriem in fisier

		int bytes_sendfile = sendfile(conn->sockfd,conn->fd,0,BUFSIZ);

		//in cazul in care mai este de transmis, ii vom cere request de input

		if (bytes_sendfile == BUFSIZ / 2){
			rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(rc < 0, "w_epoll_add_ptr_inout");
			return STATE_DATA_SENT;
		}
	}
	//in cazul fisierelor dinamice
	if (strstr(conn->pathname,"dynamic") != NULL){
		struct io_event *events;
		events = (struct io_event *) malloc(1 * sizeof(struct io_event));
		conn->piocb[0] = &conn->iocb[0];
		memset(conn->buffer,0,BUFSIZ);
		io_prep_pread(&conn->iocb[0],conn->fd,conn->buffer,BUFSIZ,0);
		io_set_eventfd(&conn->iocb[0],conn->efd);
		io_setup(1,&conn->io);
		io_submit(conn->io,1,conn->piocb);
		io_getevents(conn->io, 1, 1, events, NULL);
		send(conn->sockfd,conn->buffer,strlen(conn->buffer),0);
		io_destroy(conn->io);
		close(conn->efd);
	}
	//in cazul in care se greseste calea(nu este nici static nici dinamic)
	if (strstr(conn->pathname,"dynamic") == NULL && strstr(conn->pathname,"static") == NULL)
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->bytes_sent, conn->send_len - conn->bytes_sent, 0);
		
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	printf("--\n%s--\n", conn->send_buffer);

	/* all done - remove out notification */
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	//parsam cererile HTTP din partea clientilor

	ret_state = receive_message(conn); 

	http_parser_init(&request_parser, HTTP_REQUEST);

	http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	//formam pathname-ul in functie de cum ne este dat (cu .dat sau fara .dat)
	
	if(strstr(request_path,"dat") != NULL){
		strcpy(conn->pathname,AWS_DOCUMENT_ROOT);
		strcat(conn->pathname,request_path + 1);
	} else{
		strcpy(conn->pathname,AWS_DOCUMENT_ROOT);
		strcat(conn->pathname,request_path + 1);
	}
	
	conn->fd = open(conn->pathname, O_RDWR);

	//in cazul in care nu exista parser-ul ii vom da ca pagina nu exista, in caz contrar exista

	if (conn->fd == -1){
		strcpy(conn->send_buffer, "HTTP/1.0 404 Not Found\r\n\r\n");
		conn->send_len = strlen(conn->send_buffer);
	} else{
		strcpy(conn->send_buffer, "HTTP/1.0 200 OK\r\n\r\n");
		conn->send_len = strlen(conn->send_buffer);
	}

	//verificam daca am ajuns la sfarsit
	//in caz negativ vom continua sa cerem request la input
	//in caz pozitiv vom cere request la output

	if(strstr(conn->recv_buffer,"\r\n\r\n") != NULL){
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_inout");
	} else{
		/* Add socket to epoll for out events */
		rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_inout");	
	}
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_message(rev.data.ptr);
			}
		}
	}

	return 0;
}


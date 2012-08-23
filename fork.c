#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

/*---------------------------------------------
 * Global variables.
 *
 * global_pipe: Pipe for communication between 
 * child processes and the parent I/O wrangler.
 * global_envp: Holds a reference to the envp of
 * this process.
 * global_pipe_output_lock: UNUSED.
 * global_connection_key: The connection key used
 * to authenticate all client connections.
 *--------------------------------------------*/
int global_pipe[2] ;
char **global_envp;
pthread_mutex_t global_pipe_output_lock;
char *global_connection_key;

#include "common.h"
#include "process.h"
#include "encode.h"

typedef enum {
	KEY = 0,
	COMMAND = 1,
	TAG = 2,
	EXTRA = 3,
} message_tokens_t;

/*---------------------------------------------
 *--------------------------------------------*/
int read_message(int client, char **message) {
	int message_len = 0, message_idx = 0; 
	int expected_colons = 4, colon_count = 1;

	message_len = 1;
	message_idx = 0;
	*message = (char*)calloc(message_len, sizeof(char));

	do {
		int just_read = 0;
		if ((just_read = read(client, *message+message_idx, 1))<=0) {
			/*
			 * stop reading early!
			 */
			return -1; 	
		}

		*message=(char*)realloc(*message,sizeof(char)*(message_len+1));
		memset(*message + message_len, 0, sizeof(char));

		if (*(*message + message_idx) == ':') {
			if (colon_count == expected_colons)
				break;
			else 
				colon_count++;
		}

		message_len+=1;
		message_idx+=1;

		DEBUG_3("message: %s\n", *message);
	} while (1);
	return 0;
}

/*---------------------------------------------
 *--------------------------------------------*/
void *output_monitor(void *param) {
	struct process *p = (struct process *)param;
	FILE *output = NULL, *global_output;
	int will_break = 0; 
	fd_set read_set, except_set;

	FD_ZERO(&read_set);
	FD_ZERO(&except_set);
	FD_SET(p->output, &read_set);
	FD_SET(p->output, &except_set);
	FD_SET(global_pipe[0], &except_set);
	FD_SET(global_pipe[1], &except_set);

	if (!(output = fdopen(p->output, "r"))) {
		fprintf(stderr, "Cannot do fdopen() for output!\n");
		return NULL;
	}

	if (!(global_output = fdopen(global_pipe[1], "w"))) {
		fprintf(stderr, "Cannot do fdopen() for global_output\n"); 
		return NULL;
	}
	DEBUG_2("Starting output monitor for %s.\n", p->tag);

	while (!will_break) {
		char buffer[LINE_BUFFER_SIZE+1] = {0,};
		struct timeval timeout;
		int read_size = 0;

		timeout.tv_sec = 5;
		timeout.tv_usec = 0;

		select(FD_SETSIZE, &read_set, NULL, &except_set, &timeout);
		
		DEBUG_3("Done output monitor select()ing\n");

		if (FD_ISSET(p->output, &except_set) ||
		    FD_ISSET(global_pipe[0], &except_set) ||
		    FD_ISSET(global_pipe[1], &except_set)) {
			fprintf(stderr, "Exception occurred!\n");
			break;
		}

		if (FD_ISSET(p->output, &read_set)) {
			char *newline = "\n";
			if (fgets(buffer, LINE_BUFFER_SIZE, output) == NULL)
				break;

			DEBUG_3("buffer: %s-\n", buffer);
#if 0
			write(global_pipe[1], p->tag, strlen(p->tag));
			write(global_pipe[1], buffer, read_size);
			write(global_pipe[1], newline, strlen(newline));
#endif
			pthread_mutex_lock(&global_pipe_output_lock);
			fprintf(global_output, "OUTPUT:%s:%s", p->tag, buffer);
			fflush(global_output);
			pthread_mutex_unlock(&global_pipe_output_lock);
#ifdef DEBUG_MODE
			fprintf(stderr, "stderr: %s:%s\n", p->tag, buffer);
			fflush(stderr);
#endif
		}
		FD_SET(p->output, &read_set);
		FD_SET(p->output, &except_set);
		FD_SET(global_pipe[0], &except_set);
		FD_SET(global_pipe[1], &except_set);
	}
	remove_process(p);
	
	fprintf(global_output, "STOPPED:%s:\n", p->tag);
	fflush(global_output);
	
	close(p->input);
	close(p->output);
	
	DEBUG_2("Stopping output monitor for %s.\n", p->tag);

#ifdef DEBUG
	print_processes();
#endif

	free_process(p);
	return NULL;
}

/*---------------------------------------------
 *--------------------------------------------*/
void *threaded_wait_pid(void *arg) {
	struct process *p = (struct process *)arg;
	int status = 0;

	waitpid(p->pid, &status, 0);

	DEBUG_2("Child (%d) finished: %d.\n", p->pid, status);

	return NULL;
}

/*---------------------------------------------
 *--------------------------------------------*/
void debug_tokenize_cmd(char **argv, int argc) {
	int i = 0;
	fprintf(stderr, "argc: %d\n", argc);
	for (i = 0; i<argc; i++)
		fprintf(stderr, "argv[%d]: -%s-\n", i, (argv[i]) ? argv[i] : "!");
}

/*---------------------------------------------
 *--------------------------------------------*/
int tokenize_cmd(char *cmd, char ***argv, int *argc) {
	char *saveptr = NULL;

	for (*argc = 0, *argv = NULL; ; (*argc)++, cmd = NULL) {
		int will_break = 0;
		char *token = strtok_r(cmd, " ", &saveptr);
		if (token == NULL)
			will_break = 1;
		
		*argv = (char**)realloc(*argv, ((*argc)+1)*sizeof(char**));
		(*argv)[*argc] = token;

		if (will_break)
			break;
	}

	return 0;
}

/*---------------------------------------------
 *--------------------------------------------*/
void handle_stop_cmd(char *tag, char *extra) {
	struct process *p = NULL;

	if (p = find_process_by_tag(tag)) {
		DEBUG_3("Found process to kill\n");
//		if (kill(p->pid, SIGINT)) {
		if (kill(p->pid, SIGKILL)) {
			fprintf(stderr, "kill() failed: %d\n", errno);
		}
	}
}

/*---------------------------------------------
 *--------------------------------------------*/
void handle_input_cmd(char *tag, char *extra) {
	struct process *p = NULL;
	char *newline = "\n";

	if (p = find_process_by_tag(tag)) {
		DEBUG_3("INPUT extra: %s\n", extra);
		DEBUG_3("p->input: %d\n", p->input);
		DEBUG_3("p->output: %d\n", p->output);
#if 0
		//write(p->input, extra, strlen(extra));
		//write(p->input, newline, strlen(newline));
		write(p->input, "any\r\n", 4);
		//close(p->input);
#endif
		FILE *in = NULL;

		if (!(in = fdopen(p->input, "w"))) {
			fprintf(stderr, "Could not send input to process (%s).\n", tag);
			return;
		}
		fprintf(in, "%s\n", extra);
		fflush(in);
		//fclose(in);
	} else {
		fprintf(stderr, "Could not find process (%s) for input.\n",tag);
	}
	return;
}

/*---------------------------------------------
 *--------------------------------------------*/
void handle_start_cmd(char *tag, char *cmd) {
	struct process *p;
	int pid = 0;
	int local_stdin_pipe[2], local_stdout_pipe[2];
	char **tokenized_cmd = NULL;
	int tokenized_cmd_len = 0;

	if (global_pipe[0] == GLOBAL_PIPE_UNINITIALIZED || 
	    global_pipe[1] == GLOBAL_PIPE_UNINITIALIZED) {
		fprintf(stderr, "Cannot do commands without a listener.\n");
		return;
	}

	p = (struct process*)malloc(sizeof(struct process));
	memset(p, 0, sizeof(struct process));

	p->tag = strdup(tag);

	if (pipe(local_stdin_pipe)) 
		fprintf(stderr, "pipe2(): %d", errno);
	else {
		DEBUG_3("local_stdin_pipe[0]: %d\n", local_stdin_pipe[0]);
		DEBUG_3("local_stdin_pipe[1]: %d\n", local_stdin_pipe[1]);
	}
	if (pipe(local_stdout_pipe))
		fprintf(stderr, "pipe2(): %d", errno);
	else {
		DEBUG_3("local_stdout_pipe[0]: %d\n", local_stdout_pipe[0]);
		DEBUG_3("local_stdout_pipe[1]: %d\n", local_stdout_pipe[1]);
	}

	if (tokenize_cmd(cmd, &tokenized_cmd, &tokenized_cmd_len))
		fprintf(stderr, "Error tokenizing the cmd!\n");

	debug_tokenize_cmd(tokenized_cmd, tokenized_cmd_len);

	if (!(pid = fork())) {
		/* TODO: Support command line arguments!
		 */
		
		dup2(local_stdin_pipe[0], 0);
		dup2(local_stdout_pipe[1], 1);

		close(local_stdin_pipe[0]);
		close(local_stdin_pipe[1]);
		close(local_stdout_pipe[0]);
		close(local_stdout_pipe[1]);

		if (execvp(tokenized_cmd[0], tokenized_cmd)<0) {
			fprintf(stderr, "execvp() failed!\n");
			fflush(stderr);
			exit(1);
		}
	}
	DEBUG_2("fork()ed: %d\n", pid);
	p->pid = pid;

	p->input = local_stdin_pipe[1];
	p->output = local_stdout_pipe[0];

	close(local_stdin_pipe[0]);
	close(local_stdout_pipe[1]);

	add_process(p);
#ifdef DEBUG
	print_processes();
#endif

	pthread_create(&(p->output_thread), NULL, output_monitor, p);
	pthread_create(&(p->wait_thread), NULL, threaded_wait_pid, p);
}

/*---------------------------------------------
 *--------------------------------------------*/
int setup_server_socket(unsigned short port, unsigned long addr) {
	int server;
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(addr);
	sin.sin_family = AF_INET;

	if ((server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		fprintf(stderr, "Error in socket()\n");
		return -1;
	}
	if (fcntl(server, F_SETFD, FD_CLOEXEC)) {
		fprintf(stderr, "Error in socket()\n");
		return -1;
	}
	if (bind(server, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
		fprintf(stderr, "Error in bind(): %d\n", errno);
		return -1;
	}
	
	if (listen(server, -1) == -1) {
		fprintf(stderr, "Error in listen()\n");
		return -1;
	}
	return server;
}

/*---------------------------------------------
 *--------------------------------------------*/
void handle_cmd_client(int client) {
	char *message = NULL, *token = NULL, *saveptr = NULL;
	message_tokens_t message_token = KEY;
	char *parsed_message[4];
	
	DEBUG_2("Client connected!\n");
	
	if (read_message(client, &message) < 0) {
		/* 
		 * error occurred reading message.
		 */
		fprintf(stderr, "Error reading message!\n");
		goto out;
	}
	DEBUG_2("message: -%s-\n", message);

	token = strtok_r(message, ":", &saveptr);
	while (token) {
		DEBUG_3("token: %s\n", token);
		parsed_message[message_token] = token;
		message_token++;
		token = strtok_r(NULL, ":", &saveptr);
	}

	DEBUG_2("1. key    : %s\n", parsed_message[KEY]);
	DEBUG_2("2. command: %s\n", parsed_message[COMMAND]);
	DEBUG_2("3. tag    : %s\n", parsed_message[TAG]);
	DEBUG_2("4. extra  : %s\n", parsed_message[EXTRA]);

	if (!(strlen(parsed_message[KEY]) == 8 &&
	check_connection_key(parsed_message[KEY], global_connection_key))) {
		/*
		 * key does not match!
		 */
		fprintf(stderr, "KEY does not match.\n");
		goto out;
	}

	if (!strcmp(parsed_message[COMMAND], "START")) {
		DEBUG_3("start\n");
		handle_start_cmd(parsed_message[TAG], parsed_message[EXTRA]);
	} else if (!strcmp(parsed_message[COMMAND], "STOP")) {
		DEBUG_3("stop\n");
		handle_stop_cmd(parsed_message[TAG], parsed_message[EXTRA]);
	} else if (!strcmp(parsed_message[COMMAND], "INPUT")) {
		DEBUG_3("input\n");
		handle_input_cmd(parsed_message[TAG], parsed_message[EXTRA]);
	} else {
		fprintf(stderr,"Unknown COMMAND: %s\n",parsed_message[COMMAND]);
	}
out:

	print_processes();	

	shutdown(client, SHUT_RDWR);
	close(client);
}

/*---------------------------------------------
 *--------------------------------------------*/
void handle_io_client(int client) {
	FILE *global_output, *client_output;
	sigset_t block_sig_set;
	fd_set global_pipe_set;
	

	sigemptyset(&block_sig_set);
	sigaddset(&block_sig_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &block_sig_set, NULL);

//	pipe2(global_pipe, O_CLOEXEC);
	pipe(global_pipe);
	FD_ZERO(&global_pipe_set);
	FD_SET(global_pipe[0], &global_pipe_set);

	if (!(global_output = fdopen(global_pipe[0], "r"))) {
		fprintf(stderr, "OOPS: Cannot open global output!\n");
		return;
	}

	if (!(client_output = fdopen(client, "w"))) {
		fprintf(stderr, "OOPS: Cannot open client output!\n");
		return;
	}

	while (1) {
		char buffer[LINE_BUFFER_SIZE+1] = {0,};
		struct timeval timeout;

		timeout.tv_sec = 5;
		timeout.tv_usec = 0;

		select(FD_SETSIZE, &global_pipe_set, NULL, NULL, &timeout);

#if 0
This is supposed to handle the case
where an i/o client goes away. 
Unfortunately we cannot detect that 
case with any certainty.
		if (FD_ISSET(client, &client_set)) {
			DEBUG("client disconnected!\n");
			close(global_pipe[0]);	
			close(global_pipe[1]);
			global_pipe[0] = GLOBAL_PIPE_UNINITIALIZED;
			global_pipe[1] = GLOBAL_PIPE_UNINITIALIZED;
			fclose(client_output);
			break;
		}
#endif
		DEBUG_3("Done global select()ing\n");
		if (FD_ISSET(global_pipe[0], &global_pipe_set)) {
			if (fgets(buffer, LINE_BUFFER_SIZE, global_output)) {
				DEBUG_3("OUTPUT: %s", buffer);
				fprintf(client_output, "%s", buffer);
				if (fflush(client_output)) {
					fprintf(stderr, "Got error from fflush(client_output)!\n");
					if (errno == EPIPE) {
						fprintf(stderr, "fflush() failed with EPIPE!\n");
					}
					break;
				}
			} else {
				fprintf(stderr, "Got EOF from fgets()\n");
				break;
			}
		}
		FD_SET(global_pipe[0], &global_pipe_set);
	}
	DEBUG_3("Stopping handle_io_client.\n");
	close(global_pipe[0]);	
	close(global_pipe[1]);
	global_pipe[0] = GLOBAL_PIPE_UNINITIALIZED;
	global_pipe[1] = GLOBAL_PIPE_UNINITIALIZED;
	fclose(client_output);
	return;
}

/*---------------------------------------------
 *--------------------------------------------*/
void *dummy_handle_io_client(void *dummy) {
	handle_io_client(0);
	return NULL;
}

/*---------------------------------------------
 *--------------------------------------------*/
void *io_listener(void *unused) {
	int server, client;
	struct sockaddr child_in;
	socklen_t child_in_len = sizeof(struct sockaddr);

	if ((server = setup_server_socket(5001, INADDR_ANY)) < 1) {
		fprintf(stderr, "Error from setup_server_socket()\n");
		return NULL;
	}

	while (client = accept(server, 
			     (struct sockaddr*)&child_in, &child_in_len)) {
		handle_io_client(client);
	}
	return (void*)1;
}

/*---------------------------------------------
 *--------------------------------------------*/
void *command_listener(void *unused) {
	int server, client;
	struct sockaddr child_in;
	socklen_t child_in_len = sizeof(struct sockaddr);

	if ((server = setup_server_socket(5000, INADDR_ANY)) < 1) {
		fprintf(stderr, "Error from setup_server_socket()\n");
		return NULL;
	}

	while ((client = accept(server, 
			(struct sockaddr*)&child_in, &child_in_len)) >= 0) {
		handle_cmd_client(client);
	}

	fprintf(stderr, "command_listener: %d, %s\n", client, strerror(errno));
	return NULL;
}


int main(int argc, char *argv[], char *envp[]) {
	void *retval;
	pthread_t cmd_server_thread, io_server_thread;
	int pthread_error = 0;

	global_pipe[0] = -1;
	global_pipe[1] = -1;

	global_envp = envp;

	init_process_list();

	if (pthread_error = pthread_mutex_init(&global_pipe_output_lock, NULL)) {
		/* error occurred initializing mutex.
		 */
		fprintf(stderr, "pthread_mutex_init() failed: %d\n", pthread_error);
		return 1;
	}

	if (pthread_error = pthread_create(&cmd_server_thread, NULL, command_listener, NULL)) {
		/* error
		 */
		fprintf(stderr,"pthread_create(cmd_server_thread) failed: %d.\n", pthread_error);
		return 1;
	}
#ifdef GLOBAL_OUTPUT
	if (pthread_error = pthread_create(&io_server_thread, NULL, io_listener, NULL)) {
		/* error
		 */
		fprintf(stderr, "pthread_create(io_server_thread) failed: %d.\n", pthread_error);
		return 1;
	}
#else
	if (pthread_error = pthread_create(&io_server_thread, NULL, dummy_handle_io_client, NULL)) {
		fprintf(stderr, "pthread_create(dummy_handle_io_client) failed: %d.\n", pthread_error);
		return 1;
	}

#endif

	global_connection_key = generate_connection_key();
	printf("KEY:%c%c%c%c%c%c%c%c\n", global_connection_key[0], 
		global_connection_key[1],
		global_connection_key[2],
		global_connection_key[3],
		global_connection_key[4],
		global_connection_key[5],
		global_connection_key[6],
		global_connection_key[7]);
	fflush(stdout);
	
	pthread_join(io_server_thread, &retval);
	if (retval == NULL) {
		fprintf(stderr, "io_server_thread failed. Exiting.\n");
		return 1;
	}
	pthread_join(cmd_server_thread, &retval);

	return 0;
}

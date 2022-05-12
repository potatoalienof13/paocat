#include "hashmap.h/hashmap.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <uv.h>


int server_socket;

void sucks_to_sucks(char *string) {
	puts(string);
	puts("\n");
	fprintf(stderr, "failed with, %s as errno\n", strerror(errno));
	close(server_socket);
	exit(-1);
}

enum message_type {
	MESSAGE_TYPE_SEND,
	MESSAGE_TYPE_LINK,
	MESSAGE_TYPE_UNLINK,
	
	// Unimplemented for now.
	MESSAGE_TYPE_FIND,
	MESSAGE_TYPE_REPLY,
};

enum SOCKET_TYPE {
	SOCKET_TYPE_NONE,
	SOCKET_TYPE_SERVER,
	SOCKET_TYPE_LISTEN,
	SOCKET_TYPE_BIND,
	SOCKET_TYPE_STDIN,
};

typedef int64_t group_t;

const int MESSAGE_HEADER_SIZE =
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t);

struct pollfd_with_metadata {
	struct pollfd pollfd;
	int socket_type;
	bool is_currently_in_middle_of_message;
	char *message_buffer;
	size_t message_buffer_size;
	int full_message_length;
	int current_message_length;
	group_t *groups;
	int groups_size;
};

void free_pfdwm_slot(struct pollfd_with_metadata *slot) {
	slot->pollfd.fd = -1;
	slot->socket_type = SOCKET_TYPE_NONE;
	//hashmap_destroy(&slot->groups);
	slot->is_currently_in_middle_of_message = false;
	if (!slot->message_buffer) {
		free(slot->message_buffer);
		slot->message_buffer = NULL;
	}
	slot->message_buffer_size = 0;
	slot->full_message_length = 0;
	slot->current_message_length = 0;
}

#define MAX_CLIENTS 255
#define BUFFER_SIZE 255
#define MAX_COMMAND_SIZE 255

void copy_pollfd_with_metadata_to_pollfd(struct pollfd pfd[],
                                         struct pollfd_with_metadata pfdwm[],
                                         int number) {
	for (int i = 0; i < number; i++)
		pfd[i] = pfdwm[i].pollfd;
}

void copy_pollfd_to_pollfd_with_metadata(struct pollfd pfd[],
                                         struct pollfd_with_metadata pfdwm[],
                                         int number) {
	for (int i = 0; i < number; i++)
		pfdwm[i].pollfd = pfd[i];
}

struct pollfd_with_metadata *
get_first_empty_slot_in_pfds(struct pollfd_with_metadata pfds[],
                             int pfds_size) {
	for (int i = 0; i < pfds_size; i++) {
		if (pfds[i].pollfd.fd == -1) {
			if (pfds[i].socket_type != SOCKET_TYPE_NONE)
				puts("Someone failed to set the socket type of any empty socket to "
				     "NONE");
			return &pfds[i];
		}
	}
	sucks_to_sucks("ran out of slots for clients");
	exit(-1); // sucks_to_sucks will exit, but this will get rid of a warning.
}


struct hashmap_s group_to_pfd_list;
// Message header:
// uint32_t length
// uint32_t message_type
// uint64_t client_id
// uint64_t message_id

uint64_t our_peer_id;
uint64_t message_id = 0;

void send_message_header(struct pollfd_with_metadata *pfd, int32_t length,
                         int32_t message_type) {
	int full_length = length + sizeof(length) + sizeof(our_peer_id) +
	                  sizeof(message_type) + sizeof(message_id);
	send(pfd->pollfd.fd, &full_length, sizeof(full_length), 0);
	send(pfd->pollfd.fd, &message_type, sizeof(message_type), 0);
	send(pfd->pollfd.fd, &message_id, sizeof(message_id), 0);
	send(pfd->pollfd.fd, &our_peer_id, sizeof(our_peer_id), 0);
	message_id++;
}

void send_message_type_send(struct pollfd_with_metadata *pfd, group_t group,
                            char *data, size_t data_length) {
	send_message_header(pfd, data_length + sizeof(group), MESSAGE_TYPE_SEND);
	send(pfd->pollfd.fd, data, data_length, 0);
}

void send_message_type_any_link(struct pollfd_with_metadata *pfd,
                                group_t target_group, int message_type) {
	send_message_header(pfd, sizeof(target_group), message_type);
	send(pfd->pollfd.fd, &target_group, sizeof(target_group), 0);
}

void send_message_type_link(struct pollfd_with_metadata *pfd,
                            group_t target_group) {
	send_message_type_any_link(pfd, target_group, MESSAGE_TYPE_LINK);
}

void send_message_type_unlink(struct pollfd_with_metadata *pfd,
                              group_t target_group) {
	send_message_type_any_link(pfd, target_group, MESSAGE_TYPE_UNLINK);
}


void parse_message_header(char *buffer, int buffer_size,
                          struct message_header *mh) {
	if (buffer_size < MESSAGE_HEADER_SIZE)
		sucks_to_sucks("sent too small header to the header parser");
	mh->length = *(uint32_t *)buffer;
	mh->message_type = *(uint32_t *)(buffer + sizeof(mh->length));
	mh->peer_id =
	    *(uint64_t *)(buffer + sizeof(mh->length) + sizeof(mh->message_type));
	mh->message_id =
	    *(uint64_t *)(buffer + sizeof(mh->length) + sizeof(mh->message_type) +
	                  sizeof(mh->peer_id));
}

group_t get_group_from_message(char *message, int message_length) {
	if (message_length < (MESSAGE_HEADER_SIZE + sizeof(group_t)))
		sucks_to_sucks("passed too small message to get group func");
	// The group is always the first argument
	return *(group_t *)(message + MESSAGE_HEADER_SIZE);
}

// We have to copy a message sent to one of our groups to every pfdwm that has that group.
// We also have to be able to insert and delete group->pfdwm relationships
void handle_message_type_send(struct message_header *mh,
                              struct pollfd_with_metadata *pfd,
                              struct pollfd_with_metadata pfdwms[],
                              int pfdwms_size) {
	group_t group = get_group_from_message(pfd->message_buffer, mh->length);
	fwrite(pfd->message_buffer + MESSAGE_HEADER_SIZE, sizeof(char), mh->length - MESSAGE_HEADER_SIZE, stdout);
	for (int i = 0; i < pfdwms_size; i++) {
		if (pfdwms[i].pollfd.fd == -1) continue;
		if (pfdwms[i].pollfd.fd == pfd->pollfd.fd) continue;
		for (int j = 0; j < pfdwms[i].groups_size; j++) {
			if (pfdwms[i].groups[j] == group) { send(pfd->pollfd.fd, pfd->message_buffer, mh->length, 0); break; }
		}
	}
}
void handle_message_type_link(struct message_header *mh,
                              struct pollfd_with_metadata *pfd,
                              struct pollfd_with_metadata pfdwms[],
                              int pfdwms_size) {
	group_t group = get_group_from_message(pfd->message_buffer, mh->length);
	if (mh->length != MESSAGE_HEADER_SIZE + sizeof(group_t)) {
		free_pfdwm_slot(pfd);
		return;
	}
	for (int i = 0; i < pfd->groups_size; i++) {
		if (pfd->groups[i] == 0) { pfd->groups[i] = group; return; }
	}
	pfd->groups  = realloc(pfd->groups, (1 + pfd->groups_size) * sizeof(group_t));
	pfd->groups_size += 1;
}

void handle_message_type_unlink(struct message_header *mh,
                                struct pollfd_with_metadata *pfd,
                                struct pollfd_with_metadata pfdwms[],
                                int pfdwms_size) {
	group_t group = get_group_from_message(pfd->message_buffer, mh->length);
	for (int i = 0; i < pfd->groups_size; i++) {
		if (pfd->groups[i] == group) { pfd->groups[i] = 0; return; }
	}
}

// This needs to handle commands, but it doesnt, TOO SAD
void handle_socket_type_stdin(struct pollfd_with_metadata pfdwm) {
	int bytes_read = 0;
	char buffer[BUFFER_SIZE];
	
	
	bytes_read = read(pfdwm.pollfd.fd, buffer, BUFFER_SIZE);
	if (errno == EAGAIN) {
		// we are done reading,
		// is_done_reading = true;
		sucks_to_sucks("why cant we read() aaaa");
	}
	if (bytes_read == BUFFER_SIZE)
		sucks_to_sucks("HOW DARE YOU INPUT MORE BYTES THAN CAN FIT IN THE BUFFER "
		               "WITHOUT DROPPING OUT OLD CONTENTS");
		               
	buffer[BUFFER_SIZE - 1] = '\0';
	
	if (strncmp("LINK", buffer, strlen("LINK"))) {
	} else if (strncmp("UNLINK", buffer, strlen("UNLINK"))) {
	} else if (strncmp("SEND", buffer, strlen("SEND"))) {
	} else
		sucks_to_sucks("thats not a real command, nerd\nI could just ignore it, but fuck you");
}

struct hashmap_s client_id_to_highest_message_id;

void handle_socket_type_listen(struct pollfd_with_metadata *pfd) {
	// We only care about being able to read, if we cant, then we do nothing
	if (!(pfd->pollfd.revents & POLLIN))
		return;
		
	// char buffer[BUFFER_SIZE];
	int total_bytes_read = 0;
	errno = 0;
	
	if (!pfd->is_currently_in_middle_of_message) {
		// No buffer has been set up yet
		pfd->message_buffer = malloc(MESSAGE_HEADER_SIZE);
		pfd->message_buffer_size = MESSAGE_HEADER_SIZE;
	}
	
	// We dont know the size of the message yet
	if (pfd->current_message_length < MESSAGE_HEADER_SIZE) {
		int bytes_read = recv(
		                     pfd->pollfd.fd, pfd->message_buffer + pfd->current_message_length,
		                     MESSAGE_HEADER_SIZE - pfd->current_message_length, MSG_DONTWAIT);
		if (bytes_read == 0 || errno == EAGAIN)
			return;
		else if (errno) {
			free_pfdwm_slot(pfd);
			puts("socket broke or closed or smth");
			puts(strerror(errno));
			return;
		}
		if (pfd->current_message_length < MESSAGE_HEADER_SIZE)
			return; // We cant handle it yet.
		pfd->is_currently_in_middle_of_message = true;
		pfd->current_message_length += bytes_read;
	}
	// Now we have a full message buffer, yay
	
	struct message_header mh;
	parse_message_header(pfd->message_buffer, MESSAGE_HEADER_SIZE, &mh);
	if (mh.length < MESSAGE_HEADER_SIZE) {
		puts("Someone is sending corrupted message so sad");
		free_pfdwm_slot(pfd);
		return;
	}
	if (mh.length > pfd->message_buffer_size) {
		pfd->message_buffer = realloc(pfd->message_buffer, mh.length);
		pfd->message_buffer_size = mh.length;
	}
	
	total_bytes_read =
	    recv(
	        pfd->pollfd.fd, pfd->message_buffer + pfd->current_message_length,
	        pfd->full_message_length - pfd->current_message_length, MSG_DONTWAIT
	    );
	    
	if (total_bytes_read == 0 || errno == EAGAIN)
		return;
	else if (errno) {
		free_pfdwm_slot(pfd);
		puts("socket broke or closed or smth");
		puts(strerror(errno));
		return;
	}
	pfd->current_message_length += total_bytes_read;
	
	if (pfd->current_message_length < pfd->full_message_length)
		return;
	// Now we have completely received the message
	
	// client ids cant be stored in the pfdwm array because we may not be
	// necessarily talking to that peer
	int64_t *sender_peer_id;
	int64_t *sender_message_id;
	if (!(sender_message_id =
	          hashmap_get(&client_id_to_highest_message_id, (char *)&mh.peer_id,
	                      sizeof(mh.peer_id)))) {
		sender_message_id = malloc(sizeof(int64_t));
		*sender_message_id = mh.message_id;
		hashmap_put(&client_id_to_highest_message_id, (char *)sender_peer_id,
		            sizeof(sender_peer_id), sender_message_id);
	} else if (mh.message_id < *sender_message_id)
		return;
		
	switch (mh.message_type) {
	case MESSAGE_TYPE_SEND:
	case MESSAGE_TYPE_LINK:
	case MESSAGE_TYPE_UNLINK:
	default:
		free_pfdwm_slot(pfd);
	}
	// Otherwise
}

void handle_socket_type_server(struct pollfd_with_metadata pfds[],
                               int pfds_size,
                               struct pollfd_with_metadata *pfd) {
	// This means that we can accept a new connection.
	if (!(pfd->pollfd.revents & POLLIN))
		return;
		
	struct pollfd_with_metadata *pfdwm =
	    get_first_empty_slot_in_pfds(pfds, pfds_size);
	int ret = accept(server_socket, NULL, NULL);
	// puts(strerror(errno));
	
	if (ret < 0)
		sucks_to_sucks("accept failed");
	pfdwm->pollfd.fd = ret;
	pfdwm->socket_type = SOCKET_TYPE_LISTEN;
	puts("accepted connection");
	return;
}




int main(int argc, char **argv) {
	int ret = fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	if (ret == 1)
		sucks_to_sucks("failed to make stdin nonblocking");
		
	// We will first connect to the servers we want to connect to.
	// Only stupid argc handling, parsing pairs of parameters
	
	struct pollfd_with_metadata pollfds_with_metadata[MAX_CLIENTS];
	
	// not gonna error check this, lol
	getrandom(&our_peer_id, sizeof(our_peer_id), 0);
	
	// intialize this to array to contain nothing that poll would try to do
	// anything with
	for (int i = 0; i < MAX_CLIENTS; i++) {
		pollfds_with_metadata[i].pollfd.fd = -1;
		pollfds_with_metadata[i].pollfd.events = POLLIN;
		pollfds_with_metadata[i].pollfd.revents = 0;
		pollfds_with_metadata[i].socket_type = SOCKET_TYPE_NONE;
		//	if (0 != hashmap_create(2, &pollfds_with_metadata[i].groups))
		//		sucks_to_sucks("failed to create a hashmap");
		
		// We will be reading on stdin sooo might as use poll for that
		struct pollfd_with_metadata *pfdwm =
		    get_first_empty_slot_in_pfds(pollfds_with_metadata, MAX_CLIENTS);
		pfdwm->pollfd.fd = STDIN_FILENO;
		pfdwm->socket_type = SOCKET_TYPE_STDIN;
		
		pfdwm->groups = malloc(sizeof(group_t) * 2);
		pfdwm->groups_size = 2;
	}
	// Connect to the peers specified by the command line arguments.
	/*struct addrinfo hints;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	struct addrinfo *results;
	int num_ip_address_and_port_arguments = (argc - 1) / 2;
	// struct sockaddr_in6 remotes[num_ip_address_and_port_arguments];
	for (int i = 0; i < num_ip_address_and_port_arguments; i++) {
		int ret =
		    getaddrinfo(argv[1 + i / 2], argv[2 + i / 2], &hints, &results);
		if (ret != 0)
			sucks_to_sucks("failed a getaddrinfo");
		int new_socket =
		    socket(results->ai_family, results->ai_flags, results->ai_protocol);
	
		if (connect(new_socket, results->ai_addr, results->ai_addrlen) == -1)
			sucks_to_sucks("failed a connect");
		struct pollfd_with_metadata *slot =
		    get_first_empty_slot_in_pfds(pollfds_with_metadata, MAX_CLIENTS);
		slot->pollfd.fd = new_socket;
		slot->socket_type = SOCKET_TYPE_BIND;
		freeaddrinfo(results);
	};
	*/
	hashmap_create(2, &group_to_pfd_list);
	
	// Then we will run our own server.
	server_socket = socket(AF_INET6, SOCK_STREAM, 0);
	if (server_socket < 0)
		sucks_to_sucks("sockeett");
	struct sockaddr_in6 address;
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	memcpy(&address.sin6_addr, &in6addr_any, sizeof(in6addr_any));
	address.sin6_port = htons(46163);
	
	ret = bind(server_socket, (struct sockaddr *)&address, sizeof(address));
	puts(strerror(errno));
	if (ret < 0)
		sucks_to_sucks("bind failed");
		
	// size_t address_size = sizeof(address);
	
	pollfds_with_metadata[0].pollfd.fd = server_socket;
	pollfds_with_metadata[0].socket_type = SOCKET_TYPE_SERVER;

	uv_tcp_t *tcp_listener =malloc(sizeof(uv_tcp_t));
	
	int on = 1; // no clue why this is necessary
	if (ioctl(server_socket, FIONBIO, (char *)&on) < 0)
		sucks_to_sucks("ioctl");
	if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) <
	    0)
		sucks_to_sucks("sock opt");
	if (listen(server_socket, 32) < 0)
		sucks_to_sucks("listen");
		
	struct pollfd pollfds[MAX_CLIENTS];

	uv_loop_t *loop = malloc(sizeof(uv_loop_t));
	uv_loop_init(loop);
	for (;;) {
		copy_pollfd_with_metadata_to_pollfd(pollfds, pollfds_with_metadata,
		                                    MAX_CLIENTS);
		ret = poll(pollfds, MAX_CLIENTS, -1);
		copy_pollfd_to_pollfd_with_metadata(pollfds, pollfds_with_metadata,
		                                    MAX_CLIENTS);
		if (ret < 0)
			sucks_to_sucks("poll");
			
		for (int i = 1; i < MAX_CLIENTS; i++) {
			switch (pollfds_with_metadata[i].socket_type) {
			case SOCKET_TYPE_BIND:
			case SOCKET_TYPE_LISTEN:
				handle_socket_type_listen(&pollfds_with_metadata[i]);
				break;
			case SOCKET_TYPE_SERVER:
				handle_socket_type_server(pollfds_with_metadata, MAX_CLIENTS,
				                          &pollfds_with_metadata[i]);
				break;
			case SOCKET_TYPE_NONE:
				// Socket should be empty
				break;
			default:
				sucks_to_sucks("UNIMPLEMENTED SOCKET_TYPE");
			}
			//	printf("%b\n", pollfd_clients[i].revents);
		}
		puts("finished loop");
	}
	
	close(server_socket);
}




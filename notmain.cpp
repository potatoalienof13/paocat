//#include "hashmap.h/hashmap.h"
#include "notmain.h"
#include <uv.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <sys/random.h>
#include <limits.h>
#include <unordered_map>
#include <vector>
#include <optional>
#include <list>
#include <cstring>

#define DEFAULT_PORT 46163
#define DEFAULT_CONNECTION_BACKLOG 100

// todo: need to send links for all groups when we connect to new peer

// group representation:
// hashmap from group to list of stream_datas that are part of that group

static uv_loop_t *default_loop;
static peer_t our_peer;
static message_id_t global_message_id;

std::unordered_map<group_t, std::list<uv_stream_t *>> group_to_peer_hashmap;
std::unordered_map<peer_t, message_id_t> peer_id_to_highest_message_id;
std::list<group_t> groups_we_like;


static void parse_message_header(char *data, size_t data_size, struct paocat_header_s *header) {
	if (data_size < sizeof(struct paocat_header_s)) {
		fprintf(stderr, "Data that was too small was passed to be parsed as a header");
		return;
	}
	memcpy(header, data, sizeof(*header));
}


static void clear_peer_from_hashmap(uv_stream_t *peer) {
	for (auto [key, value] : group_to_peer_hashmap)
		value.remove(peer);
}

static void free_peer_stream(uv_handle_t *handle) {
	struct stream_data *peer_data = (stream_data *)handle->data;
	if (peer_data->partial_message->message.data)
		free(peer_data->partial_message->message.data);
	free(peer_data);
}

static void close_stream_data_cb(uv_handle_t *handle) {
	free_peer_stream(handle);
	clear_peer_from_hashmap((uv_stream_t *)handle);
}

static void free_partial_message(struct paocat_partial_message_s *partial_message) {
	free(partial_message->message.data);
	partial_message->amount_received = 0;
	partial_message->data_capacity = 0;
	partial_message->message.data_size = 0;
}

static void message_callback(uv_write_t *req, int status) {
	if (status) {
		// error, lol dont care
		uv_close((uv_handle_t *)req->handle, close_stream_data_cb);
	}
}

static void paocat_one_message(uv_stream_t *peer, struct paocat_message_s *message) {
	uv_buf_t buffers[2];
	
	buffers[0].base = (char *)&message->header;
	buffers[0].len = sizeof(struct paocat_header_s);
	
	buffers[1].base = message->data;
	buffers[1].len = message->data_size;
	
	uv_write_t request;
	uv_write(&request, peer, buffers, 2, message_callback);
}

static void broadcast_message_to_group(struct paocat_message_s *message) {
	std::list<uv_stream_t *> peer_datas = group_to_peer_hashmap.at(message->header.group);
	
	for (auto i : peer_datas)
		paocat_one_message(i, message);
}


static int paocat_generic_link_group(int message_type, group_t group) {
	if (message_type != PAOCAT_MESSAGE_TYPE_LINK
	    && message_type != PAOCAT_MESSAGE_TYPE_UNLINK) fprintf(stderr, "Generic link called with a non-link messagetype");
	    
	struct paocat_message_s *message = new struct paocat_message_s;;
	message->header.message_type = message_type;
	message->header.peer_id = our_peer;
	message->header.message_id = global_message_id;
	message->header.length = sizeof(struct paocat_header_s);
	message->header.group = group;
	for (auto peer_list : group_to_peer_hashmap) {
		for (auto peer : peer_list.second)
			paocat_one_message(peer, message);
	}
	delete message;
	return 0;
}

int paocat_join_group(group_t group) {
	groups_we_like.push_back(group);
	return paocat_generic_link_group(PAOCAT_MESSAGE_TYPE_LINK, group);
}

int paocat_leave_group(group_t group) {
	return paocat_generic_link_group(PAOCAT_MESSAGE_TYPE_UNLINK, group);
}

int paocat_send_message(group_t group, char message_data[], size_t message_size) {
	struct paocat_message_s *message =  new struct paocat_message_s;
	message->header.message_type = PAOCAT_MESSAGE_TYPE_SEND;
	message->header.peer_id = our_peer;
	message->header.message_id = global_message_id;
	message->header.length = message_size;
	message->header.group = group;
	message->data = message_data;
	message->data_size = message_size;
	broadcast_message_to_group(message);
	delete message;
	return 0;
}

static void deal_with_read_data(uv_stream_t *stream, ssize_t number_read, const uv_buf_t *buffer) {
	// uv_buf_t SHOULD be a *stream_data
	if (number_read < 0) {
		// dont care lol
		uv_close((uv_handle_t *)stream, close_stream_data_cb);
		fprintf(stderr, "Closed a stream that had error %zi\n", number_read);
		return;
	} else if (number_read == 0) {
		// We dont free *buffer because it is part of the message
		return;
	}
	
	struct stream_data *peer_data = (stream_data *)stream->data;
	struct paocat_partial_message_s *partial_message = peer_data->partial_message;
	
	// the buffer has been filled to this much data
	peer_data->partial_message->amount_received += number_read;
	
	if (peer_data->partial_message->amount_received < sizeof(struct paocat_header_s))
		return;
	else if (!peer_data->partial_message->header_parsed) {
		parse_message_header(partial_message->message.data, partial_message->amount_received, &partial_message->message.header);
		partial_message->header_parsed = true;
		if (partial_message->amount_received == partial_message->message.header.length)
			fprintf(stderr, "Someone sent an empty message lol");
	}
	// header should be parsed at this point
	if (!partial_message->header_parsed) fprintf(stderr, "Header not actually parsed when it should have been, so sad");
	if (partial_message->amount_received < partial_message->message.header.length) return;
	
	if (our_peer == partial_message->message.header.peer_id) {
		free_partial_message(partial_message);
		return;
	}
	
	
	
	
	
	switch (partial_message->message.header.message_type) {
	case PAOCAT_MESSAGE_TYPE_SEND:
		// Only care about message_id for send messages because they are the only type that is rebroadcasted
		// Cant use && here because [] has side effects and execution order is unguaranteed
		if (peer_id_to_highest_message_id.count(partial_message->message.header.peer_id)) {
			if (peer_id_to_highest_message_id[partial_message->message.header.peer_id] >=
			    partial_message->message.header.message_id) {
				free_partial_message(partial_message);
				return;
			}
		}
		peer_id_to_highest_message_id[partial_message->message.header.peer_id] = partial_message->message.header.message_id;
		broadcast_message_to_group(&partial_message->message);
		break;
	case PAOCAT_MESSAGE_TYPE_LINK:
		// Very important semicolon do not remove
		;
		if (partial_message->message.data_size != sizeof(struct paocat_message_s)) {
			fprintf(stderr, "Peer sent an incorrect link message, with an incorrectly sized header");
			uv_close((uv_handle_t *)stream, close_stream_data_cb);
		}
		
		group_to_peer_hashmap[partial_message->message.header.group].push_back(stream);
		
		break;
	case PAOCAT_MESSAGE_TYPE_UNLINK:
		group_to_peer_hashmap[partial_message->message.header.group].remove(stream);
		break;
	default:
		uv_close((uv_handle_t *)stream, close_stream_data_cb);
		fprintf(stderr, "Closed a stream that sent an invalid message");
	}
	peer_data->on_message_cb(&partial_message->message);
	
}


// The challenge for this function is that it needs to either allocate a buffer, or resume into a previously allocated buffer
static void allocate_a_buffer_for_readable_tcp_peer_data(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
	if (!handle->data)
		fprintf(stderr, "Handle did not have a data pointer that was expected!");
	struct stream_data *peer_data = (stream_data *)handle->data;
	struct paocat_partial_message_s *partial_message = peer_data->partial_message;
	
	// First case: no buffer at all is allocated
	// Second case: the header buffer is full and needs to be reallocated to a buffer that will fit the entire message
	// Third case: need to resume an old buffer
	
	if (partial_message->data_capacity == 0) {
		if (partial_message->message.data != 0)
			fprintf(stderr, "Failed to initialize a tcp handle properly");
		buf->base = (char *)malloc(sizeof(struct paocat_header_s));
		partial_message->message.data = buf->base;
		buf->len = sizeof(struct paocat_header_s);
		partial_message->data_capacity = sizeof(struct paocat_header_s);
		return;
	} else if (partial_message->amount_received == sizeof(struct paocat_header_s)) {
		if (!partial_message->header_parsed) fprintf(stderr, "HEADER NOT PARSED WHEN IT SHOULD HAVE BEEN");
		// we have received the header, and therefore, can parse it
		// theoretically, we should not come back to this function for the same call to the read cb
		partial_message->message.data = (char *) malloc(partial_message->message.header.length - sizeof(
		                                                    struct paocat_header_s));
		partial_message->data_capacity = partial_message->message.header.length - sizeof(struct paocat_header_s);
	}
	buf->len = partial_message->data_capacity - partial_message->data_capacity;
	buf->base = partial_message->message.data + partial_message->amount_received;
}


static void link_all_groups_we_like(uv_stream_t *peer) {
	struct paocat_message_s *message = new struct paocat_message_s;
	message->header.length = sizeof(*message);
	message->data_size = 0;
	message->header.peer_id = our_peer;
	message->header.message_type = PAOCAT_MESSAGE_TYPE_LINK;
	/* Message ID will be filled out by paocat_one_message */
	for (auto i : groups_we_like) {
		message->header.group = i;
		paocat_one_message(peer, message);
	}
	delete message;
}

static void on_accept_connection(struct uv_stream_s *stream, int status) {
	if (status < 0) {
		fprintf(stderr, "Failed to accept connection %s\n", uv_strerror(status));
		return;
	}
	uv_tcp_t *client = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
	uv_tcp_init(default_loop, client);

	link_all_groups_we_like(stream);
	
	client->data = malloc(sizeof(struct stream_data));
	if (uv_accept(stream, (uv_stream_t *) client) == 0)
		uv_read_start((uv_stream_t *) client, allocate_a_buffer_for_readable_tcp_peer_data, deal_with_read_data);
}

int paocat_setup(bool is_server) {
	default_loop = uv_default_loop();
	if (is_server) {
		// open up a socket and stuff
		uv_tcp_t server;
		uv_tcp_init(default_loop, &server);
		
		struct sockaddr_in6 addr;
		uv_ip6_addr("::", 46163, &addr);
		
		// this blocks until done.
		uv_tcp_bind(&server, (struct sockaddr *)&addr, 0);
		int r = uv_listen((uv_stream_t *) &server, DEFAULT_CONNECTION_BACKLOG, on_accept_connection);
		if (r)
		{
			fprintf(stderr, "Listen error %s\n", uv_strerror(r));
			return 1;
		}
	}
	
	getrandom(&our_peer, sizeof(peer_t), 0);
	return 0;
}

int paocat_connect_peer(struct sockaddr addr) {
	uv_tcp_t *socket = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
	uv_tcp_init(default_loop, socket);
	
	uv_connect_t *connection = (uv_connect_t *)malloc(sizeof(uv_connect_t));
	return uv_tcp_connect(connection, socket, &addr, 0);
}

void paocat_receive_message(int (on_message)(struct paocat_message_s pcm)) {
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}


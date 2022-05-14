#ifndef notmain_h_INCLUDED
#define notmain_h_INCLUDED
#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>

typedef int64_t group_t;
typedef int64_t peer_t;
typedef int64_t message_id_t;

enum PAOCAT_MESSAGE_TYPES {
	PAOCAT_MESSAGE_TYPE_SEND,
	PAOCAT_MESSAGE_TYPE_LINK,
	PAOCAT_MESSAGE_TYPE_UNLINK,
};

struct __attribute__((packed))  paocat_header_s {
	uint32_t length;
	uint32_t message_type;
	peer_t  peer_id;
	message_id_t message_id;
	group_t group;
};

struct paocat_message_s {
	struct paocat_header_s header;
	char *data;
	size_t data_size;
};

struct paocat_partial_message_s {
	struct paocat_message_s message;
	size_t data_capacity;
	size_t amount_received;
	bool header_parsed;
};

struct stream_data {
	int (*on_message_cb)(struct paocat_message_s *);
	struct paocat_partial_message_s *partial_message;
};

int paocat_setup(bool is_server);
int paocat_send_message(group_t group,char message_data[],size_t message_size);
int paocat_leave_group(group_t group);
int paocat_join_group(group_t group);
int paocat_connect_peer(struct sockaddr addr);
void paocat_receive_message(int (on_message)(struct paocat_message_s pcm));

#endif // notmain_h_INCLUDED


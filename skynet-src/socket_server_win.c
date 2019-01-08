#include <uv.h>
#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"
#include "spinlock.h"

#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "skynet_mq.h"

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64
#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1
#define SOCKET_TYPE_PLISTEN 2
#define SOCKET_TYPE_LISTEN 3
#define SOCKET_TYPE_CONNECTING 4
#define SOCKET_TYPE_CONNECTED 5
#define SOCKET_TYPE_HALFCLOSE 6
#define SOCKET_TYPE_PACCEPT 7
#define SOCKET_TYPE_BIND 8

#define MAX_SOCKET (1<<MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id>>MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2
#define PROTOCOL_UNKNOWN 255

#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024*1024)
void force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result);
int send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result);
int send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result);
int report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result);
int ctrl_cmd(struct socket_server *ss, int type, int len, char *buffer, struct socket_message *result);

struct write_buffer {
	struct write_buffer * next;
	void *buffer;
	char *ptr;
	int sz;
	bool userobject;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};

struct socket_stat {
	uint64_t rtime;
	uint64_t wtime;
	uint64_t read;
	uint64_t write;
};

struct socket {
	uintptr_t opaque;
	struct wb_list high;
	struct wb_list low;
	int64_t wb_size;
	struct socket_stat stat;
	volatile uint32_t sending;
	union {
		uv_tcp_t tcp;
		uv_udp_t udp;
		uv_tty_t tty;
	} s;
	int id;
	uint8_t protocol;
#ifdef _MSC_VER
	uint16_t type;
#else
	uint8_t type;
#endif
	uint16_t udpconnecting;
	int64_t warn_size;
	union {
		int size;
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
	bool write;
	struct spinlock dw_lock;
	int dw_offset;
	const void * dw_buffer;
	size_t dw_size;
};

struct command_queue {
	struct spinlock lock;
	int cap;
	int head;
	int tail;
	struct skynet_message *queue;
};

struct command_queue *
command_mq_create() {
	struct command_queue *q = skynet_malloc(sizeof(*q));
	q->cap = 64;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	return q;
}

int
command_mq_pop(struct command_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q);

	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
	}
	SPIN_UNLOCK(q);
	return ret;
}

static void
command_mq_expand(struct command_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i = 0; i < q->cap; i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;

	skynet_free(q->queue);
	q->queue = new_queue;
}

void
command_mq_push(struct command_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q);

	q->queue[q->tail] = *message;
	if (++q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		command_mq_expand(q);
	}

	SPIN_UNLOCK(q);
}

void
command_mq_release(struct command_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while (!command_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	SPIN_DESTROY(q);
	skynet_free(q->queue);
	skynet_free(q);
}

struct socket_server {
	volatile uint64_t time;
	uv_loop_t *loop;
	int alloc_id;
	struct socket_object_interface soi;
	struct socket slot[MAX_SOCKET];
	char buffer[MAX_INFO];
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	uv_async_t cmd_req;
	struct command_queue *cmd_queue;
	socket_message_cb cb;
};

struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	int sz;
	char * buffer;
};

struct request_send_udp {
	struct request_send send;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int backlog;
	uintptr_t opaque;
	int family;
	struct sockaddr addr;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
	Q query info
 */

struct request_package {
	uint8_t header[8];	// 6 bytes dummy
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {
	struct spinlock *lock;
	int count;
};

static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
	sl->lock = &s->dw_lock;
	sl->count = 0;
}

static inline void
socket_lock(struct socket_lock *sl) {
	if (sl->count == 0) {
		spinlock_lock(sl->lock);
	}
	++sl->count;
}

static inline int
socket_trylock(struct socket_lock *sl) {
	if (sl->count == 0) {
		if (!spinlock_trylock(sl->lock))
			return 0;	// lock failed
	}
	++sl->count;
	return 1;
}

static inline void
socket_unlock(struct socket_lock *sl) {
	--sl->count;
	if (sl->count <= 0) {
		assert(sl->count == 0);
		spinlock_unlock(sl->lock);
	}
}

static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

static inline struct socket *
socket_from_handle(uv_handle_t *h) {
	return ((struct socket *)((char *)(h)-(size_t)&(((struct socket *)0)->s)));
}

static inline void
close_cb(uv_handle_t *h) {
	if (h->type == UV_TCP || h->type == UV_UDP || h->type == UV_TTY) {
		struct socket *s = socket_from_handle(h);
		s->type = SOCKET_TYPE_INVALID;
	}
}

static inline void
alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	buf->base = (char *)MALLOC(suggested_size);
	buf->len = suggested_size;
}

static inline void
read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	struct socket_server *ss = (struct socket_server *)stream->data;
	struct socket *s = socket_from_handle((uv_handle_t *)stream);
	struct socket_message result;

	if (nread > 0) {
		result.opaque = s->opaque;
		result.id = s->id;
		result.ud = nread;
		result.data = buf->base;
		ss->cb((stream->type != UV_UDP ? SOCKET_DATA : SOCKET_UDP), &result);
	} else {
		FREE(buf->base);
		if (nread < 0) {
			struct socket_lock l;
			socket_lock_init(s, &l);
			force_close(ss, s, &l, &result);
			ss->cb(SOCKET_ERROR, &result);
		}
	}
}

static void
connect_cb_(uv_connect_t* req, int status) {
	struct socket *s = (struct socket *)req->data;
	struct socket_server *ss = (struct socket_server *)s->s.tcp.data;
	struct socket_message result;

	if (status != 0 || uv_read_start((uv_stream_t *)&s->s, alloc_cb, read_cb) != 0) {
		struct socket_lock l;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &result);
		ss->cb(SOCKET_ERROR, &result);
	} else {
		uv_tcp_keepalive(&s->s.tcp, true, false);
		s->type = SOCKET_TYPE_CONNECTED;
		s->s.tcp.data = ss;
		result.opaque = s->opaque;
		result.id = s->id;
		result.ud = 0;
		result.data = NULL;
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (uv_tcp_getpeername(&s->s.tcp, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (uv_inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result.data = ss->buffer;
			}
		}
		ss->cb(SOCKET_OPEN, &result);
	}
	FREE(req);
}

static inline void
write_cb_(uv_write_t* req, int status) {
	struct wb_list *list = (struct wb_list *)req->data;
	struct socket *s = socket_from_handle((uv_handle_t *)req->handle);
	struct socket_server *ss = (struct socket_server *)req->handle->data;
	struct socket_message result;
	s->write = false;
	FREE(req);

	if (status == 0) {
		struct write_buffer * tmp = list->head;
		list->head = tmp->next;
		write_buffer_free(ss, tmp);
		if (send_list_tcp(ss, s, list, NULL, &result) == -1) {
			return;
		}
	}
	ss->cb(SOCKET_CLOSE, &result);
}

static inline void
udp_send_cb(uv_udp_send_t* req, int status) {
	struct wb_list *list = (struct wb_list *)req->data;
	struct socket *s = socket_from_handle((uv_handle_t *)req->handle);
	struct socket_server *ss = (struct socket_server *)req->handle->data;
	struct socket_message result;

	if (status == 0) {
		struct write_buffer * tmp = list->head;
		list->head = tmp->next;
		write_buffer_free(ss, tmp);
		if (send_list_udp(ss, s, list, &result) == -1) {
			return;
		}
	}
	// ignore udp sendto error
}

static inline void
connection_cb(uv_stream_t* server, int status) {
	struct socket_server *ss = (struct socket_server *)server->data;
	struct socket *s = socket_from_handle((uv_handle_t *)server);
	struct socket_message result;
	if (status == 0) {
		if (report_accept(ss, s, &result)) {
			ss->cb(SOCKET_ACCEPT, &result);
		}
	} else {
		struct socket_lock l;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &result);
		ss->cb(SOCKET_ERROR, &result);
	}
}

static inline void
free_request_package(struct skynet_message *msg, void *u) {
	struct request_package *r = (struct request_package *)msg->data;
	FREE(r);
}

static inline void
cmd_cb(uv_async_t* handle) {
	struct socket_message result;
	struct skynet_message msg;
	struct socket_server *ss = (struct socket_server *)handle->data;
	while (command_mq_pop(ss->cmd_queue, &msg) == 0) {
		struct request_package *r = (struct request_package *)msg.data;
		int ret = ctrl_cmd(ss, r->header[6], r->header[7], r->u.buffer, &result);
		free_request_package(&msg, NULL);
		if (ret != -1) {
			ss->cb(ret, &result);
			break;
		}
	}
}

static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_INC(&(ss->alloc_id));
		if (id < 0) {
			id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);
		}
		struct socket *s = &ss->slot[HASH_ID(id)];
		if (s->type == SOCKET_TYPE_INVALID) {
			if (ATOM_CAS16(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->protocol = PROTOCOL_UNKNOWN;
				// socket_server_udp_connect may inc s->udpconncting directly (from other thread, before new_fd), 
				// so reset it to 0 here rather than in new_fd.
				s->udpconnecting = 0;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

struct socket_server * 
socket_server_create(uint64_t time, socket_message_cb cb) {
	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->time = time;
	ss->loop = uv_loop_new();

	for (int i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
		spinlock_init(&s->dw_lock);
	}
	ss->alloc_id = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));
	ss->cb = cb;
	uv_async_init(ss->loop, &ss->cmd_req, cmd_cb);
	ss->cmd_req.data = ss;
	ss->cmd_queue = command_mq_create(0);
	return ss;
}

void
socket_server_updatetime(struct socket_server *ss, uint64_t time) {
	ss->time = time;
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

static void
free_buffer(struct socket_server *ss, const void * buffer, int sz) {
	struct send_object so;
	send_object_init(ss, &so, (void *)buffer, sz);
	so.free_func((void *)buffer);
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);
	socket_lock(l);
	uv_close((uv_handle_t *)&s->s, close_cb);
	s->type = SOCKET_TYPE_INVALID;
	if (s->dw_buffer) {
		free_buffer(ss, s->dw_buffer, s->dw_size);
		s->dw_buffer = NULL;
	}
	socket_unlock(l);
}

void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		struct socket_lock l;
		socket_lock_init(s, &l);
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s, &l, &dummy);
		}
		spinlock_destroy(&s->dw_lock);
	}
	uv_close((uv_handle_t *)&ss->cmd_req, close_cb);
	uv_run(ss->loop, UV_RUN_DEFAULT); // 等待事件循环自己退出
	uv_loop_delete(ss->loop);
	command_mq_release(ss->cmd_queue, free_request_package, NULL);
	FREE(ss);
}

static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (protocol == PROTOCOL_TCP) {
		if (_fileno(stdin) == fd || _fileno(stdout) == fd || _fileno(stderr) == fd) {
			if (uv_tty_init(ss->loop, &s->s.tty, fd, (_fileno(stdin) == fd)) != 0) {
				uv_close((uv_handle_t *)&s->s, close_cb);
				return NULL;
			}
		} else {
			uv_tcp_init(ss->loop, &s->s.tcp);
			if (fd != -1 && uv_tcp_open(&s->s.tcp, fd) != 0) {
				uv_close((uv_handle_t *)&s->s, close_cb);
				return NULL;
			}
		}
		s->s.tcp.data = ss;

		if (add) {
			if (uv_read_start((uv_stream_t *)&s->s, alloc_cb, read_cb) != 0) {
				uv_close((uv_handle_t *)&s->s, close_cb);;
				return NULL;
			}
		}
	} else if (protocol == PROTOCOL_UDP || protocol == PROTOCOL_UDPv6) {
		uv_udp_init(ss->loop, &s->s.udp);
		if (fd != -1) {
			uv_udp_open(&s->s.udp, fd);
		}
		s->s.udp.data = ss;
		if (add) {
			// if (uv_udp_recv_start(&s->s.udp, alloc_cb, ) != 0) {
			// 	s->type = SOCKET_TYPE_INVALID;
			// 	return NULL;
			// }
		}
	}

	s->id = id;
	s->sending = ID_TAG16(id) << 16 | 0;
	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	s->warn_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	s->dw_buffer = NULL;
	s->dw_size = 0;
	memset(&s->stat, 0, sizeof(s->stat));
	return s;
}

static inline void
stat_read(struct socket_server *ss, struct socket *s, int n) {
	s->stat.read += n;
	s->stat.rtime = ss->time;
}

static inline void
stat_write(struct socket_server *ss, struct socket *s, int n) {
	s->stat.write += n;
	s->stat.wtime = ss->time;
}

// return -1 when connecting
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	struct sockaddr_in addr;
	int err = uv_ip4_addr(request->host, request->port, &addr);
	if (err != 0) {
		goto _failed;
	}

	struct socket *ns = new_fd(ss, id, -1, PROTOCOL_TCP, request->opaque, false);
	if (ns == NULL) {
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	ns->type = SOCKET_TYPE_CONNECTING;
	uv_connect_t *req = (uv_connect_t *)MALLOC(sizeof(uv_connect_t));
	req->data = ns;
	int status = uv_tcp_connect(req, &ns->s.tcp, (struct sockaddr *)&addr, connect_cb_);
	if (status != 0) {
		FREE(req);
		goto _failed;
	}
	return -1;
_failed:
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERR;
}

#define MAX_SEND_BUF 1 // 这里暂时只设置为1，因为write_cb_里不知道该怎么得到这一次发送的总buf数
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	if (list->head != NULL) {
		uv_buf_t buf[MAX_SEND_BUF];
		struct write_buffer * tmp = list->head;
		int i = 0;
		for (; tmp != NULL && i < MAX_SEND_BUF; ++i, tmp = tmp->next) {
			buf[i] = uv_buf_init(tmp->ptr, tmp->sz);
		}
		uv_write_t *req = (uv_write_t *)MALLOC(sizeof(uv_write_t));
		req->data = list;
		if (uv_write(req, (uv_stream_t *)&s->s.tcp, buf, i, &write_cb_) != 0) {
			FREE(req);
			return SOCKET_CLOSE;
		}
		s->write = true;
	}

	return -1;
}

static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

static void
drop_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct write_buffer *tmp) {
	s->wb_size -= tmp->sz;
	list->head = tmp->next;
	if (list->head == NULL)
		list->tail = NULL;
	write_buffer_free(ss,tmp);
}

static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	if (list->head != NULL) {
		struct write_buffer * tmp = list->head;
		uv_buf_t buf = uv_buf_init(tmp->ptr, tmp->sz);
		uv_udp_send_t *req = (uv_udp_send_t *)MALLOC(sizeof(uv_udp_send_t));
		req->data = list;
		union sockaddr_all sa;
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		if (uv_udp_send(req, &s->s.udp, &buf, 1, &sa.s, &udp_send_cb) != 0) {
			FREE(req);
			return SOCKET_CLOSE;
		}
	}

	return -1;
}

static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, l, result);
	} else {
		return send_list_udp(ss, s, list, result);
	}
}

static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}

static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer_(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));
	// step 1
	if (send_list(ss,s,&s->high,l,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,l,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}
			// step 3
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
				return -1;
			}
			if (s->low.head)
				return -1;
		} 
		// step 4
		assert(send_buffer_empty(s) && s->wb_size == 0);
		// TODO
		// sp_write(ss->event_fd, s->fd, s, false);			

		if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, l, result);
				return SOCKET_CLOSE;
		}
		if(s->warn_size > 0){
				s->warn_size = 0;
				result->opaque = s->opaque;
				result->id = s->id;
				result->ud = 0;
				result->data = NULL;
				return SOCKET_WARNING;
		}
	}

	return -1;
}

static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	if (!socket_trylock(l))
		return -1;	// blocked by direct write, send later.
	if (s->dw_buffer) {
		// add direct write buffer before high.head
		struct write_buffer * buf = MALLOC(SIZEOF_TCPBUFFER);
		struct send_object so;
		buf->userobject = send_object_init(ss, &so, (void *)s->dw_buffer, s->dw_size);
		buf->ptr = (char*)so.buffer+s->dw_offset;
		buf->sz = so.sz - s->dw_offset;
		buf->buffer = (void *)s->dw_buffer;
		s->wb_size+=buf->sz;
		if (s->high.head == NULL) {
			s->high.head = s->high.tail = buf;
			buf->next = NULL;
		} else {
			buf->next = s->high.head;
			s->high.head = buf;
		}
		s->dw_buffer = NULL;
	}
	int r = send_buffer_(ss,s,l,result);
	socket_unlock(l);

	return r;
}

static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	buf->ptr = (char*)so.buffer;
	buf->sz = so.sz;
	buf->buffer = request->buffer;
	buf->next = NULL;
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER);
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	s->wb_size += buf->sz;
}

static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}

static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}


/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		so.free_func(request->buffer);
		return -1;
	}
	if (s->type == SOCKET_TYPE_PLISTEN || s->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->buffer);
		return -1;
	}
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		if (s->protocol == PROTOCOL_TCP) {
			append_sendbuffer(ss, s, request);	// add to high priority list, even priority == PRIORITY_LOW
		} else {
			// udp
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			if (sasz == 0) {
				// udp type mismatch, just drop it.
				fprintf(stderr, "socket-server: udp socket (%d) type mistach.\n", id);
				so.free_func(request->buffer);
				return -1;
			}
			int n = sendto(s->s.udp.socket, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n != so.sz) {
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
				stat_write(ss,s,n);
				so.free_func(request->buffer);
				return -1;
			}
		}
		// TODO
		// sp_write(ss->event_fd, s->fd, s, true);
	} else {
		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);
		}
	}
	if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
		s->warn_size = s->warn_size == 0 ? WARNING_SIZE *2 : s->warn_size*2;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = (int)(s->wb_size%1024 == 0 ? s->wb_size/1024 : s->wb_size/1024 + 1);
		result->data = NULL;
		return SOCKET_WARNING;
	}
	return -1;
}

static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = new_fd(ss, id, -1, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	if (uv_tcp_bind(&s->s.tcp, &request->addr, 0) != 0) {
		uv_close((uv_handle_t *)&s->s.tcp, close_cb);
		return SOCKET_ERROR;
	}
	s->s.tcp.data = (void *)request->backlog; // 把backlog存在data里面
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = "reach skynet socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERR;
}

static inline int
nomore_sending_data(struct socket *s) {
	return send_buffer_empty(s) && s->dw_buffer == NULL && (s->sending & 0xffff) == 0;
}

static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	if (!nomore_sending_data(s)) {
		int type = send_buffer(ss,s,&l,result);
		// type : -1 or SOCKET_WARNING or SOCKET_CLOSE, SOCKET_WARNING means nomore_sending_data
		if (type != -1 && type != SOCKET_WARNING)
			return type;
	}
	if (request->shutdown || nomore_sending_data(s)) {
		force_close(ss,s,&l,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERR;
	}
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		result->data = "invalid socket";
		return SOCKET_ERR;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	if (s->type == SOCKET_TYPE_PACCEPT) {
		if (uv_read_start((uv_stream_t *)&s->s, alloc_cb, read_cb) != 0) {
			force_close(ss, s, &l, result);
			return SOCKET_ERROR;
		}
		s->s.tcp.data = ss;
		s->type = SOCKET_TYPE_CONNECTED;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_PLISTEN) {
		int backlog = (int)s->s.tcp.data;
		if (uv_listen((uv_stream_t *)&s->s, backlog, connection_cb) != 0) {
			force_close(ss, s, &l, result);
			return SOCKET_ERROR;
		}
		s->s.tcp.data = ss;
		s->type = SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		// todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	// if s->type == SOCKET_TYPE_HALFCLOSE , SOCKET_CLOSE message will send later
	return -1;
}

static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;
	setsockopt(s->s.tcp.socket, IPPROTO_TCP, request->what, (char *)&v, sizeof(v));
}

static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		closesocket(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = "protocol mismatch";

		return SOCKET_ERR;
	}
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	ATOM_DEC16(&s->udpconnecting);
	return -1;
}

static inline void
inc_sending_ref(struct socket *s, int id) {
	if (s->protocol != PROTOCOL_TCP)
		return;
	for (;;) {
		uint32_t sending = s->sending;
		if ((sending >> 16) == ID_TAG16(id)) {
			if ((sending & 0xffff) == 0xffff) {
				// s->sending may overflow (rarely), so busy waiting here for socket thread dec it. see issue #794
				continue;
			}
			// inc sending only matching the same socket id
			if (ATOM_CAS(&s->sending, sending, sending + 1))
				return;
			// atom inc failed, retry
		} else {
			// socket id changed, just return
			return;
		}
	}
}

static inline void
dec_sending_ref(struct socket_server *ss, int id) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	// Notice: udp may inc sending while type == SOCKET_TYPE_RESERVE
	if (s->id == id && s->protocol == PROTOCOL_TCP) {
		assert((s->sending & 0xffff) != 0);
		ATOM_DEC(&s->sending);
	}
}

// return type
static int
ctrl_cmd(struct socket_server *ss, int type, int len, char *buffer, struct socket_message *result) {
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
	case 'P': {
		int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
		struct request_send * request = (struct request_send *) buffer;
		int ret = send_socket(ss, request, result, priority, NULL);
		dec_sending_ref(ss, request->id);
		return ret;
	}
	case 'A': {
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':
		return set_udp_address(ss, (struct request_setudp *)buffer, result);
	case 'T':
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	case 'U':
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

static int
getname(union sockaddr_all *u, char *buffer, size_t sz) {
	char tmp[INET6_ADDRSTRLEN];
	void * sin_addr = (u->s.sa_family == AF_INET) ? (void*)&u->v4.sin_addr : (void *)&u->v6.sin6_addr;
	int sin_port = ntohs((u->s.sa_family == AF_INET) ? u->v4.sin_port : u->v6.sin6_port);
	if (inet_ntop(u->s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(buffer, sz, "%s:%d", tmp, sin_port);
		return 1;
	} else {
		buffer[0] = '\0';
		return 0;
	}
}

// return 0 when failed, or -1 when file limit
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);

	int id = reserve_id(ss);
	if (id < 0) {
		return 0;
	}
	struct socket *ns = new_fd(ss, id, -1, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		return 0;
	}

	if (uv_accept((uv_stream_t *)&s->s.tcp, (uv_stream_t *)&ns->s.tcp) != 0) {
		uv_close((uv_handle_t *)&ns->s.tcp, close_cb);
		return 0;
	}

	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	uv_tcp_getpeername(&ns->s.tcp, &u.s, &len);
	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (uv_inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp)) == 0) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);
		result->data = ss->buffer;
	}
	return 1;
}

// return type
int 
socket_server_poll(struct socket_server *ss) {
	return uv_run(ss->loop, UV_RUN_ONCE);
}

static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	struct skynet_message msg;
	msg.data = MALLOC(sizeof(*request));
	memcpy(msg.data, request, sizeof(*request));
	msg.sz = sizeof(*request);
	command_mq_push(ss->cmd_queue, &msg);
	uv_async_send(&ss->cmd_req);
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) >= 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

static inline int
can_direct_write(struct socket *s, int id) {
	return s->id == id && nomore_sending_data(s) && s->type == SOCKET_TYPE_CONNECTED && s->udpconnecting == 0;
}

// return -1 when error, 0 when success
int 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);

	if (can_direct_write(s,id) && socket_trylock(&l)) {
		// may be we can send directly, double check
		if (can_direct_write(s,id)) {
			// send directly
			struct send_object so;
			send_object_init(ss, &so, (void *)buffer, sz);
			ssize_t n;
			if (s->protocol == PROTOCOL_TCP) {
				n = send(s->s.tcp.socket, so.buffer, so.sz, 0);
			} else {
				union sockaddr_all sa;
				socklen_t sasz = udp_socket_address(s, s->p.udp_address, &sa);
				if (sasz == 0) {
					fprintf(stderr, "socket-server : set udp (%d) address first.\n", id);
					socket_unlock(&l);
					so.free_func((void *)buffer);
					return -1;
				}
				n = sendto(s->s.udp.socket, so.buffer, so.sz, 0, &sa.s, sasz);
			}
			if (n<0) {
				// ignore error, let socket thread try again
				n = 0;
			}
			stat_write(ss,s,n);
			if (n == so.sz) {
				// write done
				socket_unlock(&l);
				so.free_func((void *)buffer);
				return 0;
			}
			// write failed, put buffer into s->dw_* , and let socket thread send it. see send_buffer()
			s->dw_buffer = buffer;
			s->dw_size = sz;
			s->dw_offset = n;

			// TODO
			// sp_write(ss->event_fd, s->fd, s, true);

			socket_unlock(&l);
			return 0;
		}
		socket_unlock(&l);
	}

	inc_sending_ref(s, id);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return 0;
}

// return -1 when error, 0 when success
int 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	inc_sending_ref(s, id);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'P', sizeof(request.u.send));
	return 0;
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 0;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}


void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

static int
do_getaddr(const char *host, int port, int protocol, int *family, struct sockaddr *addr) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	snprintf(portstr, sizeof(portstr), "%d", port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
	if (status != 0) {
		return -1;
	}
	*addr = *ai_list->ai_addr;
	freeaddrinfo(ai_list);
	return 0;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	struct request_package request;
	if (do_getaddr(addr, port, IPPROTO_TCP, &request.u.listen.family, &request.u.listen.addr) != 0) {
		return -1;
	}
	int id = reserve_id(ss);
	if (id < 0) {
		return id;
	}
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.backlog = backlog;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP

int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		// bind
		fd = -1;
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}

	int id = reserve_id(ss);
	if (id < 0) {
		closesocket(fd);
		return -1;
	}
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

int 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);

	if (can_direct_write(s,id) && socket_trylock(&l)) {
		// may be we can send directly, double check
		if (can_direct_write(s,id)) {
			// send directly
			struct send_object so;
			send_object_init(ss, &so, (void *)buffer, sz);
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			if (sasz == 0) {
				socket_unlock(&l);
				so.free_func((void *)buffer);
				return -1;
			}
			int n = sendto(s->s.udp.socket, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n >= 0) {
				// sendto succ
				stat_write(ss,s,n);
				socket_unlock(&l);
				so.free_func((void *)buffer);
				return 0;
			}
		}
		socket_unlock(&l);
		// let socket thread try again, udp doesn't care the order
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	memcpy(request.u.send_udp.address, udp_address, addrsz);

	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return 0;
}

static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address + addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address + addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address + addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address + addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	socket_lock(&l);
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		socket_unlock(&l);
		return -1;
	}
	ATOM_INC16(&s->udpconnecting);
	socket_unlock(&l);

	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}

	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}


struct socket_info *
socket_info_create(struct socket_info *last) {
	struct socket_info *si = skynet_malloc(sizeof(*si));
	memset(si, 0 , sizeof(*si));
	si->next = last;
	return si;
}

void
socket_info_release(struct socket_info *si) {
	while (si) {
		struct socket_info *temp = si;
		si = si->next;
		skynet_free(temp);
	}
}

static int
query_info(struct socket *s, struct socket_info *si) {
	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	switch (s->type) {
	case SOCKET_TYPE_BIND:
		si->type = SOCKET_INFO_BIND;
		si->name[0] = '\0';
		break;
	case SOCKET_TYPE_LISTEN:
		si->type = SOCKET_INFO_LISTEN;
		if (getsockname(s->s.tcp.socket, &u.s, &slen) == 0) {
			getname(&u, si->name, sizeof(si->name));
		}
		break;
	case SOCKET_TYPE_CONNECTED:
		if (s->protocol == PROTOCOL_TCP) {
			si->type = SOCKET_INFO_TCP;
			if (getpeername(s->s.tcp.socket, &u.s, &slen) == 0) {
				getname(&u, si->name, sizeof(si->name));
			}
		} else {
			si->type = SOCKET_INFO_UDP;
			if (udp_socket_address(s, s->p.udp_address, &u)) {
				getname(&u, si->name, sizeof(si->name));
			}
		}
		break;
	default:
		return 0;
	}
	si->id = s->id;
	si->opaque = (uint64_t)s->opaque;
	si->read = s->stat.read;
	si->write = s->stat.write;
	si->rtime = s->stat.rtime;
	si->wtime = s->stat.wtime;
	si->wbuffer = s->wb_size;

	return 1;
}

struct socket_info *
socket_server_info(struct socket_server *ss) {
	int i;
	struct socket_info * si = NULL;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket * s = &ss->slot[i];
		int id = s->id;
		struct socket_info temp;
		if (query_info(s, &temp) && s->id == id) {
			// socket_server_info may call in different thread, so check socket id again
			si = socket_info_create(si);
			temp.next = si->next;
			*si = temp;
		}
	}
	return si;
}

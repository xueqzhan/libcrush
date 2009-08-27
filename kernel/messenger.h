#ifndef __FS_CEPH_MESSENGER_H
#define __FS_CEPH_MESSENGER_H

#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/radix-tree.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "types.h"
#include "buffer.h"

struct ceph_msg;
struct ceph_connection;

#define IPQUADPORT(n)							\
	(unsigned int)((be32_to_cpu((n).sin_addr.s_addr) >> 24)) & 0xFF, \
	(unsigned int)((be32_to_cpu((n).sin_addr.s_addr)) >> 16) & 0xFF, \
	(unsigned int)((be32_to_cpu((n).sin_addr.s_addr))>>8) & 0xFF, \
	(unsigned int)((be32_to_cpu((n).sin_addr.s_addr))) & 0xFF, \
	(unsigned int)(ntohs((n).sin_port))


extern struct workqueue_struct *ceph_msgr_wq;       /* receive work queue */

/*
 * Ceph defines these callbacks for handling connection events.
 */
struct ceph_connection_operations {
	void (*get)(struct ceph_connection *);
	void (*put)(struct ceph_connection *);

	/* handle an incoming message. */
	void (*dispatch) (struct ceph_connection *con, struct ceph_msg *m);

	/* a remote host as terminated a message exchange session, and messages
	 * we sent (or they tried to send us) may be lost. */
	void (*peer_reset) (struct ceph_connection *con);

	struct ceph_msg * (*alloc_msg) (struct ceph_connection *con,
					struct ceph_msg_header *hdr);
	int (*alloc_middle) (struct ceph_connection *con,
			     struct ceph_msg *msg);
	/* an incoming message has a data payload; tell me what pages I
	 * should read the data into. */
	int (*prepare_pages) (struct ceph_connection *con, struct ceph_msg *m,
			      int want);
};

static inline const char *ceph_name_type_str(int t)
{
	switch (t) {
	case CEPH_ENTITY_TYPE_MON: return "mon";
	case CEPH_ENTITY_TYPE_MDS: return "mds";
	case CEPH_ENTITY_TYPE_OSD: return "osd";
	case CEPH_ENTITY_TYPE_CLIENT: return "client";
	case CEPH_ENTITY_TYPE_ADMIN: return "admin";
	default: return "???";
	}
}

/* use format string %s%d */
#define ENTITY_NAME(n)				   \
	ceph_name_type_str(le32_to_cpu((n).type)), \
		le32_to_cpu((n).num)

struct ceph_messenger {
	void *parent;                    /* normally struct ceph_client * */

	struct ceph_entity_inst inst;    /* my name+address */

	struct page *zero_page;          /* used in certain error cases */

	/*
	 * the global_seq counts connections i (attempt to) initiate
	 * in order to disambiguate certain connect race conditions.
	 */
	u32 global_seq;
	spinlock_t global_seq_lock;
};

/*
 * a single message.  it contains a header (src, dest, message type, etc.),
 * footer (crc values, mainly), a "front" message body, and possibly a
 * data payload (stored in some number of pages).  The page_mutex protects
 * access to the page vector.
 */
struct ceph_msg {
	struct ceph_msg_header hdr;	/* header */
	struct ceph_msg_footer footer;	/* footer */
	struct kvec front;              /* unaligned blobs of message */
	struct ceph_buffer *middle;
	struct mutex page_mutex;
	struct page **pages;            /* data payload.  NOT OWNER. */
	unsigned nr_pages;              /* size of page array */
	struct list_head list_head;
	atomic_t nref;
	bool front_is_vmalloc;
	bool more_to_follow;
	int front_max;

	struct ceph_msg_pool *pool;
};

struct ceph_msg_pos {
	int page, page_pos;  /* which page; offset in page */
	int data_pos;        /* offset in data payload */
	int did_page_crc;    /* true if we've calculated crc for current page */
};

/* ceph connection fault delay defaults, for exponential backoff */
#define BASE_DELAY_INTERVAL	(HZ/2)
#define MAX_DELAY_INTERVAL	(5 * 60 * HZ)

/*
 * ceph_connection state bit flags
 *
 * QUEUED and BUSY are used together to ensure that only a single
 * thread is currently opening, reading or writing data to the socket.
 */
#define LOSSYTX         0  /* we can close channel or drop messages on errors */
#define LOSSYRX         1  /* peer may reset/drop messages */
#define CONNECTING	2
#define KEEPALIVE_PENDING      3
#define WRITE_PENDING	4  /* we have data ready to send */
#define QUEUED          5  /* there is work queued on this connection */
#define BUSY            6  /* work is being done */
#define STANDBY		8  /* no outgoing messages, socket closed.  we keep
			    * the ceph_connection around to maintain shared
			    * state with the peer. */
#define WAIT		9  /* waiting for peer to connect to us (during a
			    * connection race) */
#define CLOSED		10 /* we've closed the connection */
#define SOCK_CLOSED	11 /* socket state changed to closed */
#define REGISTERED      12 /* connection appears in con_tree */

/*
 * A single connection with another host.
 *
 * We maintain a queue of outgoing messages, and some session state to
 * ensure that we can preserve the lossless, ordered delivery of
 * messages in the case of a TCP disconnect.
 */
struct ceph_connection {
	void *private;
	atomic_t nref;

	const struct ceph_connection_operations *ops;

	struct ceph_messenger *msgr;
	struct socket *sock;
	unsigned long state;	/* connection state (see flags above) */
	const char *error_msg;  /* error message, if any */

	struct ceph_entity_addr peer_addr; /* peer address */
	struct ceph_entity_name peer_name; /* peer name */
	u32 connect_seq;      /* identify the most recent connection
				 attempt for this connection, client */
	u32 peer_global_seq;  /* peer's global seq for this connection */

	/* out queue */
	spinlock_t out_queue_lock;   /* protects out_queue, out_sent, out_seq */
	struct list_head out_queue;
	struct list_head out_sent;   /* sending/sent but unacked */
	u32 out_seq;		     /* last message queued for send */
	bool out_keepalive_pending;

	u32 in_seq, in_seq_acked;  /* last message received, acked */

	/* connection negotiation temps */
	char in_banner[CEPH_BANNER_MAX_LEN];
	union {
		struct {  /* outgoing connection */
			struct ceph_msg_connect out_connect;
			struct ceph_msg_connect_reply in_reply;
		};
		struct {  /* incoming */
			struct ceph_msg_connect in_connect;
			struct ceph_msg_connect_reply out_reply;
		};
	};
	struct ceph_entity_addr actual_peer_addr;

	/* message out temps */
	struct ceph_msg *out_msg;        /* sending message (== tail of
					    out_sent) */
	struct ceph_msg_pos out_msg_pos;

	struct kvec out_kvec[6],         /* sending header/footer data */
		*out_kvec_cur;
	int out_kvec_left;   /* kvec's left in out_kvec */
	int out_kvec_bytes;  /* total bytes left */
	int out_more;        /* there is more data after the kvecs */
	__le32 out_temp_ack; /* for writing an ack */

	/* message in temps */
	struct ceph_msg_header in_hdr;
	struct ceph_msg *in_msg;
	struct ceph_msg_pos in_msg_pos;
	u32 in_front_crc, in_middle_crc, in_data_crc;  /* calculated crc */

	char in_tag;         /* protocol control byte */
	int in_base_pos;     /* bytes read */
	__le32 in_temp_ack;  /* for reading an ack */

	struct delayed_work work;	    /* send|recv work */
	unsigned long       delay;          /* current delay interval */
};

extern int ceph_msgr_init(void);
extern void ceph_msgr_exit(void);

extern struct ceph_messenger *ceph_messenger_create(
	struct ceph_entity_addr *myaddr);
extern void ceph_messenger_destroy(struct ceph_messenger *);

extern void ceph_con_init(struct ceph_messenger *msgr,
			  struct ceph_connection *con,
			  struct ceph_entity_addr *addr);
extern void ceph_con_destroy(struct ceph_connection *con);
extern void ceph_con_send(struct ceph_connection *con, struct ceph_msg *msg);
extern void ceph_con_keepalive(struct ceph_connection *con);
extern void ceph_con_close(struct ceph_connection *con);
extern void ceph_con_get(struct ceph_connection *con);
extern void ceph_con_put(struct ceph_connection *con);

extern struct ceph_msg *ceph_msg_new(int type, int front_len,
				     int page_len, int page_off,
				     struct page **pages);
extern void ceph_msg_kfree(struct ceph_msg *m);

extern struct ceph_msg *ceph_alloc_msg(struct ceph_connection *con,
				       struct ceph_msg_header *hdr);
extern int ceph_alloc_middle(struct ceph_connection *con, struct ceph_msg *msg);


static inline struct ceph_msg *ceph_msg_get(struct ceph_msg *msg)
{
	atomic_inc(&msg->nref);
	return msg;
}
extern void ceph_msg_put(struct ceph_msg *msg);

static inline void ceph_msg_remove(struct ceph_msg *msg)
{
	list_del_init(&msg->list_head);
	ceph_msg_put(msg);
}
static inline void ceph_msg_put_list(struct list_head *head)
{
	while (!list_empty(head)) {
		struct ceph_msg *msg = list_first_entry(head, struct ceph_msg,
							list_head);
		ceph_msg_remove(msg);
	}
}

extern struct ceph_msg *ceph_msg_maybe_dup(struct ceph_msg *msg);

#endif

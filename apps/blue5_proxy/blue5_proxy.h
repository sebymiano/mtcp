#ifndef BLUE5_PROXY_BLUE5_PROXY_H
#define BLUE5_PROXY_BLUE5_PROXY_H

#define DEBUG_INFO 0
#define DEBUG_TRACE 0
#define USE_MTCP 0

#define MAX_PROXY_NUM          1

#if DEBUG_INFO
#define TRACE_APP_CUSTOM(f, m...) {                                         \
	fprintf(stdout, "[%10s:%4d] " f,__FUNCTION__, __LINE__, ##m);    \
    }
#else
#define TRACE_APP_CUSTOM(f, m...) (void)0
#endif

#if DEBUG_TRACE
#define TRACE_APP_CUSTOM_DEBUG(f, m...) {                                         \
	fprintf(stdout, "[%10s:%4d] " f,__FUNCTION__, __LINE__, ##m);    \
    }
#else
#define TRACE_APP_CUSTOM_DEBUG(f, m...)	(void)0
#endif

#include <sys/queue.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/queue.h>
#include <stdint.h>

/*----------------------------------------------------------------------------*/
typedef struct stream_buf {
    int   cnt_refs;			/* number of references by tcp_stream */
    char *data;			    /* payload buffer */
    int   data_len;			/* bytes used in the payload buffer */

    TAILQ_ENTRY (stream_buf) link;

} stream_buf;

typedef struct tcp_stream {
    int sock_id;            /* socket of itself */
    int endpoint_sock;      /* socket to its endpoint */

    uint8_t is_fronted;
    uint8_t write_blocked;

    // TODO: Maybe needed in the future when supporting multiple input bindings
    //struct backend_info* backend;

    stream_buf *rbuf;
    stream_buf *wbuf;

    int64_t bytes_to_write; /* (frontend) bytes to write */

} tcp_stream;

/*----------------------------------------------------------------------------*/

typedef struct thread_context
{
    mctx_t mctx;                 /* mtcp context */
	int cpu;                    /* CPU core number */
    int listener;                /* listener socket */
    int ep;                      /* epoll socket */

    //http_stream *stream;         /* per-socket HTTP stream structure */
    tcp_stream *tcp_streams;

    stream_buf *hbmap;             /* per-stream HTTP buffer structure */
    TAILQ_HEAD (, stream_buf) free_hbmap;  /* list of free HTTP buffers */

//    struct sticky_table sticky_map;  /* sticky table */

} thread_context;

typedef struct backend_info {
    struct sockaddr_in addr;	/* backend address (IPv4) */
    char* name;                 /* backend name */

    uint8_t addr_done:1,
            name_done:1;

} backend_info;

/*----------------------------------------------------------------------------*/
struct proxy_context {
    /* listening address of the frontend server */
    struct sockaddr_in listen_addr;

    /* backend server information */
    struct backend_info backend; /* backend info */

    /* consistent hash node map for backend servers */
    //TAILQ_HEAD (, backend_node) bnode_hmap;

    /* number of persistent backend connections */
    int conn_per_backend;

    /* config parameter */
    uint8_t listen_done:1,
            conn_pool_done:1;

    int backend_num;
};

/*---------------------------------------------------------------------------*/
struct config {
    int proxy_num;
    struct proxy_context pconf[MAX_PROXY_NUM];
} g_conf;

/*---------------------------------------------------------------------------*/
struct proxy_context *g_proxy_ctx;
/*---------------------------------------------------------------------------*/

#endif //BLUE5_PROXY_BLUE5_PROXY_H

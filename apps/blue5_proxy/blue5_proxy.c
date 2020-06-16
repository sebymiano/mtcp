#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "netlib.h"
#include "debug.h"
#include "blue5_proxy.h"
#include "config.h"


#define MAX_FLOW_NUM  (10000)
#define MAX_PORTS 3

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define MAX_BUF_SIZE 8192

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS		16
#endif

/*----------------------------------------------------------------------------*/
static int num_cores;
static int num_cores_used;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;


//void XorPacketPayload(char *buf, int buf_size) {
//    for (int i = 0; i < buf_size; i++) {
//        buf[i] = buf[i] ^ 0x01;
//    }
//}

static inline void
RegisterEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    int ret;

    struct mtcp_epoll_event ev;
    switch (events) {
        case MTCP_EPOLLIN:
            ev.events = MTCP_EPOLLIN;
            break;
        case MTCP_EPOLLOUT:
            ev.events = MTCP_EPOLLOUT;
            break;
        default:
            TRACE_ERROR("This should not happen!\n");
            exit(-1);
    }
    ev.data.sockid = sock;

    ret = mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sock, &ev);

    if (ret < 0 && errno != EEXIST) {
        TRACE_ERROR("epoll_ctl() with EPOLL_CTL_ADD error\n");
        exit(-1);
    }
}

static inline void
ModifyEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    int ret;

    struct mtcp_epoll_event ev;
	switch (events) {
	case MTCP_EPOLLIN:
		ev.events = MTCP_EPOLLIN;
		break;
	case MTCP_EPOLLOUT:
		ev.events = MTCP_EPOLLOUT;
		break;
	default:
		TRACE_ERROR("This should not happen!\n");
		exit(-1);
	}
	ev.data.sockid = sock;

    ret = mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sock, &ev);

    if (ret < 0 && errno != EEXIST) {
        TRACE_ERROR("epoll_ctl() with EPOLL_CTL_MOD error (errno = %d)\n", errno);
        exit(-1);
    }
}

static inline void
UnregisterEvent(struct thread_context *ctx, int sock)
{
    int ret;

    ret = mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sock, NULL);

    if (ret < 0 && errno != EEXIST) {
        TRACE_ERROR("epoll_ctl() with EPOLL_CTL_DEL error\n");
        exit(-1);
    }
}


static void
FreeBuffer(struct thread_context *ctx, struct stream_buf *buf)
{
    if (buf == NULL)
        return;

    /* add it to the free list, only if nobody uses this buffer */
    buf->cnt_refs--;

    if (buf->cnt_refs == 0) {
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, buf, link);
        buf->data_len = 0;
    }
}

/*----------------------------------------------------------------------------*/
void
CloseConnection(struct thread_context *ctx, int sockid) {
    struct tcp_stream* hs = &ctx->tcp_streams[sockid];

    UnregisterEvent(ctx, sockid);

    mtcp_close(ctx->mctx, sockid);

    FreeBuffer(ctx, hs->rbuf);
    FreeBuffer(ctx, hs->wbuf);

    if (hs->endpoint_sock >= 0) {
        ctx->tcp_streams[hs->endpoint_sock].endpoint_sock = -1;
    }
}

/*----------------------------------------------------------------------------*/
int
AcceptConnection(struct thread_context *ctx, int listener) {
    struct tcp_stream *t_stream;
    struct sockaddr addr;
    socklen_t addrlen;
    int c_sock, ret;

    c_sock = mtcp_accept(ctx->mctx, listener, &addr, &addrlen);

    if (c_sock < 0) {
        if (errno == EAGAIN)
            return -1;
        TRACE_ERROR("Failed to accept incoming connection.\n");
        exit(-1);
    }

    if (c_sock >= MAX_FLOW_NUM) {
        TRACE_ERROR("sock id (%d) exceeds the max concurrency (%d).\n",
                    c_sock, MAX_FLOW_NUM);
        exit(-1);
    }

    ret = mtcp_setsock_nonblock(ctx->mctx, c_sock);

    if (ret < 0) {
        TRACE_ERROR("setting socket %d nonblocking returns error\n", c_sock);
    }

    t_stream = &ctx->tcp_streams[c_sock];
    memset(t_stream, 0, sizeof(struct tcp_stream));

    t_stream->sock_id = c_sock;
    t_stream->endpoint_sock = -1;
    t_stream->is_fronted = TRUE;

    RegisterEvent(ctx, c_sock, MTCP_EPOLLIN);

    return c_sock;
}

void CheckOrAllocateFreeBuffer(struct thread_context *ctx, struct tcp_stream *stream) {
    if (stream->rbuf == NULL) {
        stream->rbuf = TAILQ_FIRST(&ctx->free_hbmap);
        if (!stream->rbuf) {
            fprintf(stderr, "alloc from free_hbmap fails\n");
            exit(-1);
        }
        TAILQ_REMOVE(&ctx->free_hbmap, stream->rbuf, link);

        /* (for safety) check if the given buffer is being used or has data */
        if (stream->rbuf->cnt_refs > 0) {
            fprintf(stderr, "(should not happen) there are still some refs.\n");
            exit(-1);
        }
        if (stream->rbuf->data_len > 0) {
            fprintf(stderr, "(should not happen) there are still some data.\n");
            exit(-1);
        }

        /* if there is no peer stream, it is referenced by one TCP stream */
        if (stream->endpoint_sock < 0) {
            stream->rbuf->cnt_refs = 1;
        }

        /* if there is a peer stream, it is referenced by two TCP streams */
        else {
            ctx->tcp_streams[stream->endpoint_sock].wbuf = stream->rbuf;
            stream->rbuf->cnt_refs = 2;
        }
    }

    /* make sure that it has payload buffer which is allocated during init */
    if (!stream->rbuf->data) {
        fprintf(stderr, "hs->rbuf holds a NULL buffer\n");
        exit(-1);
    }
}

/*----------------------------------------------------------------------------*/
static int
CreateBackendConnection(struct thread_context *ctx, int frontend_sock) {
    struct backend_info* backend;
    struct sockaddr_in* backend_addr;
    tcp_stream *backend_stream;
    int backend_fd;
    int ret;

    backend = &g_proxy_ctx->backend;
    backend_addr = &(backend->addr);

    backend_fd = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);

    if (backend_fd < 0) {
        TRACE_ERROR("error when creating a socket");
        return -1;
    }

    if (backend_fd >= MAX_FLOW_NUM) {
        TRACE_ERROR("invalid socket id %d.\n", backend_fd);
        return -1;
    }

    //memset(&ctx->wvars[sockid], 0, sizeof(struct wget_vars));
    ret = mtcp_setsock_nonblock(ctx->mctx, backend_fd);
    if (ret < 0) {
        TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    ret = mtcp_connect(ctx->mctx, backend_fd, (struct sockaddr*) backend_addr,
            sizeof(struct sockaddr_in));

    if (ret < 0 && errno != EINPROGRESS) {
        perror("mtcp_connect");
        mtcp_close(ctx->mctx, backend_fd);
        return -1;
    }

    /* record the socket number of peer HTTP stream */
    ctx->tcp_streams[frontend_sock].endpoint_sock = backend_fd;

    backend_stream = &ctx->tcp_streams[backend_fd];
    memset(backend_stream, 0, sizeof(tcp_stream));
    backend_stream->sock_id = backend_fd;
    backend_stream->endpoint_sock = frontend_sock;

    /* forward from front's read buf to backend write buf */
    backend_stream->wbuf = ctx->tcp_streams[frontend_sock].rbuf;
    ctx->tcp_streams[frontend_sock].rbuf->cnt_refs++;

    backend_stream->write_blocked = TRUE;
    RegisterEvent(ctx, backend_fd, MTCP_EPOLLOUT);

    return ret;
}

static int
WriteAvailData(struct thread_context *ctx, int fd)
{
    tcp_stream *t_stream = &ctx->tcp_streams[fd];
    stream_buf *buff = t_stream->wbuf;
    int res;

    if (buff->data_len < 1 || t_stream->write_blocked == 1) {
        return 0;
    }

    res = mtcp_write(ctx->mctx, fd, buff->data, buff->data_len);

    if (res < 0) {
        /* we might have been full but didn't realize it */
        if (errno == EAGAIN) {
            t_stream->write_blocked = 1;
            ModifyEvent(ctx, fd, MTCP_EPOLLOUT);
            return 0;
        }

        /* error occured while writing to remote host */
        return -1;
    }

    /* if (res > 0) */
    buff->data_len -= res;

    if (t_stream->is_fronted) {
        t_stream->bytes_to_write -= res;

        /* mismatch cases (exit for debugging purposes now) */
        if (t_stream->bytes_to_write < 0 ||
            (t_stream->bytes_to_write == 0 && buff->data_len > 0)) {
            fprintf(stderr, "content-length mismatch (bytes_to_write: %d, data_len: %d)\n",
                    (int) t_stream->bytes_to_write, buff->data_len);
            exit(-1);
        }

        /* finished a HTTP GET, so wait for the next connection */
        if (t_stream->bytes_to_write == 0) {
            if (t_stream->wbuf->data_len > 0 || t_stream->rbuf->data_len > 0) {
                fprintf(stderr, "hs->wbuf->data_len = %d, hs->rbuf->data_len = %d\n",
                        t_stream->wbuf->data_len, t_stream->rbuf->data_len);
                exit(-1);
            }

            /* backend connection is already closed */
            if (t_stream->endpoint_sock < 0) {
                CloseConnection(ctx, fd);
                return 0;
            }

            /* if (hs->peer_sock >= 0) */
            /* backend server may close the connection */
            ModifyEvent(ctx, fd, MTCP_EPOLLIN);
            ModifyEvent(ctx, t_stream->endpoint_sock, MTCP_EPOLLIN);
        }
    }

    /* since we could not write all, assume that it's blocked */
    if (buff->data_len > 0) {
        memmove(buff->data, &buff->data[res], buff->data_len);
        t_stream->write_blocked = 1;
        ModifyEvent(ctx, fd, MTCP_EPOLLOUT);
    }

    return 0;
}

static void HandleReadEvent(struct thread_context *ctx, int fd) {
    tcp_stream *t_stream;
    int space_left, res;

    // if peer is closed, close ourselves
    t_stream = &ctx->tcp_streams[fd];
    // If the backend connection has not been created (yet), the value will be 0
    if (t_stream->endpoint_sock < 0) {
        CloseConnection(ctx, fd);
        return;
    }

    /* if there is no read buffer in this stream, bring one from free list */
    CheckOrAllocateFreeBuffer(ctx, t_stream);

    if ((space_left = MAX_BUF_SIZE - t_stream->rbuf->data_len - 1) <= 0) {
        // Unregister from read event for a while */
        UnregisterEvent(ctx, fd);
        return;
    }

    res = mtcp_read(ctx->mctx, fd, &t_stream->rbuf->data[t_stream->rbuf->data_len], space_left);

    /* when a connection closed by remote host */
    if (res == 0) {
        CloseConnection(ctx, fd);
        if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0) {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
        return;
    }

    /* read is unavailable or an error occured */
    if (res == -1) {
        if (errno != EAGAIN) {
            TRACE_ERROR("mtcp_read() error\n");
            CloseConnection(ctx, fd);
            if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0) {
                CloseConnection(ctx, t_stream->endpoint_sock);
                t_stream->endpoint_sock = -1;
            }
        }
        return;
    }

    /* res > 0 */
    t_stream->rbuf->data_len += res;
    t_stream->rbuf->data[t_stream->rbuf->data_len] = 0;

    if (t_stream->is_fronted) {
        /* so let's connect to the backend server */
        if (t_stream->endpoint_sock < 0) {
            /* case 1: create a new connection (or bring one from pool) */
            if (CreateBackendConnection(ctx, fd) < 0) {
                CloseConnection(ctx, fd);
            }
            return;
        }
        else {	/* t_stream->peer_sock >= 0 */
            /* proceed and write available data (= request) to server */
            /* (you already have a backend connetion, go ahead) */
            ModifyEvent(ctx, t_stream->endpoint_sock, MTCP_EPOLLIN);
        }
    } else {
        assert(t_stream->endpoint_sock >= 0);
    }

    /* try writing available data in the buffer including that we read */
    if (WriteAvailData(ctx, t_stream->endpoint_sock) < 0) {
        TRACE_ERROR("WriteAvailData() error\n");
        /* close both side of HTTP stream */
        CloseConnection(ctx, fd);
        if (t_stream->endpoint_sock >= 0) {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
    }
}

static void
HandleWriteEvent(struct thread_context *ctx, int fd)
{
    tcp_stream *hs = &ctx->tcp_streams[fd];

    /* unblock it and read what it has */
    hs->write_blocked = 0;
    ModifyEvent(ctx, fd, MTCP_EPOLLIN);

    /* enable reading on peer just in case it was off */
    if (hs->endpoint_sock >= 0) {
        RegisterEvent(ctx, hs->endpoint_sock, MTCP_EPOLLIN);
    }

    /* if we have data, write it */
    if (WriteAvailData(ctx, fd) < 0) {
        /* if write fails, close the HTTP stream */
        CloseConnection(ctx, fd);
        if (hs->endpoint_sock >= 0) {
            CloseConnection(ctx, hs->endpoint_sock);
            hs->endpoint_sock = -1;
        }
        return;
    }

    /* if peer is closed and we're done writing, we should close */
    if (hs->endpoint_sock < 0 && hs->wbuf->data_len == 0) {
        CloseConnection(ctx, fd);
    }
}

/*----------------------------------------------------------------------------*/
int
CreateListeningSocket(struct thread_context *ctx) {
    int listener;
    int ret;

    /* create socket and set it as nonblocking */
    listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        TRACE_ERROR("Failed to create listening socket!\n");
        return -1;
    }

    ret = mtcp_setsock_nonblock(ctx->mctx, listener);
    if (ret < 0) {
        TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    ret = mtcp_bind(ctx->mctx, listener,
                    (struct sockaddr *) &(g_proxy_ctx->listen_addr), sizeof(struct sockaddr_in));
    if (ret < 0) {
        TRACE_ERROR("Failed to bind to the listening socket!\n");
        return -1;
    }

    /* listen (backlog: can be configured) */
    ret = mtcp_listen(ctx->mctx, listener, backlog);
    if (ret < 0) {
        TRACE_ERROR("mtcp_listen() failed!\n");
        return -1;
    }

    /* wait for incoming accept events */
    RegisterEvent(ctx, listener, MTCP_EPOLLIN);

    return listener;
}

int initEpollDescriptor(struct thread_context *ctx, struct mtcp_epoll_event **events) {
    ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);

    if (ctx->ep < 0) {
        TRACE_ERROR("Failed to create epoll descriptor!\n");
        return -1;
    }

    *events = (struct mtcp_epoll_event *)
            calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));

    if (!*events) {
        TRACE_ERROR("Failed to create event struct!\n");
        return -1;
    }

    return 0;
}

int initServerVariables(struct thread_context *ctx) {
    ctx->tcp_streams = (struct tcp_stream*)calloc(MAX_FLOW_NUM,
                                              sizeof(struct tcp_stream));
    if (!ctx->tcp_streams) {
        return -1;
    }

    return 0;
}

int initFreeFlowBuffers(struct thread_context *ctx) {
    /* initialize memory pool for flow buffers */
    ctx->hbmap = (stream_buf*) calloc(MAX_FLOW_NUM,
                                    sizeof(struct stream_buf));

    if (!ctx->hbmap) {
        TRACE_ERROR("Failed to allocate memory for flow buffer map.\n");
        return -1;
    }

    for (int i = 0; i < MAX_FLOW_NUM; i++) {
        ctx->hbmap[i].data = (char*) calloc(1, MAX_BUF_SIZE);
        if (!ctx->hbmap[i].data) {
            TRACE_ERROR("Failed to allocate memory for flow buffer.\n");
            return -1;
        }
    }

    TAILQ_INIT(&ctx->free_hbmap);
    for (int i = 0; i < MAX_FLOW_NUM; i++)
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, &ctx->hbmap[i], link);

    return 0;
}

/*----------------------------------------------------------------------------*/
void RunMainLoop(void *arg_ctx) {
    struct thread_context *ctx;
    struct mtcp_epoll_event *events;
    int nevents;
    int i, ret, err;
    socklen_t len = sizeof(err);
    int do_accept;

    ctx = (struct thread_context *) arg_ctx;

    TRACE_APP_CUSTOM("Run application on core %d\n", ctx->cpu);
    struct backend_info *backend = &g_proxy_ctx->backend;
    mtcp_init_rss(ctx->mctx, g_proxy_ctx->listen_addr.sin_addr.s_addr, 1,
            backend->addr.sin_addr.s_addr, backend->addr.sin_port);

    // Create epoll descriptor
    ret = initEpollDescriptor(ctx, &events);
    if (ret < 0) {
        TRACE_APP_CUSTOM("Error while initializing epoll descriptor!\n");
        exit(-1);
    }

    // Allocate memory for server variables
    ret = initServerVariables(ctx);
    if (ret < 0) {
        TRACE_APP_CUSTOM("Failed to create server_vars struct!\n");
        exit(-1);
    }

    // Allocate memory for free flow buffers
    ret = initFreeFlowBuffers(ctx);
    if (ret < 0) {
        TRACE_APP_CUSTOM("Failed to allocate flow buffers!\n");
        exit(-1);
    }

    ctx->listener = CreateListeningSocket(ctx);
    if (ctx->listener < 0) {
        TRACE_ERROR("Failed to create listening socket.\n");
        exit(-1);
    }

    while (1) {
        nevents = mtcp_epoll_wait(ctx->mctx, ctx->ep, events, MAX_EVENTS, 1000);

        if (nevents < 0 && errno != EINTR) {
            if (errno == EPERM)
                break;
            TRACE_ERROR("mtcp_epoll_wait() error\n");
            exit(-1);
        }

        do_accept = FALSE;
        for (i = 0; i < nevents; i++) {
            // if the event is for the listener, accept connection
            if (events[i].data.sockid == ctx->listener) {
                do_accept = TRUE;
                // when read becomes available, handle read event
            } else if (events[i].events & MTCP_EPOLLIN) {
                HandleReadEvent(ctx, events[i].data.sockid);
                // when write becomes available handle write event
            } else if (events[i].events & MTCP_EPOLLOUT) {
                HandleWriteEvent(ctx, events[i].data.sockid);
                // Handling an error on the connection
            } else if (events[i].events & MTCP_EPOLLERR) {
                ret = mtcp_getsockopt(ctx->mctx, events[i].data.sockid,
                                      SOL_SOCKET, SO_ERROR,
                                      (void *) &err, &len);
                if (ret == 0) {
                    if (err == ETIMEDOUT)
                        continue; /* continue for epoll timeout case */
                    else {
                        TRACE_ERROR("epoll error: %s\n", strerror(err));
                        exit(-1);
                    }
                } else {
                    TRACE_ERROR("getsockopt error: %s\n", strerror(errno));
                    exit(-1);  /* for debugging now */
                }
            } else if (events[i].events & MTCP_EPOLLHUP) {
                fprintf(stderr, "MTCP_EPOLLHUP\n");
                exit(-1); /* for debugging now */
            } else if (events[i].events & MTCP_EPOLLRDHUP) {
                fprintf(stderr, "MTCP_EPOLLRDHUP\n");
                exit(-1); /* for debugging now */
            } else {
                /* Unknown epoll flag */
                fprintf(stderr, "unknown epoll flag\n");
                exit(-1);
            }
        }

        // if do_accept flag is set, accept connections
        if (do_accept) {
            while (AcceptConnection(ctx, ctx->listener) >= 0);
        }
    }

    free(ctx->tcp_streams);
    free(events);
}

/*----------------------------------------------------------------------------*/
void *
RunMTCP(void *arg)
{
    int core = *(int *)arg;
    mctx_t mctx;

    /* affinitize CPU cores to mTCP threads */
    mtcp_core_affinitize(core);

    /* initialize mTCP threads */
    if (!(mctx = mtcp_create_context(core))) {
        TRACE_ERROR("Failed to craete mtcp context.\n");
        pthread_exit(NULL);
        return NULL;
    }

    struct thread_context *ctx = (struct thread_context *) calloc(1, sizeof(struct thread_context));
    if (!ctx) {
        TRACE_ERROR("Failed to create thread context!\n");
        exit(-1);
    }

    ctx->cpu = core;
    ctx->mctx = mctx;

    /* run main application loop */
    RunMainLoop((void *)ctx);

    /* destroy mTCP-related contexts after main loop */
    mtcp_destroy_context(ctx->mctx);
    free(ctx);

    pthread_exit(NULL);
    return NULL;
}

/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum) {
    int i;

    for (i = 0; i < core_limit; i++) {
        if (app_thread[i] == pthread_self()) {
            //TRACE_INFO("Server thread %d got SIGINT\n", i);
            done[i] = TRUE;
        } else {
            if (!done[i]) {
                pthread_kill(app_thread[i], signum);
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name) {
    TRACE_CONFIG("%s -p <path_to_www/> -f <mtcp_conf_file> "
                 "[-N num_cores] [-c <per-process core_id>] [-h]\n",
                 prog_name);
    exit(EXIT_SUCCESS);
}

/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv) {
    int ret;
    struct mtcp_conf mcfg;
    int cores[MAX_CPUS];
    int i, o;
    int ports_to_bind[MAX_PORTS];
    int port_num = 0;
    num_cores = GetNumCPUs();

    memset(&ports_to_bind, -1, MAX_PORTS * sizeof(ports_to_bind[0]));

    if (argc < 2) {
        TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
        return FALSE;
    }

    while (-1 != (o = getopt(argc, argv, "N:f:b:h"))) {
        switch (o) {
            case 'N':
                core_limit = mystrtol(optarg, 10);
                if (core_limit > num_cores) {
                    TRACE_CONFIG("CPU limit should be smaller than the "
                                 "number of CPUs: %d\n", num_cores);
                    return FALSE;
                }
                /**
                 * it is important that core limit is set
                 * before mtcp_init() is called. You can
                 * not set core_limit after mtcp_init()
                 */
                mtcp_getconf(&mcfg);
                mcfg.num_cores = core_limit;
                mtcp_setconf(&mcfg);
                break;
            case 'f':
                conf_file = optarg;
                break;
            case 'b':
                backlog = mystrtol(optarg, 10);
                break;
            default:
                printHelp(argv[0]);
                break;
        }
    }

    /* initialize mtcp */
    if (conf_file == NULL) {
        TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
        exit(EXIT_FAILURE);
    }

    ret = mtcp_init(conf_file);
    if (ret) {
        TRACE_CONFIG("Failed to initialize mtcp\n");
        exit(EXIT_FAILURE);
    }

    mtcp_getconf(&mcfg);
    if (backlog > mcfg.max_concurrency) {
        TRACE_CONFIG("backlog can not be set larger than CONFIG.max_concurrency\n");
        return FALSE;
    }

    /* if backlog is not specified, set it to 4K */
    if (backlog == -1) {
        backlog = 4096;
    }

    /* register signal handler to mtcp */
    mtcp_register_signal(SIGINT, SignalHandler);

    TRACE_INFO("Application initialization finished.\n");

    if (core_limit != port_num) {
        TRACE_CONFIG("Number of cores is different than number of ports. This is not supported\n");
        return FALSE;
    }

    /* read epproxy configuration from config/epproxy.yaml */
    g_proxy_ctx = LoadConfigData("config/epproxy.yaml");
    if (!g_proxy_ctx) {
        TRACE_ERROR("LoadConfigData() error\n");
        exit(-1);
    }

    if (g_proxy_ctx->backend_num < 1) {
        TRACE_ERROR("No Available Backend Server.\n");
        exit(-1);
    }

    struct thread_info *t_info = NULL;

    num_cores_used = 0;
    for (i = 0; i < num_cores; i++) {
        cores[i] = i;
        num_cores_used++;
        if (pthread_create(&app_thread[i], NULL, RunMTCP, (void *)&cores[i])) {
            TRACE_ERROR("Failed to create msg_test thread.\n");
            exit(-1);
        }
    }

    for (i = 0; i < num_cores; i++) {
        pthread_join(app_thread[i], NULL);
        TRACE_INFO("Message test thread %d joined.\n", i);
    }

    free(t_info);

    mtcp_destroy();

    return 0;
}

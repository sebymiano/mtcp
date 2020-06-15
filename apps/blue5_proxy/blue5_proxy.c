#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <sys/queue.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"

#define MAX_FLOW_NUM  (10000)
#define MAX_PORTS 3

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define MAX_BUF_SIZE 16384
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 256
#define FULLNAME_LIMIT 512

#define PROXY_SOCK 20

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

#define TRACE_APP_CUSTOM(f, m...)	(void)0

/*----------------------------------------------------------------------------*/
struct client_vars {
    int client_sock;

    LIST_ENTRY(client_vars) entries;
};

struct server_vars {
    uint8_t is_server_socket;
    int sock_id;
    // This is needed for clients to hold the corresponding server socket id
    int endpoint_sock;
    uint8_t endpoint_connected;
    //int clients_num;
    //LIST_HEAD(client_list, client_vars) clients;
};

struct thread_info {
    int core_id;
    int port_num;
};


/*----------------------------------------------------------------------------*/
struct thread_context {
    mctx_t mctx;
    int ep;
    struct server_vars *svars;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;

/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv) {
    sv->is_server_socket = FALSE;
    sv->sock_id = -1;
    sv->endpoint_sock = -1;
    sv->endpoint_connected = FALSE;
}

/*----------------------------------------------------------------------------*/
void
CloseConnection(struct thread_context *ctx, int sockid) {
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
    mtcp_close(ctx->mctx, sockid);
}

//void XorPacketPayload(char *buf, int buf_size) {
//    for (int i = 0; i < buf_size; i++) {
//        buf[i] = buf[i] ^ 0x01;
//    }
//}


/*----------------------------------------------------------------------------*/
static int
ForwardSocketData(struct thread_context *ctx, int src_sock, int dst_sock) {
    struct mtcp_epoll_event ev;
    char buf1[MAX_BUF_SIZE];
    int rd;
    int len;
    int ret, total_sent = 0;

    TRACE_APP_CUSTOM("Reading packets from socket %d\n", src_sock);
    mtcp_setsock_nonblock(ctx->mctx, src_sock);
    /* HTTP request handling */
    rd = mtcp_read(ctx->mctx, src_sock, buf1, MAX_BUF_SIZE);
    if (rd <= 0) {
        return rd;
    }

    //memcpy(buf2, buf1, rd);
    TRACE_APP_CUSTOM("Read %d bytes from socket %d\n", rd, src_sock);

    //XorPacketPayload(buf1, rd);

    ret = 1;
    while (ret > 0) {
        len = MIN(MAX_BUF_SIZE, rd - total_sent);
        if (len <= 0) {
            break;
        }

        TRACE_APP_CUSTOM("Writing buffer to socket %d.\n", dst_sock);
        // Write packet buffer on the other socket
        ret = mtcp_write(ctx->mctx, dst_sock,
                         buf1 + total_sent, len);
        if (ret < 0) {
            TRACE_APP_CUSTOM("Connection closed with client.\n");
            fprintf(stderr, "Error on socket %d: %s\n", dst_sock, strerror(errno));
            return ret;
        }
        total_sent += ret;
    }

    TRACE_APP_CUSTOM("Buffer written.\n");

    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = src_sock;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, src_sock, &ev);

    return rd;
}

/*----------------------------------------------------------------------------*/
int
AcceptConnection(struct thread_context *ctx, int listener) {
    mctx_t mctx = ctx->mctx;
    struct server_vars *sv;
    struct mtcp_epoll_event ev;
    int c;

    c = mtcp_accept(mctx, listener, NULL, NULL);

    if (c >= 0) {
        if (c >= MAX_FLOW_NUM) {
            TRACE_ERROR("Invalid socket id %d.\n", c);
            return -1;
        }

        sv = &ctx->svars[c];
        CleanServerVariable(sv);
        TRACE_APP_CUSTOM("New connection %d accepted.\n", c);
        ev.events = MTCP_EPOLLIN;
        ev.data.sockid = c;
        mtcp_setsock_nonblock(ctx->mctx, c);
        mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);
        TRACE_APP_CUSTOM("Socket %d registered.\n", c);

        sv->is_server_socket = FALSE;
        sv->sock_id = c;
    } else {
        if (errno != EAGAIN) {
            TRACE_ERROR("mtcp_accept() error %s\n",
                        strerror(errno));
        }
    }

    return c;
}

/*----------------------------------------------------------------------------*/
static int
CreateServerConnection(struct thread_context *ctx, int client_socket) {
    mctx_t mctx = ctx->mctx;
    struct mtcp_epoll_event ev;
    struct sockaddr_in addr;
    struct server_vars *sv;
    int sockid;
    int ret;

    sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) {
        TRACE_INFO("Failed to create socket!\n");
        return -1;
    }

    //memset(&ctx->wvars[sockid], 0, sizeof(struct wget_vars));
    ret = mtcp_setsock_nonblock(mctx, sockid);
    if (ret < 0) {
        TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
        exit(-1);
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("10.0.1.2");;
    addr.sin_port = ntohs(8080);

    ret = mtcp_connect(mctx, sockid,
                       (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        if (errno != EINPROGRESS) {
            perror("mtcp_connect");
            mtcp_close(mctx, sockid);
            return -1;
        }
    }

    ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
    ev.data.sockid = sockid;
    mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);

    sv = &ctx->svars[sockid];
    CleanServerVariable(sv);
    //LIST_INIT(&sv->clients);
    sv->is_server_socket = TRUE;
    sv->sock_id = sockid;
    sv->endpoint_sock = client_socket;
    sv->endpoint_connected = TRUE;

    return sockid;
}

/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core) {
    struct thread_context *ctx;

    /* affinitize application thread to a CPU core */
#if HT_SUPPORT
    mtcp_core_affinitize(core + (num_cores / 2));
#else
    mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

    ctx = (struct thread_context *) calloc(1, sizeof(struct thread_context));
    if (!ctx) {
        TRACE_ERROR("Failed to create thread context!\n");
        return NULL;
    }

    /* create mtcp context: this will spawn an mtcp thread */
    ctx->mctx = mtcp_create_context(core);
    if (!ctx->mctx) {
        TRACE_ERROR("Failed to create mtcp context!\n");
        free(ctx);
        return NULL;
    }

    /* create epoll descriptor */
    ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
    if (ctx->ep < 0) {
        mtcp_destroy_context(ctx->mctx);
        free(ctx);
        TRACE_ERROR("Failed to create epoll descriptor!\n");
        return NULL;
    }

    /* allocate memory for server variables */
    ctx->svars = (struct server_vars *)
            calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
    if (!ctx->svars) {
        mtcp_close(ctx->mctx, ctx->ep);
        mtcp_destroy_context(ctx->mctx);
        free(ctx);
        TRACE_ERROR("Failed to create server_vars struct!\n");
        return NULL;
    }

    return ctx;
}

/*----------------------------------------------------------------------------*/
int
CreateListeningSocket(struct thread_context *ctx, struct thread_info *t_info) {
    int listener;
    struct mtcp_epoll_event ev;
    struct sockaddr_in saddr;
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

    /* bind to port 80 */
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(t_info->port_num);
    ret = mtcp_bind(ctx->mctx, listener,
                    (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
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
    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = listener;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

    return listener;
}

/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg) {
    struct thread_info *t_info = (struct thread_info *) arg;
    int core = t_info->core_id;
    //int core = *(int *)arg;
    struct thread_context *ctx;
    mctx_t mctx;
    int listener;
    int ep;
    struct mtcp_epoll_event *events;
    int nevents;
    int i, ret;
    int do_accept;

    TRACE_APP_CUSTOM("Starting server thread!\n");
    /* initialization */
    ctx = InitializeServerThread(core);
    if (!ctx) {
        TRACE_ERROR("Failed to initialize server thread.\n");
        return NULL;
    }
    mctx = ctx->mctx;
    ep = ctx->ep;

    events = (struct mtcp_epoll_event *)
            calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
    if (!events) {
        TRACE_ERROR("Failed to create event struct!\n");
        exit(-1);
    }

    listener = CreateListeningSocket(ctx, t_info);
    if (listener < 0) {
        TRACE_ERROR("Failed to create listening socket.\n");
        exit(-1);
    }

    while (!done[core]) {
        nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
        if (nevents < 0) {
            if (errno != EINTR)
                perror("mtcp_epoll_wait");
            break;
        }

        do_accept = FALSE;
        for (i = 0; i < nevents; i++) {

            if (events[i].data.sockid == listener) {
                /* if the event is for the listener, accept connection */
                do_accept = TRUE;

            } else if (events[i].events & MTCP_EPOLLERR) {
                int err;
                socklen_t len = sizeof(err);

                /* error on the connection */
                TRACE_APP_CUSTOM("[CPU %d] Error on socket %d\n",
                          core, events[i].data.sockid);
                if (mtcp_getsockopt(mctx, events[i].data.sockid,
                                    SOL_SOCKET, SO_ERROR, (void *) &err, &len) == 0) {
                    if (err != ETIMEDOUT) {
                        fprintf(stderr, "Error on socket %d: %s\n",
                                events[i].data.sockid, strerror(err));
                    }
                } else {
                    perror("mtcp_getsockopt");
                }
                CloseConnection(ctx, events[i].data.sockid);

            } else if (events[i].events & MTCP_EPOLLIN) {
                TRACE_APP_CUSTOM("Socket %d is ready for read operations\n", events[i].data.sockid);
                struct server_vars *sv = &ctx->svars[events[i].data.sockid];

                if (sv->endpoint_sock > 0 && sv->endpoint_connected) {
                    // Packets coming from frontend clients, redirect to corresponding server
                    ret = ForwardSocketData(ctx, events[i].data.sockid, sv->endpoint_sock);
                    if (ret == 0) {
                        TRACE_APP_CUSTOM("Connection closed by remote host\n");
                        /* connection closed by remote host */
                        CloseConnection(ctx, events[i].data.sockid);
                        if (sv->endpoint_sock > 0) {
                            CloseConnection(ctx, sv->endpoint_sock);
                        }
                    } else if (ret < 0) {
                        /* if not EAGAIN, it's an error */
                        if (errno != EAGAIN) {
                            CloseConnection(ctx, events[i].data.sockid);
                        }
                    }
                } else {
                    // Wait for the server to establish the connection, do not read the data
                    struct mtcp_epoll_event ev;
                    ev.events = MTCP_EPOLLIN;
                    ev.data.sockid = events[i].data.sockid;
                    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, events[i].data.sockid, &ev);
                }
            } else if (events[i].events & MTCP_EPOLLHUP) {
                //Check if close is requested from server or clients
                struct server_vars *sv = &ctx->svars[events[i].data.sockid];
                if (sv->is_server_socket) {
                    // Close all client sockets as well
                    TRACE_APP_CUSTOM("Close request from socket %d (server), closing all clients' sockets\n",
                              events[i].data.sockid);
//
//			      struct client_vars *n1, *n2;
//			      n1 = LIST_FIRST(&sv->clients);                 /* Faster List Deletion. */
//                  while (n1 != NULL) {
//                      CloseConnection(ctx, n1->client_sock);
//                      n2 = LIST_NEXT(n1, entries);
//                      free(n1);
//                      n1 = n2;
//                  }

                    CloseConnection(ctx, events[i].data.sockid);
                    CloseConnection(ctx, sv->endpoint_sock);
                } else {
                    // Close request from client
                    TRACE_APP_CUSTOM("Close request from socket %d (client)\n", events[i].data.sockid);
                    CloseConnection(ctx, events[i].data.sockid);
                    CloseConnection(ctx, sv->endpoint_sock);
                }

            } else if (events[i].events & MTCP_EPOLLOUT) {
                TRACE_APP_CUSTOM("Socket %d is ready for write operations\n", events[i].data.sockid);
                struct server_vars *sv = &ctx->svars[events[i].data.sockid];
                if (sv->is_server_socket) {
                    ctx->svars[sv->endpoint_sock].endpoint_connected = TRUE;
                }
            } else {
                TRACE_APP_CUSTOM("Received unknown event: %d\n", events[i].events);
                assert(0);
            }
        }

        /* if do_accept flag is set, accept connections */
        if (do_accept) {
            while (1) {
                int client_sock, server_sock;

                client_sock = AcceptConnection(ctx, listener);
                if (client_sock < 0)
                    break;

                TRACE_APP_CUSTOM("New client socket accepted with fd: %d\n", client_sock);

                // Every time a new client request is coming a new connection towards the
                // server is created to handle that client
                server_sock = CreateServerConnection(ctx, client_sock);
                if (server_sock < 0)
                    break;

                TRACE_APP_CUSTOM("Created corresponding server connection with fd: %d\n", server_sock);

                //struct server_vars *sv = &ctx->svars[server_sock];

                //struct client_vars *item = malloc(sizeof(struct client_vars));
                //item->client_sock = client_sock;
                //sv->endpoint_sock = client_sock;
                //LIST_INSERT_HEAD(&sv->clients, item, entries);

                //sv->clients_num += 1;

                // Now, let's also add the server socket to the corresponding client
                struct server_vars *sv = &ctx->svars[client_sock];
                sv->endpoint_sock = server_sock;
            }
        }

    }

    /* destroy mtcp context: this will kill the mtcp thread */
    mtcp_destroy_context(mctx);
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
    int process_cpu;
    int i, o;
    int ports_to_bind[MAX_PORTS];
    int port_num = 0;
    num_cores = GetNumCPUs();
    core_limit = num_cores;
    process_cpu = -1;

    memset(&ports_to_bind, -1, MAX_PORTS * sizeof(ports_to_bind[0]));

    if (argc < 2) {
        TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
        return FALSE;
    }

    while (-1 != (o = getopt(argc, argv, "N:f:c:b:p:h"))) {
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
            case 'p':
                if (port_num < MAX_PORTS) {
                    ports_to_bind[port_num++] = mystrtol(optarg, 10);
                } else {
                    TRACE_CONFIG("Number of ports is way off limits!\n");
                    return FALSE;
                }
                break;
            case 'c':
                process_cpu = mystrtol(optarg, 10);
                if (process_cpu > core_limit) {
                    TRACE_CONFIG("Starting CPU is way off limits!\n");
                    return FALSE;
                }
                break;
            case 'b':
                backlog = mystrtol(optarg, 10);
                break;
            case 'h':
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

    struct thread_info *t_info = NULL;

    for (i = 0; i < port_num; i++) {
        cores[i] = i;
        done[i] = FALSE;

        t_info = (struct thread_info *) calloc(1, sizeof(struct thread_info));
        t_info->core_id = cores[i];
        t_info->port_num = ports_to_bind[i];

        if (pthread_create(&app_thread[i],
                           NULL, RunServerThread, (void *) t_info)) {
            perror("pthread_create");
            TRACE_CONFIG("Failed to create server thread.\n");
            exit(EXIT_FAILURE);
        }
        if (process_cpu != -1)
            break;
    }

    for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
        pthread_join(app_thread[i], NULL);

        if (process_cpu != -1)
            break;
    }

    free(t_info);

    mtcp_destroy();
    return 0;
}

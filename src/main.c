/*
+-----------------------------------------------------------------------------------+
|  httpush                                                                          |
|  Copyright (c) 2010, Mikko Koppanen <mkoppanen@php.net>                           |
|  All rights reserved.                                                             |
+-----------------------------------------------------------------------------------+
|  Redistribution and use in source and binary forms, with or without               |
|  modification, are permitted provided that the following conditions are met:      |
|     * Redistributions of source code must retain the above copyright              |
|       notice, this list of conditions and the following disclaimer.               |
|     * Redistributions in binary form must reproduce the above copyright           |
|       notice, this list of conditions and the following disclaimer in the         |
|       documentation and/or other materials provided with the distribution.        |
|     * Neither the name of the copyright holder nor the                            |
|       names of its contributors may be used to endorse or promote products        |
|       derived from this software without specific prior written permission.       |
+-----------------------------------------------------------------------------------+
|  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND  |
|  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED    |
|  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE           |
|  DISCLAIMED. IN NO EVENT SHALL MIKKO KOPPANEN BE LIABLE FOR ANY                   |
|  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES       |
|  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;     |
|  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND      |
|  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT       |
|  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS    |
|  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                     |
+-----------------------------------------------------------------------------------+
 */

#include "httpush.h"
#include <grp.h>
#include <pwd.h>

/* Indicate that it's time to shut down */
volatile sig_atomic_t shutting_down = 0;

static void hp_show_help(const char *d) {

    fprintf(stderr, "Usage: %s [OPTIONS]\n", d);
    fprintf(stderr, " -b <value>    Hostname or ip to for the HTTP daemon\n");
    fprintf(stderr, " -d            Daemonize the program\n");
    fprintf(stderr, " -g <value>    Group to run as\n");
    fprintf(stderr, " -i <value>    Number of zeromq IO threads\n");
    fprintf(stderr, " -l <value>    Linger value for zeromq sockets\n");
    fprintf(stderr, " -m <value>    Bind dsn for zeromq monitoring socket\n");
    fprintf(stderr, " -o            Optimize for bandwidth usage (exclude headers from messages)\n");
    fprintf(stderr, " -p <value>    HTTP listen port\n");
    fprintf(stderr, " -s <value>    Disk offload size (G/M/k/B)\n");
    fprintf(stderr, " -t <value>    Number of httpd threads\n");
    fprintf(stderr, " -u <value>    User to run as\n");
    fprintf(stderr, " -w <value>    The 0MQ high watermark limit\n");
    fprintf(stderr, " -z <value>    Comma-separated list of zeromq URIs to connect to\n");
}

static void hp_signal_handler(int sig) {

    switch (sig) {
        case SIGHUP:
        case SIGINT:
        case SIGTERM:
            shutting_down = 1;
            break;

        default:
            break;
    }
}

static bool hp_background() {
    pid_t pid, sid;

    HP_LOG_DEBUG("starting the daemonizing process");

    pid = fork();

    if (pid < 0) {
        fprintf(stderr, "Failed to fork: %s", strerror(errno));
        return false;
    }

    /* Exit parent */
    if (pid > 0) {
        return false;
    }

    /* Change the file mode mask */
    umask(0);

    /* Create a new SID for the child process */
    sid = setsid();
    if (sid < 0) {
        fprintf(stderr, "setsid failed: %s", strerror(errno));
        return false;
    }

    (void) freopen("/dev/null", "r", stdin);
    (void) freopen("/dev/null", "w", stdout);
    (void) freopen("/dev/null", "w", stderr);

    return true;
}

static int64_t hp_unit_to_bytes(const char *expression, bool *success) {
    int64_t ret;

    char *end = NULL;
    long converted, factor = 1;

    *success = false;
    converted = strtol(expression, &end, 0);

    /* Failed */
    if (ERANGE == errno || end == expression) {
        return 0;
    }

    if (*end) {
        if (*end == 'G' || *end == 'g') {
            factor = 1024 * 1024 * 1024;
        } else if (*end == 'M' || *end == 'm') {
            factor = 1024 * 1024;
        } else if (*end == 'K' || *end == 'k') {
            factor = 1024;
        } else if (*end == 'B' || *end == 'b') {
            /* Noop */
        } else {
            fprintf(stderr, "Unknown swap size unit '%s'\n", end);
            return 0;
        }
    }
    *success = true;
    ret = (int64_t) converted * factor;
    return ret;
}

static struct hp_uri_t *hp_parse_uri(const char *uri, int64_t default_hwm, uint64_t default_swap) {
    char *pch, *ptr, *last = NULL;
    struct hp_uri_t *retval;

    struct evkeyval *item;
    struct evkeyvalq *q;

    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);

    retval = malloc(sizeof (*retval));
    retval->swap = default_swap;
    retval->hwm = default_hwm;
    retval->linger = 2000;

    ptr = strdup(uri);
    if (!ptr)
        return NULL;

    pch = strtok_r(ptr, "?", &last);
    if (!pch) {
        free(ptr);
        return NULL;
    }

    retval->uri = strdup(pch);
    q = &params;

    TAILQ_FOREACH(item, q, next) {
        if (!strcmp(item->key, "swap")) {
            bool success;
            retval->swap = (int64_t) hp_unit_to_bytes(item->value, &success);

            if (!success) {
                free(ptr);
                evhttp_clear_headers(&params);
                return NULL;
            }

        } else if (!strcmp(item->key, "hwm")) {
            retval->hwm = (uint64_t) atoi(item->value);
        } else if (!strcmp(item->key, "linger")) {
            retval->linger = atoi(item->value);
        }
    }
    free(ptr);
    evhttp_clear_headers(&params);
    return retval;
}

static size_t hp_count_chr(const char *haystack, char needle) {
    size_t occurances = 0;

    while (*haystack != '\0') {
        if (*(haystack++) == needle) {
            occurances++;
        }
    }
    return occurances;
}

static struct hp_uri_t **hp_parse_dsn_param(const char *param, size_t *num, int64_t default_hwm, uint64_t default_swap) {
    size_t num_dsn = 0;
    bool success = true;
    char *tmp, *pch, *last = NULL;
    struct hp_uri_t **retval = NULL;

    *num = 0;

    num_dsn = (hp_count_chr(param, ',') + 1);
    retval = calloc(num_dsn, sizeof (struct hp_uri_t *));

    // calloc failed
    if (!retval) {
        fprintf(stderr, "Failed to allocate memory: %s", strerror(errno));
        return NULL;
    }

    tmp = strdup(param);
    if (!tmp) {
        fprintf(stderr, "Failed to allocate memory: %s", strerror(errno));
        return NULL;
    }

    pch = strtok_r(tmp, ", ", &last);
    if (!pch) {
        free(tmp);
        return NULL;
    }

    while (pch) {
        struct hp_uri_t *uri_ptr;

        uri_ptr = hp_parse_uri(pch, default_hwm, default_swap);
        if (!uri_ptr) {
            success = false;
            break;
        }

        retval[(*num)++] = uri_ptr;
        pch = strtok_r(NULL, ", ", &last);
    }
    free(tmp);

    if (!success && *num > 0) {
        size_t i;
        for (i = 0; i < *num; i++) {
            free(retval[i]);
        }
        free(retval);
        *num = 0;
    }
    return retval;
}

static bool hp_drop_privileges(const char *to_user, const char *to_group) {
    struct passwd *resolved_user = NULL;
    struct group *resolved_group = NULL;

    resolved_group = getgrnam(to_group);
    if (!resolved_group) {
        fprintf(stderr, "Failed to drop privileges. The group(%s) does not exist\n", to_group);
        return false;
    }

    resolved_user = getpwnam(to_user);
    if (!resolved_user) {
        fprintf(stderr, "Failed to drop privileges. The user(%s) does not exist\n", to_user);
        return false;
    }

    if (setegid(resolved_group->gr_gid) != 0) {
        fprintf(stderr, "could not set egid to %s: %s\n", to_group, strerror(errno));
        return false;
    }

    if (seteuid(resolved_user->pw_uid) != 0) {
        fprintf(stderr, "could not set euid to %s: %s\n", to_user, strerror(errno));
        return false;
    }
    return true;
}

static bool hp_change_working_directory() {
    const char *tmp;

    tmp = getenv("TMPDIR");

    if (!tmp) {
        tmp = "/tmp";
    }

    if (chdir(tmp) < 0) {
        return false;
    }
    return true;
}

static int hp_create_listen_socket(const char *ip, const char *port) {
    struct addrinfo *res, hints;
    int rc, sockfd, reuse = 1;

    memset(&hints, 0, sizeof (hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (!ip)
        hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(ip, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(rc));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        fprintf(stderr, "failed to create socket: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    rc = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (int));
    if (rc != 0) {
        fprintf(stderr, "failed to set SO_REUSEADDR: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    rc = bind(sockfd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc != 0) {
        (void) close(sockfd);
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        return -1;
    }

    rc = evutil_make_socket_nonblocking(sockfd);
    if (rc != 0) {
        (void) close(sockfd);
        fprintf(stderr, "fcntl failed: %s\n", strerror(errno));
        return -1;
    }

    rc = listen(sockfd, 1024);
    if (rc == -1) {
        (void) close(sockfd);
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        return -1;
    }
    return sockfd;
}

int main(int argc, char **argv) {
    size_t i;

    /* -- start default values -- */
    const char *monitor_dsn = "tcp://127.0.0.1:5567";

    const char *zmq_dsn = "tcp://127.0.0.1:5555";

    const char *user = "nobody";
    const char *group = "nobody";

    const char *http_host = NULL;
    const char *http_port = "8080";

    uint64_t hwm = 0;
    int64_t swap = 0;

    int io_threads = 1;
    int linger = 2000;
    int http_threads = 5;

    bool daemonize = false;

    /* -- end default values --- */

    int c, rc;
    struct httpush_args_t args;

    args.ctx = NULL;
    args.fd = -1;
    args.include_headers = true;

    opterr = 0;

    while ((c = getopt(argc, argv, "b:dg:i:l:m:op:s:t:u:w:z:")) != -1) {
        switch (c) {

            case 'b':
                http_host = optarg;
                break;

            case 'd':
                daemonize = true;
                break;

            case 'g':
                group = optarg;
                break;

            case 'i':
                io_threads = atoi(optarg);
                if (io_threads < 1) {
                    fprintf(stderr, "Option -i argument must be a positive integer\n");
                    exit(1);
                }
                break;

            case 'l':
                linger = atoi(optarg);

                if (linger < 0) {
                    fprintf(stderr, "Option -l argument must be zero or larger\n");
                    exit(1);
                }
                break;

            case 'm':
                monitor_dsn = optarg;
                break;

            case 'o':
                args.include_headers = false;
                break;

            case 'p':
                http_port = optarg;
                break;

            case 's':
            {
                bool success;
                swap = hp_unit_to_bytes(optarg, &success);
                if (!success) {
                    fprintf(stderr, "Failed to set swap size\n");
                    exit(1);
                }
            }
                break;

            case 't':
                http_threads = atoi(optarg);

                if (http_threads < 1) {
                    fprintf(stderr, "Option -l argument must be a positive integer");
                    exit(1);
                }

                break;

            case 'u':
                user = optarg;
                break;

            case 'w':
                hwm = (uint64_t) atoi(optarg);
                break;

            case 'z':
                zmq_dsn = optarg;
                break;

            case '?':
                if (optopt == 'b' || optopt == 'g' || optopt == 'i' || optopt == 'l' ||
                        optopt == 'p' || optopt == 's' || optopt == 't' || optopt == 'u' ||
                        optopt == 'w' || optopt == 'z') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                }
                hp_show_help(argv[0]);
                exit(1);
                break;

            default:
                hp_show_help(argv[0]);
                exit(1);
                break;
        }
    }

    args.fd = hp_create_listen_socket(http_host, http_port);
    if (args.fd == -1) {
        exit(1);
    }

    if (hp_drop_privileges(user, group) == false) {
        fprintf(stderr, "hp_drop_privileges failed\n");
        exit(1);
    }

    args.uris = hp_parse_dsn_param(zmq_dsn, &(args.num_uris), hwm, swap);
    if (!args.uris) {
        fprintf(stderr, "hp_parse_dsn_param failed for backend uris\n");
        exit(1);
    }

    args.m_uris = hp_parse_dsn_param(monitor_dsn, &(args.num_m_uris), hwm, swap);
    if (!args.m_uris) {
        fprintf(stderr, "hp_parse_dsn_param failed for monitor uris\n");
        exit(1);
    }

    HP_LOG_INFO("HTTP listen: %s:%s", (http_host ? http_host : "0.0.0.0"), http_port);

    if (daemonize) {
        HP_LOG_DEBUG("Launching into background..");
    }

    signal(SIGHUP, hp_signal_handler);
    signal(SIGTERM, hp_signal_handler);
    signal(SIGINT, hp_signal_handler);
    signal(SIGQUIT, hp_signal_handler);

    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    if (daemonize) {
        if (hp_background() == false) {
            exit(1);
        }
    }

    /* Change the current working directory */
    if (hp_change_working_directory() == false) {
        HP_LOG_ERROR("Failed to change directory: %s", strerror(errno));
        exit(1);
    }

    /* Initialize the 0MQ context after fork */
    args.ctx = zmq_init(io_threads);

    if (!args.ctx) {
        HP_LOG_ERROR("Failed to initialize zmq context: %s", zmq_strerror(errno));
        exit(1);
    }

    /* This call will block */
    rc = hp_server_boostrap(&args, http_threads);

    for (i = 0; i < args.num_uris; i++) {
        free(args.uris[i]->uri);
        free(args.uris[i]);
    }
    free(args.uris);

    for (i = 0; i < args.num_m_uris; i++) {
        free(args.m_uris[i]->uri);
        free(args.m_uris[i]);
    }
    free(args.m_uris);

    HP_LOG_DEBUG("Terminating zmq context");
    (void) zmq_term(args.ctx);

    HP_LOG_INFO("Terminating process");
    exit(rc);
}

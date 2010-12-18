/*
+-----------------------------------------------------------------------------------+
|  httpublish.c                                                                     |
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

static void show_help(const char *d)
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n", d);
	fprintf(stderr, " -b <value>    Hostname or ip to for the HTTP daemon\n");
    fprintf(stderr, " -d            Daemonize the program\n");
	fprintf(stderr, " -g <value>    Group to run as\n");
	fprintf(stderr, " -i <value>    Number of 0MQ IO threads\n");
	fprintf(stderr, " -l <value>    The 0MQ high watermark limit\n");
	fprintf(stderr, " -o            Optimize for bandwidth usage (exclude headers from messages)\n");
	fprintf(stderr, " -p <value>    HTTP listen port\n");
	fprintf(stderr, " -s <value>    Disk offload size (G/M/k/B)\n");
	fprintf(stderr, " -u <value>    User to run as\n");
	fprintf(stderr, " -z <value>    Comma-separated list of 0MQ URIs to connect to\n");
}

static void signal_handler(int sig) {

    switch(sig) {
        case SIGHUP:
		case SIGINT:
        case SIGTERM:
			shutting_down = 1;
		break;

		default:
        break;
    }
}

static bool background() 
{
	pid_t pid, sid;

	HP_LOG_DEBUG("starting the daemonizing process");

    pid = fork();

    if (pid < 0) {
		HP_LOG_ERROR("Failed to fork: %s", strerror(errno));
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
        HP_LOG_ERROR("setsid failed: %s", strerror(errno));
        return false;
    }

    (void) freopen("/dev/null", "r", stdin);
    (void) freopen("/dev/null", "w", stdout);
    (void) freopen("/dev/null", "w", stderr);

    return true;
}

static struct httpush_uri_t **parse_dsn(const char *param, size_t *num)
{
    size_t num_dsn = 0;
    bool success = true;
	char *tmp, *pch, *last = NULL;
    struct httpush_uri_t **retval = NULL;

	*num = 0;

    /* How many? */
    num_dsn = (hp_count_chr(param, ',') + 1);
    retval  = calloc(num_dsn, sizeof(struct httpush_uri_t *));

    // calloc failed
    if (!retval) {
        HP_LOG_ERROR("Failed to allocate memory: %s", strerror(errno));
        return NULL;
    }

	tmp = strdup(param);
	if (!tmp) {
	    HP_LOG_ERROR("Failed to allocate memory: %s", strerror(errno));
        return NULL;
	}

	pch = strtok_r(tmp, ", ", &last);
	if (!pch) {
		free(tmp);
		return NULL;
	}

	while (pch) {
        struct httpush_uri_t *uri_ptr;

        uri_ptr = hp_parse_uri(pch);
        if (!uri_ptr) {
            success = false;
            break;
        }

		retval[(*num)++] = uri_ptr;
		pch              = strtok_r(NULL, ", ", &last);
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

static bool drop_privileges(const char *to_user, const char *to_group)
{
    struct passwd *resolved_user = NULL;
    struct group *resolved_group = NULL;

    resolved_group = getgrnam(to_group);
    if (!resolved_group) {
        HP_LOG_ERROR("Failed to drop privileges. The group(%s) does not exist", to_group);
        return false;
    }

    resolved_user = getpwnam(to_user);
    if (!resolved_user) {
        HP_LOG_ERROR("Failed to drop privileges. The user(%s) does not exist", to_user);
        return false;
    }

    if (setegid(resolved_group->gr_gid) != 0) {
        HP_LOG_ERROR("could not set egid to %s: %s", to_group, strerror(errno));
        return false;
    }

    if (seteuid(resolved_user->pw_uid) != 0) {
        HP_LOG_ERROR("could not set euid to %s: %s", to_user, strerror(errno));
        return false;
    }
    return true;
}

static bool change_working_directory()
{
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

static int create_listen_socket(const char *ip, const char *port)
{
    struct addrinfo *res, hints;
    int rc, sockfd;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (!ip)
        hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(ip, port, &hints, &res);
    if (rc) {
        HP_LOG_ERROR("getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        HP_LOG_ERROR("failed to create socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    rc = bind(sockfd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc) {
        (void) close(sockfd);
        HP_LOG_ERROR("bind failed: %s", strerror(errno));
        return -1;
    }

    rc = fcntl(sockfd, F_SETFL, O_NONBLOCK);
    if (rc) {
        (void) close(sockfd);
        HP_LOG_ERROR("fcntl failed: %s", strerror(errno));
        return -1;
    }

    rc = listen(sockfd, 1024);
    if (rc) {
        (void) close(sockfd);
        HP_LOG_ERROR("listen failed: %s", strerror(errno));
        return -1;
    }

    return sockfd;
}

int main(int argc, char **argv)
{
    size_t i;
	const char *user = "nobody";
	const char *group = "nobody";

    const char *http_host = NULL;
    const char *http_port = "8080";

	int c, rc, io_threads = 1;
	struct httpush_args_t args = {0};

    bool daemonize = false;

	args.ctx       = NULL;
	args.httpd_fd  = -1;
	args.num_dsn   = 0;
    args.hwm       = 0;
    args.swap      = 0;
    args.include_headers = true;

	opterr = 0;

	while ((c = getopt (argc, argv, "b:dg:i:l:op:s:u:z:")) != -1) {
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
					HP_LOG_ERROR("Option -i argument must be a positive integer");
					exit(1);
				}
			break;

			case 'l':
                args.hwm = (uint64_t) atoi(optarg);
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
			        args.swap = hp_unit_to_bytes(optarg, &success);
                    if (!success) {
                        HP_LOG_ERROR("Failed to set swap size");
                        exit(1);
                    }
                }
            break;

			case 'u':
				user = optarg;
			break;

			case 'z':
				args.dsn = parse_dsn(optarg, &(args.num_dsn));
			break;

			case '?':
				if (optopt == 'b' || optopt == 'g' || optopt == 'i' ||
				    optopt == 'p' || optopt == 'u' || optopt == 'z') {
					fprintf(stderr, "Option -%c requires an argument.\n", optopt);
				}
				show_help(argv[0]);
				exit(1);
			break;

			default:
				show_help(argv[0]);
				exit(1);
			break;
		}
	}

    args.httpd_fd = create_listen_socket(http_host, http_port);
    if (args.httpd_fd == -1) {
        exit(1);
    }

	if (drop_privileges(user, group) == false) {
		exit(1);
	}

	if (!args.num_dsn) {
	    // Set the default dsn
		args.dsn = parse_dsn("tcp://127.0.0.1:5555", &(args.num_dsn));
	}

	HP_LOG_INFO("HTTP listen: %s:%s", http_host, http_port);

	if (daemonize) {
		HP_LOG_DEBUG("Launching into background..");
	}

	signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);

	// Ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);

    if (daemonize) {
		if (background() == false) {
            exit(1);
		}
    }

    /* Change the current working directory */
    if (change_working_directory() == false) {
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
	rc = server_boostrap(&args);

    if (args.num_dsn) {
        for (i = 0; i < args.num_dsn; i++) {
            free(args.dsn[i]);
        }
        free(args.dsn);
    }

    HP_LOG_DEBUG("Closing all sockets");
    hp_socket_list_free();

    HP_LOG_DEBUG("Terminating zmq context");
    zmq_term(args.ctx);

    HP_LOG_INFO("Terminating process");
    exit(rc);
}

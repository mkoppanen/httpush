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

void signal_handler(int sig) {

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

void background() 
{
	pid_t pid, sid;

	HP_LOG_DEBUG("starting the daemonizing process");

    pid = fork();

    if (pid < 0) {
		HP_LOG_ERROR("Failed to fork: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

	/* Exit parent */
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    /* Change the file mode mask */
    umask(0);

    /* Create a new SID for the child process */
    sid = setsid();
    if (sid < 0) {
        fprintf(stderr, "setsid failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    (void) freopen("/dev/null", "r", stdin);
    (void) freopen("/dev/null", "w", stdout);
    (void) freopen("/dev/null", "w", stderr);
}

static char **parse_dsn(const char *param, size_t *num)
{
	char *ptr, *pch, **retval = NULL;

	*num = 0;

	ptr = strdup(param);
	pch = strtok(ptr, ", ");

	if (!pch) {
		free(ptr);
		return NULL;
	}

	while (pch) {
        retval = realloc(retval, (*num + 1) * sizeof(char *));
		retval[(*num)++] = strdup(pch);
		pch = strtok(NULL, ", ");
	}
	free(ptr);
	return retval;
}

bool drop_privileges(const char *to_user, const char *to_group)
{
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

static int64_t unit_to_bytes(const char *expression)
{
    int64_t ret;

    char *end = NULL;
    long converted, factor = 1;

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
            exit(1);
        }
    }
    ret = (int64_t) converted * factor;
    return ret;
}

bool change_working_directory()
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


int main(int argc, char **argv)
{
    size_t i;
	const char *user = "nobody";
	const char *group = "nobody";

	int c, rc, io_threads = 1;
	struct httpush_args_t args = {0};

    bool daemonize = false;

	args.ctx        = NULL;
	args.http_host  = "0.0.0.0";
	args.http_port  = 8080;
	args.num_dsn    = 0;
    args.hwm        = 0;
    args.swap       = 0;
    args.include_headers = true;

	opterr = 0;

	while ((c = getopt (argc, argv, "bd:g:i:l:op:s:u:z:")) != -1) {
		switch (c) {

			case 'b':
				args.http_host = optarg;
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
					fprintf(stderr, "Option -i argument must be a positive integer");
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
				args.http_port = atoi(optarg);
			break;

			case 's':
			    args.swap = unit_to_bytes(optarg);
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

	if (drop_privileges(user, group) == false) {
		exit(1);
	}

	if (!args.num_dsn) {
		args.dsn = parse_dsn("tcp://127.0.0.1:5555", &(args.num_dsn));
	}

	fprintf(stderr, "HTTP listen: %s:%d\n", args.http_host, args.http_port);

	if (daemonize) {
		fprintf(stderr, "Launching into background..\n");
	}

	signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);

	// Ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);

    if (daemonize) {
		background();
    }

    /* Change the current working directory */
    if (change_working_directory() == false) {
        HP_LOG_ERROR("Failed to change directory: %s", strerror(errno));
		exit(1);
    }

	/* Initialize the 0MQ context after fork */
	args.ctx = zmq_init(io_threads);

	if (!args.ctx) {
		HP_LOG_ERROR("Failed to initialize zmq context: %s\n", zmq_strerror(errno));
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

    HP_LOG_DEBUG("Terminating zmq context");
    zmq_term(args.ctx);

    HP_LOG_WARN("Terminating process");
    exit(rc);
}

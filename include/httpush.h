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

#ifndef __HTTPUSH_H__
# define __HTTPUSH_H__

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <unistd.h>

#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include <err.h>
#include <event.h>
#include <evhttp.h>

#include <zmq.h>
#include <syslog.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>


struct hp_uri_t {
    char *uri;
    
	uint64_t hwm;
	
    int64_t swap;

	int linger;
};

struct httpush_args_t {
	/* 0MQ context */
	void *ctx;

	int fd;
	
	/* 0MQ backend uri */
	struct hp_uri_t **uris;
	size_t num_uris;

	/* Whether to include headers in the messages */
	bool include_headers;
	
};

struct hp_pair_t {
    void *front;
    void *back;
};

struct hp_httpd_counters_t {
	uint64_t code_200;
             
	uint64_t code_404;
             
	uint64_t code_412;
             
	uint64_t code_503;
             
	uint64_t requests;
};

struct hp_httpd_thread_t {
	/* Thead id */
	int thread_id;
	
    /* The thread */
    pthread_t thread;

	struct hp_pair_t intercomm;

    /* Socket to communicate with device */
    void *out_socket;

	/* Whether to include headers in the messages */
	bool include_headers;

	/* Base structure */
    struct event_base *base;

    /* httpd structure */
    struct evhttp *httpd;

    /* If the shutdown event arrives */
    struct event intercomm_ev;

	/* Counters for the current thread */
	struct hp_httpd_counters_t counters;
};

#define HP_SEC_TO_MSEC(sec_) (sec_ * 1000000)

#ifdef DEBUG
#   define HP_DO_LOG(level_, ...) fprintf(stderr, level_);   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#	define HP_LOG_ERROR(...) HP_DO_LOG("ERROR: ", __VA_ARGS__);
#	define HP_LOG_WARN(...)  HP_DO_LOG("WARNING: ", __VA_ARGS__);
#	define HP_LOG_INFO(...)  HP_DO_LOG("INFO: ", __VA_ARGS__);
#	define HP_LOG_DEBUG(...) HP_DO_LOG("DEBUG: ", __VA_ARGS__);
#else
#	define HP_LOG_ERROR(...) syslog(LOG_ERR, __VA_ARGS__);
#	define HP_LOG_WARN(...)  syslog(LOG_WARNING, __VA_ARGS__);
#	define HP_LOG_INFO(...)  syslog(LOG_INFO, __VA_ARGS__);
#	define HP_LOG_DEBUG(...)
#endif

typedef enum _httpush_msg_t {
	HTTPD_READY = 10,
	HTTPD_FAIL,
	HTTPD_SHUTDOWN,
	HTTPD_STATS,
	MONITOR_STATS
} httpush_msg_t;

bool hp_sendmsg(void *socket, const void *message, size_t message_len, int flags);
bool hp_recvmsg(void *socket, void **message, size_t *message_len, int flags);

bool hp_intercomm_send(void *socket, httpush_msg_t inproc_msg);
bool hp_intercomm_recv(void *socket, httpush_msg_t expected_msg, long timeout);

bool hp_intercomm_recv_cmd(void *socket, httpush_msg_t *cmd, long timeout);


bool hp_create_pair(void *context, struct hp_pair_t *pair, int pair_id);

int hp_server_boostrap(struct httpush_args_t *args, int http_threads);

void hp_httpd_intercomm_cb(int fd, short event, void *args);

struct hp_uri_t *hp_parse_uri(const char *uri, int64_t default_hwm, uint64_t default_swap);

int64_t hp_unit_to_bytes(const char *expression, bool *success);

size_t hp_count_chr(const char *haystack, char needle);

void hp_httpd_publish_message(struct evhttp_request *req, void *args);
void hp_httpd_reflect_request(struct evhttp_request *req, void *param);

int hp_create_listen_socket(const char *ip, const char *port);


#endif /* __HTTPUSH_H__ */

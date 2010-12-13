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

struct httpush_zdsn_t {
	char **dsn;
	size_t num_dsn;
};

struct httpush_args_t {
	/* 0MQ context */
	void *ctx;
	
	/* http host and port */
	const char *http_host;
	int http_port;
	
	/* 0MQ backend uri */
	char **dsn;
	size_t num_dsn;
	
	/* 0MQ HWM */
	uint64_t hwm;
	
	/* 0MQ swap */
	int64_t swap;
	
	/* Whether to include headers in the messages */
	bool include_headers;
};

struct httpush_device_args_t {
	void *ctx;
	uint64_t hwm;  
	int64_t swap;
	char **dsn;
	size_t num_dsn;
	void *in_socket;
	void *out_socket;
	void *intercomm;
};

struct httpush_httpd_args_t {
    /* 0MQ context */
    void *ctx;

    /* Socket to communicate with parent */
    void *intercomm;

    /* Socket to communicate with device */
    void *device;

    /* Http bind host */
    const char *http_host;

    /* Http bind port */
    int http_port;

    struct event_base *base;

    /* If the shutdown event arrives */
    struct event shutdown;

	/* Whether to include headers in the messages */
	bool include_headers;
};

struct httpush_pair_t {
    void *front;
    void *back;
};

#define HP_SEC_TO_MSEC(sec_) (sec_ * 1000000)

#ifdef DEBUG
#	define HP_LOG_ERROR(...) fprintf(stderr, "ERROR: ");   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#	define HP_LOG_WARN(...)  fprintf(stderr, "WARNING: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#	define HP_LOG_DEBUG(...) fprintf(stderr, "DEBUG: ");   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#else
#	define HP_LOG_ERROR(...) syslog(LOG_ERR, __VA_ARGS__);
#	define HP_LOG_WARN(...)  syslog(LOG_WARNING, __VA_ARGS__);
#	define HP_LOG_DEBUG(...)
#endif

typedef enum _httpush_msg_t {
	HTTPD_READY = 10,
	HTTPD_FAIL,
	HTTPD_SHUTDOWN,
	DEVICE_READY,
	DEVICE_FAIL,
	DEVICE_SHUTDOWN
} httpush_msg_t;

int hp_sendmsg(void *socket, const void *message, size_t message_len, int flags);
int hp_recvmsg(void *socket, void **message, size_t *message_len, int flags);

void *hp_socket(void *context, int type, const char *dsn);

int hp_intercomm_send(void *socket, httpush_msg_t inproc_msg);
bool hp_intercomm_recv(void *socket, httpush_msg_t expected_msg, long timeout);

int server_boostrap(struct httpush_args_t *args);

void *start_device_thread(void *thread_args);

void hp_socket_list_free();

bool hp_create_device(pthread_t *thread, void *thread_args);

bool hp_create_httpd(pthread_t *thread, struct httpush_httpd_args_t *httpd_args);

bool hp_create_pair(void *context, const char *dsn, struct httpush_pair_t *pair);


#endif /* __HTTPUSH_H__ */

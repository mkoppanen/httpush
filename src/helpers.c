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

/**
 * Wrapper for sending messages to 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
bool hp_sendmsg(void *socket, const void *message, size_t message_len, int flags)
{
	int rc;
	zmq_msg_t msg;

	rc = zmq_msg_init_size(&msg, message_len);
	if (rc != 0)
        return false;

	memcpy(zmq_msg_data(&msg), message, message_len);
	rc = zmq_send(socket, &msg, flags);

	zmq_msg_close(&msg);
	return (rc == 0);
}

/**
 * Wrapper for receiving messages from 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
bool hp_recvmsg(void *socket, void **message, size_t *message_len, int flags)
{
	int rc;
	zmq_msg_t msg;
    size_t msg_max_len = *message_len;

    *message_len = 0;

	rc = zmq_msg_init(&msg);
	if (rc != 0)
        return false;

	rc = zmq_recv(socket, &msg, flags);

	if (rc != 0) {
		zmq_msg_close(&msg);
		return false;
	}

	if (msg_max_len > 0 && zmq_msg_size(&msg) > msg_max_len) {
	    zmq_msg_close(&msg);
		return false;
	}

	*message = malloc(zmq_msg_size(&msg));

	if (!*message) {
		zmq_msg_close(&msg);
		return false;
	}

	memcpy(*message, zmq_msg_data(&msg), zmq_msg_size(&msg));
	*message_len = zmq_msg_size(&msg);

	zmq_msg_close(&msg);
    return true;
}

bool hp_intercomm_recv_cmd(void *socket, httpush_msg_t *cmd, long timeout)
{
    int rc;
    zmq_pollitem_t items[1];

    *cmd = 0;

    items[0].socket = socket;
    items[0].events = ZMQ_POLLIN;

    rc = zmq_poll(items, 1, timeout);

    if (rc < 0) {
        return false;
    }

    if (rc && items[0].revents | ZMQ_POLLIN) {
        zmq_msg_t msg;

        rc = zmq_msg_init(&msg);
    	if (rc != 0) {
    		return false;
    	}
        rc = zmq_recv(socket, &msg, ZMQ_NOBLOCK);

        if (rc != 0) {
    		zmq_msg_close(&msg);
    		return false;
    	}

    	if (zmq_msg_size(&msg) == sizeof(httpush_msg_t)) {
    	    memcpy(cmd, zmq_msg_data(&msg), sizeof(httpush_msg_t));
        }

    	zmq_msg_close(&msg);
    }
	return cmd;
}


bool hp_intercomm_recv(void *socket, httpush_msg_t expected_msg, long timeout)
{
    httpush_msg_t received;

    if (hp_intercomm_recv_cmd(socket, &received, timeout) == false) {
        return false;
    }
	return (received == expected_msg);
}

/** 
 * Send a message to intercomm. Used to send messages from children to
 * master process
 *
 */
bool hp_intercomm_send(void *socket, httpush_msg_t inproc_msg)
{
	return hp_sendmsg(socket, (const void *) &inproc_msg,
	                    sizeof(httpush_msg_t), ZMQ_NOBLOCK);
}

bool hp_create_pair(void *context, struct hp_pair_t *pair, int pair_id)
{
    int rc;
    char pair_uri[48];

    pair->front = NULL;
    pair->back  = NULL;

    /* Create uri for the pair socket */
    (void) snprintf(pair_uri, 48, "inproc://httpush/pair-%d", pair_id);

    pair->front = zmq_socket(context, ZMQ_PAIR);
    if (!pair->front) {
        goto return_error;
    }

    rc = zmq_bind(pair->front, pair_uri);
    if (rc != 0) {
        goto return_error;
    }

    pair->back = zmq_socket(context, ZMQ_PAIR);
    if (!pair->back) {
        goto return_error;
    }

    rc = zmq_connect(pair->back, pair_uri);
    if (rc != 0) {
        goto return_error;
    }

    return true;

return_error:
    if (pair->back)
        (void) zmq_close(pair->back);

    if (pair->front)
        (void) zmq_close(pair->front);

    return false;
}

struct hp_uri_t *hp_parse_uri(const char *uri, int64_t default_hwm, uint64_t default_swap)
{
    char *pch, *ptr, *last = NULL;
    struct hp_uri_t *retval;

    struct evkeyval *item;
    struct evkeyvalq *q;

    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);

    retval         = malloc(sizeof(*retval));
    retval->swap   = default_swap;
    retval->hwm    = default_hwm;
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

            if (!success)
                return NULL;

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

int64_t hp_unit_to_bytes(const char *expression, bool *success)
{
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
            HP_LOG_ERROR("Unknown swap size unit '%s'", end);
            return 0;
        }
    }
    *success = true;
    ret = (int64_t) converted * factor;
    return ret;
}

size_t hp_count_chr(const char *haystack, char needle)
{
	size_t occurances = 0;

	while (*haystack != '\0') {
		if (*(haystack++) == needle) {
			occurances++;
		}
	}
	return occurances;
}

int hp_create_listen_socket(const char *ip, const char *port)
{
    struct addrinfo *res, hints;
    int rc, sockfd, reuse = 1;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (!ip)
        hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(ip, port, &hints, &res);
    if (rc != 0) {
        HP_LOG_ERROR("getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        HP_LOG_ERROR("failed to create socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    rc = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));
    if (rc != 0) {
        HP_LOG_ERROR("failed to set SO_REUSEADDR: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    rc = bind(sockfd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc != 0) {
        (void) close(sockfd);
        HP_LOG_ERROR("bind failed: %s", strerror(errno));
        return -1;
    }

    rc = evutil_make_socket_nonblocking(sockfd);
    if (rc != 0) {
        (void) close(sockfd);
        HP_LOG_ERROR("fcntl failed: %s", strerror(errno));
        return -1;
    }

    rc = listen(sockfd, 1024);
    if (rc == -1) {
        (void) close(sockfd);
        HP_LOG_ERROR("listen failed: %s", strerror(errno));
        return -1;
    }
    return sockfd;
}

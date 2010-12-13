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

/* Keep track of sockets so they can be freed before terminating context */
static void **allocated_sockets = NULL;
static size_t num_allocated_sockets = 0;

/**
 * Wrapper for sending messages to 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
int hp_sendmsg(void *socket, const void *message, size_t message_len, int flags)
{
	int rc;
	zmq_msg_t msg;

	rc = zmq_msg_init_size(&msg, message_len);
	if (rc != 0) {
		return rc;
	}

	memcpy(zmq_msg_data(&msg), message, message_len);
	rc = zmq_send(socket, &msg, flags);

	zmq_msg_close(&msg);
	return rc;
}

/**
 * Wrapper for receiving messages from 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
int hp_recvmsg(void *socket, void **message, size_t *message_len, int flags)
{
	int rc;
	zmq_msg_t msg;

	rc = zmq_msg_init(&msg);
	if (rc != 0) {
		return rc;
	}

	rc = zmq_recv(socket, &msg, flags);

	if (rc != 0) {
		zmq_msg_close(&msg);
		return rc;
	}
	*message = malloc(zmq_msg_size(&msg));

	if (!*message) {
		zmq_msg_close(&msg);
		return rc;
	}

	memcpy(*message, zmq_msg_data(&msg), zmq_msg_size(&msg));
	*message_len = zmq_msg_size(&msg);

	zmq_msg_close(&msg);
	return 0;
}

#define tv_to_msec(tv_) (((tv_)->tv_sec * 1000 * 1000) + (tv_)->tv_usec)

/**
 * Receive a message from the intercomm. The message contains 
 * httpush_msg_t incating an event (such as HTTPD_READY) on the child
 * Returns true if a message was received and matches inproc_msg param
 * Returns false on failure
 */
bool hp_intercomm_recv(void *socket, httpush_msg_t expected_msg, long timeout)
{
    zmq_pollitem_t items[1];
    struct timeval now, elapsed;
    long timeout_abs, wait;

	int rc;
	httpush_msg_t x = 0;

    items[0].socket = socket;
    items[0].events = ZMQ_POLLIN;

    rc = gettimeofday(&now, NULL);
    if (rc != 0) {
        return false;
    }
    timeout_abs = tv_to_msec(&now) + timeout;

    wait = timeout;

    while (true) {
        rc = zmq_poll(items, 1, wait);

        if (!rc) {
            rc = gettimeofday(&elapsed, NULL);

            if (rc == 0) {
                wait = timeout_abs - tv_to_msec(&elapsed);
                if (wait > 0) {
                    continue;
                }
            }
        }
        break;
    }

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
    	    memcpy(&x, zmq_msg_data(&msg), sizeof(httpush_msg_t));
    	}
    	zmq_msg_close(&msg);
    }
	return (x == expected_msg);
}

/** 
 * Send a message to intercomm. Used to send messages from children to
 * master process
 *
 */
int hp_intercomm_send(void *socket, httpush_msg_t inproc_msg)
{
	return hp_sendmsg(socket, (const void *) &inproc_msg,
	                    sizeof(httpush_msg_t), ZMQ_NOBLOCK);
}

/**
 * Wrapper for creating PUSH and PULL sockets
 * PULL sockets will bind to 'dsn'
 * PUSH sockets will connect to 'dsn' (passing NULL is ok)
 * Returns the newly created socket or NULL on failure
 */
void *hp_socket(void *context, int type, const char *dsn)
{
	int rc = 0;
	void *socket = NULL;
    int linger = 2000;

	socket = zmq_socket(context, type);

	if (socket && dsn) {
		switch (type) {
			case ZMQ_PUSH:
				rc = zmq_connect(socket, dsn);
			break;

			case ZMQ_PULL:
				rc = zmq_bind(socket, dsn);
			break;
		}
		if (rc != 0) {
			zmq_close(socket);
			socket = NULL;
		}
	}
	/* Set linger on all sockets we send out 
	    TODO: configurable value
	*/
    rc = zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));
    if (rc != 0) {
        HP_LOG_WARN("Failed to set linger value: %s", zmq_strerror(errno));
    }

    ++num_allocated_sockets;
    allocated_sockets = realloc(allocated_sockets, num_allocated_sockets * sizeof(void *));
    allocated_sockets[num_allocated_sockets - 1] = socket;
	return socket;
}

bool hp_create_pair(void *context, const char *dsn, struct httpush_pair_t *pair)
{
    int rc;

    pair->front = hp_socket(context, ZMQ_PAIR, NULL);
    if (!pair->front)
        return false;

    rc = zmq_bind(pair->front, dsn);
    if (rc != 0)
        return false;

    pair->back = hp_socket(context, ZMQ_PAIR, NULL);
    if (!pair->back)
        return false;

    rc = zmq_connect(pair->back, dsn);
    if (rc != 0)
        return false;

    return true;
}


void hp_socket_list_free() 
{
    size_t i;

#ifdef __GNUC__
    /* Ensure full memory barrier */
    __sync_synchronize();
#endif

    if (!num_allocated_sockets)
        return;

    for (i = 0; i < num_allocated_sockets; i++) {
        int rc = zmq_close(allocated_sockets[i]);
        if (rc != 0) {
            HP_LOG_WARN("Failed to close socket");
        }
    }
    free(allocated_sockets);
}

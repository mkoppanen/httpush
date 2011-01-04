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

/**
 * Wrapper for sending messages to 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
bool hp_sendmsg(void *socket, const void *message, size_t message_len, int flags) {
    int i = 0, rc;
    zmq_msg_t msg;

    rc = zmq_msg_init_size(&msg, message_len);
    if (rc != 0)
        return false;

    memcpy(zmq_msg_data(&msg), message, message_len);
    while (++i < 3) {
        rc = zmq_send(socket, &msg, flags);
        if (rc == 0)
            break;

        else if (rc != 0 && errno == EAGAIN)
            continue;

        break;
    }

    zmq_msg_close(&msg);
    return (rc == 0);
}

/**
 * Wrapper for receiving messages from 0MQ socket
 * Returns 0 on success and <> 0 on failure
 * 'errno' should indicate the error 
 */
bool hp_recvmsg(void *socket, void **message, size_t *message_len, int flags) {
    int rc;
    zmq_msg_t msg;
    size_t msg_max_len = *message_len;

    *message_len = 0;
    *message = NULL;

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

    if (zmq_msg_size(&msg) == 0) {
        zmq_msg_close(&msg);
        return true;
    }

    *message = malloc(zmq_msg_size(&msg));

    if (!*message) {
        zmq_msg_close(&msg);
        return false;
    }

    memcpy(*message, zmq_msg_data(&msg), zmq_msg_size(&msg));
    *message_len = zmq_msg_size(&msg);

    (void) zmq_msg_close(&msg);
    return true;
}

bool hp_recv_command(void *socket, hp_command_t *cmd, long timeout) {
    int rc;
    zmq_pollitem_t items[1];

    *cmd = 0;

    items[0].socket = socket;
    items[0].fd = 0;
    items[0].events = ZMQ_POLLIN;
    items[0].revents = 0;

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

        if (zmq_msg_size(&msg) == sizeof (hp_command_t)) {
            memcpy(cmd, zmq_msg_data(&msg), sizeof (hp_command_t));
        }
        zmq_msg_close(&msg);
    }
    return true;
}

/** 
 * Send a message to intercomm. Used to send messages from children to
 * master process
 *
 */
bool hp_send_command(void *socket, hp_command_t inproc_msg) {
    return hp_sendmsg(socket, (const void *) &inproc_msg,
            sizeof (hp_command_t), ZMQ_NOBLOCK);
}

bool hp_create_pair(void *context, struct hp_pair_t *pair, int pair_id) {
    int rc;
    char pair_uri[48];

    pair->front = NULL;
    pair->back = NULL;

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

bool hp_close_pair(struct hp_pair_t *pair) 
{
    bool retval = true;
    int rc;

    rc = zmq_close(pair->back);
    if (rc == 0) {
        pair->back = NULL;
    } else {
        retval = false;
    }

    rc = zmq_close(pair->front);
    if (rc == 0) {
        pair->front = NULL;
    } else {
        retval = false;
    }

    return retval;
}

bool hp_sendmsg_ident(void *socket, char identity[HP_IDENTITY_MAX], size_t identity_size, const void *message, size_t message_size)
{
    if (identity_size > HP_IDENTITY_MAX) {
        return false;
    }

    if (hp_sendmsg(socket, identity, identity_size, ZMQ_NOBLOCK|ZMQ_SNDMORE) == false) {
        return false;
    }

    if (hp_sendmsg(socket, (const void *) NULL, 0, ZMQ_NOBLOCK|ZMQ_SNDMORE) == false) {
        return false;
    }

    if (hp_sendmsg(socket, (const void *) message, message_size, ZMQ_NOBLOCK) == false) {
        return false;
    }

    return true;
}

bool hp_recvmsg_ident(void *socket, char identity[HP_IDENTITY_MAX], size_t *identity_size, void *message, size_t *message_size)
{
    int64_t more;
    size_t moresz, max_size;

    int rc;
    char *buffer;
    size_t buffer_size;

    if (*identity_size > HP_IDENTITY_MAX) {
        return false;
    }

    if (hp_recvmsg(socket, (void **) &buffer, identity_size, 0) == false) {
        return false;
    }

    /* Copy the identity */
    memcpy(identity, buffer, *identity_size);
    free(buffer);

    moresz = sizeof (int64_t);
    rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moresz);

    if (rc != 0) {
        return false;
    }

    /* No need to recv larger chunks than the buffer */
    buffer_size   = *message_size;
    max_size      = *message_size;
    *message_size = 0;

    while (more) {
        if (hp_recvmsg(socket, (void **) &buffer, &buffer_size, 0) == false) {
            *message_size = 0;
            return false;
        }

        if (!buffer) {
            continue;
        }

        if (*message_size + buffer_size > max_size) {
            *message_size = 0;
            free(buffer);
            return false;
        }

        memcpy((char *) message + *message_size, buffer, buffer_size);
        *message_size += buffer_size;

        free(buffer);

        moresz = sizeof (int64_t);
        rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moresz);

        if (rc != 0) {
            *message_size = 0;
            return false;
        }
    }
    return true;
}

struct evbuffer *hp_counters_to_xml(struct hp_httpd_counters_t *counter, int responses, int threads)
{
    struct evbuffer *evb = evbuffer_new();

    if (!evb)
        return NULL;

    evbuffer_add_printf(evb, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
    evbuffer_add_printf(evb, "<httpush>\n");
    evbuffer_add_printf(evb, "  <statistics>\n");
    evbuffer_add_printf(evb, "    <threads>%d</threads>\n", threads);
    evbuffer_add_printf(evb, "    <responses>%d</responses>\n", responses);
    evbuffer_add_printf(evb, "    <requests>%" PRIu64 "</requests>\n", counter->requests);
    evbuffer_add_printf(evb, "    <status code=\"200\">%" PRIu64 "</status>\n", counter->code_200);
    evbuffer_add_printf(evb, "    <status code=\"404\">%" PRIu64 "</status>\n", counter->code_404);
    evbuffer_add_printf(evb, "    <status code=\"412\">%" PRIu64 "</status>\n", counter->code_412);
    evbuffer_add_printf(evb, "    <status code=\"503\">%" PRIu64 "</status>\n", counter->code_503);
    evbuffer_add_printf(evb, "  </statistics>\n");
    evbuffer_add_printf(evb, "</httpush>\n");

    return evb;
}



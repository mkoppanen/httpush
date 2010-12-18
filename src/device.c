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

static void *hp_device_thread(void *thread_args)
{
    zmq_msg_t msg;
    int rc;

    int64_t more;
    size_t moresz;
    zmq_pollitem_t items [2];

    struct httpush_device_args_t *args = (struct httpush_device_args_t *) thread_args;

    rc = zmq_msg_init (&msg);

    if (rc != 0) {
        if (hp_intercomm_send(args->intercomm, DEVICE_FAIL) == false) {
            HP_LOG_ERROR("Failed to indicate device failure: %s", zmq_strerror(errno));
        }
        return NULL;
    }

    items [0].socket  = args->in_socket;
    items [0].fd      = 0;
    items [0].events  = ZMQ_POLLIN;
    items [0].revents = 0;

    items [1].socket  = args->intercomm;
    items [1].fd      = 0;
    items [1].events  = ZMQ_POLLIN;
    items [1].revents = 0;

    hp_intercomm_send(args->intercomm, DEVICE_READY);

    while (true) {

        //  Wait while there are either requests or replies to process.
        rc = zmq_poll(items, 2, HP_SEC_TO_MSEC(1));
        if (rc < 0) {
            zmq_msg_close(&msg);
            hp_intercomm_send(args->intercomm, DEVICE_FAIL);
            return NULL;
        }

        //  The algorithm below asumes ratio of request and replies processed
        //  under full load to be 1:1. Although processing requests replies
        //  first is tempting it is suspectible to DoS attacks (overloading
        //  the system with unsolicited replies).

        //  Process a request.
        if (items[0].revents & ZMQ_POLLIN) {
            while (true) {
                rc = zmq_recv(args->in_socket, &msg, 0);
                if (rc < 0) {
                    zmq_msg_close(&msg);
                    hp_intercomm_send(args->intercomm, DEVICE_FAIL);
                    return NULL;
                }

                moresz = sizeof(more);
                rc = zmq_getsockopt(args->in_socket, ZMQ_RCVMORE, &more, &moresz);
                if (rc < 0) {
                    zmq_msg_close(&msg);
                    hp_intercomm_send(args->intercomm, DEVICE_FAIL);
                    return NULL;
                }

                rc = zmq_send(args->out_socket, &msg, ((more ? ZMQ_SNDMORE : 0) | ZMQ_NOBLOCK));
                if (rc != 0 && errno == EAGAIN) {
                    HP_LOG_WARN("Overflow of messages in device, messages will be discarded");
                    rc = 0;
                }
                if (rc < 0) {
                    hp_intercomm_send(args->intercomm, DEVICE_FAIL);
                    zmq_msg_close(&msg);
                    return NULL;
                }

                if (!more)
                    break;
            }
        }

        /* Messages coming from controlling process */
        if (items[1].revents & ZMQ_POLLIN) {
            bool time_to_exit = false;
            HP_LOG_DEBUG("device received intercomm event");

            while (true) {
                if (hp_intercomm_recv(args->intercomm, DEVICE_SHUTDOWN, 1000) == true)  {
                    time_to_exit = true;
                    break;
                }
            }
            if (time_to_exit) {
                break;
            }
        }
    }
    zmq_msg_close(&msg);
    return NULL;
}

bool hp_create_device(pthread_t *thread, void *thread_args)
{
    int rc;
	size_t i;
    struct httpush_device_args_t *args = (struct httpush_device_args_t *) thread_args;

	/* the back socket, pushes to network */
	args->out_socket = hp_socket(args->ctx, ZMQ_PUSH, NULL);

	if (!args->out_socket) {
        return false;
	}

    for (i = 0; i < args->num_dsn; i++) {
        uint64_t hwm = args->hwm;
        int64_t swap = args->swap;

        /* Override values for individual sockets */
        if (args->dsn[i]->hwm_set == true) {
            hwm = args->dsn[i]->hwm;
        }

        rc = zmq_setsockopt(args->out_socket, ZMQ_HWM, (void *) &hwm, sizeof(uint64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set HWM value: %s", zmq_strerror(errno));
            return false;
        }

        if (args->dsn[i]->swap_set == true) {
            swap = args->dsn[i]->swap;
        }

        rc = zmq_setsockopt(args->out_socket, ZMQ_SWAP, (void *) &swap, sizeof(int64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set swap value: %s", zmq_strerror(errno));
            return false;
        }

        HP_LOG_DEBUG("zmq_connect: %s, swap=[%" PRIi64 "], hwm=[%" PRIu64 "]", args->dsn[i]->uri, swap, hwm);
		if (zmq_connect(args->out_socket, args->dsn[i]->uri) != 0) {
			return false;
		}
    }

	if (pthread_create(thread, NULL, hp_device_thread, (void *) args) != 0) {
		HP_LOG_ERROR("Failed to create zmq device thread: %s", strerror(errno));
        return false;
	}
    return true;
}

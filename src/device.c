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

void *start_device_thread(void *thread_args)
{
    int rc;
	size_t i;
	void *front, *back, *intercomm;
	struct httpush_args_t *args = (struct httpush_args_t *) thread_args;

	/* This socket is used to communicate with parent */
	intercomm = hp_socket(args->ctx, ZMQ_PUSH, HTTPUSH_INTERCOMM);
	if (!intercomm) {
		return NULL;
	}

	/* This socket connects to httpd thread */
	front = hp_socket(args->ctx, ZMQ_PULL, HTTPUSH_INPROC);

	if (!front) {
		hp_intercomm_send(intercomm, DEVICE_FAIL);
		return NULL;
	}

	/* the back socket, pushes to network */
	back = hp_socket(args->ctx, ZMQ_PUSH, NULL);

    /* Set socket options */
    if (args->swap > 0) {
        rc = zmq_setsockopt(back, ZMQ_SWAP, (void *) &(args->swap), sizeof(int64_t));
        if (rc != 0) {
            HP_LOG_WARN("Failed to set swap value: %s", zmq_strerror(errno));
        }
    }

    if (args->hwm > 0) {
        rc = zmq_setsockopt(back, ZMQ_HWM, (void *) &(args->hwm), sizeof(uint64_t));
        if (rc != 0) {
            HP_LOG_WARN("Failed to set HWM value: %s", zmq_strerror(errno));
        }
    }

	for (i = 0; i < args->num_dsn; i++) {
		HP_LOG_DEBUG("Connecting to: %s", args->dsn[i]);
		if (zmq_connect(back, args->dsn[i]) != 0) {
			hp_intercomm_send(intercomm, DEVICE_FAIL);
			return NULL;
		}
	}

	if (!back) {
		hp_intercomm_send(intercomm, DEVICE_FAIL);
		return NULL;
	}

	hp_intercomm_send(intercomm, DEVICE_READY);
	zmq_device(ZMQ_QUEUE, front, back);

	return NULL;
}

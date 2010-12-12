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

extern sig_atomic_t shutting_down;

int server_boostrap(struct httpush_args_t *args)
{
    int rc;
	void *intercomm;
	pthread_t httpd_thread = {0};
	pthread_t device_thread = {0};

	/* Master pulls messages from children */
	intercomm = hp_socket(args->ctx, ZMQ_PULL, HTTPUSH_INTERCOMM);

	if (!intercomm) {
		HP_LOG_ERROR("Failed to create intercomm 0MQ socket: %s", zmq_strerror(errno));
		return 1;
	}

	/* Start the device thread */
	if (pthread_create(&device_thread, NULL, start_device_thread, (void *) args) != 0) {
		HP_LOG_ERROR("Failed to create thread: %s", strerror(errno));
		return 1;
	}

	if (hp_intercomm_recv(intercomm, DEVICE_READY, 5) == false) {
		HP_LOG_ERROR("Failed to start the 0MQ device");
		return 1;
	}

	/* Start the httpd thread */
	if (pthread_create(&httpd_thread, NULL, start_httpd_thread, (void *) args) != 0) {
		HP_LOG_ERROR("Failed to create thread: %s", strerror(errno));
		return 1;
	}

	if (hp_intercomm_recv(intercomm, HTTPD_READY, 5) == false) {
		HP_LOG_ERROR("Failed to start the httpd server");
		return 1;
	}

	while (1) {
		/* TODO: maybe a bit cleaner shutdown */
		if (shutting_down) {
		    HP_LOG_DEBUG("Canceling device thread");
            rc = pthread_cancel(device_thread);
            assert (rc == 0);

		    HP_LOG_DEBUG("Joining device thread");
            rc = pthread_join(device_thread, NULL);
            assert (rc == 0);

			HP_LOG_DEBUG("Joining httpd thread");
            rc = pthread_join(httpd_thread, NULL);
            assert (rc == 0);

            HP_LOG_DEBUG("Closing all sockets");
            hp_socket_list_free();

            HP_LOG_DEBUG("Terminating context");
            zmq_term(args->ctx);

            HP_LOG_WARN("Terminating process");
            return 0;
		}
		sleep(1);
	}
}

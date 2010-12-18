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
    bool device_started = false, httpd_started = false;

    int rc, exit_code = 0;
    struct httpush_pair_t device_pair = {0};
    struct httpush_pair_t httpd_pair = {0};
    struct httpush_pair_t message_pair = {0};
    struct httpush_device_args_t device_args = {0};
    struct httpush_httpd_args_t httpd_args = {0};

	pthread_t httpd_thread = {0};
	pthread_t device_thread = {0};

	/* This pair is a bridge between the device and httpd */
    if (hp_create_pair(args->ctx, "inproc://message-pair", &message_pair) == false) {
        return 1;
    }

    /* This pair is a bridge between the device and master process */
    if (hp_create_pair(args->ctx, "inproc://device-pair", &device_pair) == false) {
        return 1;
    }

	/* This pair is used to communicate from httpd to master process */
    if (hp_create_pair(args->ctx, "inproc://httpd-pair", &httpd_pair) == false) {
        return 1;
    }

    device_args.ctx       = args->ctx;
    device_args.hwm       = args->hwm;
    device_args.swap      = args->swap;
    device_args.dsn       = args->dsn;
    device_args.num_dsn   = args->num_dsn;
    device_args.in_socket = message_pair.back;
    device_args.intercomm = device_pair.back;

    /* Block here until sigint arrives */
    if (hp_create_device(&device_thread, &device_args) == false ||
        hp_intercomm_recv(device_pair.front, DEVICE_READY, HP_SEC_TO_MSEC(2)) == false) {
		HP_LOG_ERROR("Failed to start the zmq device server");
    } else {
        device_started = true;
    }

    if (device_started) {
        httpd_args.ctx             = args->ctx;
        httpd_args.httpd_fd        = args->httpd_fd;
        httpd_args.include_headers = args->include_headers;
        httpd_args.device          = message_pair.front;
        httpd_args.intercomm       = httpd_pair.back;

        if (hp_create_httpd(&httpd_thread, &httpd_args) == false ||
            hp_intercomm_recv(httpd_pair.front, HTTPD_READY, HP_SEC_TO_MSEC(2)) == false) {
            HP_LOG_ERROR("Failed to start the httpd server");
        } else {
            httpd_started = true;
        }
    }

    if (!device_started || !httpd_started) {
        shutting_down = 1;
        exit_code     = 1;
    }

	while (1) {

		if (shutting_down) {

		    if (httpd_started) {
                if (hp_intercomm_send(httpd_pair.front, HTTPD_SHUTDOWN) == false) {
                    return 1;
                }
                rc = pthread_join(httpd_thread, NULL);
                assert (rc == 0);
            }

            if (device_started) {
                if (hp_intercomm_send(device_pair.front, DEVICE_SHUTDOWN) == false) {
                    return 1;
                }
                rc = pthread_join(device_thread, NULL);
                assert (rc == 0);
            }

            return exit_code;
		}
		if (hp_intercomm_recv(httpd_pair.front, HTTPD_FAIL, HP_SEC_TO_MSEC(1)) == true) {
            HP_LOG_ERROR("httpd exit");
            shutting_down = 1;
        }
		if (hp_intercomm_recv(device_pair.front, DEVICE_FAIL, HP_SEC_TO_MSEC(1)) == true) {
            HP_LOG_ERROR("device exit");
            shutting_down = 1;
        }
	}
}

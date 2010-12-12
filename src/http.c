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

struct httpush_timer_args_t {
    struct event_base *base;
    struct event timeout;
};

extern sig_atomic_t shutting_down;

static uint64_t httpd_code_200 = 0;

static uint64_t httpd_code_404 = 0;

static uint64_t httpd_code_412 = 0;

static uint64_t httpd_code_503 = 0;

static uint64_t httpd_requests = 0;

static void publish_stats(struct evhttp_request *req, void *param)
{
	struct evbuffer *evb;

	evb = evbuffer_new();

	++httpd_requests;

	if (!evb) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++httpd_code_503;
	}

	++httpd_code_200;

	evhttp_add_header(req->output_headers, "Content-Type", "text/xml");

	evbuffer_add_printf(evb, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
	evbuffer_add_printf(evb, "<httpush>\n");
#ifdef PACKAGE_VERSION	
	evbuffer_add_printf(evb, "  <version>%s</version>\n", PACKAGE_VERSION);
#endif	
	evbuffer_add_printf(evb, "  <statistics>\n");
	evbuffer_add_printf(evb, "    <requests>%" PRIu64 "</requests>\n", httpd_requests);
	evbuffer_add_printf(evb, "    <status code=\"200\">%" PRIu64 "</status>\n", httpd_code_200);
	evbuffer_add_printf(evb, "    <status code=\"404\">%" PRIu64 "</status>\n", httpd_code_404);
	evbuffer_add_printf(evb, "    <status code=\"412\">%" PRIu64 "</status>\n", httpd_code_412);
	evbuffer_add_printf(evb, "    <status code=\"503\">%" PRIu64 "</status>\n", httpd_code_503);
	evbuffer_add_printf(evb, "  </statistics>\n");
	evbuffer_add_printf(evb, "</httpush>\n");

	evhttp_send_reply(req, HTTP_OK, "OK", evb);
	evbuffer_free(evb);
}

static void print_headers_to_buffer(struct evhttp_request *req, struct evbuffer *evb)
{
    const char *method = NULL;

    struct evkeyval *header;
    struct evkeyvalq *q;

    switch (req->type) {
        case EVHTTP_REQ_GET:
            method = "GET";
        break;

        case EVHTTP_REQ_POST:
            method = "POST";
        break;

        case EVHTTP_REQ_HEAD:
            method = "HEAD";
        break;
    }

    evbuffer_add_printf(evb, "%s %s HTTP/1.1\r\n", method, evhttp_request_uri(req));

    q = req->input_headers;

    TAILQ_FOREACH(header, q, next) {
        evbuffer_add_printf(evb, "%s: %s\r\n", header->key, header->value);
    }
    /* Add a header indicating where the connection came from */
    evbuffer_add_printf(evb, "X-Remote-Peer: %s:%d\r\n", req->remote_host, req->remote_port);
}

static void reflect_request(struct evhttp_request *req, void *param)
{
    struct evbuffer *evb = evbuffer_new();

	++httpd_requests;

	if (!evb) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++httpd_code_503;
	}

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain");

    evbuffer_add_printf(evb, "--------------------------------------------------------------------\n");

        print_headers_to_buffer(req, evb);

    evbuffer_add_printf(evb, "\n--------------------------------------------------------------------\n");

        evbuffer_add(evb, EVBUFFER_DATA(req->input_buffer), EVBUFFER_LENGTH(req->input_buffer));

    evbuffer_add_printf(evb, "\n--------------------------------------------------------------------\n");

    evhttp_send_reply(req, HTTP_OK, "OK", evb);
	evbuffer_free(evb);
}

static void publish_message(struct evhttp_request *req, void *inproc_socket)
{
	int rc = 0;
    struct evbuffer *header_evb;

    ++httpd_requests;
    HP_LOG_DEBUG("Handling request to %s from %s:%d", evhttp_request_uri(req), req->remote_host, req->remote_port);

	if (EVBUFFER_LENGTH(req->input_buffer) < 1) {
		evhttp_send_error(req, 412, "Precondition Failed");
		++httpd_code_412;
		return;
	}

	/* Send the first part of the message, headers */
    header_evb = evbuffer_new();
    if (!header_evb) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++httpd_code_503;
        return;
    }

    print_headers_to_buffer(req, header_evb);
	rc = hp_sendmsg(inproc_socket, (const void *) EVBUFFER_DATA(header_evb), EVBUFFER_LENGTH(header_evb), ZMQ_SNDMORE);
    evbuffer_free(header_evb);

    if (rc != 0) {
        HP_LOG_ERROR("Failed to send message: %s\n", zmq_strerror(errno));
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++httpd_code_503;
        return;
    }

	rc = hp_sendmsg(inproc_socket, (const void *) EVBUFFER_DATA(req->input_buffer), EVBUFFER_LENGTH(req->input_buffer), 0);

	if (rc != 0) {
		HP_LOG_ERROR("Failed to send message: %s\n", zmq_strerror(errno));
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++httpd_code_503;
	} else {
		struct evbuffer *evb;

		evb = evbuffer_new();

		if (!evb) {
    		evhttp_send_error(req, HTTP_OK, "OK");
    		++httpd_code_200;
            return;
        }

		evbuffer_add(evb, "Sent", sizeof("Sent") - 1);
		evhttp_send_reply(req, HTTP_OK, "OK", evb);
		evbuffer_free(evb);

		++httpd_code_200;
	}
}

static void hp_intercomm_cb(int fd, short event, void *socket)
{
    fprintf(stderr, "Intercomm called\n");
}

void *start_httpd_thread(void *thread_args) 
{
    struct event shutdown;
    struct event_base *base;
	struct evhttp *httpd;

    int rc;
	void *inproc, *intercomm;
	struct httpush_args_t *args = (struct httpush_args_t *)thread_args;

	/* This socket is used to communicate with parent */
	intercomm = hp_socket(args->ctx, ZMQ_PUSH, HTTPUSH_INTERCOMM);
	if (!intercomm) {
		return NULL;
	}

	/* front socket takes messages from http */
	inproc = hp_socket(args->ctx, ZMQ_PUSH, HTTPUSH_INPROC);
	if (!inproc) {
		hp_intercomm_send(intercomm, HTTPD_FAIL);
		return NULL;
	}

	/* Initialize libevent */
	base = event_base_new();
	if (!base) {
		hp_intercomm_send(intercomm, HTTPD_FAIL);
		return NULL;
	}

	/* ------- */

    /* Use libevent to monitor the intercomm socket. The master process signals 
       shutdown using this fd */
    int fd = -1;
    size_t fd_size = sizeof(int);
    struct timeval tv;

    rc = zmq_getsockopt(intercomm, ZMQ_FD, &fd, &fd_size);
    assert(rc == 0);

    /* Create the event */
    event_set(&shutdown, fd, EV_READ, hp_intercomm_cb, intercomm);
    event_base_set(base, &shutdown);

    event_add(&shutdown, &tv);

    /* ------- */

	/* HTTP server init */
	httpd = evhttp_new(base);

	if (!httpd) {
		hp_intercomm_send(intercomm, HTTPD_FAIL);
		return NULL;
	}

	rc = evhttp_bind_socket(httpd, args->http_host, args->http_port);
	if (rc != 0) {
		hp_intercomm_send(intercomm, HTTPD_FAIL);
		return NULL;
	}

	/* Specific action for displaying stats */
	evhttp_set_cb(httpd, "/stats",   publish_stats,   NULL);
	evhttp_set_cb(httpd, "/reflect", reflect_request, NULL);

	/* Catch all */
	evhttp_set_gencb(httpd, publish_message, inproc);

	/* Start main loop */
	hp_intercomm_send(intercomm, HTTPD_READY);
	event_base_dispatch(base);

	/* Not reached in this code as it is now. */
	evhttp_free(httpd);
    event_base_free(base);
	return NULL;
}

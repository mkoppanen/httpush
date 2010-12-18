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

static void publish_message(struct evhttp_request *req, void *args)
{
	bool sent;
    struct httpush_httpd_args_t *httpd_args = (struct httpush_httpd_args_t *)args;

    ++httpd_requests;

	if (EVBUFFER_LENGTH(req->input_buffer) < 1) {
		evhttp_send_error(req, 412, "Precondition Failed");
		++httpd_code_412;
		return;
	}

	if (httpd_args->include_headers == true) {
    	/* Send the first part of the message, headers */
        struct evbuffer *header_evb = evbuffer_new();
        if (!header_evb) {
    		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
    		++httpd_code_503;
            return;
        }

        print_headers_to_buffer(req, header_evb);
    	sent = hp_sendmsg(httpd_args->device, (const void *) EVBUFFER_DATA(header_evb), EVBUFFER_LENGTH(header_evb), ZMQ_SNDMORE);
        evbuffer_free(header_evb);

        if (!sent) {
            HP_LOG_ERROR("Failed to send message: %s\n", zmq_strerror(errno));
    		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
    		++httpd_code_503;
            return;
        }
    }

    /* This should never block. Fingers crossed */
	sent = hp_sendmsg(httpd_args->device, (const void *) EVBUFFER_DATA(req->input_buffer), EVBUFFER_LENGTH(req->input_buffer), 0);

	if (!sent) {
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

static void hp_intercomm_cb(int fd, short event, void *args)
{
    int rc;
    struct httpush_httpd_args_t *httpd_args = (struct httpush_httpd_args_t *)args;

    /* Reschedule the event */
    event_add(&(httpd_args->shutdown), NULL);

    while (true) {
        uint32_t events;
        size_t siz = sizeof(uint32_t);

        rc = zmq_getsockopt(httpd_args->intercomm, ZMQ_EVENTS, &events, &siz);
        if (rc != 0) {
            break;
        }

        if (!(events & ZMQ_POLLIN)) {
            break;
        }

        HP_LOG_DEBUG("httpd received intercomm event");

        /* Wait for maximum of 1 seconds */
        if (hp_intercomm_recv(httpd_args->intercomm, HTTPD_SHUTDOWN, HP_SEC_TO_MSEC(1)) == true) {
            struct timeval tv = { 0, 2 };

            HP_LOG_DEBUG("Terminating event loop");
            event_base_loopexit(httpd_args->base, &tv);
        }
    }
}

static void init_intercomm_event(struct httpush_httpd_args_t *httpd_args)
{
    int rc, fd = -1;
    size_t fd_size = sizeof(int);
    struct timeval tv = {0};

    rc = zmq_getsockopt(httpd_args->intercomm, ZMQ_FD, &fd, &fd_size);
    assert(rc == 0);

    /* Create the event */
    event_set(&(httpd_args->shutdown), fd, EV_READ, hp_intercomm_cb, httpd_args);
    event_base_set(httpd_args->base, &(httpd_args->shutdown));

    event_add(&(httpd_args->shutdown), &tv);
}

static void *start_httpd_thread(void *thread_args) 
{
    int rc;
	struct evhttp *httpd;
    struct httpush_httpd_args_t *httpd_args;

    httpd_args = (struct httpush_httpd_args_t *) thread_args;

    /* Use libevent to monitor the intercomm socket. The master process signals 
       shutdown using this fd */
    init_intercomm_event(httpd_args);

	/* HTTP server init */
	httpd = evhttp_new(httpd_args->base);

	if (!httpd) {
		if (hp_intercomm_send(httpd_args->intercomm, HTTPD_FAIL) == false) {
            HP_LOG_ERROR("Failed to signal httpd exit: %s", zmq_strerror(errno));
		}
		return NULL;
	}

    rc = evhttp_accept_socket(httpd, httpd_args->httpd_fd);

	// rc = evhttp_bind_socket(httpd, httpd_args->http_host, httpd_args->http_port);
	if (rc != 0) {
		if (hp_intercomm_send(httpd_args->intercomm, HTTPD_FAIL) == false) {
            HP_LOG_ERROR("Failed to signal httpd exit: %s", zmq_strerror(errno));
		}
		return NULL;
	}

	/* Specific action for displaying stats */
	evhttp_set_cb(httpd, "/stats",   publish_stats,   NULL);
	evhttp_set_cb(httpd, "/reflect", reflect_request, NULL);

	/* Catch all */
	evhttp_set_gencb(httpd, publish_message, httpd_args);

	/* Start main loop */
	hp_intercomm_send(httpd_args->intercomm, HTTPD_READY);
	event_base_dispatch(httpd_args->base);

	/* Not reached in this code as it is now. */
	evhttp_free(httpd);
    event_base_free(httpd_args->base);
	return NULL;
}

/*  
    Creates the httpd thread and returns a socket that can be used to 
    communicate with this thread.
*/
bool hp_create_httpd(pthread_t *thread, struct httpush_httpd_args_t *httpd_args)
{
    /* Initialize libevent */
	httpd_args->base = event_base_new();

	if (!httpd_args->base) {
	    HP_LOG_ERROR("Failed to initialize libevent: %s", strerror(errno));
		return false;
	}

    /*
        Run the httpd in its own thread. 
    */
	if (pthread_create(thread, NULL, start_httpd_thread, (void *) httpd_args) != 0) {
		HP_LOG_ERROR("Failed to create httpd thread: %s", strerror(errno));
        return false;
	}

    return true;
}


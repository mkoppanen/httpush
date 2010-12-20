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

#if 0
void hp_httpd_publish_stats(struct evhttp_request *req, void *args)
{
    struct hp_httpd_thread_t *thread = (struct hp_httpd_thread_t *) args;
	struct evbuffer *evb;

	evb = evbuffer_new();

	++(thread->counters.requests);

	if (!evb) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++(thread->counters.code_503);
	}

	++(thread->counters.code_200);

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
#endif

void hp_httpd_reflect_request(struct evhttp_request *req, void *args)
{
    struct hp_httpd_thread_t *thread = (struct hp_httpd_thread_t *) args;
    struct evbuffer *evb = evbuffer_new();

	++(thread->counters.requests);

	if (!evb) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++(thread->counters.code_503);
	}

	++(thread->counters.code_200);

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain");

    evbuffer_add_printf(evb, "--------------------------------------------------------------------\n");

        print_headers_to_buffer(req, evb);

    evbuffer_add_printf(evb, "\n--------------------------------------------------------------------\n");

        evbuffer_add(evb, EVBUFFER_DATA(req->input_buffer), EVBUFFER_LENGTH(req->input_buffer));

    evbuffer_add_printf(evb, "\n--------------------------------------------------------------------\n");

    evhttp_send_reply(req, HTTP_OK, "OK", evb);
	evbuffer_free(evb);
}

void hp_httpd_publish_message(struct evhttp_request *req, void *args)
{
    struct hp_httpd_thread_t *thread = (struct hp_httpd_thread_t *) args;
	bool sent;

    ++(thread->counters.requests);

    HP_LOG_DEBUG("thread %d handling request", thread->thread_id);

	if (EVBUFFER_LENGTH(req->input_buffer) < 1) {
		evhttp_send_error(req, 412, "Precondition Failed");
		++(thread->counters.code_412);
		return;
	}

	if (thread->include_headers == true) {
    	/* Send the first part of the message, headers */
        struct evbuffer *header_evb = evbuffer_new();
        if (!header_evb) {
    		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
    		++(thread->counters.code_503);
            return;
        }

        print_headers_to_buffer(req, header_evb);
    	sent = hp_sendmsg(thread->out_socket, (const void *) EVBUFFER_DATA(header_evb), EVBUFFER_LENGTH(header_evb), ZMQ_SNDMORE|ZMQ_NOBLOCK);
        evbuffer_free(header_evb);

        if (!sent) {
            HP_LOG_ERROR("Failed to send message: %s\n", zmq_strerror(errno));
    		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
    		++(thread->counters.code_503);
            return;
        }
    }

    /* This should never block. Fingers crossed */
	sent = hp_sendmsg(thread->out_socket, (const void *) EVBUFFER_DATA(req->input_buffer), EVBUFFER_LENGTH(req->input_buffer), ZMQ_NOBLOCK);

	if (!sent) {
		HP_LOG_ERROR("Failed to send message: %s\n", zmq_strerror(errno));
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
		++(thread->counters.code_503);
	} else {
		struct evbuffer *evb;

		evb = evbuffer_new();

		if (!evb) {
    		evhttp_send_error(req, HTTP_OK, "OK");
    		++(thread->counters.code_200);
            return;
        }

		evbuffer_add(evb, "Sent", sizeof("Sent") - 1);
		evhttp_send_reply(req, HTTP_OK, "OK", evb);
		evbuffer_free(evb);

		++(thread->counters.code_200);
	}
}

static void shutdown_httpd(struct event_base *base)
{
    struct timeval tv = { 0, 2 };

    HP_LOG_DEBUG("Terminating event loop");
    event_base_loopexit(base, &tv);
    return;
}

void hp_httpd_intercomm_cb(int fd, short event, void *args)
{
    struct hp_httpd_thread_t *thread = (struct hp_httpd_thread_t *)args;

    while (true) {
        int rc;
        uint32_t events;
        httpush_msg_t cmd;
        size_t siz = sizeof(uint32_t);

        rc = zmq_getsockopt(thread->intercomm.back, ZMQ_EVENTS, &events, &siz);
        if (rc != 0) {
            break;
        }

        if (!(events & ZMQ_POLLIN)) {
            break;
        }

        HP_LOG_DEBUG("httpd thread %d received intercomm event", thread->thread_id);

        if (hp_intercomm_recv_cmd(thread->intercomm.back, &cmd, HP_SEC_TO_MSEC(1)) == true) {
            switch (cmd) {
                case HTTPD_SHUTDOWN:
                    shutdown_httpd(thread->base);
                    return;
                break;

                case HTTPD_STATS:
                    {
                        struct hp_httpd_counters_t counters;

                        HP_LOG_DEBUG("httpd thread %d sending back stats", thread->thread_id);

                        /* Take a copy of the values */
                        counters.code_200 = thread->counters.code_200;
                        counters.code_404 = thread->counters.code_404;
                        counters.code_412 = thread->counters.code_412;
                        counters.code_503 = thread->counters.code_503;
                        counters.requests = thread->counters.requests;

                        rc = hp_sendmsg(thread->intercomm.back, (void *) &counters, sizeof(struct hp_httpd_counters_t), ZMQ_NOBLOCK);
                        assert(rc == true);
                    }
                break;

                default:
                break;
            }
        }
    }
    /* Reschedule the event */
    event_add(&(thread->intercomm_ev), NULL);
}

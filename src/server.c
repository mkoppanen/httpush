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

#define NUM_HTTPD_THREADS 10

/* Start running the thread */
static void *httpd_thread_start(void *base)
{
    event_base_dispatch((struct event_base *) base);
    return NULL;
}

static void init_intercomm_event(struct hp_httpd_thread_t *thread)
{
    int rc, fd = -1;
    size_t fd_size = sizeof(int);
    struct timeval tv = {0};

    rc = zmq_getsockopt(thread->intercomm.back, ZMQ_FD, &fd, &fd_size);
    assert(rc == 0);

    /* Create the event */
    event_set(&(thread->intercomm_ev), fd, EV_READ, hp_httpd_intercomm_cb, thread);
    event_base_set(thread->base, &(thread->intercomm_ev));

    event_add(&(thread->intercomm_ev), &tv);
}

static void *hp_create_out_socket(void *context, struct hp_uri_t **uris, size_t num_uris)
{
    void *out_socket;
    int rc;
	size_t i;

	/* the back socket, pushes to network */
	out_socket = zmq_socket(context, ZMQ_PUSH);

	if (!out_socket) {
        return NULL;
	}

    for (i = 0; i < num_uris; i++) {
        rc = zmq_setsockopt(out_socket, ZMQ_HWM, (void *) &(uris[i]->hwm), sizeof(uint64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set HWM value: %s", zmq_strerror(errno));
            return NULL;
        }

        rc = zmq_setsockopt(out_socket, ZMQ_SWAP, (void *) &(uris[i]->swap), sizeof(int64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set swap value: %s", zmq_strerror(errno));
            return NULL;
        }

        rc = zmq_setsockopt(out_socket, ZMQ_LINGER, (void *) &(uris[i]->linger), sizeof(int));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set linger value: %s", zmq_strerror(errno));
            return NULL;
        }

        HP_LOG_DEBUG("zmq_connect: %s, swap=[%" PRIi64 "], hwm=[%" PRIu64 "], linger=[%d]", \
                        uris[i]->uri, uris[i]->swap, uris[i]->hwm, uris[i]->linger);
		if (zmq_connect(out_socket, uris[i]->uri) != 0) {
			return NULL;
		}
    }
    return out_socket;
}

static void handle_monitoring_command(void *monitor_socket, zmq_pollitem_t *t_items, int num_threads)
{
    int rc, i;

    char *message;
    size_t message_len = 6;
    struct hp_httpd_counters_t counters = {0};

    HP_LOG_DEBUG("Message in monitoring socket");

    if (hp_recvmsg(monitor_socket, (void **) &message, &message_len, ZMQ_NOBLOCK) == true) {

        if (message_len < 5 || memcmp(message, "stats", 5)) {
            free(message);
            return;
        }
        free(message);

        /* Received stats message from client */
        for (i = 0; i < num_threads; i++) {
            if (hp_intercomm_send(t_items[i].socket, HTTPD_STATS) == false) {
                assert(false); // TODO: fix
            }
        }

        int received = 0;
        struct hp_httpd_counters_t thread_counters;

        while (received < num_threads) {

            rc = zmq_poll(&t_items[0], num_threads, HP_SEC_TO_MSEC(1));

            for (i = 0; i < num_threads; i++) {

                HP_LOG_DEBUG("checking thread events");

                if (t_items[i].revents & ZMQ_POLLIN) {

                    size_t t_siz = sizeof(struct hp_httpd_counters_t);

                    if (hp_recvmsg(t_items[i].socket, (void *) &thread_counters, &t_siz, ZMQ_NOBLOCK) == true &&
                        t_siz == sizeof(struct hp_httpd_counters_t)) {

                        counters.code_200 += thread_counters.code_200;
                        counters.code_404 += thread_counters.code_404;
                        counters.code_412 += thread_counters.code_412;
                        counters.code_503 += thread_counters.code_503;
                        counters.requests += thread_counters.requests;

                        received++;
                    }
                }
            }
            HP_LOG_DEBUG("received %d expecting %d", received, num_threads);
        }
    }
}

static int hp_run_parent_loop(void *monitor_socket, struct hp_httpd_thread_t *threads, int num_threads)
{
    int i, rc;
    zmq_pollitem_t m_items[1];
    zmq_pollitem_t t_items[num_threads];

    m_items[0].socket  = monitor_socket;
    m_items[0].fd      = 0;
    m_items[0].events  = ZMQ_POLLIN;
    m_items[0].revents = 0;

    for (i = 0; i < num_threads; i++) {
        t_items[i].socket  = threads[i].intercomm.front;
        t_items[i].fd      = 0;
        t_items[i].events  = ZMQ_POLLIN;
        t_items[i].revents = 0;
    }

    while (!shutting_down) {
        /* Poll the monitor socket for incoming events */
        rc = zmq_poll(&m_items[0], 1, -1);

        if (rc < 0) {
            HP_LOG_WARN("Shutting down: %s", zmq_strerror(errno));
            break;
        }

        if (rc > 0 && m_items[0].revents & ZMQ_POLLIN) {
            /* Handle command coming in from monitoring socket */
            handle_monitoring_command(monitor_socket, t_items, num_threads);
        }
    }

    for (i = 0; i < num_threads; i++) {
        if (hp_intercomm_send(threads[i].intercomm.front, HTTPD_SHUTDOWN) == false) {
            assert(false); // TODO: fix
        }

        if (pthread_join(threads[i].thread, NULL)) {
            assert(false); // TODO: fix
        }

        /* httpd related things */
        evhttp_free(threads[i].httpd);
        event_base_free(threads[i].base);

        rc = zmq_close(threads[i].intercomm.front);
        assert(rc == 0);

        rc = zmq_close(threads[i].intercomm.back);
        assert(rc == 0);

        rc = zmq_close(threads[i].out_socket);
        assert(rc == 0);
    }
    rc = zmq_close(monitor_socket);
    assert(rc == 0);

    return 0;

#if 0

                    struct hp_httpd_counters_t thread_counters;

                    if (hp_intercomm_send(threads[i].intercomm.front, HTTPD_STATS) == false) {
                        assert(false); // TODO: fix
                    }

                    rc = zmq_poll(items, 1, HP_SEC_TO_MSEC(1));

                    if (hp_recvmsg_size(threads[i].intercomm.front, (void *) &thread_counters, sizeof(struct hp_httpd_counters_t), ZMQ_NOBLOCK) == true &&
                        message_len == sizeof(struct hp_httpd_counters_t)) {
                        counters.code_200 += thread_counters->code_200;
                        counters.code_404 += thread_counters->code_404;
                        counters.code_412 += thread_counters->code_412;
                        counters.code_503 += thread_counters->code_503;
                        counters.requests += thread_counters->requests;

                        HP_LOG_DEBUG("Incremented from thread %d\n", i);
                    }
                }


            }
        }
    }



    while (true) {
        /* Shutdown event */
        if (shutting_down) {
            for (i = 0; i < num_threads; i++) {
                int rc;

                if (hp_intercomm_send(threads[i].intercomm.front, HTTPD_SHUTDOWN) == false) {
                    assert(false); // TODO: fix
                }

                if (pthread_join(threads[i].thread, NULL)) {
                    assert(false); // TODO: fix
                }

                /* httpd related things */
                evhttp_free(threads[i].httpd);
                event_base_free(threads[i].base);

                rc = zmq_close(threads[i].intercomm.front);
                assert(rc == 0);

                rc = zmq_close(threads[i].intercomm.back);
                assert(rc == 0);

                rc = zmq_close(threads[i].out_socket);
                assert(rc == 0);
            }
            rc = zmq_close(monitor_socket);
            assert(rc == 0);

            return 0;
        }

        rc = zmq_poll(items, 1, HP_SEC_TO_MSEC(1));

        if (rc < 0) {
            HP_LOG_ERROR("monitoring socket is in failed state. exiting..");
            shutting_down = 1;
        } else {
            /* Got item */
            if (items[0].revents & ZMQ_POLLIN) {
                char *message;
                size_t message_len;
                struct hp_httpd_counters_t counters = {0};

                if (hp_recvmsg(monitor_socket, (void **) &message, &message_len, ZMQ_NOBLOCK) == true) {
                    if (message_len >= 5 && !memcmp(message, "stats", 5)) {
                        for (i = 0; i < num_threads; i++) {

                            struct hp_httpd_counters_t thread_counters;

                            if (hp_intercomm_send(threads[i].intercomm.front, HTTPD_STATS) == false) {
                                assert(false); // TODO: fix
                            }

                            rc = zmq_poll(items, 1, HP_SEC_TO_MSEC(1));

                            if (hp_recvmsg_size(threads[i].intercomm.front, (void *) &thread_counters, sizeof(struct hp_httpd_counters_t), ZMQ_NOBLOCK) == true &&
                                message_len == sizeof(struct hp_httpd_counters_t)) {
                                counters.code_200 += thread_counters->code_200;
                                counters.code_404 += thread_counters->code_404;
                                counters.code_412 += thread_counters->code_412;
                                counters.code_503 += thread_counters->code_503;
                                counters.requests += thread_counters->requests;

                                HP_LOG_DEBUG("Incremented from thread %d\n", i);
                            }
                        }
                        struct evbuffer *evb = evbuffer_new();

                    	evbuffer_add_printf(evb, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
                        evbuffer_add_printf(evb, "<httpush>\n");
                    	evbuffer_add_printf(evb, "  <statistics>\n");
                    	evbuffer_add_printf(evb, "    <requests>%" PRIu64 "</requests>\n", counters.requests);
                    	evbuffer_add_printf(evb, "    <status code=\"200\">%" PRIu64 "</status>\n", counters.code_200);
                    	evbuffer_add_printf(evb, "    <status code=\"404\">%" PRIu64 "</status>\n", counters.code_404);
                    	evbuffer_add_printf(evb, "    <status code=\"412\">%" PRIu64 "</status>\n", counters.code_412);
                    	evbuffer_add_printf(evb, "    <status code=\"503\">%" PRIu64 "</status>\n", counters.code_503);
                    	evbuffer_add_printf(evb, "  </statistics>\n");
                    	evbuffer_add_printf(evb, "</httpush>\n");

                    	hp_sendmsg(monitor_socket, (const void *) EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb), ZMQ_NOBLOCK);
                        evbuffer_free(evb);
                    }
                }
            }
        }

    }
#endif
    return 1;
}

int hp_server_boostrap(struct httpush_args_t *args, int http_threads)
{
    int rc, i;
    struct hp_httpd_thread_t threads[http_threads];
    void *monitor_socket;

    for (i = 0; i < http_threads; i++) {

        // init
        memset(&(threads[i]), 0, sizeof(struct hp_httpd_thread_t));
        memset(&(threads[i].counters), 0, sizeof(struct hp_httpd_counters_t));

        threads[i].thread_id = i;

        /* zmq */
        threads[i].out_socket = hp_create_out_socket(args->ctx, args->uris, args->num_uris);
        threads[i].include_headers = args->include_headers;

        /* libevent */
        threads[i].base = event_init();
        assert(threads[i].base); // TODO: fix

        threads[i].httpd = evhttp_new(threads[i].base);
        assert(threads[i].httpd); // TODO: fix

        /* Specific action for displaying stats */
    	evhttp_set_cb(threads[i].httpd, "/reflect", hp_httpd_reflect_request, &(threads[i]));

    	/* Catch all */
    	evhttp_set_gencb(threads[i].httpd, hp_httpd_publish_message, &(threads[i]));

    	rc = evhttp_accept_socket(threads[i].httpd, args->fd);
        assert(rc == 0); // TODO: fix

        /* A pair socket to communicate with the master */
        if (hp_create_pair(args->ctx, &(threads[i].intercomm), i) == false) {
            return 1;
        }

        /* Start listening on intercomm */
        init_intercomm_event(&(threads[i]));

        // Start the thread
        if (pthread_create(&(threads[i].thread), NULL, httpd_thread_start, threads[i].base)) {
            assert(false); // TODO: fix
        }
    }

    /* Monitoring the threads */
    monitor_socket = zmq_socket(args->ctx, ZMQ_REP);
    assert(monitor_socket);

    rc = zmq_bind(monitor_socket, "tcp://*:5567");
    assert(rc == 0);

    // Got threads running, poll to see if they exit
    return hp_run_parent_loop(monitor_socket, threads, http_threads);
}





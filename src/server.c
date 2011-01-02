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

/* Start running the thread */
static void *hp_httpd_thread_start(void *base) {
    event_base_dispatch((struct event_base *) base);
    return NULL;
}

static bool hp_init_intercomm_event(struct hp_httpd_thread_t *thread) {
    int rc, fd = -1;
    size_t fd_size = sizeof (int);
    struct timeval tv = {0};

    rc = zmq_getsockopt(thread->intercomm.back, ZMQ_FD, &fd, &fd_size);
    if (rc != 0) {
        HP_LOG_ERROR("Failed to acquire thread file descriptor");
        return false;
    }

    /* Create the event */
    event_set(&(thread->intercomm_ev), fd, EV_READ, hp_httpd_intercomm_cb, thread);
    event_base_set(thread->base, &(thread->intercomm_ev));

    event_add(&(thread->intercomm_ev), &tv);
    return true;
}

/*
 if type is ZMQ_PUSH then the socket will connect
 on all other socket types bind
 */
static void *hp_create_socket(void *context, struct hp_uri_t **uris, size_t num_uris, int type) {
    void *socket;
    int rc;
    size_t i;

    socket = zmq_socket(context, type);

    if (!socket) {
        return NULL;
    }

    for (i = 0; i < num_uris; i++) {
        rc = zmq_setsockopt(socket, ZMQ_HWM, (void *) &(uris[i]->hwm), sizeof (uint64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set HWM value: %s", zmq_strerror(errno));
            return NULL;
        }

        rc = zmq_setsockopt(socket, ZMQ_SWAP, (void *) &(uris[i]->swap), sizeof (int64_t));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set swap value: %s", zmq_strerror(errno));
            return NULL;
        }

        rc = zmq_setsockopt(socket, ZMQ_LINGER, (void *) &(uris[i]->linger), sizeof (int));
        if (rc != 0) {
            HP_LOG_ERROR("Failed to set linger value: %s", zmq_strerror(errno));
            return NULL;
        }

        HP_LOG_DEBUG("(%s) %s, swap=[%" PRIi64 "], hwm=[%" PRIu64 "], linger=[%d]", (type == ZMQ_PUSH ? "connect" : "bind"),
                uris[i]->uri, uris[i]->swap, uris[i]->hwm, uris[i]->linger);

        /* Connect push sockets and bind all other sockets */
        if (type == ZMQ_PUSH) {
            rc = zmq_connect(socket, uris[i]->uri);
        } else {
            rc = zmq_bind(socket, uris[i]->uri);
        }

        if (rc != 0) {
            (void) zmq_close(socket);
            return NULL;
        }
    }
    return socket;
}

static bool hp_handle_monitoring_command(void *monitor_socket, zmq_pollitem_t *t_items, int num_threads) {
    int rc, i;
    httpush_msg_t cmd;
    bool retval = false;

    char identity[255];
    size_t identity_size = 255;

    HP_LOG_DEBUG("Message in monitoring socket");

    if (hp_monitor_recv_cmd(monitor_socket, identity, &identity_size, &cmd) == true && cmd == MONITOR_STATS) {

        struct evbuffer *evb;

        int retries = 0, received = 0;
        struct hp_httpd_counters_t sum = {0};

        /* Send message to threads saying that they should return stats */
        for (i = 0; i < num_threads; i++) {
            if (hp_intercomm_send(t_items[i].socket, HTTPD_STATS) == false) {
                HP_LOG_WARN("Failed to request statistics from thread %d", i);
            }
        }

        while ((received < num_threads) && (retries < 5)) {

            rc = zmq_poll(&t_items[0], num_threads, HP_SEC_TO_MSEC(1));

            if (rc < 0) {
                shutting_down = 1;
                break;
            }

            if (rc == 0) {
                ++retries;
                continue;
            }

            for (i = 0; i < num_threads; i++) {

                if (t_items[i].revents & ZMQ_POLLIN) {
                    struct hp_httpd_counters_t *thread_counter;
                    size_t msiz = sizeof (struct hp_httpd_counters_t);

                    if (hp_recvmsg(t_items[i].socket, (void **) &thread_counter, &msiz, 0) == true) {

                        if (msiz == sizeof (struct hp_httpd_counters_t)) {
                            sum.code_200 += thread_counter->code_200;
                            sum.code_404 += thread_counter->code_404;
                            sum.code_412 += thread_counter->code_412;
                            sum.code_503 += thread_counter->code_503;
                            sum.requests += thread_counter->requests;
                            ++received;
                        }
                        free(thread_counter);
                    }
                }
            }
            HP_LOG_DEBUG("received %d expecting %d", received, num_threads);
        }

        evb = evbuffer_new();
        assert(evb);

        evbuffer_add_printf(evb, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
        evbuffer_add_printf(evb, "<httpush>\n");
        evbuffer_add_printf(evb, "  <statistics>\n");
        evbuffer_add_printf(evb, "    <threads>%d</threads>\n", num_threads);
        evbuffer_add_printf(evb, "    <responses>%d</responses>\n", received);
        evbuffer_add_printf(evb, "    <requests>%" PRIu64 "</requests>\n", sum.requests);
        evbuffer_add_printf(evb, "    <status code=\"200\">%" PRIu64 "</status>\n", sum.code_200);
        evbuffer_add_printf(evb, "    <status code=\"404\">%" PRIu64 "</status>\n", sum.code_404);
        evbuffer_add_printf(evb, "    <status code=\"412\">%" PRIu64 "</status>\n", sum.code_412);
        evbuffer_add_printf(evb, "    <status code=\"503\">%" PRIu64 "</status>\n", sum.code_503);
        evbuffer_add_printf(evb, "  </statistics>\n");
        evbuffer_add_printf(evb, "</httpush>\n");

        retval = hp_sendmsg_ident(monitor_socket, identity, identity_size, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
        evbuffer_free(evb);
    }
    return retval;
}

static bool hp_free_threads(struct hp_httpd_thread_t *threads, int num_threads) {
    int i, rc;
    bool success = true;

    for (i = 0; i < num_threads; i++) {
        if (hp_intercomm_send(threads[i].intercomm.front, HTTPD_SHUTDOWN) == false) {
            HP_LOG_ERROR("Failed to request thread id %d to terminate: %s", threads[i].thread_id, zmq_strerror(errno));
            success = false;
            continue;
        }

        if (pthread_join(threads[i].thread, NULL)) {
            HP_LOG_ERROR("Failed to join thread id %d: %s", threads[i].thread_id, strerror(errno));
            success = false;
            continue;
        }

        /* httpd related things */
        evhttp_free(threads[i].httpd);
        event_base_free(threads[i].base);

        if (hp_close_pair(&(threads[i].intercomm)) == false) {
            HP_LOG_ERROR("Failed to close thread %d intercomm", threads[i].thread_id);
            success = false;
        }

        rc = zmq_close(threads[i].out_socket);
        if (rc != 0) {
            HP_LOG_ERROR("Failed to close thread id %d intercomm out socket", threads[i].thread_id);
            success = false;
        }
    }
    return success;
}

static int hp_run_parent_loop(void *monitor_socket, struct hp_httpd_thread_t *threads, int num_threads) {
    int i, rc;
    zmq_pollitem_t m_items[1];
    zmq_pollitem_t t_items[num_threads];

    m_items[0].socket = monitor_socket;
    m_items[0].fd = 0;
    m_items[0].events = ZMQ_POLLIN;
    m_items[0].revents = 0;

    for (i = 0; i < num_threads; i++) {
        t_items[i].socket = threads[i].intercomm.front;
        t_items[i].fd = 0;
        t_items[i].events = ZMQ_POLLIN;
        t_items[i].revents = 0;
    }

    while (!shutting_down) {
        /* Poll the monitor socket for incoming events */
        rc = zmq_poll(&m_items[0], 1, -1);

        if (rc < 0) {
            HP_LOG_WARN("Shutting down: %s", zmq_strerror(errno));
            break;
        }

        if (rc > 0 && (m_items[0].revents & ZMQ_POLLIN)) {
            /* Handle command coming in from monitoring socket */
            if (hp_handle_monitoring_command(monitor_socket, &t_items[0], num_threads) == false) {
                HP_LOG_WARN("monitoring command failed");
            }
        }
    }

    if (hp_free_threads(threads, num_threads) == false) {
        HP_LOG_ERROR("Thread termination failed. The process is likely to hang");
    }

    rc = zmq_close(monitor_socket);
    assert(rc == 0);

    return 0;
}

static bool hp_thread_init_events(struct hp_httpd_thread_t *thread, int fd) {
    int rc;

    /* libevent */
    thread->base = event_init();
    if (!thread->base)
        return false;

    thread->httpd = evhttp_new(thread->base);
    if (!thread->httpd) {
        event_base_free(thread->base);
        return false;
    }

    /* Specific action for displaying back data */
    evhttp_set_cb(thread->httpd, "/reflect", hp_httpd_reflect_request, thread);

    /* Catch all */
    evhttp_set_gencb(thread->httpd, hp_httpd_publish_message, thread);

    rc = evhttp_accept_socket(thread->httpd, fd);
    if (rc != 0) {
        evhttp_free(thread->httpd);
        event_base_free(thread->base);
        return false;
    }

    /* Start listening on intercomm */
    if (hp_init_intercomm_event(thread) == false) {
        evhttp_free(thread->httpd);
        event_base_free(thread->base);
        return false;
    }
    return true;
}

/*
 Returns the number of threads successfully initialized

 */
static int hp_init_threads(struct httpush_args_t *args, struct hp_httpd_thread_t *threads, int num_threads) {
    int i, initialized = 0;

    /* Run a loop an initialize sockets */
    for (i = 0; i < num_threads; i++) {

        /* init */
        memset(&(threads[i]), 0, sizeof (struct hp_httpd_thread_t));

        threads[i].thread_id = i;
        threads[i].include_headers = args->include_headers;

        /* init outgoing socket */
        threads[i].out_socket = hp_create_socket(args->ctx, args->uris, args->num_uris, ZMQ_PUSH);
        if (!threads[i].out_socket) {
            HP_LOG_ERROR("Failed to create out_socket for thread id %d", i);
            break;
        }

        /* A pair socket to communicate with the master */
        if (hp_create_pair(args->ctx, &(threads[i].intercomm), i) == false) {
            HP_LOG_ERROR("Failed to create pair for thread id %d", i);
            (void) zmq_close(threads[i].out_socket);
            break;
        }

        if (hp_thread_init_events(&(threads[i]), args->fd) == false) {
            HP_LOG_ERROR("Failed to create init event loop for thread %d", i);
            (void) zmq_close(threads[i].out_socket);
            (void) hp_close_pair(&(threads[i].intercomm));
            break;
        }

        /* Start the thread */
        if (pthread_create(&(threads[i].thread), NULL, hp_httpd_thread_start, threads[i].base)) {
            HP_LOG_ERROR("Failed to create launch thread id %d", i);
            (void) zmq_close(threads[i].out_socket);
            (void) hp_close_pair(&(threads[i].intercomm));
            break;
        }
        ++initialized;
    }

    HP_LOG_DEBUG("Initialized %d/%d threads", initialized, num_threads);
    return initialized;
}

int hp_server_boostrap(struct httpush_args_t *args, int num_threads) {
    int rc;
    struct hp_httpd_thread_t threads[num_threads];
    void *monitor_socket;

    rc = hp_init_threads(args, threads, num_threads);
    if (rc != num_threads) {
        HP_LOG_ERROR("Freeing threads");

        hp_free_threads(threads, rc);

        return 1;
    }

    /* Monitoring the threads */
    monitor_socket = hp_create_socket(args->ctx, args->m_uris, args->num_m_uris, ZMQ_XREP);
    assert(monitor_socket);

    /* Got threads running, poll to see if they exit */
    return hp_run_parent_loop(monitor_socket, threads, num_threads);
}

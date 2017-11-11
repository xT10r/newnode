#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "utp.h"
#include "utp_bufferevent.h"
#include "log.h"


typedef struct bufferevent bufferevent;
typedef struct evbuffer evbuffer;


// utp_read > bev_output > other_fd_recv
// other_fd_send > bev_input > utp_write

typedef struct {
    utp_socket *utp;
    bool utp_read_shutdown:1;
    bufferevent *bev;
    bufferevent *other_bev;
} utp_bufferevent;


void ubev_cleanup(utp_bufferevent *u)
{
    if (u->utp || u->bev) {
        return;
    }
    free(u);
}

void ubev_utp_close(utp_bufferevent *u)
{
    utp_set_userdata(u->utp, NULL);
    utp_close(u->utp);
    u->utp = NULL;
    if (u->other_bev) {
        bufferevent_decref(u->other_bev);
        u->other_bev = NULL;
    }
}

void ubev_bev_close(utp_bufferevent *u)
{
    debug("ubev_bev_close %p\n", u);
    assert(!bufferevent_get_enabled(u->bev));
    assert(!evbuffer_get_length(bufferevent_get_input(u->bev)));
    assert(!evbuffer_get_length(bufferevent_get_output(u->bev)));
    bufferevent_free(u->bev);
    u->bev = NULL;
}

void ubev_bev_graceful_close(utp_bufferevent *u)
{
    if (u->bev) {
        evbuffer_clear(bufferevent_get_input(u->bev));
        bufferevent_disable(u->bev, EV_READ);
        if (!evbuffer_get_length(bufferevent_get_output(u->bev))) {
            bufferevent_disable(u->bev, EV_WRITE);
            ubev_bev_close(u);
        }
    }
    ubev_cleanup(u);
}

void utp_bufferevent_flush(utp_bufferevent *u)
{
    evbuffer *in = bufferevent_get_input(u->bev);
    while (evbuffer_get_length(in)) {
        // the libutp interface for write is Very Broken.
        ssize_t len = MIN(1500, evbuffer_get_length(in));
        unsigned char *buf = evbuffer_pullup(in, len);
        ssize_t r = utp_write(u->utp, buf, len);
        if (r < 0) {
            fprintf(stderr, "utp_write failed\n");
            ubev_utp_close(u);
            ubev_bev_graceful_close(u);
            return;
        }
        if (!r) {
            break;
        }
        evbuffer_drain(in, r);
    }
    if (!(bufferevent_get_enabled(u->bev) & EV_READ) && !evbuffer_get_length(in)) {
        if (!(bufferevent_get_enabled(u->bev) & EV_WRITE)) {
            ubev_utp_close(u);
            ubev_bev_close(u);
            ubev_cleanup(u);
            return;
        }
        utp_shutdown(u->utp, SHUT_WR);
        return;
    }
}

uint64 utp_on_error(utp_callback_arguments *a)
{
    debug("utp error: %s\n", utp_error_code_names[a->error_code]);
    utp_bufferevent *u = (utp_bufferevent*)utp_get_userdata(a->socket);
    if (u) {
        ubev_utp_close(u);
        ubev_bev_graceful_close(u);
    }
    return 0;
}

uint64 utp_on_read(utp_callback_arguments *a)
{
    utp_bufferevent *u = (utp_bufferevent*)utp_get_userdata(a->socket);
    if (u->bev && bufferevent_get_enabled(u->bev) & EV_WRITE) {
        //debug("writing utp>bev %d bytes\n", a->len);
        bufferevent_write(u->bev, a->buf, a->len);
    }
    return 0;
}

void ubev_bev_stop_writing(utp_bufferevent *u)
{
    if (bufferevent_get_enabled(u->bev) & EV_READ) {
        bufferevent_disable(u->bev, EV_WRITE);
        shutdown(bufferevent_getfd(u->bev), SHUT_WR);
        return;
    }
    assert(!evbuffer_get_length(bufferevent_get_input(u->bev)));
    utp_bufferevent_flush(u);
}

uint64 utp_on_state_change(utp_callback_arguments *a)
{
    utp_bufferevent *u = (utp_bufferevent*)utp_get_userdata(a->socket);
    if (a->state != UTP_STATE_WRITABLE) {
        debug("state %d: %s\n", a->state, utp_state_names[a->state]);
    }

    switch (a->state) {
    case UTP_STATE_CONNECT:
        if (u->other_bev) {
            bufferevent_event_cb event_cb;
            void *d;
            bufferevent_getcb(u->other_bev, NULL, NULL, &event_cb, &d);
            if (event_cb) {
                event_cb(u->other_bev, BEV_EVENT_CONNECTED, d);
            }
            bufferevent_decref(u->other_bev);
            u->other_bev = NULL;
        }
    case UTP_STATE_WRITABLE:
        if (u->bev) {
            utp_bufferevent_flush(u);
        }
        break;
    case UTP_STATE_EOF:
        u->utp_read_shutdown = true;
        if (!evbuffer_get_length(bufferevent_get_output(u->bev))) {
            ubev_bev_stop_writing(u);
        }
        break;
    case UTP_STATE_DESTROYING: {
        utp_socket_stats *stats = utp_get_stats(a->socket);
        if (stats) {
            debug("Socket Statistics:\n");
            debug("    Bytes sent:          %d\n", stats->nbytes_xmit);
            debug("    Bytes received:      %d\n", stats->nbytes_recv);
            debug("    Packets received:    %d\n", stats->nrecv);
            debug("    Packets sent:        %d\n", stats->nxmit);
            debug("    Duplicate receives:  %d\n", stats->nduprecv);
            debug("    Retransmits:         %d\n", stats->rexmit);
            debug("    Fast Retransmits:    %d\n", stats->fastrexmit);
            debug("    Best guess at MTU:   %d\n", stats->mtu_guess);
        }
        break;
    }
    }

    return 0;
}

void ubev_read_cb(bufferevent *bev, void *ctx)
{
    //debug("ubev_read_cb %p\n", ctx);
    utp_bufferevent* u = (utp_bufferevent*)ctx;
    assert(u->utp);
    utp_bufferevent_flush(u);
}

void ubev_write_cb(struct bufferevent *bev, void *ctx)
{
    //debug("ubev_write_cb %p\n", ctx);
    utp_bufferevent* u = (utp_bufferevent*)ctx;
    // the output buffer is flushed
    assert(!evbuffer_get_length(bufferevent_get_output(u->bev)));
    if (!u->utp) {
        ubev_bev_close(u);
        ubev_cleanup(u);
        return;
    }
    utp_read_drained(u->utp);
}

void ubev_event_cb(struct bufferevent *bev, short events, void *ctx)
{
    debug("ubev_event_cb %p %x\n", ctx, events);
    utp_bufferevent* u = (utp_bufferevent*)ctx;
    assert(u->bev == bev);
    if (events & BEV_EVENT_ERROR) {
        if (u->utp) {
            if (!evbuffer_get_length(bufferevent_get_input(u->bev))) {
                u->utp_read_shutdown = true;
                utp_shutdown(u->utp, SHUT_RD);
                return;
            }
            ubev_utp_close(u);
        }
        ubev_bev_close(u);
        ubev_cleanup(u);
    } else if (events & BEV_EVENT_EOF) {
        if (events & BEV_EVENT_WRITING) {
            if (!(bufferevent_get_enabled(bev) & EV_READ)) {
                if (u->utp) {
                    ubev_utp_close(u);
                }
                ubev_bev_close(u);
                ubev_cleanup(u);
                return;
            }
            if (u->utp) {
                u->utp_read_shutdown = true;
                utp_shutdown(u->utp, SHUT_RD);
            }
            evbuffer_clear(bufferevent_get_output(bev));
        }
        if (events & BEV_EVENT_READING) {
            if (!u->utp_read_shutdown) {
                utp_shutdown(u->utp, SHUT_WR);
            } else {
                ubev_utp_close(u);
                if (!evbuffer_get_length(bufferevent_get_output(u->bev))) {
                    ubev_bev_close(u);
                    ubev_cleanup(u);
                    return;
                }
            }
        }
    }
}

utp_bufferevent* utp_bufferevent_new(event_base *base, utp_socket *s, int fd)
{
    utp_bufferevent *u = alloc(utp_bufferevent);
    u->utp = s;
    utp_set_userdata(s, u);
    u->bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!u->bev) {
        ubev_utp_close(u);
        ubev_cleanup(u);
        return NULL;
    }
    bufferevent_setcb(u->bev, ubev_read_cb, ubev_write_cb, ubev_event_cb, u);
    bufferevent_enable(u->bev, EV_READ);
    return u;
}

int utp_socket_create_fd(event_base *base, utp_socket *s)
{
    int fds[2];
    socketpair(PF_LOCAL, SOCK_STREAM, 0, fds);
    evutil_make_socket_closeonexec(fds[0]);
    evutil_make_socket_nonblocking(fds[0]);
    utp_bufferevent *u = utp_bufferevent_new(base, s, fds[0]);
    if (!u) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    return fds[1];
}

bufferevent* utp_socket_create_bev(event_base *base, utp_socket *s)
{
    int fds[2];
    socketpair(PF_LOCAL, SOCK_STREAM, 0, fds);
    evutil_make_socket_closeonexec(fds[0]);
    evutil_make_socket_nonblocking(fds[0]);
    utp_bufferevent *u = utp_bufferevent_new(base, s, fds[0]);
    if (!u) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    evutil_make_socket_closeonexec(fds[1]);
    evutil_make_socket_nonblocking(fds[1]);
    u->other_bev = bufferevent_socket_new(base, fds[1], BEV_OPT_CLOSE_ON_FREE);
    bufferevent_incref(u->other_bev);
    return u->other_bev;
}

void utp_connect_tcp(event_base *base, utp_socket *s, const sockaddr *address, socklen_t address_len)
{
    utp_bufferevent *u = utp_bufferevent_new(base, s, -1);
    if (bufferevent_socket_connect(u->bev, address, address_len) < 0) {
        bufferevent_free(u->bev);
        u->bev = NULL;
        ubev_utp_close(u);
        ubev_cleanup(u);
        fprintf(stderr, "bufferevent_socket_connect failed");
    }
}

#define PY_SSIZE_T_CLEAN
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <Python.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usrsctp.h>

#ifndef MSG_EOR
#define MSG_EOR 0x80
#endif

/*
 * CPython glue around usrsctp for aiortc DataChannels.
 *
 * Design constraints:
 *   - AF_CONN: ICE / DTLS remains owned by aiortc.
 *   - usrsctp_init_nothreads(): asyncio drives usrsctp_handle_timers().
 *   - callbacks only append C-owned queue nodes; Python objects are created
 *     later by Association.poll().
 */

typedef struct packet_node {
    struct packet_node *next;
    size_t len;
    unsigned char data[1];
} packet_node;

typedef struct message_node {
    struct message_node *next;
    uint16_t sid;
    uint32_t ppid;
    size_t len;
    unsigned char data[1];
} message_node;

typedef enum {
    EVENT_ASSOC = 1,
    EVENT_RESET = 2,
    EVENT_SHUTDOWN = 3,
    EVENT_SEND_FAILED = 4
} event_kind;

typedef struct event_node {
    struct event_node *next;
    event_kind kind;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint16_t count;
    uint16_t streams[1];
} event_node;

typedef struct {
    PyObject_HEAD
    struct socket *sock;
    int registered;
    int closed;
    int writable;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t mtu;

    packet_node *out_head;
    packet_node *out_tail;
    message_node *msg_head;
    message_node *msg_tail;
    event_node *event_head;
    event_node *event_tail;

    unsigned char *partial;
    size_t partial_len;
    size_t partial_cap;
    uint16_t partial_sid;
    uint32_t partial_ppid;
    int partial_active;
} AssociationObject;

static int g_initialized = 0;
static uint32_t g_sendspace = 4U * 1024U * 1024U;
static uint32_t g_recvspace = 4U * 1024U * 1024U;
static uint32_t g_sack_freq = 2;
static uint32_t g_delayed_sack_ms = 20;

static void debug_printf(const char *format, ...) {
    (void)format;
}

static void free_packets(AssociationObject *self) {
    packet_node *n = self->out_head;
    while (n != NULL) {
        packet_node *next = n->next;
        free(n);
        n = next;
    }
    self->out_head = self->out_tail = NULL;
}

static void free_messages(AssociationObject *self) {
    message_node *n = self->msg_head;
    while (n != NULL) {
        message_node *next = n->next;
        free(n);
        n = next;
    }
    self->msg_head = self->msg_tail = NULL;
}

static void free_events(AssociationObject *self) {
    event_node *n = self->event_head;
    while (n != NULL) {
        event_node *next = n->next;
        free(n);
        n = next;
    }
    self->event_head = self->event_tail = NULL;
}

static int append_packet(AssociationObject *self, const void *data, size_t len) {
    packet_node *n = (packet_node *)malloc(sizeof(packet_node) + (len ? len - 1 : 0));
    if (n == NULL) {
        return -1;
    }
    n->next = NULL;
    n->len = len;
    if (len) {
        memcpy(n->data, data, len);
    }
    if (self->out_tail) {
        self->out_tail->next = n;
    } else {
        self->out_head = n;
    }
    self->out_tail = n;
    return 0;
}

static int append_message(AssociationObject *self, uint16_t sid, uint32_t ppid,
                          const void *data, size_t len) {
    message_node *n = (message_node *)malloc(sizeof(message_node) + (len ? len - 1 : 0));
    if (n == NULL) {
        return -1;
    }
    n->next = NULL;
    n->sid = sid;
    n->ppid = ppid;
    n->len = len;
    if (len) {
        memcpy(n->data, data, len);
    }
    if (self->msg_tail) {
        self->msg_tail->next = n;
    } else {
        self->msg_head = n;
    }
    self->msg_tail = n;
    return 0;
}

static event_node *new_event(event_kind kind, uint16_t count) {
    size_t extra = count > 0 ? (size_t)(count - 1) * sizeof(uint16_t) : 0;
    event_node *n = (event_node *)calloc(1, sizeof(event_node) + extra);
    if (n != NULL) {
        n->kind = kind;
        n->count = count;
    }
    return n;
}

static void append_event_node(AssociationObject *self, event_node *n) {
    if (n == NULL) {
        return;
    }
    n->next = NULL;
    if (self->event_tail) {
        self->event_tail->next = n;
    } else {
        self->event_head = n;
    }
    self->event_tail = n;
}

static int partial_append(AssociationObject *self, const void *data, size_t len,
                          uint16_t sid, uint32_t ppid, int eor) {
    if (!self->partial_active || self->partial_sid != sid || self->partial_ppid != ppid) {
        if (self->partial_active && self->partial_len) {
            if (append_message(self, self->partial_sid, self->partial_ppid,
                               self->partial, self->partial_len) < 0) {
                return -1;
            }
        }
        self->partial_active = 1;
        self->partial_sid = sid;
        self->partial_ppid = ppid;
        self->partial_len = 0;
    }

    if (self->partial_len + len > self->partial_cap) {
        size_t new_cap = self->partial_cap ? self->partial_cap : 4096;
        while (new_cap < self->partial_len + len) {
            if (new_cap > (SIZE_MAX / 2)) {
                return -1;
            }
            new_cap *= 2;
        }
        unsigned char *tmp = (unsigned char *)realloc(self->partial, new_cap);
        if (tmp == NULL) {
            return -1;
        }
        self->partial = tmp;
        self->partial_cap = new_cap;
    }

    if (len) {
        memcpy(self->partial + self->partial_len, data, len);
        self->partial_len += len;
    }

    if (eor) {
        if (append_message(self, sid, ppid, self->partial, self->partial_len) < 0) {
            return -1;
        }
        self->partial_active = 0;
        self->partial_len = 0;
    }
    return 0;
}

static int outbound_packet_cb(void *addr, void *buffer, size_t length,
                              uint8_t tos, uint8_t set_df) {
    AssociationObject *self = (AssociationObject *)addr;
    (void)tos;
    (void)set_df;
    if (self == NULL || self->closed) {
        return 0;
    }
    return append_packet(self, buffer, length) == 0 ? 0 : ENOBUFS;
}

static void handle_notification(AssociationObject *self, void *data, size_t datalen) {
    union sctp_notification *n = (union sctp_notification *)data;
    if (datalen < sizeof(n->sn_header) || n->sn_header.sn_length > datalen) {
        return;
    }

    switch (n->sn_header.sn_type) {
    case SCTP_ASSOC_CHANGE: {
        struct sctp_assoc_change *sac = &n->sn_assoc_change;
        event_node *ev = new_event(EVENT_ASSOC, 0);
        if (ev) {
            ev->a = sac->sac_state;
            ev->b = sac->sac_outbound_streams;
            ev->c = sac->sac_inbound_streams;
            append_event_node(self, ev);
        }
        break;
    }
    case SCTP_STREAM_RESET_EVENT: {
        struct sctp_stream_reset_event *rst = &n->sn_strreset_event;
        size_t base = sizeof(struct sctp_stream_reset_event);
        uint16_t count = 0;
        if (rst->strreset_length >= base) {
            count = (uint16_t)((rst->strreset_length - base) / sizeof(uint16_t));
        }
        event_node *ev = new_event(EVENT_RESET, count);
        if (ev) {
            ev->a = rst->strreset_flags;
            for (uint16_t i = 0; i < count; ++i) {
                ev->streams[i] = rst->strreset_stream_list[i];
            }
            append_event_node(self, ev);
        }
        break;
    }
    case SCTP_SHUTDOWN_EVENT:
        append_event_node(self, new_event(EVENT_SHUTDOWN, 0));
        break;
#ifdef SCTP_SEND_FAILED_EVENT
    case SCTP_SEND_FAILED_EVENT:
        append_event_node(self, new_event(EVENT_SEND_FAILED, 0));
        break;
#endif
    default:
        break;
    }
}

static int receive_cb(struct socket *sock, union sctp_sockstore addr, void *data,
                      size_t datalen, struct sctp_rcvinfo rcv, int flags,
                      void *ulp_info) {
    AssociationObject *self = (AssociationObject *)ulp_info;
    (void)sock;
    (void)addr;

    if (self == NULL) {
        if (data) free(data);
        return 1;
    }

    if (data == NULL) {
        append_event_node(self, new_event(EVENT_SHUTDOWN, 0));
        return 1;
    }

    if (flags & MSG_NOTIFICATION) {
        handle_notification(self, data, datalen);
    } else {
        uint32_t ppid = ntohl(rcv.rcv_ppid);
        if (partial_append(self, data, datalen, rcv.rcv_sid, ppid,
                           (flags & MSG_EOR) != 0) < 0) {
            free(data);
            return 0;
        }
    }

    free(data);
    return 1;
}

static int send_cb(struct socket *sock, uint32_t sb_free, void *ulp_info) {
    AssociationObject *self = (AssociationObject *)ulp_info;
    (void)sock;
    (void)sb_free;
    if (self != NULL) {
        self->writable = 1;
    }
    /* usrsctp / historical WebRTC integrations return 0 here. */
    return 0;
}

static int ensure_initialized(void) {
    if (g_initialized) {
        return 0;
    }
    usrsctp_init_nothreads(0, outbound_packet_cb, debug_printf);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_sysctl_set_sctp_sendspace(g_sendspace);
    usrsctp_sysctl_set_sctp_recvspace(g_recvspace);
    usrsctp_sysctl_set_sctp_sack_freq_default(g_sack_freq);
    usrsctp_sysctl_set_sctp_delayed_sack_time_default(g_delayed_sack_ms);
    g_initialized = 1;
    return 0;
}

static int enable_event(struct socket *sock, uint16_t type) {
    struct sctp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.se_assoc_id = SCTP_ALL_ASSOC;
    ev.se_type = type;
    ev.se_on = 1;
    return usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof(ev));
}

static int configure_socket(AssociationObject *self,
                            uint16_t outbound_streams, uint16_t inbound_streams) {
    int on = 1;
    struct linger linger_opt;
    struct sctp_assoc_value reset;
    struct sctp_initmsg initmsg;

    if (usrsctp_set_non_blocking(self->sock, 1) < 0) return -1;

    memset(&linger_opt, 0, sizeof(linger_opt));
    linger_opt.l_onoff = 1;
    linger_opt.l_linger = 0;
    if (usrsctp_setsockopt(self->sock, SOL_SOCKET, SO_LINGER,
                           &linger_opt, sizeof(linger_opt)) < 0) return -1;

    if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_NODELAY,
                           &on, sizeof(on)) < 0) return -1;
    if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_EXPLICIT_EOR,
                           &on, sizeof(on)) < 0) return -1;
    if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_RECVRCVINFO,
                           &on, sizeof(on)) < 0) return -1;

    /* Keep partial delivery non-interleaved.  The bridge deliberately
       reassembles one user message at a time before crossing into Python. */
    {
        int fragment_interleave = 0;
        if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE,
                               &fragment_interleave, sizeof(fragment_interleave)) < 0) return -1;
    }

    memset(&reset, 0, sizeof(reset));
    reset.assoc_id = SCTP_ALL_ASSOC;
    reset.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ |
                        SCTP_ENABLE_RESET_ASSOC_REQ |
                        SCTP_ENABLE_CHANGE_ASSOC_REQ;
    if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET,
                           &reset, sizeof(reset)) < 0) return -1;

    memset(&initmsg, 0, sizeof(initmsg));
    initmsg.sinit_num_ostreams = outbound_streams;
    initmsg.sinit_max_instreams = inbound_streams;
    if (usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_INITMSG,
                           &initmsg, sizeof(initmsg)) < 0) return -1;

    if (enable_event(self->sock, SCTP_ASSOC_CHANGE) < 0) return -1;
    if (enable_event(self->sock, SCTP_STREAM_RESET_EVENT) < 0) return -1;
    if (enable_event(self->sock, SCTP_SHUTDOWN_EVENT) < 0) return -1;
    if (enable_event(self->sock, SCTP_SENDER_DRY_EVENT) < 0) return -1;
#ifdef SCTP_SEND_FAILED_EVENT
    if (enable_event(self->sock, SCTP_SEND_FAILED_EVENT) < 0) return -1;
#endif
    return 0;
}

static int configure_path_mtu(AssociationObject *self) {
    struct sockaddr_conn remote;
    struct sctp_paddrparams p;

    memset(&remote, 0, sizeof(remote));
    remote.sconn_family = AF_CONN;
#ifdef HAVE_SCONN_LEN
    remote.sconn_len = sizeof(remote);
#endif
    remote.sconn_port = htons(self->remote_port);
    remote.sconn_addr = self;

    memset(&p, 0, sizeof(p));
    memcpy(&p.spp_address, &remote, sizeof(remote));
    p.spp_assoc_id = SCTP_CURRENT_ASSOC;
    p.spp_flags = SPP_PMTUD_DISABLE;
    /* Match the historical WebRTC/usrsctp integration: this value is the
       complete SCTP packet ceiling passed through the DTLS transport. */
    p.spp_pathmtu = self->mtu;
    return usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS,
                              &p, sizeof(p));
}

static PyObject *Association_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    AssociationObject *self = (AssociationObject *)type->tp_alloc(type, 0);
    if (self) {
        memset(((char *)self) + sizeof(PyObject), 0,
               sizeof(AssociationObject) - sizeof(PyObject));
        self->writable = 1;
    }
    return (PyObject *)self;
}

static int Association_init(AssociationObject *self, PyObject *args, PyObject *kwds) {
    unsigned int local_port = 5000, remote_port = 5000, mtu = 1200;
    unsigned int outbound_streams = 65535, inbound_streams = 65535;
    static char *kwlist[] = {
        "local_port", "remote_port", "mtu", "outbound_streams", "inbound_streams", NULL
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "II|III", kwlist,
                                     &local_port, &remote_port, &mtu,
                                     &outbound_streams, &inbound_streams)) {
        return -1;
    }
    if (local_port > 65535 || remote_port > 65535 ||
        outbound_streams > 65535 || inbound_streams > 65535) {
        PyErr_SetString(PyExc_ValueError, "port / stream count out of range");
        return -1;
    }

    ensure_initialized();
    self->local_port = (uint16_t)local_port;
    self->remote_port = (uint16_t)remote_port;
    self->mtu = mtu;

    self->sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                                receive_cb, send_cb, g_sendspace / 2U, self);
    if (self->sock == NULL) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    if (configure_socket(self, (uint16_t)outbound_streams,
                         (uint16_t)inbound_streams) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        usrsctp_close(self->sock);
        self->sock = NULL;
        return -1;
    }

    usrsctp_register_address(self);
    self->registered = 1;

    struct sockaddr_conn local;
    memset(&local, 0, sizeof(local));
    local.sconn_family = AF_CONN;
#ifdef HAVE_SCONN_LEN
    local.sconn_len = sizeof(local);
#endif
    local.sconn_port = htons(self->local_port);
    local.sconn_addr = self;
    if (usrsctp_bind(self->sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        usrsctp_close(self->sock);
        self->sock = NULL;
        usrsctp_deregister_address(self);
        self->registered = 0;
        self->closed = 1;
        return -1;
    }
    return 0;
}

static void Association_close_internal(AssociationObject *self) {
    if (self->closed) return;
    self->closed = 1;

    if (self->sock) {
        usrsctp_close(self->sock);
        self->sock = NULL;
    }
    if (self->registered) {
        usrsctp_deregister_address(self);
        self->registered = 0;
    }
    free_packets(self);
    free_messages(self);
    free_events(self);
    free(self->partial);
    self->partial = NULL;
    self->partial_len = self->partial_cap = 0;
}

static void Association_dealloc(AssociationObject *self) {
    Association_close_internal(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Association_connect(AssociationObject *self, PyObject *Py_UNUSED(ignored)) {
    struct sockaddr_conn remote;
    int rc;
    if (self->closed || self->sock == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "association is closed");
        return NULL;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sconn_family = AF_CONN;
#ifdef HAVE_SCONN_LEN
    remote.sconn_len = sizeof(remote);
#endif
    remote.sconn_port = htons(self->remote_port);
    remote.sconn_addr = self;

    rc = usrsctp_connect(self->sock, (struct sockaddr *)&remote, sizeof(remote));
    if (rc < 0 && errno != EWOULDBLOCK && errno != EINPROGRESS && errno != EALREADY) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    /* WebRTC's usrsctp integration applies this immediately after connect().
       Non-blocking connect may still be in progress, but the peer path exists. */
    if (configure_path_mtu(self) < 0) {
        /* Do not fail association establishment solely because this optional
           tuning call is rejected by a particular usrsctp build. */
        PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static PyObject *Association_feed(AssociationObject *self, PyObject *arg) {
    Py_buffer view;
    if (self->closed) Py_RETURN_NONE;
    if (PyObject_GetBuffer(arg, &view, PyBUF_CONTIG_RO) < 0) return NULL;
    usrsctp_conninput(self, view.buf, (size_t)view.len, 0);
    PyBuffer_Release(&view);
    Py_RETURN_NONE;
}

static PyObject *Association_send(AssociationObject *self, PyObject *args, PyObject *kwds) {
    unsigned int stream_id, ppid;
    Py_buffer data;
    int ordered = 1;
    PyObject *max_retransmits = Py_None;
    PyObject *max_lifetime_ms = Py_None;
    static char *kwlist[] = {
        "stream_id", "ppid", "data", "ordered", "max_retransmits", "max_lifetime_ms", NULL
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "IIy*|pOO", kwlist,
                                     &stream_id, &ppid, &data, &ordered,
                                     &max_retransmits, &max_lifetime_ms)) {
        return NULL;
    }
    if (stream_id > 65535) {
        PyBuffer_Release(&data);
        PyErr_SetString(PyExc_ValueError, "stream_id out of range");
        return NULL;
    }
    if (self->closed || self->sock == NULL) {
        PyBuffer_Release(&data);
        PyErr_SetString(PyExc_RuntimeError, "association is closed");
        return NULL;
    }

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = (uint16_t)stream_id;
    spa.sendv_sndinfo.snd_ppid = htonl(ppid);
    spa.sendv_sndinfo.snd_flags = SCTP_EOR;
    if (!ordered) {
        spa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
    }

    if (max_retransmits != Py_None) {
        unsigned long v = PyLong_AsUnsignedLong(max_retransmits);
        if (PyErr_Occurred()) {
            PyBuffer_Release(&data);
            return NULL;
        }
        spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
        spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
        spa.sendv_prinfo.pr_value = (uint32_t)v;
    } else if (max_lifetime_ms != Py_None) {
        unsigned long v = PyLong_AsUnsignedLong(max_lifetime_ms);
        if (PyErr_Occurred()) {
            PyBuffer_Release(&data);
            return NULL;
        }
        spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
        spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
        spa.sendv_prinfo.pr_value = (uint32_t)v;
    }

    errno = 0;
    ssize_t rc = usrsctp_sendv(self->sock, data.buf, (size_t)data.len,
                               NULL, 0, &spa, sizeof(spa), SCTP_SENDV_SPA, 0);
    PyBuffer_Release(&data);

    if (rc < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS) {
            self->writable = 0;
            return PyLong_FromLong(0);
        }
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    /*
     * SCTP_EXPLICIT_EOR intentionally makes this a non-atomic send.  A
     * non-blocking usrsctp_sendv() may therefore accept only a prefix of one
     * user message.  The caller MUST keep the unsent suffix and retry it with
     * the same SID / PPID / reliability parameters.  Returning only bool here
     * used to lose that offset and could merge the suffix of one DataChannel
     * message with the next message under send-buffer pressure.
     *
     * This mirrors the WebRTC/usrsctp integration contract: return the exact
     * number of user bytes accepted by usrsctp, and return 0 for backpressure.
     */
    return PyLong_FromSsize_t((Py_ssize_t)rc);
}

static PyObject *Association_reset_stream(AssociationObject *self, PyObject *arg) {
    long sid = PyLong_AsLong(arg);
    if (sid < 0 || sid > 65535 || PyErr_Occurred()) return NULL;
    if (self->closed || self->sock == NULL) Py_RETURN_FALSE;

    size_t sz = sizeof(struct sctp_reset_streams) + sizeof(uint16_t);
    struct sctp_reset_streams *rst = (struct sctp_reset_streams *)calloc(1, sz);
    if (!rst) return PyErr_NoMemory();
    rst->srs_assoc_id = SCTP_CURRENT_ASSOC;
    rst->srs_flags = SCTP_STREAM_RESET_OUTGOING;
    rst->srs_number_streams = 1;
    rst->srs_stream_list[0] = (uint16_t)sid;

    int rc = usrsctp_setsockopt(self->sock, IPPROTO_SCTP, SCTP_RESET_STREAMS,
                                rst, (socklen_t)sz);
    int err = errno;
    free(rst);
    if (rc < 0) {
        if (err == EBUSY || err == EALREADY || err == EWOULDBLOCK) {
            Py_RETURN_FALSE;
        }
        errno = err;
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    Py_RETURN_TRUE;
}

static int dict_set_owned(PyObject *d, const char *key, PyObject *value) {
    if (value == NULL) {
        return -1;
    }
    int rc = PyDict_SetItemString(d, key, value);
    Py_DECREF(value);
    return rc;
}

static PyObject *Association_status(AssociationObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *d = PyDict_New();
    if (!d) return NULL;
    if (self->sock == NULL) return d;

    struct sctp_status st;
    socklen_t len = sizeof(st);
    memset(&st, 0, sizeof(st));
    if (usrsctp_getsockopt(self->sock, IPPROTO_SCTP, SCTP_STATUS, &st, &len) == 0) {
        if (dict_set_owned(d, "state", PyLong_FromLong(st.sstat_state)) < 0 ||
            dict_set_owned(d, "rwnd", PyLong_FromUnsignedLong(st.sstat_rwnd)) < 0 ||
            dict_set_owned(d, "unackdata", PyLong_FromUnsignedLong(st.sstat_unackdata)) < 0 ||
            dict_set_owned(d, "penddata", PyLong_FromUnsignedLong(st.sstat_penddata)) < 0 ||
            dict_set_owned(d, "instrms", PyLong_FromUnsignedLong(st.sstat_instrms)) < 0 ||
            dict_set_owned(d, "outstrms", PyLong_FromUnsignedLong(st.sstat_outstrms)) < 0 ||
            dict_set_owned(d, "fragmentation_point", PyLong_FromUnsignedLong(st.sstat_fragmentation_point)) < 0 ||
            dict_set_owned(d, "cwnd", PyLong_FromUnsignedLong(st.sstat_primary.spinfo_cwnd)) < 0 ||
            /* usrsctp exposes these path-info values in milliseconds. */
            dict_set_owned(d, "srtt_ms", PyLong_FromUnsignedLong(st.sstat_primary.spinfo_srtt)) < 0 ||
            dict_set_owned(d, "rto_ms", PyLong_FromUnsignedLong(st.sstat_primary.spinfo_rto)) < 0 ||
            dict_set_owned(d, "mtu", PyLong_FromUnsignedLong(st.sstat_primary.spinfo_mtu)) < 0) {
            Py_DECREF(d);
            return NULL;
        }
    }
    return d;
}

static PyObject *Association_poll(AssociationObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *result = PyDict_New();
    PyObject *outbound = PyList_New(0);
    PyObject *messages = PyList_New(0);
    PyObject *events = PyList_New(0);
    if (!result || !outbound || !messages || !events) {
        Py_XDECREF(result); Py_XDECREF(outbound); Py_XDECREF(messages); Py_XDECREF(events);
        return NULL;
    }

    packet_node *pn;
    while ((pn = self->out_head) != NULL) {
        self->out_head = pn->next;
        if (self->out_head == NULL) self->out_tail = NULL;
        PyObject *b = PyBytes_FromStringAndSize((const char *)pn->data, (Py_ssize_t)pn->len);
        free(pn);
        if (!b || PyList_Append(outbound, b) < 0) {
            Py_XDECREF(b); goto error;
        }
        Py_DECREF(b);
    }

    message_node *mn;
    while ((mn = self->msg_head) != NULL) {
        self->msg_head = mn->next;
        if (self->msg_head == NULL) self->msg_tail = NULL;
        PyObject *payload = PyBytes_FromStringAndSize((const char *)mn->data, (Py_ssize_t)mn->len);
        PyObject *tuple = payload ? Py_BuildValue("(IIN)", (unsigned int)mn->sid,
                                                  (unsigned int)mn->ppid, payload) : NULL;
        free(mn);
        if (!tuple || PyList_Append(messages, tuple) < 0) {
            Py_XDECREF(tuple); goto error;
        }
        Py_DECREF(tuple);
    }

    event_node *en;
    while ((en = self->event_head) != NULL) {
        self->event_head = en->next;
        if (self->event_head == NULL) self->event_tail = NULL;
        PyObject *tuple = NULL;
        if (en->kind == EVENT_ASSOC) {
            tuple = Py_BuildValue("(sIII)", "assoc", (unsigned int)en->a,
                                  (unsigned int)en->b, (unsigned int)en->c);
        } else if (en->kind == EVENT_RESET) {
            PyObject *ids = PyList_New(en->count);
            if (ids) {
                for (uint16_t i = 0; i < en->count; ++i) {
                    PyList_SET_ITEM(ids, i, PyLong_FromUnsignedLong(en->streams[i]));
                }
                tuple = Py_BuildValue("(sIN)", "reset", (unsigned int)en->a, ids);
            }
        } else if (en->kind == EVENT_SHUTDOWN) {
            tuple = Py_BuildValue("(s)", "shutdown");
        } else if (en->kind == EVENT_SEND_FAILED) {
            tuple = Py_BuildValue("(s)", "send_failed");
        }
        free(en);
        if (tuple) {
            if (PyList_Append(events, tuple) < 0) {
                Py_DECREF(tuple); goto error;
            }
            Py_DECREF(tuple);
        }
    }

    int writable = self->writable;
    self->writable = 0;
    if (PyDict_SetItemString(result, "outbound", outbound) < 0 ||
        PyDict_SetItemString(result, "messages", messages) < 0 ||
        PyDict_SetItemString(result, "events", events) < 0 ||
        PyDict_SetItemString(result, "writable", writable ? Py_True : Py_False) < 0) {
        goto error;
    }

    Py_DECREF(outbound); Py_DECREF(messages); Py_DECREF(events);
    return result;

error:
    Py_DECREF(outbound); Py_DECREF(messages); Py_DECREF(events); Py_DECREF(result);
    return NULL;
}

static PyObject *Association_close(AssociationObject *self, PyObject *Py_UNUSED(ignored)) {
    Association_close_internal(self);
    Py_RETURN_NONE;
}

static PyMethodDef Association_methods[] = {
    {"connect", (PyCFunction)Association_connect, METH_NOARGS, "Start SCTP association."},
    {"feed", (PyCFunction)Association_feed, METH_O, "Feed one decrypted SCTP datagram."},
    {"send", (PyCFunction)Association_send, METH_VARARGS | METH_KEYWORDS, "Send bytes from one SCTP user message; return bytes accepted, or 0 on backpressure."},
    {"poll", (PyCFunction)Association_poll, METH_NOARGS, "Drain raw packets, messages and events."},
    {"reset_stream", (PyCFunction)Association_reset_stream, METH_O, "Request outgoing stream reset."},
    {"status", (PyCFunction)Association_status, METH_NOARGS, "Return SCTP association status."},
    {"close", (PyCFunction)Association_close, METH_NOARGS, "Abort and close association."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject AssociationType = {
    PyVarObject_HEAD_INIT(NULL, 0)
};

static PyObject *mod_configure(PyObject *self, PyObject *args, PyObject *kwds) {
    unsigned int sendspace = g_sendspace, recvspace = g_recvspace;
    unsigned int sack_freq = g_sack_freq, delayed_sack_ms = g_delayed_sack_ms;
    static char *kwlist[] = {"sendspace", "recvspace", "sack_freq", "delayed_sack_ms", NULL};
    (void)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|IIII", kwlist,
                                     &sendspace, &recvspace, &sack_freq, &delayed_sack_ms)) {
        return NULL;
    }
    g_sendspace = sendspace;
    g_recvspace = recvspace;
    g_sack_freq = sack_freq;
    g_delayed_sack_ms = delayed_sack_ms;
    ensure_initialized();
    usrsctp_sysctl_set_sctp_sendspace(g_sendspace);
    usrsctp_sysctl_set_sctp_recvspace(g_recvspace);
    usrsctp_sysctl_set_sctp_sack_freq_default(g_sack_freq);
    usrsctp_sysctl_set_sctp_delayed_sack_time_default(g_delayed_sack_ms);
    Py_RETURN_NONE;
}

static PyObject *mod_tick(PyObject *self, PyObject *arg) {
    unsigned long ms = PyLong_AsUnsignedLong(arg);
    (void)self;
    if (PyErr_Occurred()) return NULL;
    ensure_initialized();
    usrsctp_handle_timers((uint32_t)ms);
    Py_RETURN_NONE;
}


static PyMethodDef module_methods[] = {
    {"configure", (PyCFunction)mod_configure, METH_VARARGS | METH_KEYWORDS, "Configure global usrsctp defaults."},
    {"tick", (PyCFunction)mod_tick, METH_O, "Drive usrsctp timers by elapsed milliseconds."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_native_sctp",
    "Pure-C usrsctp bridge for aiortc DataChannels.",
    -1,
    module_methods,
};

PyMODINIT_FUNC PyInit__native_sctp(void) {
    AssociationType.tp_name = "aiortc_native_sctp._native_sctp.Association";
    AssociationType.tp_basicsize = sizeof(AssociationObject);
    AssociationType.tp_dealloc = (destructor)Association_dealloc;
    AssociationType.tp_flags = Py_TPFLAGS_DEFAULT;
    AssociationType.tp_doc = "Native usrsctp association using AF_CONN.";
    AssociationType.tp_methods = Association_methods;
    AssociationType.tp_init = (initproc)Association_init;
    AssociationType.tp_new = Association_new;

    if (PyType_Ready(&AssociationType) < 0) return NULL;
    PyObject *m = PyModule_Create(&moduledef);
    if (!m) return NULL;
    Py_INCREF(&AssociationType);
    if (PyModule_AddObject(m, "Association", (PyObject *)&AssociationType) < 0) {
        Py_DECREF(&AssociationType);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}

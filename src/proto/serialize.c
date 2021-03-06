// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2010, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <pthread.h>
#include "junkie/tools/ext.h"
#include "junkie/tools/sock.h"
#include "junkie/tools/queue.h"
#include "junkie/tools/objalloc.h"
#include "junkie/tools/tempstr.h"
#include "junkie/proto/serialize.h"
#include "pkt_source.h"

/*
 * Serialization
 */

static void serialize_info_rec(unsigned depth, uint8_t **buf, struct proto_info const *info, struct timeval const *now)
{
    if (info->parent != NULL) {
        serialize_info_rec(depth+1, buf, info->parent, now);
    } else {
        // The msg starts with the depth of the protocol stack (so that we can pack several info into a single msg)
        serialize_1(buf, depth);
        // And is followed by the timestamp
        timeval_serialize(now, buf);
    }
    ASSERT_COMPILE(PROTO_CODE_MAX <= 255);
    serialize_1(buf, info->parser->proto->code);    // each proto start with its code
    if (info->parser->proto->ops->serialize) {
        // Some protocols may not implement this
        info->parser->proto->ops->serialize(info, buf);
    }
}

void serialize_proto_stack(uint8_t **buf, struct proto_info const *last, struct timeval const *now)
{
    serialize_info_rec(1, buf, last, now);
}

/*
 * Deserialization
 */

#include "junkie/proto/cap.h"
#include "junkie/proto/eth.h"
#include "junkie/proto/ip.h"
#include "junkie/proto/gre.h"
#include "junkie/proto/arp.h"
#include "junkie/proto/udp.h"
#include "junkie/proto/icmp.h"
#include "junkie/proto/tcp.h"
#include "junkie/proto/sip.h"
#include "junkie/proto/http.h"
#include "junkie/proto/rtp.h"
#include "junkie/proto/netbios.h"
#include "junkie/proto/dns.h"
#include "junkie/proto/rtcp.h"
#include "junkie/proto/ftp.h"
#include "junkie/proto/mgcp.h"
#include "junkie/proto/sdp.h"
#include "junkie/proto/sql.h"
#include "junkie/proto/tls.h"
#include "junkie/proto/erspan.h"
#include "junkie/proto/skinny.h"
#include "junkie/proto/dhcp.h"
#include "junkie/proto/cifs.h"
#include "junkie/proto/discovery.h"

static void deserialize_proto_info_rec(unsigned depth, uint8_t const **buf, struct proto_info *last, struct timeval const *now)
{
    if (last) {
        proto_subscribers_call(last->parser->proto, last, 0, NULL, now);
        if (depth == 0) full_pkt_subscribers_call(last, 0, NULL, now);
    }

    if (depth == 0) return;

    enum proto_code code = deserialize_1(buf); // read the code
    if (code >= PROTO_CODE_MAX) {
        SLOG(LOG_WARNING, "Unknown protocol code %u", code);
        return;
    }

    union {
        struct cap_proto_info cap;
        struct eth_proto_info eth;
        struct arp_proto_info arp;
        struct ip_proto_info ip;
        struct ip6_proto_info ip6;
        struct udp_proto_info udp;
        struct tcp_proto_info tcp;
        struct dns_proto_info dns;
        struct dns_proto_info dns_tcp;
        struct ftp_proto_info ftp;
        struct gre_proto_info gre;
        struct http_proto_info http;
        struct icmp_proto_info icmp;
        struct mgcp_proto_info mgcp;
        struct rtcp_proto_info rtcp;
        struct rtp_proto_info rtp;
        struct sdp_proto_info sdp;
        struct sip_proto_info sip;
        struct sql_proto_info tns;
        struct sql_proto_info pgsql;
        struct sql_proto_info mysql;
        struct tls_proto_info tls;
        struct netbios_proto_info netbios;
        struct erspan_proto_info erspan;
        struct skinny_proto_info skinny;
        struct dhcp_proto_info dhcp;
        struct cifs_proto_info cifs;
        struct discovery_proto_info discovery;
    } i;
    struct proto_info *info = NULL;
    struct proto *proto = NULL;
    switch (code) {
#       define CASE(NAME, name) \
        case PROTO_CODE_##NAME: \
            info = &i.name.info; \
            proto = proto_##name; \
            break
        CASE(CAP, cap); CASE(ETH, eth); CASE(ARP, arp);
        CASE(IP, ip); CASE(IP6, ip6); CASE(UDP, udp);
        CASE(TCP, tcp); CASE(FTP, ftp);
        CASE(DNS, dns); CASE(DNSoTCP, dns_tcp);
        CASE(GRE, gre); CASE(HTTP, http); CASE(ICMP, icmp);
        CASE(MGCP, mgcp); CASE(RTCP, rtcp); CASE(RTP, rtp);
        CASE(SDP, sdp); CASE(SIP, sip); CASE(TNS, tns);
        CASE(PGSQL, pgsql); CASE(MYSQL, mysql);
        CASE(NETBIOS, netbios); CASE(TLS, tls) ;
        CASE(ERSPAN, erspan); CASE(SKINNY, skinny);
        CASE(DHCP, dhcp); CASE(CIFS, cifs);
        CASE(DISCOVERY, discovery);
#       undef CASE
        case PROTO_CODE_DUMMY:
        case PROTO_CODE_MAX:
            break;
    }
    if (! info) {
        SLOG(LOG_WARNING, "Unknown proto code %u", code);
        return;
    }

    assert(proto);
    if (proto->ops->deserialize) {
        proto->ops->deserialize(info, buf);
        info->parent = last;
        struct parser dummy_parser = { .proto = proto }; // A dummy parser just so that subscribers can dereference info->parser->proto
        info->parser = &dummy_parser;
    } else {
        if (proto->ops->serialize) {
            SLOG(LOG_WARNING, "No deserializer for proto %s", proto->name);
            return;
        }
        info = last;    // skip this layer
    }

    deserialize_proto_info_rec(depth-1, buf, info, now);
}

void deserialize_proto_stack(uint8_t const **buf)
{
    // The msg starts with the protocol stack depth
    unsigned depth = deserialize_1(buf);
    // Then the timestamp
    struct timeval now;
    timeval_deserialize(&now, buf);
    deserialize_proto_info_rec(depth, buf, NULL, &now);
}

/*
 * Extentions: ability to open/close/list (de)serializers
 */

struct deserializer_source {
    LIST_ENTRY(deserializer_source) entry;
    uint32_t id;
    uint64_t nb_rcvd_msgs, nb_lost_msgs;
};

struct deserializer {
    LIST_ENTRY(deserializer) entry;
    struct sock *sock;
    pthread_t server_pth;
    bool running;   // set to false when the thread exits
    unsigned nb_sources;    // how many sources did we ever saw
    LIST_HEAD(sources, deserializer_source) sources;
};

static LIST_HEAD(deserializers, deserializer) deserializers;    // FIXME: a mutex should not be necessary in practice but won't hurt either

static int deserializer_source_ctor(struct deserializer_source *source, struct deserializer *deser, uint32_t id)
{
    source->id = id;
    source->nb_rcvd_msgs = source->nb_lost_msgs = 0;
    LIST_INSERT_HEAD(&deser->sources, source, entry);
    return 0;
}

static struct deserializer_source *deserializer_source_new(struct deserializer *deser, uint32_t id)
{
    struct deserializer_source *source = objalloc(sizeof(*source), "deserializer srcs");
    if (! source) return NULL;
    if (0 != deserializer_source_ctor(source, deser, id)) {
        objfree(source);
        return NULL;
    }
    return source;
}

// we not normally deletes any sources
static void deserializer_source_dtor(struct deserializer_source *source)
{
    LIST_REMOVE(source, entry);
}

static void unused_ deserializer_source_del(struct deserializer_source *source)
{
    deserializer_source_dtor(source);
}

static struct deserializer_source *deserializer_source_lookup(struct deserializer *deser, uint32_t id)
{
    struct deserializer_source *source;
    // shortcut for the most probable case, that the first source is the one we're looking for
    if (! LIST_EMPTY(&deser->sources) && LIST_FIRST(&deser->sources)->id == id) {
        return LIST_FIRST(&deser->sources);
    }

    LIST_FOREACH(source, &deser->sources, entry) {
        if (source->id == id) break;
    }
    if (! source) {
        source = deserializer_source_new(deser, id);
        if (! source) return NULL;
    }

    // promotes this source
    LIST_REMOVE(source, entry);
    LIST_INSERT_HEAD(&deser->sources, source, entry);

    return source;
}

static int deserializer_receiver(struct sock unused_ *sock, size_t len, uint8_t const *buf, struct ip_addr const unused_ *sender, void *deser_)
{
    struct deserializer *deser = deser_;
    uint8_t const *ptr = buf;
    uint32_t src_id;
    struct deserializer_source *source;

    while (ptr < buf + len) {
        switch (*ptr++) {
            case MSG_PROTO_INFO:;
                src_id = deserialize_4(&ptr);
                source = deserializer_source_lookup(deser, src_id);
                if (! source) break;
                (void)deserialize_proto_stack(&ptr);
                source->nb_rcvd_msgs ++;
                break;
            case MSG_PROTO_STATS:;
                src_id = deserialize_4(&ptr);
                uint64_t nb_sent_msgs = deserialize_4(&ptr);
                source = deserializer_source_lookup(deser, src_id);
                if (! source) break;
                uint64_t new_lost = nb_sent_msgs - source->nb_rcvd_msgs;   // 2-complement rules!
                if (source->nb_lost_msgs != new_lost) {
                    source->nb_lost_msgs = new_lost;
                    fprintf(stderr, "deserializer: lost %"PRIu64" msgs from source %"PRIu32"\n", source->nb_lost_msgs, src_id);
                }
                break;
            default:
                SLOG(LOG_ERR, "Unknown message of type %"PRIu8, ptr[-1]);
                break;
        }
    }

    return 0;
}

// The thread serving the deserializer port
static void *deserializer_thread(void *deser_)
{
    struct deserializer *deser = deser_;

    deser->running = true;

    while (deser->sock) {
        fd_set set;
        if (0 != sock_select_single(deser->sock, &set)) break;
        if (0 != deser->sock->ops->recv(deser->sock, &set, deserializer_receiver, deser)) break;
    }

    deser->running = false;
    return NULL;
}


static int deserializer_ctor(struct deserializer *deser, struct sock *sock)
{
    deser->sock = sock;
    deser->running = false;
    LIST_INIT(&deser->sources);

    int err = pthread_create(&deser->server_pth, NULL, deserializer_thread, deser);
    if (err) {
        SLOG(LOG_ERR, "Cannot start server thread: %s", strerror(err));
        return -1;
    }

    LIST_INSERT_HEAD(&deserializers, deser, entry);

    return 0;
}

// We are given full responsibility over sock
static struct deserializer *deserializer_new(struct sock *sock)
{
    if (! sock) return NULL;

    struct deserializer *deser = objalloc(sizeof(*deser), "deserializers");
    if (! deser) return NULL;

    if (0 != deserializer_ctor(deser, sock)) {
        objfree(deser);
        sock->ops->del(sock);
        return NULL;
    }

    return deser;
}

static void deserializer_dtor(struct deserializer *deser)
{
    LIST_REMOVE(deser, entry);
    int err = pthread_cancel(deser->server_pth);
    if (err) {
        SLOG(LOG_ERR, "Cannot cancel server thread: %s", strerror(err));
        (void)pthread_detach(deser->server_pth);
    } else {
        err = pthread_join(deser->server_pth, NULL);
        if (err) {
            SLOG(LOG_ERR, "Cannot join server thread: %s", strerror(err));
            // so be it
        }
    }
    deser->sock->ops->del(deser->sock);
}

static void deserializer_del(struct deserializer *deser)
{
    deserializer_dtor(deser);
    objfree(deser);
}

// FIXME: instead of a port, should take a sock blob
static struct ext_function sg_open_deserializer;
static SCM g_open_deserializer(SCM port_)
{
    SCM ret = SCM_BOOL_F;
    scm_dynwind_begin(0);
    char *service = SERIALIZER_DEFAULT_SERVICE;
    if (!SCM_UNBNDP(port_)) {
        if (scm_is_string(port_)) {
            service = scm_to_locale_string(port_);
            scm_dynwind_free(service);
        } else { // we assume a number then
            service = tempstr_printf("%u", scm_to_uint(port_));
        }
    }

    struct sock *sock = sock_udp_server_new(service, 0);
    struct deserializer *deser = deserializer_new(sock);
    if (deser) {
        ret = scm_from_latin1_string(deser->sock->name);
    }

    scm_dynwind_end();
    return ret;
}

static struct deserializer *deserializer_of_scm(SCM name_)
{
    char *name = scm_to_tempstr(name_);

    struct deserializer *deser;
    LIST_FOREACH(deser, &deserializers, entry) {
        if (0 == strcmp(deser->sock->name, name)) break;
    }

    return deser;
}

static struct ext_function sg_close_deserializer;
static SCM g_close_deserializer(SCM name_)
{
    SCM ret = SCM_BOOL_F;

    struct deserializer *deser = deserializer_of_scm(name_);
    if (deser) {
        deserializer_del(deser);
        ret = SCM_BOOL_T;
    }

    return ret;
}

static struct ext_function sg_deserializer_names;
static SCM g_deserializer_names(void)
{
    SCM ret = SCM_EOL;

    struct deserializer *deser;
    LIST_FOREACH(deser, &deserializers, entry) {
        SCM name = scm_from_latin1_string(deser->sock->name);
        ret = scm_cons(name, ret);
    }

    return ret;
}

static SCM nb_rcvd_msgs_sym;
static SCM nb_lost_msgs_sym;

static SCM deserializer_source_stats(struct deserializer_source *source)
{
    return scm_list_2(
        scm_cons(nb_rcvd_msgs_sym,  scm_from_uint64(source->nb_rcvd_msgs)),
        scm_cons(nb_lost_msgs_sym,  scm_from_uint64(source->nb_lost_msgs)));
}

static SCM name_sym;
static SCM running_sym;
static SCM sources_sym;

static struct ext_function sg_deserializer_stats;
static SCM g_deserializer_stats(SCM name_)
{
    struct deserializer *deser = deserializer_of_scm(name_);
    if (! deser) return SCM_BOOL_F;

    SCM srcs = SCM_EOL;
    struct deserializer_source *source;
    LIST_FOREACH(source, &deser->sources, entry) {
        srcs = scm_cons(
            scm_cons(scm_from_uint32(source->id), deserializer_source_stats(source)),
            srcs);
    }

    return scm_list_3(
        scm_cons(name_sym,          scm_from_latin1_string(deser->sock->name)),
        scm_cons(running_sym,       scm_from_bool(deser->running)),
        scm_cons(sources_sym,       srcs));
}


/*
 * Init
 */

static unsigned inited;
void serialize_init(void)
{
    if (inited++) return;
    objalloc_init();
    ext_init();
    sock_init();

    name_sym         = scm_permanent_object(scm_from_latin1_symbol("name"));
    running_sym      = scm_permanent_object(scm_from_latin1_symbol("running?"));
    nb_rcvd_msgs_sym = scm_permanent_object(scm_from_latin1_symbol("nb-rcvd-msgs"));
    nb_lost_msgs_sym = scm_permanent_object(scm_from_latin1_symbol("nb-lost-msgs"));
    sources_sym      = scm_permanent_object(scm_from_latin1_symbol("sources"));

    LIST_INIT(&deserializers);

    ext_function_ctor(&sg_open_deserializer,
        "open-deserializer", 0, 1, 0, g_open_deserializer,
        "(open-deserializer): listen on default port (" SERIALIZER_DEFAULT_SERVICE ") and supply received frames info to local plugins.\n"
        "(open-deserializer 28100): listen on alternate port.\n"
        "Will return the name of the deserializer or #f if the operation fails.\n"
        "See also (? 'close-deserializer) and (? 'deserializers).\n");

    ext_function_ctor(&sg_close_deserializer,
        "close-deserializer", 0, 0, 0, g_close_deserializer,
        "Will return #t if this deserializer was successfully closed.\n"
        "(close-deserializer \"name\"): closes this deserializer.\n"
        "See also (? 'open-deserializer) and (? 'deserializers).\n");

    ext_function_ctor(&sg_deserializer_names,
        "deserializer-names", 0, 0, 0, g_deserializer_names,
        "(deserializer-names): list all currently opened deserializers.\n"
        "See also (? 'open-deserializer) and (? 'close-deserializer).\n");

    ext_function_ctor(&sg_deserializer_stats,
        "deserializer-stats", 1, 0, 0, g_deserializer_stats,
        "(deserializer-stats \"name\"): return some stats about this deserializer.\n"
        "See also (? 'deserializer-names)\n");
}

void serialize_fini(void)
{
    if (--inited) return;

    ext_fini();
    sock_fini();
    objalloc_fini();
}

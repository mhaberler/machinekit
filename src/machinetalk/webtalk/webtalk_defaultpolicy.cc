// this callback defines how frames are relayed between websockets and zmq
// it can be replaced by a user-defined function referred to by name in the URI
// a named policy may be added by calling zwsproxy_add_policy()
// see example code in zwsmain.c


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libwebsockets.h>

#include "webtalk.hh"


// working around JS not being able to zeroconf-lookup services yet:
//
// use the 'machinekit' pseudo-uri:
// giving a destination like 'connect=machinekit://halrcmd' will cause the following:
// - a zeroconf lookup of "_halrcmd._sub._machinekit._tcp"
// - select answers by matching the TXT records for "uuid=<uuid>"
// - retrieve the 'dsn=' TXT record
// - use the dsn= destination in lieu of the URI proper

#define MKPREFIX "machinekit://"
#define RESOLVE_TIMEOUT 3000

static const char *zerconf_dsn(wtself_t *self, const char *service);

int default_policy(wtself_t *self,
		   zws_session_t *wss,
		   zwscb_type type)
{
    zmsg_t *m;
    zframe_t *f;

    lwsl_debug("%s op=%d\n",__func__,  type);

    switch (type) {
    case ZWS_CONNECTING:
	{
	    const char *identity = NULL;
	    wss->txmode = LWS_WRITE_BINARY;
	    UriQueryListA *q = wss->queryList;
	    int fd = libwebsocket_get_socket_fd(wss->wsiref);

	    while (q != NULL) {
		lwsl_uri("%s %d: key='%s' value='%s'\n",
			   __func__, fd, q->key,q->value);

		// FIXME this needs way better error reporting (.i.e. some)
		// should create a Container with a log message, and convert to JSON
		// here to report errors in-band
		if (!strcmp(q->key,"text")) wss->txmode = LWS_WRITE_TEXT;
		if (!strcmp(q->key,"identity")) identity =  q->value;
		if (!strcmp(q->key,"type")) {
		    if (!strcasecmp(q->value,"dealer")) wss->socket_type = ZMQ_DEALER;
		    if (!strcasecmp(q->value,"sub")) wss->socket_type = ZMQ_SUB;
		    if (!strcasecmp(q->value,"xsub")) wss->socket_type = ZMQ_XSUB;
		    if (!strcasecmp(q->value,"pub")) wss->socket_type = ZMQ_PUB;
		    if (!strcasecmp(q->value,"xpub")) wss->socket_type = ZMQ_XPUB;
		    if (!strcasecmp(q->value,"router")) wss->socket_type = ZMQ_ROUTER;
		    if (!strcasecmp(q->value,"req")) wss->socket_type = ZMQ_REQ;
		    if (!strcasecmp(q->value,"rep")) wss->socket_type = ZMQ_REP;
		    if (!strcasecmp(q->value,"push")) wss->socket_type = ZMQ_PUSH;
		    if (!strcasecmp(q->value,"pull")) wss->socket_type = ZMQ_PULL;
		    if (!strcasecmp(q->value,"pair")) wss->socket_type = ZMQ_PAIR;
		}
		q = q->next;
	    }
	    wss->socket = zsock_new (wss->socket_type);
	    if (wss->socket == NULL) {
		lwsl_err("%s %d: cant create ZMQ socket: %s\n",
			 __func__, fd, strerror(errno));
		return -1;
	    }
	    if (self->cfg->ipv6) {
		zsock_set_ipv6 (wss->socket, 1);
		assert (zsock_ipv6 (wss->socket) == 1);
	    }
	    if ((wss->socket_type == ZMQ_DEALER) || (wss->socket_type == ZMQ_ROUTER)) {
		if (identity) {
		    zsock_set_identity (wss->socket, identity);
		    lwsl_uri("%s %d: set client-provided identity to '%s'\n",
			     __func__,fd, identity);
		}
	    }
	    // bind/connect to all destinations
	    q = wss->queryList;
	    int destcount = 0;
	    while (q != NULL) {
		if (!strcmp(q->key,"connect")) {
		    // handle the 'connect=machinekit://<foo>' case
		    // extract foo, and zeroconf-lookup this subtype

		    if (!strncmp(q->value, MKPREFIX, strlen(MKPREFIX))) {
			char ipcuri[100];
			const char *uri;

			const char *service = q->value + strlen(MKPREFIX);

			if (self->netopts.remote) {
			    lwsl_uri("%s %d: doing zeroconf lookup '%s'\n", __func__, fd,service);
			    uri = zerconf_dsn(self,service);
			    if (uri == NULL)
				return -1;
			} else {
			    // assume a local IPC socket
			    snprintf(ipcuri, sizeof(ipcuri),ZMQIPC_FORMAT,
				     RUNDIR, 0, service, self->netopts.service_uuid);
			    uri = ipcuri;
			}

			lwsl_uri("%s %d: URI= '%s'\n", __func__, fd,uri);
			if (zsock_connect (wss->socket, uri)) {
			    lwsl_err("%s %d: cant connect to %s: %s\n",
				     __func__, fd, uri, strerror(errno));
			    return -1;
			} else {
			    lwsl_uri("%s %d: connect to %s type %d (%s)\n",
				     __func__, fd, uri, wss->socket_type,
				     self->netopts.remote ? "zeroconf resolved" : "local IPC");
			    destcount++;
			}
		    } else {
			lwsl_uri("%s %d: connecting to %s type %d\n",
				 __func__, fd, q->value, wss->socket_type);
			if (zsock_connect (wss->socket, q->value)) {
			    lwsl_err("%s %d: cant connect to %s: %s\n",
				     __func__, fd, q->value, strerror(errno));
			    return -1;
			}
			destcount++;
		    }
		}
		if (!strcmp(q->key,"bind")) {
		    if (zsock_bind (wss->socket, q->value) < 0) {
			lwsl_err("%s %d: cant bind to %s: %s\n",
				 __func__, fd, q->value, strerror(errno));
			return -1;
		    } else {
			destcount++;
			lwsl_uri("%s %d: bind to %s type %d\n",
				   __func__, fd, q->value, wss->socket_type);
		    }
		}
		q = q->next;
	    }
	    if (destcount == 0) {
		lwsl_err("%s %d: no 'bind' or 'connect' arg given, closing\n",
			 __func__,fd);
		return -1;
	    }

	    q = wss->queryList;
	    while (q != NULL) {
		if (!strcmp(q->key,"subscribe")) {

		    const char *topic = (q->value == NULL) ? "" : q->value;
		    switch (wss->socket_type) {

		    case ZMQ_SUB:
			zsock_set_subscribe (wss->socket, topic);
			lwsl_uri("%s %d: subscribe topic '%s' on SUB\n",
				 __func__, fd, topic);
			break;

		    case ZMQ_XSUB:
			{
			    lwsl_uri("%s %d: xsub '%s'\n",  __func__, fd, topic);
			    size_t len = strlen(topic) + 1;
			    zframe_t *f = zframe_new (NULL, len);
			    char *s = (char *) zframe_data(f);
			    *s = '\001';
			    strcpy(s + 1, topic);
			    if (zframe_send(&f, wss->socket, 0)) {
				lwsl_err("%s %d: sending subscribe message '%s' to xsub failed: %s\n",
					 __func__, fd, topic, strerror(errno));
				return -1;
			    } else {
				lwsl_uri("%s %d: sent '%s' to xsub socket to subscribe\n",
				 __func__, fd, topic);
			    }
			}
			break;

		    default:
			lwsl_err("%s %d: subscribe doesnt make sense on socket type %d, closing\n",
				 __func__,fd, wss->socket_type);
			return -1;
		    }
		}
		q = q->next;
	    }
	}
	break;

    case ZWS_ESTABLISHED:
	return register_zmq_poller(wss);
	break;

    case ZWS_FROM_WS:
	// ws->zmq: just send as standalone frame.
	f = zframe_new (wss->buffer, wss->length);
	lwsl_fromws("%s: %d:'%.*s'\n", __func__, wss->length, wss->length, wss->buffer);
	return zframe_send(&f, wss->socket, 0);
	break;

    case ZWS_TO_WS:
	// zmq->ws: unwrap all frames and send individually by stuffing into wsq_out
	// this might not make sense on subscribe sockets which send the topic frame
	// first
	m = zmsg_recv(wss->socket);
	while ((f = zmsg_pop (m)) != NULL) {
	    wss->zmq_bytes += zframe_size(f);
	    wss->zmq_msgs++;
	    lwsl_tows("%s: %d:'%.*s'\n", __func__, zframe_size(f),zframe_size(f),zframe_data(f));
	    zframe_send(&f, wss->wsq_out, 0);
	}
	zmsg_destroy(&m);
	break;

    default:
	break;
    }
    return 0;
}

static const char *zerconf_dsn(wtself_t *self, const char *service)
{
    char subtype[100], match[50];
    const char   *result = NULL;

    snprintf(subtype, sizeof(subtype), "_%s._sub._machinekit._tcp", service);
    snprintf(match, sizeof(match), "uuid=%s", self->netopts.service_uuid);

    zresolve_t res = {0};
    res.proto =	 AVAHI_PROTO_UNSPEC;
    res.interface = AVAHI_IF_UNSPEC;
    res.type =  subtype;
    res.match =  match;
    res.domain = NULL;
    res.name = NULL;
    res.timeout_ms = RESOLVE_TIMEOUT;

    resolve_context_t *p  = ll_zeroconf_resolve(&res);

    if (res.result == SD_OK) {

	// fish out the dsn=<uri> TXT record
	AvahiStringList *dsn = avahi_string_list_find(res.txt, "dsn");
	char *key, *uri;
	size_t vlen;

	if ((dsn == NULL) || avahi_string_list_get_pair(dsn, &key, &uri, &vlen)) {
	    lwsl_err("zerconf_dsn service discovery failed - no dsn= key for service '%s'\n", service);
	    return NULL;
	}
	lwsl_uri("zerconf_dsn(%s,%s): uri = '%s'\n",service, match, uri);
	result = strdup(uri);
    } else {
	lwsl_err("zerconf_dsn service discovery failed - cant retrieve URI for service '%s' result=%d\n",
		 service, res.result);
	return NULL;
    }
    ll_zeroconf_resolve_free(p);
    return result;
}

/*
 * Copyright (C) 2013-2014 Michael Haberler <license@mah.priv.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "haltalk.hh"

// zeroconf-register haltalk services
int
ht_zeroconf_announce_services(htself_t *self)
{
    char name[LINELEN];
    char uri[PATH_MAX];

    // use mDNS addressing if running over TCP:
    // construct a URI of the form 'tcp://<hostname>.local.:<portnumber>'

    if (self->cfg->remote)
	snprintf(uri,sizeof(uri), "tcp://%s.local.:%d", self->hostname, self->z_group_port);

    snprintf(name,sizeof(name), "HAL Group service on %s.local pid %d", self->hostname, getpid());
    self->halgroup_publisher = zeroconf_service_announce(name,
							 MACHINEKIT_DNSSD_SERVICE_TYPE,
							 HALGROUP_DNSSD_SUBTYPE,
							 self->z_group_port,
							 self->cfg->remote ? uri :
							 (char *)self->z_halgroup_dsn,
							 self->cfg->service_uuid,
							 self->puuid,
							 "halgroup", NULL,
							 self->av_loop);
    if (self->halgroup_publisher == NULL) {
	syslog_async(LOG_ERR, "%s: failed to start zeroconf HAL Group publisher\n",
		     self->cfg->progname);
	return -1;
    }

    snprintf(name,sizeof(name), "HAL Rcomp service on %s.local pid %d", self->hostname, getpid());

    if (self->cfg->remote)
	snprintf(uri,sizeof(uri), "tcp://%s.local.:%d",self->hostname, self->z_rcomp_port);

    self->halrcomp_publisher = zeroconf_service_announce(name,
							 MACHINEKIT_DNSSD_SERVICE_TYPE,
							 HALRCOMP_DNSSD_SUBTYPE,
							 self->z_rcomp_port,
							 self->cfg->remote ? uri :
							 (char *)self->z_halrcomp_dsn,
							 self->cfg->service_uuid,
							 self->puuid,
							 "halrcomp", NULL,
							 self->av_loop);
    if (self->halrcomp_publisher == NULL) {
	syslog_async(LOG_ERR, "%s: failed to start zeroconf HAL Rcomp publisher\n",
		     self->cfg->progname);
	return -1;
    }

    snprintf(name,sizeof(name),  "HAL Rcommand service on %s.local pid %d", self->hostname, getpid());

    if (self->cfg->remote)
	snprintf(uri,sizeof(uri), "tcp://%s.local.:%d",self->hostname, self->z_rcomp_port);

    self->halrcmd_publisher = zeroconf_service_announce(name,
							MACHINEKIT_DNSSD_SERVICE_TYPE,
							HALRCMD_DNSSD_SUBTYPE,
							self->z_halrcmd_port,
							self->cfg->remote ? uri :
							(char *)self->z_halrcmd_dsn,
							self->cfg->service_uuid,
							self->puuid,
							"halrcmd", NULL,
							self->av_loop);
    if (self->halrcmd_publisher == NULL) {
	syslog_async(LOG_ERR, "%s: failed to start zeroconf HAL Rcomp publisher\n",
		     self->cfg->progname);
	return -1;
    }

    return 0;
}

int
ht_zeroconf_withdraw_services(htself_t *self)
{
    if (self->halgroup_publisher)
	zeroconf_service_withdraw(self->halgroup_publisher);
    if (self->halrcomp_publisher)
	zeroconf_service_withdraw(self->halrcomp_publisher);
    if (self->halrcmd_publisher)
	zeroconf_service_withdraw(self->halrcmd_publisher);

    // deregister all rings
    for (ringmap_iterator r = self->rings.begin(); r != self->rings.end(); r++) {
	ht_zeroconf_withdraw_ring(r->second);
    }

    // deregister poll adapter
    if (self->av_loop)
        avahi_czmq_poll_free(self->av_loop);
    return 0;
}

int ht_zeroconf_announce_ring(htself_t *self, const char *ringname)
{
    char name[LINELEN];
    char uri[PATH_MAX];

    assert(self->rings.count(ringname) == 1);

    htring_t *ring = self->rings[ringname];

    if (self->cfg->remote)
	snprintf(uri,sizeof(uri), "tcp://%s.local.:%d", self->hostname, ring->z_ring_port);

    snprintf(name, sizeof(name), "HAL %s ringbuffer %s  on %s.local pid %d",
	     zsocket_type_str(ring->z_ring), ringname, self->hostname, getpid());

    ring->ring_publisher = zeroconf_service_announce(name,
						     MACHINEKIT_DNSSD_SERVICE_TYPE,
						     RING_DNSSD_SUBTYPE,
						     ring->z_ring_port,
						     self->cfg->remote ? uri :
						     (char *) "FIXME",
						     // (char *)self->z_halgroup_dsn,
						     self->cfg->service_uuid,
						     self->puuid,
						     ringname, NULL,
						     self->av_loop);
    if (ring->ring_publisher == NULL) {
	syslog_async(LOG_ERR, "%s: failed to start zeroconf HAL ring for '%s'\n",
		     self->cfg->progname, uri, self->cfg->remote ? uri : "FIXME");
	return -1;
    }
    return 0;
}

int ht_zeroconf_withdraw_ring(htring_t *ring)
{
    assert(ring != NULL);
    if (ring->ring_publisher) {
	zeroconf_service_withdraw(ring->ring_publisher);
	ring->ring_publisher = NULL;
    }
    return 0;
}

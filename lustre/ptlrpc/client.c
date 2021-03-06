/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

/** Implementation of client-side PortalRPC interfaces */

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
#include <errno.h>
#include <signal.h>
#include <liblustre.h>
#endif

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_lib.h>
#include <lustre_ha.h>
#include <lustre_import.h>
#include <lustre_req_layout.h>

#include "ptlrpc_internal.h"

/**
 * Initialize passed in client structure \a cl.
 */
void ptlrpc_init_client(int req_portal, int rep_portal, char *name,
                        struct ptlrpc_client *cl)
{
        cl->cli_request_portal = req_portal;
        cl->cli_reply_portal   = rep_portal;
        cl->cli_name           = name;
}

/**
 * Return PortalRPC connection for remore uud \a uuid
 */
struct ptlrpc_connection *ptlrpc_uuid_to_connection(struct obd_uuid *uuid)
{
        struct ptlrpc_connection *c;
        lnet_nid_t                self;
        lnet_process_id_t         peer;
        int                       err;

        err = ptlrpc_uuid_to_peer(uuid, &peer, &self);
        if (err != 0) {
                CNETERR("cannot find peer %s!\n", uuid->uuid);
                return NULL;
        }

        c = ptlrpc_connection_get(peer, self, uuid);
        if (c) {
                memcpy(c->c_remote_uuid.uuid,
                       uuid->uuid, sizeof(c->c_remote_uuid.uuid));
        }

        CDEBUG(D_INFO, "%s -> %p\n", uuid->uuid, c);

        return c;
}

/**
 * Allocate and initialize new bulk descriptor
 * Returns pointer to the descriptor or NULL on error.
 */
static inline struct ptlrpc_bulk_desc *new_bulk(int npages, int type, int portal)
{
        struct ptlrpc_bulk_desc *desc;

        OBD_ALLOC(desc, offsetof (struct ptlrpc_bulk_desc, bd_iov[npages]));
        if (!desc)
                return NULL;

        cfs_spin_lock_init(&desc->bd_lock);
        cfs_waitq_init(&desc->bd_waitq);
        desc->bd_max_iov = npages;
        desc->bd_iov_count = 0;
        LNetInvalidateHandle(&desc->bd_md_h);
        desc->bd_portal = portal;
        desc->bd_type = type;

        return desc;
}

/**
 * Prepare bulk descriptor for specified outgoing request \a req that
 * can fit \a npages * pages. \a type is bulk type. \a portal is where
 * the bulk to be sent. Used on client-side.
 * Returns pointer to newly allocatrd initialized bulk descriptor or NULL on
 * error.
 */
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_imp(struct ptlrpc_request *req,
                                              int npages, int type, int portal)
{
        struct obd_import *imp = req->rq_import;
        struct ptlrpc_bulk_desc *desc;

        ENTRY;
        LASSERT(type == BULK_PUT_SINK || type == BULK_GET_SOURCE);
        desc = new_bulk(npages, type, portal);
        if (desc == NULL)
                RETURN(NULL);

        desc->bd_import_generation = req->rq_import_generation;
        desc->bd_import = class_import_get(imp);
        desc->bd_req = req;

        desc->bd_cbid.cbid_fn  = client_bulk_callback;
        desc->bd_cbid.cbid_arg = desc;

        /* This makes req own desc, and free it when she frees herself */
        req->rq_bulk = desc;

        return desc;
}

/**
 * Prepare bulk descriptor for specified incoming request \a req that
 * can fit \a npages * pages. \a type is bulk type. \a portal is where
 * the bulk to be sent. Used on server-side after request was already
 * received.
 * Returns pointer to newly allocatrd initialized bulk descriptor or NULL on
 * error.
 */
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_exp(struct ptlrpc_request *req,
                                              int npages, int type, int portal)
{
        struct obd_export *exp = req->rq_export;
        struct ptlrpc_bulk_desc *desc;

        ENTRY;
        LASSERT(type == BULK_PUT_SOURCE || type == BULK_GET_SINK);

        desc = new_bulk(npages, type, portal);
        if (desc == NULL)
                RETURN(NULL);

        desc->bd_export = class_export_get(exp);
        desc->bd_req = req;

        desc->bd_cbid.cbid_fn  = server_bulk_callback;
        desc->bd_cbid.cbid_arg = desc;

        /* NB we don't assign rq_bulk here; server-side requests are
         * re-used, and the handler frees the bulk desc explicitly. */

        return desc;
}

/**
 * Add a page \a page to the bulk descriptor \a desc.
 * Data to transfer in the page starts at offset \a pageoffset and
 * amount of data to transfer from the page is \a len
 */
void ptlrpc_prep_bulk_page(struct ptlrpc_bulk_desc *desc,
                           cfs_page_t *page, int pageoffset, int len)
{
        LASSERT(desc->bd_iov_count < desc->bd_max_iov);
        LASSERT(page != NULL);
        LASSERT(pageoffset >= 0);
        LASSERT(len > 0);
        LASSERT(pageoffset + len <= CFS_PAGE_SIZE);

        desc->bd_nob += len;

        cfs_page_pin(page);
        ptlrpc_add_bulk_page(desc, page, pageoffset, len);
}

/**
 * Uninitialize and free bulk descriptor \a desc.
 * Works on bulk descriptors both from server and client side.
 */
void ptlrpc_free_bulk(struct ptlrpc_bulk_desc *desc)
{
        int i;
        ENTRY;

        LASSERT(desc != NULL);
        LASSERT(desc->bd_iov_count != LI_POISON); /* not freed already */
        LASSERT(!desc->bd_network_rw);         /* network hands off or */
        LASSERT((desc->bd_export != NULL) ^ (desc->bd_import != NULL));

        sptlrpc_enc_pool_put_pages(desc);

        if (desc->bd_export)
                class_export_put(desc->bd_export);
        else
                class_import_put(desc->bd_import);

        for (i = 0; i < desc->bd_iov_count ; i++)
                cfs_page_unpin(desc->bd_iov[i].kiov_page);

        OBD_FREE(desc, offsetof(struct ptlrpc_bulk_desc,
                                bd_iov[desc->bd_max_iov]));
        EXIT;
}

/**
 * Set server timelimit for this req, i.e. how long are we willing to wait
 * for reply before timing out this request.
 */
void ptlrpc_at_set_req_timeout(struct ptlrpc_request *req)
{
        __u32 serv_est;
        int idx;
        struct imp_at *at;

        LASSERT(req->rq_import);

        if (AT_OFF) {
                /* non-AT settings */
                /**
                 * \a imp_server_timeout means this is reverse import and
                 * we send (currently only) ASTs to the client and cannot afford
                 * to wait too long for the reply, otherwise the other client
                 * (because of which we are sending this request) would
                 * timeout waiting for us
                 */
                req->rq_timeout = req->rq_import->imp_server_timeout ?
                                  obd_timeout / 2 : obd_timeout;
        } else {
                at = &req->rq_import->imp_at;
                idx = import_at_get_index(req->rq_import,
                                          req->rq_request_portal);
                serv_est = at_get(&at->iat_service_estimate[idx]);
                req->rq_timeout = at_est2timeout(serv_est);
        }
        /* We could get even fancier here, using history to predict increased
           loading... */

        /* Let the server know what this RPC timeout is by putting it in the
           reqmsg*/
        lustre_msg_set_timeout(req->rq_reqmsg, req->rq_timeout);
}

/* Adjust max service estimate based on server value */
static void ptlrpc_at_adj_service(struct ptlrpc_request *req,
                                  unsigned int serv_est)
{
        int idx;
        unsigned int oldse;
        struct imp_at *at;

        LASSERT(req->rq_import);
        at = &req->rq_import->imp_at;

        idx = import_at_get_index(req->rq_import, req->rq_request_portal);
        /* max service estimates are tracked on the server side,
           so just keep minimal history here */
        oldse = at_measured(&at->iat_service_estimate[idx], serv_est);
        if (oldse != 0)
                CDEBUG(D_ADAPTTO, "The RPC service estimate for %s ptl %d "
                       "has changed from %d to %d\n",
                       req->rq_import->imp_obd->obd_name,req->rq_request_portal,
                       oldse, at_get(&at->iat_service_estimate[idx]));
}

/* Expected network latency per remote node (secs) */
int ptlrpc_at_get_net_latency(struct ptlrpc_request *req)
{
        return AT_OFF ? 0 : at_get(&req->rq_import->imp_at.iat_net_latency);
}

/* Adjust expected network latency */
static void ptlrpc_at_adj_net_latency(struct ptlrpc_request *req,
                                      unsigned int service_time)
{
        unsigned int nl, oldnl;
        struct imp_at *at;
        time_t now = cfs_time_current_sec();

        LASSERT(req->rq_import);
        at = &req->rq_import->imp_at;

        /* Network latency is total time less server processing time */
        nl = max_t(int, now - req->rq_sent - service_time, 0) +1/*st rounding*/;
        if (service_time > now - req->rq_sent + 3 /* bz16408 */)
                CWARN("Reported service time %u > total measured time "
                      CFS_DURATION_T"\n", service_time,
                      cfs_time_sub(now, req->rq_sent));

        oldnl = at_measured(&at->iat_net_latency, nl);
        if (oldnl != 0)
                CDEBUG(D_ADAPTTO, "The network latency for %s (nid %s) "
                       "has changed from %d to %d\n",
                       req->rq_import->imp_obd->obd_name,
                       obd_uuid2str(
                               &req->rq_import->imp_connection->c_remote_uuid),
                       oldnl, at_get(&at->iat_net_latency));
}

static int unpack_reply(struct ptlrpc_request *req)
{
        int rc;

        if (SPTLRPC_FLVR_POLICY(req->rq_flvr.sf_rpc) != SPTLRPC_POLICY_NULL) {
                rc = ptlrpc_unpack_rep_msg(req, req->rq_replen);
                if (rc) {
                        DEBUG_REQ(D_ERROR, req, "unpack_rep failed: %d", rc);
                        return(-EPROTO);
                }
        }

        rc = lustre_unpack_rep_ptlrpc_body(req, MSG_PTLRPC_BODY_OFF);
        if (rc) {
                DEBUG_REQ(D_ERROR, req, "unpack ptlrpc body failed: %d", rc);
                return(-EPROTO);
        }
        return 0;
}

/**
 * Handle an early reply message, called with the rq_lock held.
 * If anything goes wrong just ignore it - same as if it never happened
 */
static int ptlrpc_at_recv_early_reply(struct ptlrpc_request *req)
{
        struct ptlrpc_request *early_req;
        time_t                 olddl;
        int                    rc;
        ENTRY;

        req->rq_early = 0;
        cfs_spin_unlock(&req->rq_lock);

        rc = sptlrpc_cli_unwrap_early_reply(req, &early_req);
        if (rc) {
                cfs_spin_lock(&req->rq_lock);
                RETURN(rc);
        }

        rc = unpack_reply(early_req);
        if (rc == 0) {
                /* Expecting to increase the service time estimate here */
                ptlrpc_at_adj_service(req,
                        lustre_msg_get_timeout(early_req->rq_repmsg));
                ptlrpc_at_adj_net_latency(req,
                        lustre_msg_get_service_time(early_req->rq_repmsg));
        }

        sptlrpc_cli_finish_early_reply(early_req);

        cfs_spin_lock(&req->rq_lock);

        if (rc == 0) {
                /* Adjust the local timeout for this req */
                ptlrpc_at_set_req_timeout(req);

                olddl = req->rq_deadline;
                /* server assumes it now has rq_timeout from when it sent the
                   early reply, so client should give it at least that long. */
                req->rq_deadline = cfs_time_current_sec() + req->rq_timeout +
                            ptlrpc_at_get_net_latency(req);

                DEBUG_REQ(D_ADAPTTO, req,
                          "Early reply #%d, new deadline in "CFS_DURATION_T"s "
                          "("CFS_DURATION_T"s)", req->rq_early_count,
                          cfs_time_sub(req->rq_deadline,
                                       cfs_time_current_sec()),
                          cfs_time_sub(req->rq_deadline, olddl));
        }

        RETURN(rc);
}

/**
 * Wind down request pool \a pool.
 * Frees all requests from the pool too
 */
void ptlrpc_free_rq_pool(struct ptlrpc_request_pool *pool)
{
        cfs_list_t *l, *tmp;
        struct ptlrpc_request *req;

        LASSERT(pool != NULL);

        cfs_spin_lock(&pool->prp_lock);
        cfs_list_for_each_safe(l, tmp, &pool->prp_req_list) {
                req = cfs_list_entry(l, struct ptlrpc_request, rq_list);
                cfs_list_del(&req->rq_list);
                LASSERT(req->rq_reqbuf);
                LASSERT(req->rq_reqbuf_len == pool->prp_rq_size);
                OBD_FREE_LARGE(req->rq_reqbuf, pool->prp_rq_size);
                OBD_FREE(req, sizeof(*req));
        }
        cfs_spin_unlock(&pool->prp_lock);
        OBD_FREE(pool, sizeof(*pool));
}

/**
 * Allocates, initializes and adds \a num_rq requests to the pool \a pool
 */
void ptlrpc_add_rqs_to_pool(struct ptlrpc_request_pool *pool, int num_rq)
{
        int i;
        int size = 1;

        while (size < pool->prp_rq_size)
                size <<= 1;

        LASSERTF(cfs_list_empty(&pool->prp_req_list) ||
                 size == pool->prp_rq_size,
                 "Trying to change pool size with nonempty pool "
                 "from %d to %d bytes\n", pool->prp_rq_size, size);

        cfs_spin_lock(&pool->prp_lock);
        pool->prp_rq_size = size;
        for (i = 0; i < num_rq; i++) {
                struct ptlrpc_request *req;
                struct lustre_msg *msg;

                cfs_spin_unlock(&pool->prp_lock);
                OBD_ALLOC(req, sizeof(struct ptlrpc_request));
                if (!req)
                        return;
                OBD_ALLOC_LARGE(msg, size);
                if (!msg) {
                        OBD_FREE(req, sizeof(struct ptlrpc_request));
                        return;
                }
                req->rq_reqbuf = msg;
                req->rq_reqbuf_len = size;
                req->rq_pool = pool;
                cfs_spin_lock(&pool->prp_lock);
                cfs_list_add_tail(&req->rq_list, &pool->prp_req_list);
        }
        cfs_spin_unlock(&pool->prp_lock);
        return;
}

/**
 * Create and initialize new request pool with given attributes:
 * \a num_rq - initial number of requests to create for the pool
 * \a msgsize - maximum message size possible for requests in thid pool
 * \a populate_pool - function to be called when more requests need to be added
 *                    to the pool
 * Returns pointer to newly created pool or NULL on error.
 */
struct ptlrpc_request_pool *
ptlrpc_init_rq_pool(int num_rq, int msgsize,
                    void (*populate_pool)(struct ptlrpc_request_pool *, int))
{
        struct ptlrpc_request_pool *pool;

        OBD_ALLOC(pool, sizeof (struct ptlrpc_request_pool));
        if (!pool)
                return NULL;

        /* Request next power of two for the allocation, because internally
           kernel would do exactly this */

        cfs_spin_lock_init(&pool->prp_lock);
        CFS_INIT_LIST_HEAD(&pool->prp_req_list);
        pool->prp_rq_size = msgsize + SPTLRPC_MAX_PAYLOAD;
        pool->prp_populate = populate_pool;

        populate_pool(pool, num_rq);

        if (cfs_list_empty(&pool->prp_req_list)) {
                /* have not allocated a single request for the pool */
                OBD_FREE(pool, sizeof (struct ptlrpc_request_pool));
                pool = NULL;
        }
        return pool;
}

/**
 * Fetches one request from pool \a pool
 */
static struct ptlrpc_request *
ptlrpc_prep_req_from_pool(struct ptlrpc_request_pool *pool)
{
        struct ptlrpc_request *request;
        struct lustre_msg *reqbuf;

        if (!pool)
                return NULL;

        cfs_spin_lock(&pool->prp_lock);

        /* See if we have anything in a pool, and bail out if nothing,
         * in writeout path, where this matters, this is safe to do, because
         * nothing is lost in this case, and when some in-flight requests
         * complete, this code will be called again. */
        if (unlikely(cfs_list_empty(&pool->prp_req_list))) {
                cfs_spin_unlock(&pool->prp_lock);
                return NULL;
        }

        request = cfs_list_entry(pool->prp_req_list.next, struct ptlrpc_request,
                                 rq_list);
        cfs_list_del_init(&request->rq_list);
        cfs_spin_unlock(&pool->prp_lock);

        LASSERT(request->rq_reqbuf);
        LASSERT(request->rq_pool);

        reqbuf = request->rq_reqbuf;
        memset(request, 0, sizeof(*request));
        request->rq_reqbuf = reqbuf;
        request->rq_reqbuf_len = pool->prp_rq_size;
        request->rq_pool = pool;

        return request;
}

/**
 * Returns freed \a request to pool.
 */
static void __ptlrpc_free_req_to_pool(struct ptlrpc_request *request)
{
        struct ptlrpc_request_pool *pool = request->rq_pool;

        cfs_spin_lock(&pool->prp_lock);
        LASSERT(cfs_list_empty(&request->rq_list));
        LASSERT(!request->rq_receiving_reply);
        cfs_list_add_tail(&request->rq_list, &pool->prp_req_list);
        cfs_spin_unlock(&pool->prp_lock);
}

static int __ptlrpc_request_bufs_pack(struct ptlrpc_request *request,
                                      __u32 version, int opcode,
                                      int count, __u32 *lengths, char **bufs,
                                      struct ptlrpc_cli_ctx *ctx)
{
        struct obd_import  *imp = request->rq_import;
        int                 rc;
        ENTRY;

        if (unlikely(ctx))
                request->rq_cli_ctx = sptlrpc_cli_ctx_get(ctx);
        else {
                rc = sptlrpc_req_get_ctx(request);
                if (rc)
                        GOTO(out_free, rc);
        }

        sptlrpc_req_set_flavor(request, opcode);

        rc = lustre_pack_request(request, imp->imp_msg_magic, count,
                                 lengths, bufs);
        if (rc) {
                LASSERT(!request->rq_pool);
                GOTO(out_ctx, rc);
        }

        lustre_msg_add_version(request->rq_reqmsg, version);
        request->rq_send_state = LUSTRE_IMP_FULL;
        request->rq_type = PTL_RPC_MSG_REQUEST;
        request->rq_export = NULL;

        request->rq_req_cbid.cbid_fn  = request_out_callback;
        request->rq_req_cbid.cbid_arg = request;

        request->rq_reply_cbid.cbid_fn  = reply_in_callback;
        request->rq_reply_cbid.cbid_arg = request;

        request->rq_reply_deadline = 0;
        request->rq_phase = RQ_PHASE_NEW;
        request->rq_next_phase = RQ_PHASE_UNDEFINED;

        request->rq_request_portal = imp->imp_client->cli_request_portal;
        request->rq_reply_portal = imp->imp_client->cli_reply_portal;

        ptlrpc_at_set_req_timeout(request);

        cfs_spin_lock_init(&request->rq_lock);
        CFS_INIT_LIST_HEAD(&request->rq_list);
        CFS_INIT_LIST_HEAD(&request->rq_timed_list);
        CFS_INIT_LIST_HEAD(&request->rq_replay_list);
        CFS_INIT_LIST_HEAD(&request->rq_ctx_chain);
        CFS_INIT_LIST_HEAD(&request->rq_set_chain);
        CFS_INIT_LIST_HEAD(&request->rq_history_list);
        CFS_INIT_LIST_HEAD(&request->rq_exp_list);
        cfs_waitq_init(&request->rq_reply_waitq);
        cfs_waitq_init(&request->rq_set_waitq);
        request->rq_xid = ptlrpc_next_xid();
        cfs_atomic_set(&request->rq_refcount, 1);

        lustre_msg_set_opc(request->rq_reqmsg, opcode);

        RETURN(0);
out_ctx:
        sptlrpc_cli_ctx_put(request->rq_cli_ctx, 1);
out_free:
        class_import_put(imp);
        return rc;
}

int ptlrpc_request_bufs_pack(struct ptlrpc_request *request,
                             __u32 version, int opcode, char **bufs,
                             struct ptlrpc_cli_ctx *ctx)
{
        int count;

        count = req_capsule_filled_sizes(&request->rq_pill, RCL_CLIENT);
        return __ptlrpc_request_bufs_pack(request, version, opcode, count,
                                          request->rq_pill.rc_area[RCL_CLIENT],
                                          bufs, ctx);
}
EXPORT_SYMBOL(ptlrpc_request_bufs_pack);

/**
 * Pack request buffers for network transfer, performing necessary encryption
 * steps if necessary.
 */
int ptlrpc_request_pack(struct ptlrpc_request *request,
                        __u32 version, int opcode)
{
        return ptlrpc_request_bufs_pack(request, version, opcode, NULL, NULL);
}

/**
 * Helper function to allocate new request on import \a imp
 * and possibly using existing request from pool \a pool if provided.
 * Returns allocated request structure with import field filled or
 * NULL on error.
 */
static inline
struct ptlrpc_request *__ptlrpc_request_alloc(struct obd_import *imp,
                                              struct ptlrpc_request_pool *pool)
{
        struct ptlrpc_request *request = NULL;

        if (pool)
                request = ptlrpc_prep_req_from_pool(pool);

        if (!request)
                OBD_ALLOC_PTR(request);

        if (request) {
                LASSERTF((unsigned long)imp > 0x1000, "%p", imp);
                LASSERT(imp != LP_POISON);
                LASSERTF((unsigned long)imp->imp_client > 0x1000, "%p",
                        imp->imp_client);
                LASSERT(imp->imp_client != LP_POISON);

                request->rq_import = class_import_get(imp);
        } else {
                CERROR("request allocation out of memory\n");
        }

        return request;
}

/**
 * Helper function for creating a request.
 * Calls __ptlrpc_request_alloc to allocate new request sturcture and inits
 * buffer structures according to capsule template \a format.
 * Returns allocated request structure pointer or NULL on error.
 */
static struct ptlrpc_request *
ptlrpc_request_alloc_internal(struct obd_import *imp,
                              struct ptlrpc_request_pool * pool,
                              const struct req_format *format)
{
        struct ptlrpc_request *request;

        request = __ptlrpc_request_alloc(imp, pool);
        if (request == NULL)
                return NULL;

        req_capsule_init(&request->rq_pill, request, RCL_CLIENT);
        req_capsule_set(&request->rq_pill, format);
        return request;
}

/**
 * Allocate new request structure for import \a imp and initialize its
 * buffer structure according to capsule template \a format.
 */
struct ptlrpc_request *ptlrpc_request_alloc(struct obd_import *imp,
                                            const struct req_format *format)
{
        return ptlrpc_request_alloc_internal(imp, NULL, format);
}

/**
 * Allocate new request structure for import \a imp from pool \a pool and
 * initialize its buffer structure according to capsule template \a format.
 */
struct ptlrpc_request *ptlrpc_request_alloc_pool(struct obd_import *imp,
                                            struct ptlrpc_request_pool * pool,
                                            const struct req_format *format)
{
        return ptlrpc_request_alloc_internal(imp, pool, format);
}

/**
 * For requests not from pool, free memory of the request structure.
 * For requests obtained from a pool earlier, return request back to pool.
 */
void ptlrpc_request_free(struct ptlrpc_request *request)
{
        if (request->rq_pool)
                __ptlrpc_free_req_to_pool(request);
        else
                OBD_FREE_PTR(request);
}

/**
 * Allocate new request for operatione \a opcode and immediatelly pack it for
 * network transfer.
 * Only used for simple requests like OBD_PING where the only important
 * part of the request is operation itself.
 * Returns allocated request or NULL on error.
 */
struct ptlrpc_request *ptlrpc_request_alloc_pack(struct obd_import *imp,
                                                const struct req_format *format,
                                                __u32 version, int opcode)
{
        struct ptlrpc_request *req = ptlrpc_request_alloc(imp, format);
        int                    rc;

        if (req) {
                rc = ptlrpc_request_pack(req, version, opcode);
                if (rc) {
                        ptlrpc_request_free(req);
                        req = NULL;
                }
        }
        return req;
}

/**
 * Prepare request (fetched from pool \a poolif not NULL) on import \a imp
 * for operation \a opcode. Request would contain \a count buffers.
 * Sizes of buffers are described in array \a lengths and buffers themselves
 * are provided by a pointer \a bufs.
 * Returns prepared request structure pointer or NULL on error.
 */
struct ptlrpc_request *
ptlrpc_prep_req_pool(struct obd_import *imp,
                     __u32 version, int opcode,
                     int count, __u32 *lengths, char **bufs,
                     struct ptlrpc_request_pool *pool)
{
        struct ptlrpc_request *request;
        int                    rc;

        request = __ptlrpc_request_alloc(imp, pool);
        if (!request)
                return NULL;

        rc = __ptlrpc_request_bufs_pack(request, version, opcode, count,
                                        lengths, bufs, NULL);
        if (rc) {
                ptlrpc_request_free(request);
                request = NULL;
        }
        return request;
}

/**
 * Same as ptlrpc_prep_req_pool, but without pool
 */
struct ptlrpc_request *
ptlrpc_prep_req(struct obd_import *imp, __u32 version, int opcode, int count,
                __u32 *lengths, char **bufs)
{
        return ptlrpc_prep_req_pool(imp, version, opcode, count, lengths, bufs,
                                    NULL);
}

/**
 * Allocate "fake" request that would not be sent anywhere in the end.
 * Only used as a hack because we have no other way of performing
 * async actions in lustre between layers.
 * Used on MDS to request object preallocations from more than one OST at a
 * time.
 */
struct ptlrpc_request *ptlrpc_prep_fakereq(struct obd_import *imp,
                                           unsigned int timeout,
                                           ptlrpc_interpterer_t interpreter)
{
        struct ptlrpc_request *request = NULL;
        ENTRY;

        OBD_ALLOC(request, sizeof(*request));
        if (!request) {
                CERROR("request allocation out of memory\n");
                RETURN(NULL);
        }

        request->rq_send_state = LUSTRE_IMP_FULL;
        request->rq_type = PTL_RPC_MSG_REQUEST;
        request->rq_import = class_import_get(imp);
        request->rq_export = NULL;
        request->rq_import_generation = imp->imp_generation;

        request->rq_timeout = timeout;
        request->rq_sent = cfs_time_current_sec();
        request->rq_deadline = request->rq_sent + timeout;
        request->rq_reply_deadline = request->rq_deadline;
        request->rq_interpret_reply = interpreter;
        request->rq_phase = RQ_PHASE_RPC;
        request->rq_next_phase = RQ_PHASE_INTERPRET;
        /* don't want reply */
        request->rq_receiving_reply = 0;
        request->rq_must_unlink = 0;
        request->rq_no_delay = request->rq_no_resend = 1;
        request->rq_fake = 1;

        cfs_spin_lock_init(&request->rq_lock);
        CFS_INIT_LIST_HEAD(&request->rq_list);
        CFS_INIT_LIST_HEAD(&request->rq_replay_list);
        CFS_INIT_LIST_HEAD(&request->rq_set_chain);
        CFS_INIT_LIST_HEAD(&request->rq_history_list);
        CFS_INIT_LIST_HEAD(&request->rq_exp_list);
        cfs_waitq_init(&request->rq_reply_waitq);
        cfs_waitq_init(&request->rq_set_waitq);

        request->rq_xid = ptlrpc_next_xid();
        cfs_atomic_set(&request->rq_refcount, 1);

        RETURN(request);
}

/**
 * Indicate that processing of "fake" request is finished.
 */
void ptlrpc_fakereq_finished(struct ptlrpc_request *req)
{
        /* if we kill request before timeout - need adjust counter */
        if (req->rq_phase == RQ_PHASE_RPC) {
                struct ptlrpc_request_set *set = req->rq_set;

                if (set)
                        cfs_atomic_dec(&set->set_remaining);
        }

        ptlrpc_rqphase_move(req, RQ_PHASE_COMPLETE);
        cfs_list_del_init(&req->rq_list);
}

/**
 * Allocate and initialize new request set structure.
 * Returns a pointer to the newly allocated set structure or NULL on error.
 */
struct ptlrpc_request_set *ptlrpc_prep_set(void)
{
        struct ptlrpc_request_set *set;

        ENTRY;
        OBD_ALLOC(set, sizeof *set);
        if (!set)
                RETURN(NULL);
        CFS_INIT_LIST_HEAD(&set->set_requests);
        cfs_waitq_init(&set->set_waitq);
        cfs_atomic_set(&set->set_remaining, 0);
        cfs_spin_lock_init(&set->set_new_req_lock);
        CFS_INIT_LIST_HEAD(&set->set_new_requests);
        CFS_INIT_LIST_HEAD(&set->set_cblist);

        RETURN(set);
}

/**
 * Wind down and free request set structure previously allocated with
 * ptlrpc_prep_set.
 * Ensures that all requests on the set have completed and removes
 * all requests from the request list in a set.
 * If any unsent request happen to be on the list, pretends that they got
 * an error in flight and calls their completion handler.
 */
void ptlrpc_set_destroy(struct ptlrpc_request_set *set)
{
        cfs_list_t       *tmp;
        cfs_list_t       *next;
        int               expected_phase;
        int               n = 0;
        ENTRY;

        /* Requests on the set should either all be completed, or all be new */
        expected_phase = (cfs_atomic_read(&set->set_remaining) == 0) ?
                         RQ_PHASE_COMPLETE : RQ_PHASE_NEW;
        cfs_list_for_each (tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_set_chain);

                LASSERT(req->rq_phase == expected_phase);
                n++;
        }

        LASSERTF(cfs_atomic_read(&set->set_remaining) == 0 || 
                 cfs_atomic_read(&set->set_remaining) == n, "%d / %d\n",
                 cfs_atomic_read(&set->set_remaining), n);

        cfs_list_for_each_safe(tmp, next, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_set_chain);
                cfs_list_del_init(&req->rq_set_chain);

                LASSERT(req->rq_phase == expected_phase);

                if (req->rq_phase == RQ_PHASE_NEW) {
                        ptlrpc_req_interpret(NULL, req, -EBADR);
                        cfs_atomic_dec(&set->set_remaining);
                }

                cfs_spin_lock(&req->rq_lock);
                req->rq_set = NULL;
                req->rq_invalid_rqset = 0;
                cfs_spin_unlock(&req->rq_lock);

                ptlrpc_req_finished (req);
        }

        LASSERT(cfs_atomic_read(&set->set_remaining) == 0);

        OBD_FREE(set, sizeof(*set));
        EXIT;
}

/**
 * Add a callback function \a fn to the set.
 * This function would be called when all requests on this set are completed.
 * The function will be passed \a data argument.
 */
int ptlrpc_set_add_cb(struct ptlrpc_request_set *set,
                      set_interpreter_func fn, void *data)
{
        struct ptlrpc_set_cbdata *cbdata;

        OBD_ALLOC_PTR(cbdata);
        if (cbdata == NULL)
                RETURN(-ENOMEM);

        cbdata->psc_interpret = fn;
        cbdata->psc_data = data;
        cfs_list_add_tail(&cbdata->psc_item, &set->set_cblist);

        RETURN(0);
}

/**
 * Add a new request to the general purpose request set.
 * Assumes request reference from the caller.
 */
void ptlrpc_set_add_req(struct ptlrpc_request_set *set,
                        struct ptlrpc_request *req)
{
        /* The set takes over the caller's request reference */
        cfs_list_add_tail(&req->rq_set_chain, &set->set_requests);
        req->rq_set = set;
        cfs_atomic_inc(&set->set_remaining);
        req->rq_queued_time = cfs_time_current(); /* Where is the best place to set this? */
}

/**
 * Add a request to a request with dedicated server thread
 * and wake the thread to make any necessary processing.
 * Currently only used for ptlrpcd.
 * Returns 0 if succesful or non zero error code on error.
 * (the only possible error for now is if the dedicated server thread
 * is shutting down)
 */
int ptlrpc_set_add_new_req(struct ptlrpcd_ctl *pc,
                           struct ptlrpc_request *req)
{
        struct ptlrpc_request_set *set = pc->pc_set;

        /*
         * Let caller know that we stopped and will not handle this request.
         * It needs to take care itself of request.
         */
        if (cfs_test_bit(LIOD_STOP, &pc->pc_flags))
                return -EALREADY;

        cfs_spin_lock(&set->set_new_req_lock);
        /*
         * The set takes over the caller's request reference.
         */
        cfs_list_add_tail(&req->rq_set_chain, &set->set_new_requests);
        req->rq_set = set;
        cfs_spin_unlock(&set->set_new_req_lock);

        cfs_waitq_signal(&set->set_waitq);
        return 0;
}

/**
 * Based on the current state of the import, determine if the request
 * can be sent, is an error, or should be delayed.
 *
 * Returns true if this request should be delayed. If false, and
 * *status is set, then the request can not be sent and *status is the
 * error code.  If false and status is 0, then request can be sent.
 *
 * The imp->imp_lock must be held.
 */
static int ptlrpc_import_delay_req(struct obd_import *imp,
                                   struct ptlrpc_request *req, int *status)
{
        int delay = 0;
        ENTRY;

        LASSERT (status != NULL);
        *status = 0;

        if (req->rq_ctx_init || req->rq_ctx_fini) {
                /* always allow ctx init/fini rpc go through */
        } else if (imp->imp_state == LUSTRE_IMP_NEW) {
                DEBUG_REQ(D_ERROR, req, "Uninitialized import.");
                *status = -EIO;
                LBUG();
        } else if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                DEBUG_REQ(D_ERROR, req, "IMP_CLOSED ");
                *status = -EIO;
        } else if (ptlrpc_send_limit_expired(req)) {
                /* probably doesn't need to be a D_ERROR after initial testing */
                DEBUG_REQ(D_ERROR, req, "send limit expired ");
                *status = -EIO;
        } else if (req->rq_send_state == LUSTRE_IMP_CONNECTING &&
                   imp->imp_state == LUSTRE_IMP_CONNECTING) {
                /* allow CONNECT even if import is invalid */ ;
                if (cfs_atomic_read(&imp->imp_inval_count) != 0) {
                        DEBUG_REQ(D_ERROR, req, "invalidate in flight");
                        *status = -EIO;
                }
        } else if (imp->imp_invalid || imp->imp_obd->obd_no_recov) {
                if (!imp->imp_deactive)
                          DEBUG_REQ(D_ERROR, req, "IMP_INVALID");
                *status = -ESHUTDOWN; /* bz 12940 */
        } else if (req->rq_import_generation != imp->imp_generation) {
                DEBUG_REQ(D_ERROR, req, "req wrong generation:");
                *status = -EIO;
        } else if (req->rq_send_state != imp->imp_state) {
                /* invalidate in progress - any requests should be drop */
                if (cfs_atomic_read(&imp->imp_inval_count) != 0) {
                        DEBUG_REQ(D_ERROR, req, "invalidate in flight");
                        *status = -EIO;
                } else if (imp->imp_dlm_fake || req->rq_no_delay) {
                        *status = -EWOULDBLOCK;
                } else {
                        delay = 1;
                }
        }

        RETURN(delay);
}

/**
 * Decide if the eror message regarding provided request \a req
 * should be printed to the console or not.
 * Makes it's decision on request status and other properties.
 * Returns 1 to print error on the system console or 0 if not.
 */
static int ptlrpc_console_allow(struct ptlrpc_request *req)
{
        __u32 opc = lustre_msg_get_opc(req->rq_reqmsg);
        int err;

        /* Suppress particular reconnect errors which are to be expected.  No
         * errors are suppressed for the initial connection on an import */
        if ((lustre_handle_is_used(&req->rq_import->imp_remote_handle)) &&
            (opc == OST_CONNECT || opc == MDS_CONNECT || opc == MGS_CONNECT)) {

                /* Suppress timed out reconnect requests */
                if (req->rq_timedout)
                        return 0;

                /* Suppress unavailable/again reconnect requests */
                err = lustre_msg_get_status(req->rq_repmsg);
                if (err == -ENODEV || err == -EAGAIN)
                        return 0;
        }

        return 1;
}

/**
 * Check request processing status.
 * Returns the status.
 */
static int ptlrpc_check_status(struct ptlrpc_request *req)
{
        int err;
        ENTRY;

        err = lustre_msg_get_status(req->rq_repmsg);
        if (lustre_msg_get_type(req->rq_repmsg) == PTL_RPC_MSG_ERR) {
                struct obd_import *imp = req->rq_import;
                __u32 opc = lustre_msg_get_opc(req->rq_reqmsg);
                LCONSOLE_ERROR_MSG(0x011,"an error occurred while communicating"
                                " with %s. The %s operation failed with %d\n",
                                libcfs_nid2str(imp->imp_connection->c_peer.nid),
                                ll_opcode2str(opc), err);
                RETURN(err < 0 ? err : -EINVAL);
        }

        if (err < 0) {
                DEBUG_REQ(D_INFO, req, "status is %d", err);
        } else if (err > 0) {
                /* XXX: translate this error from net to host */
                DEBUG_REQ(D_INFO, req, "status is %d", err);
        }

        if (lustre_msg_get_type(req->rq_repmsg) == PTL_RPC_MSG_ERR) {
                struct obd_import *imp = req->rq_import;
                __u32 opc = lustre_msg_get_opc(req->rq_reqmsg);

                if (ptlrpc_console_allow(req))
                        LCONSOLE_ERROR_MSG(0x011,"an error occurred while "
                                           "communicating with %s. The %s "
                                           "operation failed with %d\n",
                                           libcfs_nid2str(
                                           imp->imp_connection->c_peer.nid),
                                           ll_opcode2str(opc), err);

                RETURN(err < 0 ? err : -EINVAL);
        }

        RETURN(err);
}

/**
 * save pre-versions of objects into request for replay.
 * Versions are obtained from server reply.
 * used for VBR.
 */
static void ptlrpc_save_versions(struct ptlrpc_request *req)
{
        struct lustre_msg *repmsg = req->rq_repmsg;
        struct lustre_msg *reqmsg = req->rq_reqmsg;
        __u64 *versions = lustre_msg_get_versions(repmsg);
        ENTRY;

        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY)
                return;

        LASSERT(versions);
        lustre_msg_set_versions(reqmsg, versions);
        CDEBUG(D_INFO, "Client save versions ["LPX64"/"LPX64"]\n",
               versions[0], versions[1]);

        EXIT;
}

/**
 * Callback function called when client receives RPC reply for \a req.
 * Returns 0 on success or error code.
 * The return alue would be assigned to req->rq_status by the caller
 * as request processing status.
 * This function also decides if the request needs to be saved for later replay.
 */
static int after_reply(struct ptlrpc_request *req)
{
        struct obd_import *imp = req->rq_import;
        struct obd_device *obd = req->rq_import->imp_obd;
        int rc;
        struct timeval work_start;
        long timediff;
        ENTRY;

        LASSERT(obd != NULL);
        /* repbuf must be unlinked */
        LASSERT(!req->rq_receiving_reply && !req->rq_must_unlink);

        if (req->rq_reply_truncate) {
                if (ptlrpc_no_resend(req)) {
                        DEBUG_REQ(D_ERROR, req, "reply buffer overflow,"
                                  " expected: %d, actual size: %d",
                                  req->rq_nob_received, req->rq_repbuf_len);
                        RETURN(-EOVERFLOW);
                }

                sptlrpc_cli_free_repbuf(req);
                /* Pass the required reply buffer size (include
                 * space for early reply).
                 * NB: no need to roundup because alloc_repbuf
                 * will roundup it */
                req->rq_replen       = req->rq_nob_received;
                req->rq_nob_received = 0;
                req->rq_resend       = 1;
                RETURN(0);
        }

        /*
         * NB Until this point, the whole of the incoming message,
         * including buflens, status etc is in the sender's byte order.
         */
        rc = sptlrpc_cli_unwrap_reply(req);
        if (rc) {
                DEBUG_REQ(D_ERROR, req, "unwrap reply failed (%d):", rc);
                RETURN(rc);
        }

        /*
         * Security layer unwrap might ask resend this request.
         */
        if (req->rq_resend)
                RETURN(0);

        rc = unpack_reply(req);
        if (rc)
                RETURN(rc);

        cfs_gettimeofday(&work_start);
        timediff = cfs_timeval_sub(&work_start, &req->rq_arrival_time, NULL);
        if (obd->obd_svc_stats != NULL) {
                lprocfs_counter_add(obd->obd_svc_stats, PTLRPC_REQWAIT_CNTR,
                                    timediff);
                ptlrpc_lprocfs_rpc_sent(req, timediff);
        }

        if (lustre_msg_get_type(req->rq_repmsg) != PTL_RPC_MSG_REPLY &&
            lustre_msg_get_type(req->rq_repmsg) != PTL_RPC_MSG_ERR) {
                DEBUG_REQ(D_ERROR, req, "invalid packet received (type=%u)",
                          lustre_msg_get_type(req->rq_repmsg));
                RETURN(-EPROTO);
        }

        if (lustre_msg_get_opc(req->rq_reqmsg) != OBD_PING)
                CFS_FAIL_TIMEOUT(OBD_FAIL_PTLRPC_PAUSE_REP, cfs_fail_val);
        ptlrpc_at_adj_service(req, lustre_msg_get_timeout(req->rq_repmsg));
        ptlrpc_at_adj_net_latency(req,
                                  lustre_msg_get_service_time(req->rq_repmsg));

        rc = ptlrpc_check_status(req);
        imp->imp_connect_error = rc;

        if (rc) {
                /*
                 * Either we've been evicted, or the server has failed for
                 * some reason. Try to reconnect, and if that fails, punt to
                 * the upcall.
                 */
                if (ll_rpc_recoverable_error(rc)) {
                        if (req->rq_send_state != LUSTRE_IMP_FULL ||
                            imp->imp_obd->obd_no_recov || imp->imp_dlm_fake) {
                                RETURN(rc);
                        }
                        ptlrpc_request_handle_notconn(req);
                        RETURN(rc);
                }
        } else {
                /*
                 * Let's look if server sent slv. Do it only for RPC with
                 * rc == 0.
                 */
                ldlm_cli_update_pool(req);
        }

        /*
         * Store transno in reqmsg for replay.
         */
        if (!(lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY)) {
                req->rq_transno = lustre_msg_get_transno(req->rq_repmsg);
                lustre_msg_set_transno(req->rq_reqmsg, req->rq_transno);
        }

        if (imp->imp_replayable) {
                cfs_spin_lock(&imp->imp_lock);
                /*
                 * No point in adding already-committed requests to the replay
                 * list, we will just remove them immediately. b=9829
                 */
                if (req->rq_transno != 0 &&
                    (req->rq_transno >
                     lustre_msg_get_last_committed(req->rq_repmsg) ||
                     req->rq_replay)) {
                        /** version recovery */
                        ptlrpc_save_versions(req);
                        ptlrpc_retain_replayable_request(req, imp);
                } else if (req->rq_commit_cb != NULL) {
                        cfs_spin_unlock(&imp->imp_lock);
                        req->rq_commit_cb(req);
                        cfs_spin_lock(&imp->imp_lock);
                }

                /*
                 * Replay-enabled imports return commit-status information.
                 */
                if (lustre_msg_get_last_committed(req->rq_repmsg)) {
                        imp->imp_peer_committed_transno =
                                lustre_msg_get_last_committed(req->rq_repmsg);
                }
                ptlrpc_free_committed(imp);

                if (req->rq_transno > imp->imp_peer_committed_transno)
                        ptlrpc_pinger_commit_expected(imp);

                cfs_spin_unlock(&imp->imp_lock);
        }

        RETURN(rc);
}

/**
 * Helper function to send request \a req over the network for the first time
 * Also adjusts request phase.
 * Returns 0 on success or error code.
 */ 
static int ptlrpc_send_new_req(struct ptlrpc_request *req)
{
        struct obd_import     *imp;
        int rc;
        ENTRY;

        LASSERT(req->rq_phase == RQ_PHASE_NEW);
        if (req->rq_sent && (req->rq_sent > cfs_time_current_sec()))
                RETURN (0);

        ptlrpc_rqphase_move(req, RQ_PHASE_RPC);

        imp = req->rq_import;
        cfs_spin_lock(&imp->imp_lock);

        req->rq_import_generation = imp->imp_generation;

        if (ptlrpc_import_delay_req(imp, req, &rc)) {
                cfs_spin_lock(&req->rq_lock);
                req->rq_waiting = 1;
                cfs_spin_unlock(&req->rq_lock);

                DEBUG_REQ(D_HA, req, "req from PID %d waiting for recovery: "
                          "(%s != %s)", lustre_msg_get_status(req->rq_reqmsg),
                          ptlrpc_import_state_name(req->rq_send_state),
                          ptlrpc_import_state_name(imp->imp_state));
                LASSERT(cfs_list_empty(&req->rq_list));
                cfs_list_add_tail(&req->rq_list, &imp->imp_delayed_list);
                cfs_atomic_inc(&req->rq_import->imp_inflight);
                cfs_spin_unlock(&imp->imp_lock);
                RETURN(0);
        }

        if (rc != 0) {
                cfs_spin_unlock(&imp->imp_lock);
                req->rq_status = rc;
                ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);
                RETURN(rc);
        }

        LASSERT(cfs_list_empty(&req->rq_list));
        cfs_list_add_tail(&req->rq_list, &imp->imp_sending_list);
        cfs_atomic_inc(&req->rq_import->imp_inflight);
        cfs_spin_unlock(&imp->imp_lock);

        lustre_msg_set_status(req->rq_reqmsg, cfs_curproc_pid());

        rc = sptlrpc_req_refresh_ctx(req, -1);
        if (rc) {
                if (req->rq_err) {
                        req->rq_status = rc;
                        RETURN(1);
                } else {
                        req->rq_wait_ctx = 1;
                        RETURN(0);
                }
        }

        CDEBUG(D_RPCTRACE, "Sending RPC pname:cluuid:pid:xid:nid:opc"
               " %s:%s:%d:"LPU64":%s:%d\n", cfs_curproc_comm(),
               imp->imp_obd->obd_uuid.uuid,
               lustre_msg_get_status(req->rq_reqmsg), req->rq_xid,
               libcfs_nid2str(imp->imp_connection->c_peer.nid),
               lustre_msg_get_opc(req->rq_reqmsg));

        rc = ptl_send_rpc(req, 0);
        if (rc) {
                DEBUG_REQ(D_HA, req, "send failed (%d); expect timeout", rc);
                req->rq_net_err = 1;
                RETURN(rc);
        }
        RETURN(0);
}

/**
 * this sends any unsent RPCs in \a set and returns 1 if all are sent
 * and no more replies are expected.
 * (it is possible to get less replies than requests sent e.g. due to timed out
 * requests or requests that we had trouble to send out)
 */
int ptlrpc_check_set(const struct lu_env *env, struct ptlrpc_request_set *set)
{
        cfs_list_t *tmp;
        int force_timer_recalc = 0;
        ENTRY;

        if (cfs_atomic_read(&set->set_remaining) == 0)
                RETURN(1);

        cfs_list_for_each(tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_set_chain);
                struct obd_import *imp = req->rq_import;
                int unregistered = 0;
                int rc = 0;

                if (req->rq_phase == RQ_PHASE_NEW &&
                    ptlrpc_send_new_req(req)) {
                        force_timer_recalc = 1;
                }

                /* delayed send - skip */
                if (req->rq_phase == RQ_PHASE_NEW && req->rq_sent)
                        continue;

                if (!(req->rq_phase == RQ_PHASE_RPC ||
                      req->rq_phase == RQ_PHASE_BULK ||
                      req->rq_phase == RQ_PHASE_INTERPRET ||
                      req->rq_phase == RQ_PHASE_UNREGISTERING ||
                      req->rq_phase == RQ_PHASE_COMPLETE)) {
                        DEBUG_REQ(D_ERROR, req, "bad phase %x", req->rq_phase);
                        LBUG();
                }

                if (req->rq_phase == RQ_PHASE_UNREGISTERING) {
                        LASSERT(req->rq_next_phase != req->rq_phase);
                        LASSERT(req->rq_next_phase != RQ_PHASE_UNDEFINED);

                        /*
                         * Skip processing until reply is unlinked. We
                         * can't return to pool before that and we can't
                         * call interpret before that. We need to make
                         * sure that all rdma transfers finished and will
                         * not corrupt any data.
                         */
                        if (ptlrpc_client_recv_or_unlink(req) ||
                            ptlrpc_client_bulk_active(req))
                                continue;

                        /*
                         * Turn fail_loc off to prevent it from looping
                         * forever.
                         */
                        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK)) {
                                OBD_FAIL_CHECK_ORSET(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK,
                                                     OBD_FAIL_ONCE);
                        }
                        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_BULK_UNLINK)) {
                                OBD_FAIL_CHECK_ORSET(OBD_FAIL_PTLRPC_LONG_BULK_UNLINK,
                                                     OBD_FAIL_ONCE);
                        }

                        /*
                         * Move to next phase if reply was successfully
                         * unlinked.
                         */
                        ptlrpc_rqphase_move(req, req->rq_next_phase);
                }

                if (req->rq_phase == RQ_PHASE_COMPLETE)
                        continue;

                if (req->rq_phase == RQ_PHASE_INTERPRET)
                        GOTO(interpret, req->rq_status);

                /*
                 * Note that this also will start async reply unlink.
                 */
                if (req->rq_net_err && !req->rq_timedout) {
                        ptlrpc_expire_one_request(req, 1);

                        /*
                         * Check if we still need to wait for unlink.
                         */
                        if (ptlrpc_client_recv_or_unlink(req) ||
                            ptlrpc_client_bulk_active(req))
                                continue;
                        /* If there is no need to resend, fail it now. */
                        if (req->rq_no_resend) {
                                if (req->rq_status == 0)
                                        req->rq_status = -EIO;
                                ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);
                                GOTO(interpret, req->rq_status);
                        } else {
                                continue;
                        }
                }

                if (req->rq_err) {
                        cfs_spin_lock(&req->rq_lock);
                        req->rq_replied = 0;
                        cfs_spin_unlock(&req->rq_lock);
                        if (req->rq_status == 0)
                                req->rq_status = -EIO;
                        ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);
                        GOTO(interpret, req->rq_status);
                }

                /* ptlrpc_set_wait->l_wait_event sets lwi_allow_intr
                 * so it sets rq_intr regardless of individual rpc
                 * timeouts. The synchronous IO waiting path sets 
                 * rq_intr irrespective of whether ptlrpcd
                 * has seen a timeout.  Our policy is to only interpret
                 * interrupted rpcs after they have timed out, so we
                 * need to enforce that here.
                 */

                if (req->rq_intr && (req->rq_timedout || req->rq_waiting ||
                                     req->rq_wait_ctx)) {
                        req->rq_status = -EINTR;
                        ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);
                        GOTO(interpret, req->rq_status);
                }

                if (req->rq_phase == RQ_PHASE_RPC) {
                        if (req->rq_timedout || req->rq_resend ||
                            req->rq_waiting || req->rq_wait_ctx) {
                                int status;

                                if (!ptlrpc_unregister_reply(req, 1))
                                        continue;

                                cfs_spin_lock(&imp->imp_lock);
                                if (ptlrpc_import_delay_req(imp, req, &status)){
                                        /* put on delay list - only if we wait
                                         * recovery finished - before send */
                                        cfs_list_del_init(&req->rq_list);
                                        cfs_list_add_tail(&req->rq_list,
                                                          &imp-> \
                                                          imp_delayed_list);
                                        cfs_spin_unlock(&imp->imp_lock);
                                        continue;
                                }

                                if (status != 0)  {
                                        req->rq_status = status;
                                        ptlrpc_rqphase_move(req,
                                                RQ_PHASE_INTERPRET);
                                        cfs_spin_unlock(&imp->imp_lock);
                                        GOTO(interpret, req->rq_status);
                                }
                                if (ptlrpc_no_resend(req) && !req->rq_wait_ctx) {
                                        req->rq_status = -ENOTCONN;
                                        ptlrpc_rqphase_move(req,
                                                RQ_PHASE_INTERPRET);
                                        cfs_spin_unlock(&imp->imp_lock);
                                        GOTO(interpret, req->rq_status);
                                }

                                cfs_list_del_init(&req->rq_list);
                                cfs_list_add_tail(&req->rq_list,
                                              &imp->imp_sending_list);

                                cfs_spin_unlock(&imp->imp_lock);

                                cfs_spin_lock(&req->rq_lock);
                                req->rq_waiting = 0;
                                cfs_spin_unlock(&req->rq_lock);

                                if (req->rq_timedout || req->rq_resend) {
                                        /* This is re-sending anyways,
                                         * let's mark req as resend. */
                                        cfs_spin_lock(&req->rq_lock);
                                        req->rq_resend = 1;
                                        cfs_spin_unlock(&req->rq_lock);
                                        if (req->rq_bulk) {
                                                __u64 old_xid;

                                                if (!ptlrpc_unregister_bulk(req, 1))
                                                        continue;

                                                /* ensure previous bulk fails */
                                                old_xid = req->rq_xid;
                                                req->rq_xid = ptlrpc_next_xid();
                                                CDEBUG(D_HA, "resend bulk "
                                                       "old x"LPU64
                                                       " new x"LPU64"\n",
                                                       old_xid, req->rq_xid);
                                        }
                                }
                                /*
                                 * rq_wait_ctx is only touched by ptlrpcd,
                                 * so no lock is needed here.
                                 */
                                status = sptlrpc_req_refresh_ctx(req, -1);
                                if (status) {
                                        if (req->rq_err) {
                                                req->rq_status = status;
                                                cfs_spin_lock(&req->rq_lock);
                                                req->rq_wait_ctx = 0;
                                                cfs_spin_unlock(&req->rq_lock);
                                                force_timer_recalc = 1;
                                        } else {
                                                cfs_spin_lock(&req->rq_lock);
                                                req->rq_wait_ctx = 1;
                                                cfs_spin_unlock(&req->rq_lock);
                                        }

                                        continue;
                                } else {
                                        cfs_spin_lock(&req->rq_lock);
                                        req->rq_wait_ctx = 0;
                                        cfs_spin_unlock(&req->rq_lock);
                                }

                                rc = ptl_send_rpc(req, 0);
                                if (rc) {
                                        DEBUG_REQ(D_HA, req, "send failed (%d)",
                                                  rc);
                                        force_timer_recalc = 1;
                                        cfs_spin_lock(&req->rq_lock);
                                        req->rq_net_err = 1;
                                        cfs_spin_unlock(&req->rq_lock);
                                }
                                /* need to reset the timeout */
                                force_timer_recalc = 1;
                        }

                        cfs_spin_lock(&req->rq_lock);

                        if (ptlrpc_client_early(req)) {
                                ptlrpc_at_recv_early_reply(req);
                                cfs_spin_unlock(&req->rq_lock);
                                continue;
                        }

                        /* Still waiting for a reply? */
                        if (ptlrpc_client_recv(req)) {
                                cfs_spin_unlock(&req->rq_lock);
                                continue;
                        }

                        /* Did we actually receive a reply? */
                        if (!ptlrpc_client_replied(req)) {
                                cfs_spin_unlock(&req->rq_lock);
                                continue;
                        }

                        cfs_spin_unlock(&req->rq_lock);

                        /* unlink from net because we are going to
                         * swab in-place of reply buffer */
                        unregistered = ptlrpc_unregister_reply(req, 1);
                        if (!unregistered)
                                continue;

                        req->rq_status = after_reply(req);
                        if (req->rq_resend)
                                continue;

                        /* If there is no bulk associated with this request,
                         * then we're done and should let the interpreter
                         * process the reply. Similarly if the RPC returned
                         * an error, and therefore the bulk will never arrive.
                         */
                        if (req->rq_bulk == NULL || req->rq_status != 0) {
                                ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);
                                GOTO(interpret, req->rq_status);
                        }

                        ptlrpc_rqphase_move(req, RQ_PHASE_BULK);
                }

                LASSERT(req->rq_phase == RQ_PHASE_BULK);
                if (ptlrpc_client_bulk_active(req))
                        continue;

                if (!req->rq_bulk->bd_success) {
                        /* The RPC reply arrived OK, but the bulk screwed
                         * up!  Dead weird since the server told us the RPC
                         * was good after getting the REPLY for her GET or
                         * the ACK for her PUT. */
                        DEBUG_REQ(D_ERROR, req, "bulk transfer failed");
                        LBUG();
                }

                ptlrpc_rqphase_move(req, RQ_PHASE_INTERPRET);

        interpret:
                LASSERT(req->rq_phase == RQ_PHASE_INTERPRET);

                /* This moves to "unregistering" phase we need to wait for
                 * reply unlink. */
                if (!unregistered && !ptlrpc_unregister_reply(req, 1)) {
                        /* start async bulk unlink too */
                        ptlrpc_unregister_bulk(req, 1);
                        continue;
                }

                if (!ptlrpc_unregister_bulk(req, 1))
                        continue;

                /* When calling interpret receiving already should be
                 * finished. */
                LASSERT(!req->rq_receiving_reply);

                ptlrpc_req_interpret(env, req, req->rq_status);

                ptlrpc_rqphase_move(req, RQ_PHASE_COMPLETE);

                CDEBUG(D_RPCTRACE, "Completed RPC pname:cluuid:pid:xid:nid:"
                       "opc %s:%s:%d:"LPU64":%s:%d\n", cfs_curproc_comm(),
                       imp->imp_obd->obd_uuid.uuid,
                       req->rq_reqmsg ? lustre_msg_get_status(req->rq_reqmsg):-1,
                       req->rq_xid,
                       libcfs_nid2str(imp->imp_connection->c_peer.nid),
                       req->rq_reqmsg ? lustre_msg_get_opc(req->rq_reqmsg) : -1);

                cfs_spin_lock(&imp->imp_lock);
                /* Request already may be not on sending or delaying list. This
                 * may happen in the case of marking it erroneous for the case
                 * ptlrpc_import_delay_req(req, status) find it impossible to
                 * allow sending this rpc and returns *status != 0. */
                if (!cfs_list_empty(&req->rq_list)) {
                        cfs_list_del_init(&req->rq_list);
                        cfs_atomic_dec(&imp->imp_inflight);
                }
                cfs_spin_unlock(&imp->imp_lock);

                cfs_atomic_dec(&set->set_remaining);
                cfs_waitq_broadcast(&imp->imp_recovery_waitq);
        }

        /* If we hit an error, we want to recover promptly. */
        RETURN(cfs_atomic_read(&set->set_remaining) == 0 || force_timer_recalc);
}

/**
 * Time out request \a req. is \a async_unlink is set, that means do not wait
 * until LNet actually confirms network buffer unlinking.
 * Return 1 if we should give up further retrying attempts or 0 otherwise.
 */
int ptlrpc_expire_one_request(struct ptlrpc_request *req, int async_unlink)
{
        struct obd_import *imp = req->rq_import;
        int rc = 0;
        ENTRY;

        cfs_spin_lock(&req->rq_lock);
        req->rq_timedout = 1;
        cfs_spin_unlock(&req->rq_lock);

        DEBUG_REQ(req->rq_fake ? D_INFO : D_WARNING, req, "Request x"LPU64
                  " sent from %s to NID %s has %s: [sent "CFS_DURATION_T"] "
                  "[real_sent "CFS_DURATION_T"] [current "CFS_DURATION_T"] "
                  "[deadline "CFS_DURATION_T"s] [delay "CFS_DURATION_T"s]",
                  req->rq_xid, imp ? imp->imp_obd->obd_name : "<?>",
                  imp ? libcfs_nid2str(imp->imp_connection->c_peer.nid) : "<?>", 
                  req->rq_net_err ? "failed due to network error" :
                     ((req->rq_real_sent == 0 ||
                       cfs_time_before(req->rq_real_sent, req->rq_sent) ||
                       cfs_time_aftereq(req->rq_real_sent, req->rq_deadline)) ?
                      "timed out for sent delay" : "timed out for slow reply"),
                  req->rq_sent, req->rq_real_sent, cfs_time_current_sec(),
                  cfs_time_sub(req->rq_deadline, req->rq_sent),
                  cfs_time_sub(cfs_time_current_sec(), req->rq_deadline));

        if (imp != NULL && obd_debug_peer_on_timeout)
                LNetCtl(IOC_LIBCFS_DEBUG_PEER, &imp->imp_connection->c_peer);

        ptlrpc_unregister_reply(req, async_unlink);
        ptlrpc_unregister_bulk(req, async_unlink);

        if (obd_dump_on_timeout)
                libcfs_debug_dumplog();

        if (imp == NULL) {
                DEBUG_REQ(D_HA, req, "NULL import: already cleaned up?");
                RETURN(1);
        }

        if (req->rq_fake)
               RETURN(1);

        cfs_atomic_inc(&imp->imp_timeouts);

        /* The DLM server doesn't want recovery run on its imports. */
        if (imp->imp_dlm_fake)
                RETURN(1);

        /* If this request is for recovery or other primordial tasks,
         * then error it out here. */
        if (req->rq_ctx_init || req->rq_ctx_fini ||
            req->rq_send_state != LUSTRE_IMP_FULL ||
            imp->imp_obd->obd_no_recov) {
                DEBUG_REQ(D_RPCTRACE, req, "err -110, sent_state=%s (now=%s)",
                          ptlrpc_import_state_name(req->rq_send_state),
                          ptlrpc_import_state_name(imp->imp_state));
                cfs_spin_lock(&req->rq_lock);
                req->rq_status = -ETIMEDOUT;
                req->rq_err = 1;
                cfs_spin_unlock(&req->rq_lock);
                RETURN(1);
        }

        /* if a request can't be resent we can't wait for an answer after
           the timeout */
        if (ptlrpc_no_resend(req)) {
                DEBUG_REQ(D_RPCTRACE, req, "TIMEOUT-NORESEND:");
                rc = 1;
        }

        ptlrpc_fail_import(imp, lustre_msg_get_conn_cnt(req->rq_reqmsg));

        RETURN(rc);
}

/**
 * Time out all uncompleted requests in request set pointed by \a data
 * Callback used when waiting on sets with l_wait_event.
 * Always returns 1.
 */
int ptlrpc_expired_set(void *data)
{
        struct ptlrpc_request_set *set = data;
        cfs_list_t                *tmp;
        time_t                     now = cfs_time_current_sec();
        ENTRY;

        LASSERT(set != NULL);

        /*
         * A timeout expired. See which reqs it applies to...
         */
        cfs_list_for_each (tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_set_chain);

                /* don't expire request waiting for context */
                if (req->rq_wait_ctx)
                        continue;

                /* Request in-flight? */
                if (!((req->rq_phase == RQ_PHASE_RPC &&
                       !req->rq_waiting && !req->rq_resend) ||
                      (req->rq_phase == RQ_PHASE_BULK)))
                        continue;

                if (req->rq_timedout ||     /* already dealt with */
                    req->rq_deadline > now) /* not expired */
                        continue;

                /* Deal with this guy. Do it asynchronously to not block
                 * ptlrpcd thread. */
                ptlrpc_expire_one_request(req, 1);
        }

        /*
         * When waiting for a whole set, we always break out of the
         * sleep so we can recalculate the timeout, or enable interrupts
         * if everyone's timed out.
         */
        RETURN(1);
}

/**
 * Sets rq_intr flag in \a req under spinlock.
 */
void ptlrpc_mark_interrupted(struct ptlrpc_request *req)
{
        cfs_spin_lock(&req->rq_lock);
        req->rq_intr = 1;
        cfs_spin_unlock(&req->rq_lock);
}

/**
 * Interrupts (sets interrupted flag) all uncompleted requests in
 * a set \a data. Callback for l_wait_event for interruptible waits.
 */
void ptlrpc_interrupted_set(void *data)
{
        struct ptlrpc_request_set *set = data;
        cfs_list_t *tmp;

        LASSERT(set != NULL);
        CERROR("INTERRUPTED SET %p\n", set);

        cfs_list_for_each(tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_set_chain);

                if (req->rq_phase != RQ_PHASE_RPC &&
                    req->rq_phase != RQ_PHASE_UNREGISTERING)
                        continue;

                ptlrpc_mark_interrupted(req);
        }
}

/**
 * Get the smallest timeout in the set; this does NOT set a timeout.
 */
int ptlrpc_set_next_timeout(struct ptlrpc_request_set *set)
{
        cfs_list_t            *tmp;
        time_t                 now = cfs_time_current_sec();
        int                    timeout = 0;
        struct ptlrpc_request *req;
        int                    deadline;
        ENTRY;

        SIGNAL_MASK_ASSERT(); /* XXX BUG 1511 */

        cfs_list_for_each(tmp, &set->set_requests) {
                req = cfs_list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                /*
                 * Request in-flight?
                 */
                if (!(((req->rq_phase == RQ_PHASE_RPC) && !req->rq_waiting) ||
                      (req->rq_phase == RQ_PHASE_BULK) ||
                      (req->rq_phase == RQ_PHASE_NEW)))
                        continue;

                /*
                 * Already timed out.
                 */
                if (req->rq_timedout)
                        continue;

                /*
                 * Waiting for ctx.
                 */
                if (req->rq_wait_ctx)
                        continue;

                if (req->rq_phase == RQ_PHASE_NEW)
                        deadline = req->rq_sent;
                else
                        deadline = req->rq_sent + req->rq_timeout;

                if (deadline <= now)    /* actually expired already */
                        timeout = 1;    /* ASAP */
                else if (timeout == 0 || timeout > deadline - now)
                        timeout = deadline - now;
        }
        RETURN(timeout);
}

/**
 * Send all unset request from the set and then wait untill all
 * requests in the set complete (either get a reply, timeout, get an
 * error or otherwise be interrupted).
 * Returns 0 on success or error code otherwise.
 */
int ptlrpc_set_wait(struct ptlrpc_request_set *set)
{
        cfs_list_t            *tmp;
        struct ptlrpc_request *req;
        struct l_wait_info     lwi;
        int                    rc, timeout;
        ENTRY;

        if (cfs_list_empty(&set->set_requests))
                RETURN(0);

        cfs_list_for_each(tmp, &set->set_requests) {
                req = cfs_list_entry(tmp, struct ptlrpc_request, rq_set_chain);
                if (req->rq_phase == RQ_PHASE_NEW)
                        (void)ptlrpc_send_new_req(req);
        }

        do {
                timeout = ptlrpc_set_next_timeout(set);

                /* wait until all complete, interrupted, or an in-flight
                 * req times out */
                CDEBUG(D_RPCTRACE, "set %p going to sleep for %d seconds\n",
                       set, timeout);

                if (timeout == 0 && !cfs_signal_pending())
                        /*
                         * No requests are in-flight (ether timed out
                         * or delayed), so we can allow interrupts.
                         * We still want to block for a limited time,
                         * so we allow interrupts during the timeout.
                         */
                        lwi = LWI_TIMEOUT_INTR_ALL(cfs_time_seconds(1), 
                                                   ptlrpc_expired_set,
                                                   ptlrpc_interrupted_set, set);
                else
                        /*
                         * At least one request is in flight, so no
                         * interrupts are allowed. Wait until all
                         * complete, or an in-flight req times out. 
                         */
                        lwi = LWI_TIMEOUT(cfs_time_seconds(timeout? timeout : 1),
                                          ptlrpc_expired_set, set);

                rc = l_wait_event(set->set_waitq, ptlrpc_check_set(NULL, set), &lwi);

                LASSERT(rc == 0 || rc == -EINTR || rc == -ETIMEDOUT);

                /* -EINTR => all requests have been flagged rq_intr so next
                 * check completes.
                 * -ETIMEDOUT => someone timed out.  When all reqs have
                 * timed out, signals are enabled allowing completion with
                 * EINTR.
                 * I don't really care if we go once more round the loop in
                 * the error cases -eeb. */
                if (rc == 0 && cfs_atomic_read(&set->set_remaining) == 0) {
                        cfs_list_for_each(tmp, &set->set_requests) {
                                req = cfs_list_entry(tmp, struct ptlrpc_request,
                                                     rq_set_chain);
                                cfs_spin_lock(&req->rq_lock);
                                req->rq_invalid_rqset = 1;
                                cfs_spin_unlock(&req->rq_lock);
                        }
                }
        } while (rc != 0 || cfs_atomic_read(&set->set_remaining) != 0);

        LASSERT(cfs_atomic_read(&set->set_remaining) == 0);

        rc = 0;
        cfs_list_for_each(tmp, &set->set_requests) {
                req = cfs_list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                LASSERT(req->rq_phase == RQ_PHASE_COMPLETE);
                if (req->rq_status != 0)
                        rc = req->rq_status;
        }

        if (set->set_interpret != NULL) {
                int (*interpreter)(struct ptlrpc_request_set *set,void *,int) =
                        set->set_interpret;
                rc = interpreter (set, set->set_arg, rc);
        } else {
                struct ptlrpc_set_cbdata *cbdata, *n;
                int err;

                cfs_list_for_each_entry_safe(cbdata, n,
                                         &set->set_cblist, psc_item) {
                        cfs_list_del_init(&cbdata->psc_item);
                        err = cbdata->psc_interpret(set, cbdata->psc_data, rc);
                        if (err && !rc)
                                rc = err;
                        OBD_FREE_PTR(cbdata);
                }
        }

        RETURN(rc);
}

/**
 * Helper fuction for request freeing.
 * Called when request count reached zero and request needs to be freed.
 * Removes request from all sorts of sending/replay lists it might be on,
 * frees network buffers if any are present.
 * If \a locked is set, that means caller is already holding import imp_lock
 * and so we no longer need to reobtain it (for certain lists manipulations)
 */
static void __ptlrpc_free_req(struct ptlrpc_request *request, int locked)
{
        ENTRY;
        if (request == NULL) {
                EXIT;
                return;
        }

        LASSERTF(!request->rq_receiving_reply, "req %p\n", request);
        LASSERTF(request->rq_rqbd == NULL, "req %p\n",request);/* client-side */
        LASSERTF(cfs_list_empty(&request->rq_list), "req %p\n", request);
        LASSERTF(cfs_list_empty(&request->rq_set_chain), "req %p\n", request);
        LASSERTF(cfs_list_empty(&request->rq_exp_list), "req %p\n", request);
        LASSERTF(!request->rq_replay, "req %p\n", request);
        LASSERT(request->rq_cli_ctx || request->rq_fake);

        req_capsule_fini(&request->rq_pill);

        /* We must take it off the imp_replay_list first.  Otherwise, we'll set
         * request->rq_reqmsg to NULL while osc_close is dereferencing it. */
        if (request->rq_import != NULL) {
                if (!locked)
                        cfs_spin_lock(&request->rq_import->imp_lock);
                cfs_list_del_init(&request->rq_replay_list);
                if (!locked)
                        cfs_spin_unlock(&request->rq_import->imp_lock);
        }
        LASSERTF(cfs_list_empty(&request->rq_replay_list), "req %p\n", request);

        if (cfs_atomic_read(&request->rq_refcount) != 0) {
                DEBUG_REQ(D_ERROR, request,
                          "freeing request with nonzero refcount");
                LBUG();
        }

        if (request->rq_repbuf != NULL)
                sptlrpc_cli_free_repbuf(request);
        if (request->rq_export != NULL) {
                class_export_put(request->rq_export);
                request->rq_export = NULL;
        }
        if (request->rq_import != NULL) {
                class_import_put(request->rq_import);
                request->rq_import = NULL;
        }
        if (request->rq_bulk != NULL)
                ptlrpc_free_bulk(request->rq_bulk);

        if (request->rq_reqbuf != NULL || request->rq_clrbuf != NULL)
                sptlrpc_cli_free_reqbuf(request);

        if (request->rq_cli_ctx)
                sptlrpc_req_put_ctx(request, !locked);

        if (request->rq_pool)
                __ptlrpc_free_req_to_pool(request);
        else
                OBD_FREE(request, sizeof(*request));
        EXIT;
}

static int __ptlrpc_req_finished(struct ptlrpc_request *request, int locked);
/**
 * Drop one request reference. Must be called with import imp_lock held.
 * When reference count drops to zero, reuqest is freed.
 */
void ptlrpc_req_finished_with_imp_lock(struct ptlrpc_request *request)
{
        LASSERT_SPIN_LOCKED(&request->rq_import->imp_lock);
        (void)__ptlrpc_req_finished(request, 1);
}

/**
 * Helper function
 * Drops one reference count for request \a request.
 * \a locked set indicates that caller holds import imp_lock.
 * Frees the request whe reference count reaches zero.
 */
static int __ptlrpc_req_finished(struct ptlrpc_request *request, int locked)
{
        ENTRY;
        if (request == NULL)
                RETURN(1);

        if (request == LP_POISON ||
            request->rq_reqmsg == LP_POISON) {
                CERROR("dereferencing freed request (bug 575)\n");
                LBUG();
                RETURN(1);
        }

        DEBUG_REQ(D_INFO, request, "refcount now %u",
                  cfs_atomic_read(&request->rq_refcount) - 1);

        if (cfs_atomic_dec_and_test(&request->rq_refcount)) {
                __ptlrpc_free_req(request, locked);
                RETURN(1);
        }

        RETURN(0);
}

/**
 * Drops one reference count for a request.
 */
void ptlrpc_req_finished(struct ptlrpc_request *request)
{
        __ptlrpc_req_finished(request, 0);
}

/**
 * Returns xid of a \a request
 */
__u64 ptlrpc_req_xid(struct ptlrpc_request *request)
{
        return request->rq_xid;
}
EXPORT_SYMBOL(ptlrpc_req_xid);

/**
 * Disengage the client's reply buffer from the network
 * NB does _NOT_ unregister any client-side bulk.
 * IDEMPOTENT, but _not_ safe against concurrent callers.
 * The request owner (i.e. the thread doing the I/O) must call...
 * Returns 0 on success or 1 if unregistering cannot be made.
 */
int ptlrpc_unregister_reply(struct ptlrpc_request *request, int async)
{
        int                rc;
        cfs_waitq_t       *wq;
        struct l_wait_info lwi;

        /*
         * Might sleep.
         */
        LASSERT(!cfs_in_interrupt());

        /*
         * Let's setup deadline for reply unlink.
         */
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK) &&
            async && request->rq_reply_deadline == 0)
                request->rq_reply_deadline = cfs_time_current_sec()+LONG_UNLINK;

        /*
         * Nothing left to do.
         */
        if (!ptlrpc_client_recv_or_unlink(request))
                RETURN(1);

        LNetMDUnlink(request->rq_reply_md_h);

        /*
         * Let's check it once again.
         */
        if (!ptlrpc_client_recv_or_unlink(request))
                RETURN(1);

        /*
         * Move to "Unregistering" phase as reply was not unlinked yet.
         */
        ptlrpc_rqphase_move(request, RQ_PHASE_UNREGISTERING);

        /*
         * Do not wait for unlink to finish.
         */
        if (async)
                RETURN(0);

        /*
         * We have to l_wait_event() whatever the result, to give liblustre
         * a chance to run reply_in_callback(), and to make sure we've
         * unlinked before returning a req to the pool.
         */
        if (request->rq_set != NULL)
                wq = &request->rq_set->set_waitq;
        else
                wq = &request->rq_reply_waitq;

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT_INTERVAL(cfs_time_seconds(LONG_UNLINK),
                                           cfs_time_seconds(1), NULL, NULL);
                rc = l_wait_event(*wq, !ptlrpc_client_recv_or_unlink(request),
                                  &lwi);
                if (rc == 0) {
                        ptlrpc_rqphase_move(request, request->rq_next_phase);
                        RETURN(1);
                }

                LASSERT(rc == -ETIMEDOUT);
                DEBUG_REQ(D_WARNING, request, "Unexpectedly long timeout "
                          "rvcng=%d unlnk=%d", request->rq_receiving_reply,
                          request->rq_must_unlink);
        }
        RETURN(0);
}

/**
 * Iterates through replay_list on import and prunes
 * all requests have transno smaller than last_committed for the
 * import and don't have rq_replay set.
 * Since requests are sorted in transno order, stops when meetign first
 * transno bigger than last_committed.
 * caller must hold imp->imp_lock
 */
void ptlrpc_free_committed(struct obd_import *imp)
{
        cfs_list_t *tmp, *saved;
        struct ptlrpc_request *req;
        struct ptlrpc_request *last_req = NULL; /* temporary fire escape */
        ENTRY;

        LASSERT(imp != NULL);

        LASSERT_SPIN_LOCKED(&imp->imp_lock);


        if (imp->imp_peer_committed_transno == imp->imp_last_transno_checked &&
            imp->imp_generation == imp->imp_last_generation_checked) {
                CDEBUG(D_INFO, "%s: skip recheck: last_committed "LPU64"\n",
                       imp->imp_obd->obd_name, imp->imp_peer_committed_transno);
                EXIT;
                return;
        }
        CDEBUG(D_RPCTRACE, "%s: committing for last_committed "LPU64" gen %d\n",
               imp->imp_obd->obd_name, imp->imp_peer_committed_transno,
               imp->imp_generation);
        imp->imp_last_transno_checked = imp->imp_peer_committed_transno;
        imp->imp_last_generation_checked = imp->imp_generation;

        cfs_list_for_each_safe(tmp, saved, &imp->imp_replay_list) {
                req = cfs_list_entry(tmp, struct ptlrpc_request,
                                     rq_replay_list);

                /* XXX ok to remove when 1357 resolved - rread 05/29/03  */
                LASSERT(req != last_req);
                last_req = req;

                if (req->rq_transno == 0) {
                        DEBUG_REQ(D_EMERG, req, "zero transno during replay");
                        LBUG();
                }
                if (req->rq_import_generation < imp->imp_generation) {
                        DEBUG_REQ(D_RPCTRACE, req, "free request with old gen");
                        GOTO(free_req, 0);
                }

                if (req->rq_replay) {
                        DEBUG_REQ(D_RPCTRACE, req, "keeping (FL_REPLAY)");
                        continue;
                }

                /* not yet committed */
                if (req->rq_transno > imp->imp_peer_committed_transno) {
                        DEBUG_REQ(D_RPCTRACE, req, "stopping search");
                        break;
                }

                DEBUG_REQ(D_INFO, req, "commit (last_committed "LPU64")",
                          imp->imp_peer_committed_transno);
free_req:
                cfs_spin_lock(&req->rq_lock);
                req->rq_replay = 0;
                cfs_spin_unlock(&req->rq_lock);
                if (req->rq_commit_cb != NULL)
                        req->rq_commit_cb(req);
                cfs_list_del_init(&req->rq_replay_list);
                __ptlrpc_req_finished(req, 1);
        }

        EXIT;
        return;
}

void ptlrpc_cleanup_client(struct obd_import *imp)
{
        ENTRY;
        EXIT;
        return;
}

/**
 * Schedule previously sent request for resend.
 * For bulk requests we assign new xid (to avoid problems with
 * lost replies and therefore several transfers landing into same buffer
 * from different sending attempts).
 */
void ptlrpc_resend_req(struct ptlrpc_request *req)
{
        DEBUG_REQ(D_HA, req, "going to resend");
        lustre_msg_set_handle(req->rq_reqmsg, &(struct lustre_handle){ 0 });
        req->rq_status = -EAGAIN;

        cfs_spin_lock(&req->rq_lock);
        req->rq_resend = 1;
        req->rq_net_err = 0;
        req->rq_timedout = 0;
        if (req->rq_bulk) {
                __u64 old_xid = req->rq_xid;

                /* ensure previous bulk fails */
                req->rq_xid = ptlrpc_next_xid();
                CDEBUG(D_HA, "resend bulk old x"LPU64" new x"LPU64"\n",
                       old_xid, req->rq_xid);
        }
        ptlrpc_client_wake_req(req);
        cfs_spin_unlock(&req->rq_lock);
}

/* XXX: this function and rq_status are currently unused */
void ptlrpc_restart_req(struct ptlrpc_request *req)
{
        DEBUG_REQ(D_HA, req, "restarting (possibly-)completed request");
        req->rq_status = -ERESTARTSYS;

        cfs_spin_lock(&req->rq_lock);
        req->rq_restart = 1;
        req->rq_timedout = 0;
        ptlrpc_client_wake_req(req);
        cfs_spin_unlock(&req->rq_lock);
}

/**
 * Grab additional reference on a request \a req
 */
struct ptlrpc_request *ptlrpc_request_addref(struct ptlrpc_request *req)
{
        ENTRY;
        cfs_atomic_inc(&req->rq_refcount);
        RETURN(req);
}

/**
 * Add a request to import replay_list.
 * Must be called under imp_lock
 */
void ptlrpc_retain_replayable_request(struct ptlrpc_request *req,
                                      struct obd_import *imp)
{
        cfs_list_t *tmp;

        LASSERT_SPIN_LOCKED(&imp->imp_lock);

        if (req->rq_transno == 0) {
                DEBUG_REQ(D_EMERG, req, "saving request with zero transno");
                LBUG();
        }

        /* clear this for new requests that were resent as well
           as resent replayed requests. */
        lustre_msg_clear_flags(req->rq_reqmsg, MSG_RESENT);

        /* don't re-add requests that have been replayed */
        if (!cfs_list_empty(&req->rq_replay_list))
                return;

        lustre_msg_add_flags(req->rq_reqmsg, MSG_REPLAY);

        LASSERT(imp->imp_replayable);
        /* Balanced in ptlrpc_free_committed, usually. */
        ptlrpc_request_addref(req);
        cfs_list_for_each_prev(tmp, &imp->imp_replay_list) {
                struct ptlrpc_request *iter =
                        cfs_list_entry(tmp, struct ptlrpc_request,
                                       rq_replay_list);

                /* We may have duplicate transnos if we create and then
                 * open a file, or for closes retained if to match creating
                 * opens, so use req->rq_xid as a secondary key.
                 * (See bugs 684, 685, and 428.)
                 * XXX no longer needed, but all opens need transnos!
                 */
                if (iter->rq_transno > req->rq_transno)
                        continue;

                if (iter->rq_transno == req->rq_transno) {
                        LASSERT(iter->rq_xid != req->rq_xid);
                        if (iter->rq_xid > req->rq_xid)
                                continue;
                }

                cfs_list_add(&req->rq_replay_list, &iter->rq_replay_list);
                return;
        }

        cfs_list_add(&req->rq_replay_list, &imp->imp_replay_list);
}

/**
 * Send request and wait until it completes.
 * Returns request processing status.
 */
int ptlrpc_queue_wait(struct ptlrpc_request *req)
{
        struct ptlrpc_request_set *set;
        int rc;
        ENTRY;

        LASSERT(req->rq_set == NULL);
        LASSERT(!req->rq_receiving_reply);

        set = ptlrpc_prep_set();
        if (set == NULL) {
                CERROR("Unable to allocate ptlrpc set.");
                RETURN(-ENOMEM);
        }

        /* for distributed debugging */
        lustre_msg_set_status(req->rq_reqmsg, cfs_curproc_pid());

        /* add a ref for the set (see comment in ptlrpc_set_add_req) */
        ptlrpc_request_addref(req);
        ptlrpc_set_add_req(set, req);
        rc = ptlrpc_set_wait(set);
        ptlrpc_set_destroy(set);

        RETURN(rc);
}

struct ptlrpc_replay_async_args {
        int praa_old_state;
        int praa_old_status;
};

/**
 * Callback used for replayed requests reply processing.
 * In case of succesful reply calls registeresd request replay callback.
 * In case of error restart replay process.
 */
static int ptlrpc_replay_interpret(const struct lu_env *env,
                                   struct ptlrpc_request *req,
                                   void * data, int rc)
{
        struct ptlrpc_replay_async_args *aa = data;
        struct obd_import *imp = req->rq_import;

        ENTRY;
        cfs_atomic_dec(&imp->imp_replay_inflight);

        if (!ptlrpc_client_replied(req)) {
                CERROR("request replay timed out, restarting recovery\n");
                GOTO(out, rc = -ETIMEDOUT);
        }

        if (lustre_msg_get_type(req->rq_repmsg) == PTL_RPC_MSG_ERR &&
            (lustre_msg_get_status(req->rq_repmsg) == -ENOTCONN ||
             lustre_msg_get_status(req->rq_repmsg) == -ENODEV))
                GOTO(out, rc = lustre_msg_get_status(req->rq_repmsg));

        /** VBR: check version failure */
        if (lustre_msg_get_status(req->rq_repmsg) == -EOVERFLOW) {
                /** replay was failed due to version mismatch */
                DEBUG_REQ(D_WARNING, req, "Version mismatch during replay\n");
                cfs_spin_lock(&imp->imp_lock);
                imp->imp_vbr_failed = 1;
                imp->imp_no_lock_replay = 1;
                cfs_spin_unlock(&imp->imp_lock);
                lustre_msg_set_status(req->rq_repmsg, aa->praa_old_status);
        } else {
                /** The transno had better not change over replay. */
                LASSERTF(lustre_msg_get_transno(req->rq_reqmsg) ==
                         lustre_msg_get_transno(req->rq_repmsg) ||
                         lustre_msg_get_transno(req->rq_repmsg) == 0,
                         LPX64"/"LPX64"\n",
                         lustre_msg_get_transno(req->rq_reqmsg),
                         lustre_msg_get_transno(req->rq_repmsg));
        }

        cfs_spin_lock(&imp->imp_lock);
        /** if replays by version then gap was occur on server, no trust to locks */
        if (lustre_msg_get_flags(req->rq_repmsg) & MSG_VERSION_REPLAY)
                imp->imp_no_lock_replay = 1;
        imp->imp_last_replay_transno = lustre_msg_get_transno(req->rq_reqmsg);
        cfs_spin_unlock(&imp->imp_lock);
        LASSERT(imp->imp_last_replay_transno);

        /* transaction number shouldn't be bigger than the latest replayed */
        if (req->rq_transno > lustre_msg_get_transno(req->rq_reqmsg)) {
                DEBUG_REQ(D_ERROR, req,
                          "Reported transno "LPU64" is bigger than the "
                          "replayed one: "LPU64, req->rq_transno,
                          lustre_msg_get_transno(req->rq_reqmsg));
                GOTO(out, rc = -EINVAL);
        }

        DEBUG_REQ(D_HA, req, "got rep");

        /* let the callback do fixups, possibly including in the request */
        if (req->rq_replay_cb)
                req->rq_replay_cb(req);

        if (ptlrpc_client_replied(req) &&
            lustre_msg_get_status(req->rq_repmsg) != aa->praa_old_status) {
                DEBUG_REQ(D_ERROR, req, "status %d, old was %d",
                          lustre_msg_get_status(req->rq_repmsg),
                          aa->praa_old_status);
        } else {
                /* Put it back for re-replay. */
                lustre_msg_set_status(req->rq_repmsg, aa->praa_old_status);
        }

        /*
         * Errors while replay can set transno to 0, but
         * imp_last_replay_transno shouldn't be set to 0 anyway
         */
        if (req->rq_transno == 0)
                CERROR("Transno is 0 during replay!\n");

        /* continue with recovery */
        rc = ptlrpc_import_recovery_state_machine(imp);
 out:
        req->rq_send_state = aa->praa_old_state;

        if (rc != 0)
                /* this replay failed, so restart recovery */
                ptlrpc_connect_import(imp, NULL);

        RETURN(rc);
}

/**
 * Prepares and queues request for replay.
 * Adds it to ptlrpcd queue for actual sending.
 * Returns 0 on success.
 */
int ptlrpc_replay_req(struct ptlrpc_request *req)
{
        struct ptlrpc_replay_async_args *aa;
        ENTRY;

        LASSERT(req->rq_import->imp_state == LUSTRE_IMP_REPLAY);

        LASSERT (sizeof (*aa) <= sizeof (req->rq_async_args));
        aa = ptlrpc_req_async_args(req);
        memset(aa, 0, sizeof *aa);

        /* Prepare request to be resent with ptlrpcd */
        aa->praa_old_state = req->rq_send_state;
        req->rq_send_state = LUSTRE_IMP_REPLAY;
        req->rq_phase = RQ_PHASE_NEW;
        req->rq_next_phase = RQ_PHASE_UNDEFINED;
        if (req->rq_repmsg)
                aa->praa_old_status = lustre_msg_get_status(req->rq_repmsg);
        req->rq_status = 0;
        req->rq_interpret_reply = ptlrpc_replay_interpret;
        /* Readjust the timeout for current conditions */
        ptlrpc_at_set_req_timeout(req);

        DEBUG_REQ(D_HA, req, "REPLAY");

        cfs_atomic_inc(&req->rq_import->imp_replay_inflight);
        ptlrpc_request_addref(req); /* ptlrpcd needs a ref */

        ptlrpcd_add_req(req, PSCOPE_OTHER);
        RETURN(0);
}

/**
 * Aborts all in-flight request on import \a imp sending and delayed lists
 */
void ptlrpc_abort_inflight(struct obd_import *imp)
{
        cfs_list_t *tmp, *n;
        ENTRY;

        /* Make sure that no new requests get processed for this import.
         * ptlrpc_{queue,set}_wait must (and does) hold imp_lock while testing
         * this flag and then putting requests on sending_list or delayed_list.
         */
        cfs_spin_lock(&imp->imp_lock);

        /* XXX locking?  Maybe we should remove each request with the list
         * locked?  Also, how do we know if the requests on the list are
         * being freed at this time?
         */
        cfs_list_for_each_safe(tmp, n, &imp->imp_sending_list) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request, rq_list);

                DEBUG_REQ(D_RPCTRACE, req, "inflight");

                cfs_spin_lock (&req->rq_lock);
                if (req->rq_import_generation < imp->imp_generation) {
                        req->rq_err = 1;
                        req->rq_status = -EINTR;
                        ptlrpc_client_wake_req(req);
                }
                cfs_spin_unlock (&req->rq_lock);
        }

        cfs_list_for_each_safe(tmp, n, &imp->imp_delayed_list) {
                struct ptlrpc_request *req =
                        cfs_list_entry(tmp, struct ptlrpc_request, rq_list);

                DEBUG_REQ(D_RPCTRACE, req, "aborting waiting req");

                cfs_spin_lock (&req->rq_lock);
                if (req->rq_import_generation < imp->imp_generation) {
                        req->rq_err = 1;
                        req->rq_status = -EINTR;
                        ptlrpc_client_wake_req(req);
                }
                cfs_spin_unlock (&req->rq_lock);
        }

        /* Last chance to free reqs left on the replay list, but we
         * will still leak reqs that haven't committed.  */
        if (imp->imp_replayable)
                ptlrpc_free_committed(imp);

        cfs_spin_unlock(&imp->imp_lock);

        EXIT;
}

/**
 * Abort all uncompleted requests in request set \a set
 */
void ptlrpc_abort_set(struct ptlrpc_request_set *set)
{
        cfs_list_t *tmp, *pos;

        LASSERT(set != NULL);

        cfs_list_for_each_safe(pos, tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(pos, struct ptlrpc_request,
                                       rq_set_chain);

                cfs_spin_lock(&req->rq_lock);
                if (req->rq_phase != RQ_PHASE_RPC) {
                        cfs_spin_unlock(&req->rq_lock);
                        continue;
                }

                req->rq_err = 1;
                req->rq_status = -EINTR;
                ptlrpc_client_wake_req(req);
                cfs_spin_unlock(&req->rq_lock);
        }
}

static __u64 ptlrpc_last_xid;
static cfs_spinlock_t ptlrpc_last_xid_lock;

/**
 * Initialize the XID for the node.  This is common among all requests on
 * this node, and only requires the property that it is monotonically
 * increasing.  It does not need to be sequential.  Since this is also used
 * as the RDMA match bits, it is important that a single client NOT have
 * the same match bits for two different in-flight requests, hence we do
 * NOT want to have an XID per target or similar.
 *
 * To avoid an unlikely collision between match bits after a client reboot
 * (which would deliver old data into the wrong RDMA buffer) initialize
 * the XID based on the current time, assuming a maximum RPC rate of 1M RPC/s.
 * If the time is clearly incorrect, we instead use a 62-bit random number.
 * In the worst case the random number will overflow 1M RPCs per second in
 * 9133 years, or permutations thereof.
 */
#define YEAR_2004 (1ULL << 30)
void ptlrpc_init_xid(void)
{
        time_t now = cfs_time_current_sec();

        cfs_spin_lock_init(&ptlrpc_last_xid_lock);
        if (now < YEAR_2004) {
                cfs_get_random_bytes(&ptlrpc_last_xid, sizeof(ptlrpc_last_xid));
                ptlrpc_last_xid >>= 2;
                ptlrpc_last_xid |= (1ULL << 61);
        } else {
                ptlrpc_last_xid = (__u64)now << 20;
        }
}

/**
 * Increase xid and returns resultng new value to the caller.
 */
__u64 ptlrpc_next_xid(void)
{
        __u64 tmp;
        cfs_spin_lock(&ptlrpc_last_xid_lock);
        tmp = ++ptlrpc_last_xid;
        cfs_spin_unlock(&ptlrpc_last_xid_lock);
        return tmp;
}

/**
 * Get a glimpse at what next xid value might have been.
 * Returns possible next xid.
 */
__u64 ptlrpc_sample_next_xid(void)
{
#if BITS_PER_LONG == 32
        /* need to avoid possible word tearing on 32-bit systems */
        __u64 tmp;
        cfs_spin_lock(&ptlrpc_last_xid_lock);
        tmp = ptlrpc_last_xid + 1;
        cfs_spin_unlock(&ptlrpc_last_xid_lock);
        return tmp;
#else
        /* No need to lock, since returned value is racy anyways */
        return ptlrpc_last_xid + 1;
#endif
}
EXPORT_SYMBOL(ptlrpc_sample_next_xid);

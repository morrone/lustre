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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ofd/ofd.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <obd_class.h>
#include <lustre_param.h>
#include <lustre_log.h>

#include "ofd_internal.h"

struct lu_object_operations filter_obj_ops;
struct lu_context_key filter_thread_key;

struct filter_intent_args {
        struct ldlm_lock **victim;
        __u64 size;
        int *liblustre;
};

static enum interval_iter filter_intent_cb(struct interval_node *n,
                                           void *args)
{
        struct ldlm_interval *node = (struct ldlm_interval *)n;
        struct filter_intent_args *arg = (struct filter_intent_args*)args;
        __u64 size = arg->size;
        struct ldlm_lock **v = arg->victim;
        struct ldlm_lock *lck;

        /* If the interval is lower than the current file size,
         * just break. */
        if (interval_high(n) <= size)
                return INTERVAL_ITER_STOP;

        cfs_list_for_each_entry(lck, &node->li_group, l_sl_policy) {
                /* Don't send glimpse ASTs to liblustre clients.
                 * They aren't listening for them, and they do
                 * entirely synchronous I/O anyways. */
                if (lck->l_export == NULL ||
                    lck->l_export->exp_libclient == 1)
                        continue;

                if (*arg->liblustre)
                        *arg->liblustre = 0;

                if (*v == NULL) {
                        *v = LDLM_LOCK_GET(lck);
                } else if ((*v)->l_policy_data.l_extent.start <
                           lck->l_policy_data.l_extent.start) {
                        LDLM_LOCK_RELEASE(*v);
                        *v = LDLM_LOCK_GET(lck);
                }

                /* the same policy group - every lock has the
                 * same extent, so needn't do it any more */
                break;
        }

        return INTERVAL_ITER_CONT;
}

static int filter_intent_policy(struct ldlm_namespace *ns,
                                struct ldlm_lock **lockp, void *req_cookie,
                                ldlm_mode_t mode, int flags, void *data)
{
        CFS_LIST_HEAD(rpc_list);
        struct ptlrpc_request *req = req_cookie;
        struct ldlm_lock *lock = *lockp, *l = NULL;
        struct ldlm_resource *res = lock->l_resource;
        ldlm_processing_policy policy;
        struct ost_lvb *res_lvb, *reply_lvb;
        struct ldlm_reply *rep;
        ldlm_error_t err;
        int idx, rc, tmpflags = 0, only_liblustre = 1;
        struct ldlm_interval_tree *tree;
        struct filter_intent_args arg;
        __u32 repsize[3] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                           [DLM_LOCKREPLY_OFF]   = sizeof(*rep),
                           [DLM_REPLY_REC_OFF]   = sizeof(*reply_lvb) };
        ENTRY;

        policy = ldlm_get_processing_policy(res);
        LASSERT(policy != NULL);
        LASSERT(req != NULL);

        rc = lustre_pack_reply(req, 3, repsize, NULL);
        if (rc)
                RETURN(req->rq_status = rc);

        rep = lustre_msg_buf(req->rq_repmsg, DLM_LOCKREPLY_OFF, sizeof(*rep));
        LASSERT(rep != NULL);

        reply_lvb = lustre_msg_buf(req->rq_repmsg, DLM_REPLY_REC_OFF,
                                   sizeof(*reply_lvb));
        LASSERT(reply_lvb != NULL);

        //fixup_handle_for_resent_req(req, lock, &lockh);

        /* Call the extent policy function to see if our request can be
         * granted, or is blocked.
         * If the OST lock has LDLM_FL_HAS_INTENT set, it means a glimpse
         * lock, and should not be granted if the lock will be blocked.
         */

        LASSERT(ns == ldlm_res_to_ns(res));
        lock_res(res);
        rc = policy(lock, &tmpflags, 0, &err, &rpc_list);
        check_res_locked(res);

        /* FIXME: we should change the policy function slightly, to not make
         * this list at all, since we just turn around and free it */
        while (!cfs_list_empty(&rpc_list)) {
                struct ldlm_lock *wlock =
                        cfs_list_entry(rpc_list.next, struct ldlm_lock, l_cp_ast);
                LASSERT((lock->l_flags & LDLM_FL_AST_SENT) == 0);
                LASSERT(lock->l_flags & LDLM_FL_CP_REQD);
                lock->l_flags &= ~LDLM_FL_CP_REQD;
                cfs_list_del_init(&wlock->l_cp_ast);
                LDLM_LOCK_RELEASE(wlock);
        }

        /* The lock met with no resistance; we're finished. */
        if (rc == LDLM_ITER_CONTINUE) {
                /* do not grant locks to the liblustre clients: they cannot
                 * handle ASTs robustly.  We need to do this while still
                 * holding ns_lock to avoid the lock remaining on the res_link
                 * list (and potentially being added to l_pending_list by an
                 * AST) when we are going to drop this lock ASAP. */
                if (lock->l_export->exp_libclient ||
                    OBD_FAIL_TIMEOUT(OBD_FAIL_LDLM_GLIMPSE, 2)) {
                        ldlm_resource_unlink_lock(lock);
                        err = ELDLM_LOCK_ABORTED;
                } else {
                        err = ELDLM_LOCK_REPLACED;
                }
                unlock_res(res);
                RETURN(err);
        }

        /* Do not grant any lock, but instead send GL callbacks.  The extent
         * policy nicely created a list of all PW locks for us.  We will choose
         * the highest of those which are larger than the size in the LVB, if
         * any, and perform a glimpse callback. */
        res_lvb = res->lr_lvb_data;
        LASSERT(res_lvb != NULL);
        *reply_lvb = *res_lvb;

        /*
         * ->ns_lock guarantees that no new locks are granted, and,
         * therefore, that res->lr_lvb_data cannot increase beyond the
         * end of already granted lock. As a result, it is safe to
         * check against "stale" reply_lvb->lvb_size value without
         * res->lr_lvb_sem.
         */
        arg.size = reply_lvb->lvb_size;
        arg.victim = &l;
        arg.liblustre = &only_liblustre;
        for (idx = 0; idx < LCK_MODE_NUM; idx++) {
                tree = &res->lr_itree[idx];
                if (tree->lit_mode == LCK_PR)
                        continue;

                interval_iterate_reverse(tree->lit_root,
                                         filter_intent_cb, &arg);
        }
        unlock_res(res);

        /* There were no PW locks beyond the size in the LVB; finished. */
        if (l == NULL) {
                if (only_liblustre) {
                        /* If we discovered a liblustre client with a PW lock,
                         * however, the LVB may be out of date!  The LVB is
                         * updated only on glimpse (which we don't do for
                         * liblustre clients) and cancel (which the client
                         * obviously has not yet done).  So if it has written
                         * data but kept the lock, the LVB is stale and needs
                         * to be updated from disk.
                         *
                         * Of course, this will all disappear when we switch to
                         * taking liblustre locks on the OST. */
                        ldlm_res_lvbo_update(res, NULL, 1);
                }
                RETURN(ELDLM_LOCK_ABORTED);
        }

        /*
         * This check is for lock taken in filter_prepare_destroy() that does
         * not have l_glimpse_ast set. So the logic is: if there is a lock
         * with no l_glimpse_ast set, this object is being destroyed already.
         *
         * Hence, if you are grabbing DLM locks on the server, always set
         * non-NULL glimpse_ast (e.g., ldlm_request.c:ldlm_glimpse_ast()).
         */
        if (l->l_glimpse_ast == NULL) {
                /* We are racing with unlink(); just return -ENOENT */
                rep->lock_policy_res1 = -ENOENT;
                goto out;
        }

        LASSERTF(l->l_glimpse_ast != NULL, "l == %p", l);
        rc = l->l_glimpse_ast(l, NULL); /* this will update the LVB */

        lock_res(res);
        *reply_lvb = *res_lvb;
        unlock_res(res);

 out:
        LDLM_LOCK_RELEASE(l);

        RETURN(ELDLM_LOCK_ABORTED);
}

/* used by MGS to process specific configurations */
static int filter_process_config(const struct lu_env *env,
                                 struct lu_device *d, struct lustre_cfg *cfg)
{
        struct filter_device *m = filter_dev(d);
        struct dt_device *dt_next = m->ofd_osd;
        struct lu_device *next = &dt_next->dd_lu_dev;
        int rc = 0;
        ENTRY;

        switch (cfg->lcfg_command) {
        case LCFG_PARAM: {
                struct lprocfs_static_vars lvars;

                lprocfs_filter_init_vars(&lvars);
                rc = class_process_proc_param(PARAM_OST, lvars.obd_vars, cfg,
                                              d->ld_obd);
                if (rc)
                        /* others are passed further */
                        rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
        case LCFG_SPTLRPC_CONF: {
                //struct sptlrpc_conf_log *log;
                //struct sptlrpc_rule_set  tmp_rset;
                LBUG();
#if 0
                log = sptlrpc_conf_log_extract(cfg);
                if (IS_ERR(log)) {
                        rc = PTR_ERR(log);
                        break;
                }

                sptlrpc_rule_set_init(&tmp_rset);

                rc = sptlrpc_rule_set_from_log(&tmp_rset, log);
                if (rc) {
                        CERROR("obd %s: failed get sptlrpc rules: %d\n",
                               d->ld_obd->obd_name, rc);
                        break;
                }

                cfs_write_lock(&m->ofd_sptlrpc_lock);
                sptlrpc_rule_set_free(&m->ofd_sptlrpc_rset);
                m->ofd_sptlrpc_rset = tmp_rset;
                cfs_write_unlock(&m->ofd_sptlrpc_lock);

                sptlrpc_target_update_exp_flavor(d->ld_obd, &tmp_rset);
#endif
                break;
        }
        default:
                /* others are passed further */
                rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
        RETURN(rc);
}

static struct lu_object *filter_object_alloc(const struct lu_env *env,
                                          const struct lu_object_header *hdr,
                                          struct lu_device *d)
{
        struct filter_object *of;

        ENTRY;

        OBD_ALLOC_PTR(of);
        if (of != NULL) {
                struct lu_object *o;
                struct lu_object_header *h;

                o = &of->ofo_obj.do_lu;
                h = &of->ofo_header;
                lu_object_header_init(h);
                lu_object_init(o, h, d);
                lu_object_add_top(h, o);
                o->lo_ops = &filter_obj_ops;
                RETURN(o);
        } else
                RETURN(NULL);
}

static int filter_object_init(const struct lu_env *env, struct lu_object *o,
                              const struct lu_object_conf *conf)
{
        struct filter_device *d = filter_dev(o->lo_dev);
        struct lu_device  *under;
        struct lu_object  *below;
        int                rc = 0;
        ENTRY;

        CDEBUG(D_INFO, "object init, fid = "DFID"\n",
               PFID(lu_object_fid(o)));

        under = &d->ofd_osd->dd_lu_dev;
        below = under->ld_ops->ldo_object_alloc(env, o->lo_header, under);
        if (below != NULL) {
                lu_object_add(o, below);
        } else
                rc = -ENOMEM;

        RETURN(rc);
}

static void filter_object_free(const struct lu_env *env, struct lu_object *o)
{
        struct filter_object *of = filter_obj(o);
        struct lu_object_header *h;
        ENTRY;

        h = o->lo_header;
        CDEBUG(D_INFO, "object free, fid = "DFID"\n",
               PFID(lu_object_fid(o)));

        lu_object_fini(o);
        lu_object_header_fini(h);
        OBD_FREE_PTR(of);
        EXIT;
}

static int filter_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *o)
{
        return (*p)(env, cookie, LUSTRE_MDT_NAME"-object@%p", o);
}

static struct lu_device_operations filter_lu_ops = {
        .ldo_object_alloc   = filter_object_alloc,
        .ldo_process_config = filter_process_config
};

struct lu_object_operations filter_obj_ops = {
        .loo_object_init    = filter_object_init,
        .loo_object_free    = filter_object_free,
        .loo_object_print   = filter_object_print
};

#if 0
static struct lu_device *filter_layer_setup(const struct lu_env *env,
                                         const char *typename,
                                         struct lu_device *child,
                                         struct lustre_cfg *cfg)
{
        const char            *dev = lustre_cfg_string(cfg, 0);
        struct obd_type       *type;
        struct lu_device_type *ldt;
        struct lu_device      *d;
        int rc;
        ENTRY;

        /* find the type */
        type = class_get_type(typename);
        if (!type) {
                CERROR("Unknown type: '%s'\n", typename);
                GOTO(out, rc = -ENODEV);
        }

        rc = lu_env_refill((struct lu_env *) &env->le_ctx);
        if (rc != 0) {
                CERROR("Failure to refill context: '%d'\n", rc);
                GOTO(out_type, rc);
        }

        if (env->le_ses != NULL) {
                rc = lu_context_refill(env->le_ses);
                if (rc != 0) {
                        CERROR("Failure to refill session: '%d'\n", rc);
                        GOTO(out_type, rc);
                }
        }

        ldt = type->typ_lu;
        if (ldt == NULL) {
                CERROR("type: '%s'\n", typename);
                GOTO(out_type, rc = -EINVAL);
        }

        ldt->ldt_obd_type = type;
        d = ldt->ldt_ops->ldto_device_alloc(env, ldt, cfg);
        if (IS_ERR(d)) {
                CERROR("Cannot allocate device: '%s'\n", typename);
                GOTO(out_type, rc = -ENODEV);
        }

        LASSERT(child->ld_site);
        d->ld_site = child->ld_site;

        type->typ_refcnt++;
        rc = ldt->ldt_ops->ldto_device_init(env, d, dev, child);
        if (rc) {
                CERROR("can't init device '%s', rc %d\n", typename, rc);
                GOTO(out_alloc, rc);
        }
        lu_device_get(d);

        RETURN(d);

out_alloc:
        ldt->ldt_ops->ldto_device_free(env, d);
        type->typ_refcnt--;
out_type:
        class_put_type(type);
out:
        return ERR_PTR(rc);
}
#endif

static int filter_connect_to_next(const struct lu_env *env, struct filter_device *m,
                                  const char *nextdev)
{
        struct obd_connect_data *data = NULL;
        struct obd_device       *obd;
        int                      rc;
        ENTRY;

        LASSERT(m->ofd_osd_exp == NULL);

        OBD_ALLOC(data, sizeof(*data));
        if (data == NULL)
                GOTO(out, rc = -ENOMEM);

        obd = class_name2obd(nextdev);
        if (obd == NULL) {
                CERROR("can't locate next device: %s\n", nextdev);
                GOTO(out, rc = -ENOTCONN);
        }

        /* XXX: which flags we need on OST? */
        data->ocd_version = LUSTRE_VERSION_CODE;

        rc = obd_connect(NULL, &m->ofd_osd_exp, obd, &obd->obd_uuid, data, NULL);
        if (rc) {
                CERROR("cannot connect to next dev %s (%d)\n", nextdev, rc);
                GOTO(out, rc);
        }

        m->ofd_dt_dev.dd_lu_dev.ld_site =
                m->ofd_osd_exp->exp_obd->obd_lu_dev->ld_site;
        LASSERT(m->ofd_dt_dev.dd_lu_dev.ld_site);
        m->ofd_osd = lu2dt_dev(m->ofd_osd_exp->exp_obd->obd_lu_dev);
        m->ofd_dt_dev.dd_lu_dev.ld_site->ls_top_dev = &m->ofd_dt_dev.dd_lu_dev;

out:
        if (data)
                OBD_FREE(data, sizeof(*data));
        RETURN(rc);
}

int filter_stack_init(const struct lu_env *env,
                          struct filter_device *m, struct lustre_cfg *cfg)
{
        struct lu_device  *d = &m->ofd_dt_dev.dd_lu_dev;
        struct lu_device  *tmp;
        char               osdname[64];
        int rc;
        ENTRY;

        LASSERT(m->ofd_osd_exp == NULL);
        snprintf(osdname, sizeof(osdname), "%s-dsk", lustre_cfg_string(cfg, 0));

        rc = filter_connect_to_next(env, m, osdname);
        LASSERT(rc == 0);

        tmp = &m->ofd_osd->dd_lu_dev;
        rc = tmp->ld_ops->ldo_prepare(env, d, tmp);
        GOTO(out, rc);

out:
        /* XXX: error handling */
        LASSERT(rc == 0);

        return rc;
}

static void filter_stack_fini(const struct lu_env *env,
                           struct filter_device *m, struct lu_device *top)
{
        struct obd_device       *obd = filter_obd(m);
        struct lustre_cfg_bufs   bufs;
        struct lustre_cfg       *lcfg;
        struct lu_device        *d = &m->ofd_dt_dev.dd_lu_dev;
        char                     flags[3] = "";
        int                      rc;
        ENTRY;

        lu_site_purge(env, d->ld_site, ~0);

        /* process cleanup, pass mdt obd name to get obd umount flags */
        lustre_cfg_bufs_reset(&bufs, obd->obd_name);
        if (obd->obd_force)
                strcat(flags, "F");
        if (obd->obd_fail)
                strcat(flags, "A");
        lustre_cfg_bufs_set_string(&bufs, 1, flags);
        lcfg = lustre_cfg_new(LCFG_CLEANUP, &bufs);
        if (!lcfg) {
                CERROR("Cannot alloc lcfg!\n");
                return;
        }

        LASSERT(top);
        top->ld_ops->ldo_process_config(env, top, lcfg);
        lustre_cfg_free(lcfg);

        lu_site_purge(env, d->ld_site, ~0);

        LASSERT(m->ofd_osd_exp);
        rc = obd_disconnect(m->ofd_osd_exp);
        m->ofd_osd = NULL;

        EXIT;
}

static int filter_procfs_init(struct filter_device *ofd)
{
        struct lprocfs_static_vars lvars;
        struct obd_device *obd = filter_obd(ofd);
        cfs_proc_dir_entry_t *entry;
        int rc = 0;
        ENTRY;

        /* lprocfs must be setup before the ofd so state can be safely added
         * to /proc incrementally as the ofd is setup */
        lprocfs_filter_init_vars(&lvars);
        rc = lprocfs_obd_setup(obd, lvars.obd_vars);
        if (rc) {
                CERROR("%s: lprocfs_obd_setup failed: %d.\n",
                       obd->obd_name, rc);
                RETURN(rc);
        }

        rc = lprocfs_alloc_obd_stats(obd, LPROC_FILTER_LAST);
        if (rc) {
                CERROR("%s: lprocfs_alloc_obd_stats failed: %d.\n",
                       obd->obd_name, rc);
                GOTO(obd_cleanup, rc);
        }

        /* Init obdofd private stats here */
        lprocfs_counter_init(obd->obd_stats, LPROC_FILTER_READ_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "read_bytes", "bytes");
        lprocfs_counter_init(obd->obd_stats, LPROC_FILTER_WRITE_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "write_bytes", "bytes");

        rc = lproc_filter_attach_seqstat(obd);
        if (rc) {
                CERROR("%s: create seqstat failed: %d.\n", obd->obd_name, rc);
                GOTO(free_obd_stats, rc);
        }

        entry = lprocfs_register("exports", obd->obd_proc_entry, NULL, NULL);
        if (IS_ERR(entry)) {
                rc = PTR_ERR(entry);
                CERROR("%s: error %d setting up lprocfs for %s\n",
                       obd->obd_name, rc, "exports");
                GOTO(free_obd_stats, rc);
        }
        obd->obd_proc_exports_entry = entry;

        entry = lprocfs_add_simple(obd->obd_proc_exports_entry, "clear",
                                   lprocfs_nid_stats_clear_read,
                                   lprocfs_nid_stats_clear_write, obd, NULL);
        if (IS_ERR(entry)) {
                rc = PTR_ERR(entry);
                CERROR("%s: add proc entry 'clear' failed: %d.\n",
                       obd->obd_name, rc);
                GOTO(free_obd_stats, rc);
        }
        RETURN(0);

free_obd_stats:
        lprocfs_free_obd_stats(obd);
obd_cleanup:
        lprocfs_obd_cleanup(obd);
        return rc;
}

static int filter_procfs_fini(struct filter_device *ofd)
{
        struct obd_device *obd = filter_obd(ofd);

        lprocfs_remove_proc_entry("clear", obd->obd_proc_exports_entry);
        lprocfs_free_per_client_stats(obd);
        lprocfs_free_obd_stats(obd);
        lprocfs_obd_cleanup(obd);
        return 0;
}

static int filter_init0(const struct lu_env *env, struct filter_device *m,
                        struct lu_device_type *ldt, struct lustre_cfg *cfg)
{
        const char *dev = lustre_cfg_string(cfg, 0);
        struct filter_thread_info *info = NULL;
        struct filter_obd *filter;
        struct obd_device *obd;
        struct dt_device  *next;
        int rc;
        ENTRY;

        obd = class_name2obd(dev);
        LASSERT(obd != NULL);

        m->ofd_fmd_max_num = FILTER_FMD_MAX_NUM_DEFAULT;
        m->ofd_fmd_max_age = FILTER_FMD_MAX_AGE_DEFAULT;

        cfs_spin_lock_init(&m->ofd_flags_lock);
        m->ofd_raid_degraded = 0;
        m->ofd_syncjournal = 0;
        filter_slc_set(m);

        /* grant data */
        cfs_spin_lock_init(&m->ofd_grant_lock);
        cfs_sema_init(&m->ofd_grant_sem, 1);
        m->ofd_tot_dirty = 0;
        m->ofd_tot_granted = 0;
        m->ofd_tot_pending = 0;

        m->ofd_max_group = 0;

#if 0
        cfs_rwlock_init(&m->ofd_sptlrpc_lock);
        sptlrpc_rule_set_init(&m->ofd_sptlrpc_rset);
#else
        filter = &obd->u.filter;
        cfs_rwlock_init(&filter->fo_sptlrpc_lock);
        sptlrpc_rule_set_init(&filter->fo_sptlrpc_rset);
#endif

        m->ofd_fl_oss_capa = 0;
        CFS_INIT_LIST_HEAD(&m->ofd_capa_keys);
        m->ofd_capa_hash = init_capa_hash();
        if (m->ofd_capa_hash == NULL)
                RETURN(-ENOMEM);

        CFS_INIT_LIST_HEAD(&m->ofd_llog_list);
        cfs_spin_lock_init(&m->ofd_llog_list_lock);
        m->ofd_lcm = NULL;

        dt_device_init(&m->ofd_dt_dev, ldt);
        m->ofd_dt_dev.dd_lu_dev.ld_ops = &filter_lu_ops;
        m->ofd_dt_dev.dd_lu_dev.ld_obd = obd;
        /* set this lu_device to obd, because error handling need it */
        obd->obd_lu_dev = &m->ofd_dt_dev.dd_lu_dev;

        rc = lu_env_refill((struct lu_env *)env);
        if (rc != 0)
                RETURN(rc);
        LASSERT(env);

        rc = filter_procfs_init(m);
        if (rc) {
                CERROR("Can't init filter lprocfs, rc %d\n", rc);
                GOTO(err_fini_proc, rc);
        }

        obd->obd_replayable = 1;
        /* No connection accepted until configurations will finish */
        obd->obd_no_conn = 1;

        if (cfg->lcfg_bufcount > 4 && LUSTRE_CFG_BUFLEN(cfg, 4) > 0) {
                char *str = lustre_cfg_string(cfg, 4);
                if (strchr(str, 'n')) {
                        CWARN("%s: recovery disabled\n", obd->obd_name);
                        obd->obd_replayable = 0;
                }
        }

        /* init the stack */
        rc = filter_stack_init(env, m, cfg);
        if (rc) {
                CERROR("Can't init device stack, rc %d\n", rc);
                GOTO(err_fini_proc, rc);
        }

        info = filter_info_init(env, NULL);
        LASSERT(info != NULL);

        snprintf(info->fti_u.ns_name, sizeof(info->fti_u.ns_name), "filter-%p", m);
        m->ofd_namespace = ldlm_namespace_new(obd, info->fti_u.ns_name,
                                              LDLM_NAMESPACE_SERVER,
                                              LDLM_NAMESPACE_GREEDY,
                                              LDLM_NS_TYPE_OST);
        if (m->ofd_namespace == NULL)
                GOTO(err_stack_fini, rc = -ENOMEM);

        dt_conf_get(env, m->ofd_osd, &m->ofd_dt_conf);

        ldlm_register_intent(m->ofd_namespace, filter_intent_policy);
        m->ofd_namespace->ns_lvbo = &filter_lvbo;
        m->ofd_namespace->ns_lvbp = m;
        /* set obd_namespace for compatibility with old code */
        obd->obd_namespace = m->ofd_namespace;

        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "filter_ldlm_cb_client", &obd->obd_ldlm_client);

        rc = filter_fs_setup(env, m, obd);
        if (rc)
                GOTO(err_free_ns, rc);

        rc = obd_llog_init(obd, &obd->obd_olg, obd, NULL);
        if (rc) {
                CERROR("failed to setup llogging subsystems\n");
                GOTO(err_lut_fini, rc);
        }


#if 0
        lvfs_init_ctxt(&obd->obd_lvfs_ctxt, lmi->lmi_mnt, &null_ops);

        LASSERT(obd->obd_olg.olg_group == OBD_LLOG_GROUP);
        rc = llog_cat_initialize(obd, &obd->obd_olg, 1, NULL);
        LASSERT(rc == 0);
#endif

        next = m->ofd_osd;
        rc = next->dd_ops->dt_quota.dt_setup(env, next, NULL);
        if (rc) {
                CERROR("failed to setup quota\n");
                GOTO(err_fs_cleanup, rc);
        }

        if (ldlm_timeout == LDLM_TIMEOUT_DEFAULT)
                ldlm_timeout = 6;

        RETURN(0);

err_fs_cleanup:
        next->dd_ops->dt_quota.dt_cleanup(env, next);
        filter_fs_cleanup(env, m);
err_lut_fini:
        lut_fini(env, &m->ofd_lut);
err_free_ns:
        ldlm_namespace_free(m->ofd_namespace, 0, obd->obd_force);
        obd->obd_namespace = m->ofd_namespace = NULL;
err_stack_fini:
        filter_stack_fini(env, m, &m->ofd_osd->dd_lu_dev);
err_fini_proc:
        filter_procfs_fini(m);

        dt_device_fini(&m->ofd_dt_dev);
        return (rc);
}

static void filter_fini(const struct lu_env *env, struct filter_device *m)
{
        struct obd_device *obd = filter_obd(m);
        struct lu_device  *d = &m->ofd_dt_dev.dd_lu_dev;
        struct dt_device  *next;

        target_recovery_fini(obd);
#if 0
        filter_obd_llog_cleanup(obd);
#endif
        obd_exports_barrier(obd);
        obd_zombie_barrier();

        lut_fini(env, &m->ofd_lut);
        filter_fs_cleanup(env, m);

        if (m->ofd_namespace != NULL) {
                ldlm_namespace_free(m->ofd_namespace, NULL, d->ld_obd->obd_force);
                d->ld_obd->obd_namespace = m->ofd_namespace = NULL;
        }

        filter_procfs_fini(m);
        if (obd->obd_fsops)
                fsfilt_put_ops(obd->obd_fsops);
#if 0
        sptlrpc_rule_set_free(&m->mdt_sptlrpc_rset);
#endif

        next = m->ofd_osd;
        next->dd_ops->dt_quota.dt_cleanup(env, next);

        /* 
         * Finish the stack 
         */
        filter_stack_fini(env, m, &m->ofd_osd->dd_lu_dev);

        LASSERT(cfs_atomic_read(&d->ld_ref) == 0);

        EXIT;
}

static struct lu_device* filter_device_fini(const struct lu_env *env,
                                            struct lu_device *d)
{
        ENTRY;
        filter_fini(env, filter_dev(d));
        RETURN(NULL);
}

static struct lu_device *filter_device_free(const struct lu_env *env,
                                            struct lu_device *d)
{
        struct filter_device *m = filter_dev(d);

        dt_device_fini(&m->ofd_dt_dev);
        OBD_FREE_PTR(m);
        RETURN(NULL);
}

static struct lu_device *filter_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *cfg)
{
        struct filter_device *m;
        struct lu_device  *l;
        int rc;

        OBD_ALLOC_PTR(m);
        if (m == NULL)
                return ERR_PTR(-ENOMEM);

        l = &m->ofd_dt_dev.dd_lu_dev;
        rc = filter_init0(env, m, t, cfg);
        if (rc != 0) {
                OBD_FREE_PTR(m);
                l = ERR_PTR(rc);
        }

        return l;
}

/* thread context key constructor/destructor */
LU_KEY_INIT_FINI(filter, struct filter_thread_info);
static void filter_key_exit(const struct lu_context *ctx,
                            struct lu_context_key *key, void *data)
{
        struct filter_thread_info *info = data;
        info->fti_exp = NULL;
        info->fti_env = NULL;

        info->fti_xid = 0;
        info->fti_transno = 0;
        info->fti_has_trans = 0;
        info->fti_no_need_trans = 0;

        memset(&info->fti_attr, 0, sizeof info->fti_attr);
}

struct lu_context_key filter_thread_key = {
        .lct_tags = LCT_DT_THREAD,
        .lct_init = filter_key_init,
        .lct_fini = filter_key_fini,
        .lct_exit = filter_key_exit
};

/* transaction context key */
LU_KEY_INIT_FINI(filter_txn, struct filter_txn_info);
LU_CONTEXT_KEY_DEFINE(filter_txn, LCT_TX_HANDLE);

/* type constructor/destructor: mdt_type_init, mdt_type_fini */
LU_TYPE_INIT_FINI(filter, &filter_thread_key, &filter_txn_thread_key);

static struct lu_device_type_operations filter_device_type_ops = {
        .ldto_init         = filter_type_init,
        .ldto_fini         = filter_type_fini,

        .ldto_start        = filter_type_start,
        .ldto_stop         = filter_type_stop,

        .ldto_device_alloc = filter_device_alloc,
        .ldto_device_free  = filter_device_free,
        .ldto_device_fini  = filter_device_fini
};

static struct lu_device_type filter_device_type = {
        .ldt_tags     = LU_DEVICE_DT,
        .ldt_name     = LUSTRE_OST_NAME,
        .ldt_ops      = &filter_device_type_ops,
        .ldt_ctx_tags = LCT_DT_THREAD
};

extern struct obd_ops filter_obd_ops;

int __init ofd_init(void)
{
        struct lprocfs_static_vars lvars;
        int rc;

        lprocfs_filter_init_vars(&lvars);

        rc = ofd_fmd_init();
        if (rc)
                return(rc);

        rc = class_register_type(&filter_obd_ops, NULL, lvars.module_vars,
                                 LUSTRE_OST_NAME, &filter_device_type);
        if (rc) {
                ofd_fmd_exit();
        }

        return rc;
}

void __exit ofd_exit(void)
{
        ofd_fmd_exit();

        class_unregister_type(LUSTRE_OST_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Filtering driver");
MODULE_LICENSE("GPL");

module_init(ofd_init);
module_exit(ofd_exit);
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
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ldlm/ldlm_pool.c
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

/*
 * Idea of this code is rather simple. Each second, for each server namespace
 * we have SLV - server lock volume which is calculated on current number of
 * granted locks, grant speed for past period, etc - that is, locking load.
 * This SLV number may be thought as a flow definition for simplicity. It is
 * sent to clients with each occasion to let them know what is current load
 * situation on the server. By default, at the beginning, SLV on server is
 * set max value which is calculated as the following: allow to one client
 * have all locks of limit ->pl_limit for 10h.
 *
 * Next, on clients, number of cached locks is not limited artificially in any
 * way as it was before. Instead, client calculates CLV, that is, client lock
 * volume for each lock and compares it with last SLV from the server. CLV is
 * calculated as the number of locks in LRU * lock live time in seconds. If
 * CLV > SLV - lock is canceled.
 *
 * Client has LVF, that is, lock volume factor which regulates how much sensitive
 * client should be about last SLV from server. The higher LVF is the more locks
 * will be canceled on client. Default value for it is 1. Setting LVF to 2 means
 * that client will cancel locks 2 times faster.
 *
 * Locks on a client will be canceled more intensively in these cases:
 * (1) if SLV is smaller, that is, load is higher on the server;
 * (2) client has a lot of locks (the more locks are held by client, the bigger
 *     chances that some of them should be canceled);
 * (3) client has old locks (taken some time ago);
 *
 * Thus, according to flow paradigm that we use for better understanding SLV,
 * CLV is the volume of particle in flow described by SLV. According to this,
 * if flow is getting thinner, more and more particles become outside of it and
 * as particles are locks, they should be canceled.
 *
 * General idea of this belongs to Vitaly Fertman (vitaly@clusterfs.com). Andreas
 * Dilger (adilger@clusterfs.com) proposed few nice ideas like using LVF and many
 * cleanups. Flow definition to allow more easy understanding of the logic belongs
 * to Nikita Danilov (nikita@clusterfs.com) as well as many cleanups and fixes.
 * And design and implementation are done by Yury Umanets (umka@clusterfs.com).
 *
 * Glossary for terms used:
 *
 * pl_limit - Number of allowed locks in pool. Applies to server and client
 * side (tunable);
 *
 * pl_granted - Number of granted locks (calculated);
 * pl_grant_rate - Number of granted locks for last T (calculated);
 * pl_cancel_rate - Number of canceled locks for last T (calculated);
 * pl_grant_speed - Grant speed (GR - CR) for last T (calculated);
 * pl_grant_plan - Planned number of granted locks for next T (calculated);
 * pl_server_lock_volume - Current server lock volume (calculated);
 *
 * As it may be seen from list above, we have few possible tunables which may
 * affect behavior much. They all may be modified via proc. However, they also
 * give a possibility for constructing few pre-defined behavior policies. If
 * none of predefines is suitable for a working pattern being used, new one may
 * be "constructed" via proc tunables.
 */

#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <lustre_dlm.h>
#else
# include <liblustre.h>
# include <libcfs/kp30.h>
#endif

#include <obd_class.h>
#include <obd_support.h>
#include "ldlm_internal.h"

#ifdef HAVE_LRU_RESIZE_SUPPORT

/*
 * 50 ldlm locks for 1MB of RAM.
 */
#define LDLM_POOL_HOST_L ((num_physpages >> (20 - CFS_PAGE_SHIFT)) * 50)

/*
 * Maximal possible grant step plan in %.
 */
#define LDLM_POOL_MAX_GSP (30)

/*
 * Minimal possible grant step plan in %.
 */
#define LDLM_POOL_MIN_GSP (1)

/*
 * This controls the speed of reaching LDLM_POOL_MAX_GSP
 * with increasing thread period. This is 4s which means
 * that for 10s thread period we will have 2 steps by 4s
 * each.
 */
#define LDLM_POOL_GSP_STEP (4)

/*
 * LDLM_POOL_GSP% of all locks is default GP.
 */
#define LDLM_POOL_GP(L)   (((L) * LDLM_POOL_MAX_GSP) / 100)

/*
 * Max age for locks on clients.
 */
#define LDLM_POOL_MAX_AGE (36000)

#ifdef __KERNEL__
extern cfs_proc_dir_entry_t *ldlm_ns_proc_dir;
#endif

#define avg(src, add) \
        ((src) = ((src) + (add)) / 2)

static inline __u64 dru(__u64 val, __u32 div)
{
        __u64 ret = val + (div - 1);
        do_div(ret, div);
        return ret;
}

static inline __u64 ldlm_pool_slv_max(__u32 L)
{
        /*
         * Allow to have all locks for 1 client for 10 hrs.
         * Formula is the following: limit * 10h / 1 client.
         */
        __u64 lim = L *  LDLM_POOL_MAX_AGE / 1;
        return lim;
}

static inline __u64 ldlm_pool_slv_min(__u32 L)
{
        return 1;
}

enum {
        LDLM_POOL_FIRST_STAT = 0,
        LDLM_POOL_GRANTED_STAT = LDLM_POOL_FIRST_STAT,
        LDLM_POOL_GRANT_STAT,
        LDLM_POOL_CANCEL_STAT,
        LDLM_POOL_GRANT_RATE_STAT,
        LDLM_POOL_CANCEL_RATE_STAT,
        LDLM_POOL_GRANT_PLAN_STAT,
        LDLM_POOL_SLV_STAT,
        LDLM_POOL_SHRINK_REQTD_STAT,
        LDLM_POOL_SHRINK_FREED_STAT,
        LDLM_POOL_RECALC_STAT,
        LDLM_POOL_TIMING_STAT,
        LDLM_POOL_LAST_STAT
};

static inline struct ldlm_namespace *ldlm_pl2ns(struct ldlm_pool *pl)
{
        return container_of(pl, struct ldlm_namespace, ns_pool);
}

/**
 * Calculates suggested grant_step in % of available locks for passed
 * \a period. This is later used in grant_plan calculations.
 */
static inline int ldlm_pool_t2gsp(int t)
{
        /*
         * This yeilds 1% grant step for anything below LDLM_POOL_GSP_STEP
         * and up to 30% for anything higher than LDLM_POOL_GSP_STEP.
         *
         * How this will affect execution is the following:
         *
         * - for thread peroid 1s we will have grant_step 1% which good from
         * pov of taking some load off from server and push it out to clients.
         * This is like that because 1% for grant_step means that server will
         * not allow clients to get lots of locks inshort period of time and
         * keep all old locks in their caches. Clients will always have to
         * get some locks back if they want to take some new;
         *
         * - for thread period 10s (which is default) we will have 23% which
         * means that clients will have enough of room to take some new locks
         * without getting some back. All locks from this 23% which were not
         * taken by clients in current period will contribute in SLV growing.
         * SLV growing means more locks cached on clients until limit or grant
         * plan is reached.
         */
        return LDLM_POOL_MAX_GSP -
                (LDLM_POOL_MAX_GSP - LDLM_POOL_MIN_GSP) /
                (1 << (t / LDLM_POOL_GSP_STEP));
}

/**
 * Recalculates next grant limit on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static inline void ldlm_pool_recalc_grant_plan(struct ldlm_pool *pl)
{
        int granted, grant_step, limit;

        limit = ldlm_pool_get_limit(pl);
        granted = atomic_read(&pl->pl_granted);

        grant_step = ldlm_pool_t2gsp(pl->pl_recalc_period);
        grant_step = ((limit - granted) * grant_step) / 100;
        pl->pl_grant_plan = granted + grant_step;
}

/**
 * Recalculates next SLV on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static inline void ldlm_pool_recalc_slv(struct ldlm_pool *pl)
{
        int grant_usage, granted, grant_plan;
        __u64 slv, slv_factor;
        __u32 limit;

        slv = pl->pl_server_lock_volume;
        grant_plan = pl->pl_grant_plan;
        limit = ldlm_pool_get_limit(pl);
        granted = atomic_read(&pl->pl_granted);

        grant_usage = limit - (granted - grant_plan);
        if (grant_usage <= 0)
                grant_usage = 1;

        /*
         * Find out SLV change factor which is the ratio of grant usage
         * from limit. SLV changes as fast as the ratio of grant plan
         * consumtion. The more locks from grant plan are not consumed
         * by clients in last interval (idle time), the faster grows
         * SLV. And the opposite, the more grant plan is over-consumed
         * (load time) the faster drops SLV.
         */
        slv_factor = (grant_usage * 100) / limit;
        if (2 * abs(granted - limit) > limit) {
                slv_factor *= slv_factor;
                slv_factor = dru(slv_factor, 100);
        }
        slv = slv * slv_factor;
        slv = dru(slv, 100);

        if (slv > ldlm_pool_slv_max(limit)) {
                slv = ldlm_pool_slv_max(limit);
        } else if (slv < ldlm_pool_slv_min(limit)) {
                slv = ldlm_pool_slv_min(limit);
        }

        pl->pl_server_lock_volume = slv;
}

/**
 * Recalculates next stats on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static inline void ldlm_pool_recalc_stats(struct ldlm_pool *pl)
{
        int grant_plan = pl->pl_grant_plan;
        __u64 slv = pl->pl_server_lock_volume;
        int granted = atomic_read(&pl->pl_granted);
        int grant_rate = atomic_read(&pl->pl_grant_rate);
        int cancel_rate = atomic_read(&pl->pl_cancel_rate);

        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_SLV_STAT,
                            slv);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                            granted);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                            grant_rate);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
                            grant_plan);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                            cancel_rate);
}

/**
 * Sets current SLV into obd accessible via ldlm_pl2ns(pl)->ns_obd.
 */
static void ldlm_srv_pool_push_slv(struct ldlm_pool *pl)
{
        struct obd_device *obd;

        /*
         * Set new SLV in obd field for using it later without accessing the
         * pool. This is required to avoid race between sending reply to client
         * with new SLV and cleanup server stack in which we can't guarantee
         * that namespace is still alive. We know only that obd is alive as
         * long as valid export is alive.
         */
        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL);
        write_lock(&obd->obd_pool_lock);
        obd->obd_pool_slv = pl->pl_server_lock_volume;
        write_unlock(&obd->obd_pool_lock);
}

/**
 * Recalculates all pool fields on passed \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
static int ldlm_srv_pool_recalc(struct ldlm_pool *pl)
{
        time_t recalc_interval_sec;
        ENTRY;

        spin_lock(&pl->pl_lock);
        recalc_interval_sec = cfs_time_current_sec() - pl->pl_recalc_time;
        if (recalc_interval_sec >= pl->pl_recalc_period) {
                /*
                 * Recalc SLV after last period. This should be done
                 * _before_ recalculating new grant plan.
                 */
                ldlm_pool_recalc_slv(pl);

                /*
                 * Make sure that pool informed obd of last SLV changes.
                 */
                ldlm_srv_pool_push_slv(pl);

                /*
                 * Update grant_plan for new period.
                 */
                ldlm_pool_recalc_grant_plan(pl);

                pl->pl_recalc_time = cfs_time_current_sec();
                lprocfs_counter_add(pl->pl_stats, LDLM_POOL_TIMING_STAT,
                                    recalc_interval_sec);
        }
        spin_unlock(&pl->pl_lock);
        RETURN(0);
}

/**
 * This function is used on server side as main entry point for memory
 * preasure handling. It decreases SLV on \a pl according to passed
 * \a nr and \a gfp_mask.
 *
 * Our goal here is to decrease SLV such a way that clients hold \a nr
 * locks smaller in next 10h.
 */
static int ldlm_srv_pool_shrink(struct ldlm_pool *pl,
                                int nr, unsigned int gfp_mask)
{
        __u32 limit;

        /*
         * VM is asking how many entries may be potentially freed.
         */
        if (nr == 0)
                return atomic_read(&pl->pl_granted);

        /*
         * Client already canceled locks but server is already in shrinker
         * and can't cancel anything. Let's catch this race.
         */
        if (atomic_read(&pl->pl_granted) == 0)
                RETURN(0);

        spin_lock(&pl->pl_lock);

        /*
         * We want shrinker to possibly cause cancelation of @nr locks from
         * clients or grant approximately @nr locks smaller next intervals.
         *
         * This is why we decresed SLV by @nr. This effect will only be as
         * long as one re-calc interval (1s these days) and this should be
         * enough to pass this decreased SLV to all clients. On next recalc
         * interval pool will either increase SLV if locks load is not high
         * or will keep on same level or even decrease again, thus, shrinker
         * decreased SLV will affect next recalc intervals and this way will
         * make locking load lower.
         */
        if (nr < pl->pl_server_lock_volume) {
                pl->pl_server_lock_volume = pl->pl_server_lock_volume - nr;
        } else {
                limit = ldlm_pool_get_limit(pl);
                pl->pl_server_lock_volume = ldlm_pool_slv_min(limit);
        }

        /*
         * Make sure that pool informed obd of last SLV changes.
         */
        ldlm_srv_pool_push_slv(pl);
        spin_unlock(&pl->pl_lock);

        /*
         * We did not really free any memory here so far, it only will be
         * freed later may be, so that we return 0 to not confuse VM.
         */
        return 0;
}

/**
 * Setup server side pool \a pl with passed \a limit.
 */
static int ldlm_srv_pool_setup(struct ldlm_pool *pl, int limit)
{
        struct obd_device *obd;
        ENTRY;

        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL && obd != LP_POISON);
        LASSERT(obd->obd_type != LP_POISON);
        write_lock(&obd->obd_pool_lock);
        obd->obd_pool_limit = limit;
        write_unlock(&obd->obd_pool_lock);

        ldlm_pool_set_limit(pl, limit);
        RETURN(0);
}

/**
 * Sets SLV and Limit from ldlm_pl2ns(pl)->ns_obd tp passed \a pl.
 */
static void ldlm_cli_pool_pop_slv(struct ldlm_pool *pl)
{
        struct obd_device *obd;

        /*
         * Get new SLV and Limit from obd which is updated with comming
         * RPCs.
         */
        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL);
        read_lock(&obd->obd_pool_lock);
        pl->pl_server_lock_volume = obd->obd_pool_slv;
        ldlm_pool_set_limit(pl, obd->obd_pool_limit);
        read_unlock(&obd->obd_pool_lock);
}

/**
 * Recalculates client sise pool \a pl according to current SLV and Limit.
 */
static int ldlm_cli_pool_recalc(struct ldlm_pool *pl)
{
        time_t recalc_interval_sec;
        ENTRY;

        spin_lock(&pl->pl_lock);
        /*
         * Check if we need to recalc lists now.
         */
        recalc_interval_sec = cfs_time_current_sec() - pl->pl_recalc_time;
        if (recalc_interval_sec < pl->pl_recalc_period) {
                spin_unlock(&pl->pl_lock);
                RETURN(0);
        }

        /*
         * Make sure that pool knows last SLV and Limit from obd.
         */
        ldlm_cli_pool_pop_slv(pl);

        pl->pl_recalc_time = cfs_time_current_sec();
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_TIMING_STAT,
                            recalc_interval_sec);
        spin_unlock(&pl->pl_lock);

        /*
         * Do not cancel locks in case lru resize is disabled for this ns.
         */
        if (!ns_connect_lru_resize(ldlm_pl2ns(pl)))
                RETURN(0);

        /*
         * In the time of canceling locks on client we do not need to maintain
         * sharp timing, we only want to cancel locks asap according to new SLV.
         * It may be called when SLV has changed much, this is why we do not
         * take into account pl->pl_recalc_time here.
         */
        RETURN(ldlm_cancel_lru(ldlm_pl2ns(pl), 0, LDLM_SYNC,
                               LDLM_CANCEL_LRUR));
}

/**
 * This function is main entry point for memory preasure handling on client side.
 * Main goal of this function is to cancel some number of locks on passed \a pl
 * according to \a nr and \a gfp_mask.
 */
static int ldlm_cli_pool_shrink(struct ldlm_pool *pl,
                                int nr, unsigned int gfp_mask)
{
        struct ldlm_namespace *ns;
        int canceled = 0, unused;

        ns = ldlm_pl2ns(pl);

        /*
         * Do not cancel locks in case lru resize is disabled for this ns.
         */
        if (!ns_connect_lru_resize(ns))
                RETURN(0);

        /*
         * Make sure that pool knows last SLV and Limit from obd.
         */
        ldlm_cli_pool_pop_slv(pl);

        spin_lock(&ns->ns_unused_lock);
        unused = ns->ns_nr_unused;
        spin_unlock(&ns->ns_unused_lock);

        if (nr) {
                canceled = ldlm_cancel_lru(ns, nr, LDLM_SYNC,
                                           LDLM_CANCEL_SHRINK);
        }
#ifdef __KERNEL__
        /*
         * Retrun the number of potentially reclaimable locks.
         */
        return ((unused - canceled) / 100) * sysctl_vfs_cache_pressure;
#else
        return unused - canceled;
#endif
}

struct ldlm_pool_ops ldlm_srv_pool_ops = {
        .po_recalc = ldlm_srv_pool_recalc,
        .po_shrink = ldlm_srv_pool_shrink,
        .po_setup  = ldlm_srv_pool_setup
};

struct ldlm_pool_ops ldlm_cli_pool_ops = {
        .po_recalc = ldlm_cli_pool_recalc,
        .po_shrink = ldlm_cli_pool_shrink
};

/**
 * Pool recalc wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 */
int ldlm_pool_recalc(struct ldlm_pool *pl)
{
        time_t recalc_interval_sec;
        int count;

        spin_lock(&pl->pl_lock);
        recalc_interval_sec = cfs_time_current_sec() - pl->pl_recalc_time;
        if (recalc_interval_sec > 0) {
                /*
                 * Update pool statistics every 1s.
                 */
                ldlm_pool_recalc_stats(pl);

                /*
                 * Zero out all rates and speed for the last period.
                 */
                atomic_set(&pl->pl_grant_rate, 0);
                atomic_set(&pl->pl_cancel_rate, 0);
                atomic_set(&pl->pl_grant_speed, 0);
        }
        spin_unlock(&pl->pl_lock);

        if (pl->pl_ops->po_recalc != NULL) {
                count = pl->pl_ops->po_recalc(pl);
                lprocfs_counter_add(pl->pl_stats, LDLM_POOL_RECALC_STAT,
                                    count);
                return count;
        }

        return 0;
}
EXPORT_SYMBOL(ldlm_pool_recalc);

/**
 * Pool shrink wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 */
int ldlm_pool_shrink(struct ldlm_pool *pl, int nr,
                     unsigned int gfp_mask)
{
        int cancel = 0;

        if (pl->pl_ops->po_shrink != NULL) {
                cancel = pl->pl_ops->po_shrink(pl, nr, gfp_mask);
                if (nr > 0) {
                        lprocfs_counter_add(pl->pl_stats,
                                            LDLM_POOL_SHRINK_REQTD_STAT,
                                            nr);
                        lprocfs_counter_add(pl->pl_stats,
                                            LDLM_POOL_SHRINK_FREED_STAT,
                                            cancel);
                        CDEBUG(D_DLMTRACE, "%s: request to shrink %d locks, "
                               "shrunk %d\n", pl->pl_name, nr, cancel);
                }
        }
        return cancel;
}
EXPORT_SYMBOL(ldlm_pool_shrink);

/**
 * Pool setup wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 *
 * Sets passed \a limit into pool \a pl.
 */
int ldlm_pool_setup(struct ldlm_pool *pl, int limit)
{
        ENTRY;
        if (pl->pl_ops->po_setup != NULL)
                RETURN(pl->pl_ops->po_setup(pl, limit));
        RETURN(0);
}
EXPORT_SYMBOL(ldlm_pool_setup);

#ifdef __KERNEL__
static int lprocfs_rd_pool_state(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        int granted, grant_rate, cancel_rate, grant_step;
        int nr = 0, grant_speed, grant_plan, lvf;
        struct ldlm_pool *pl = data;
        __u64 slv, clv;
        __u32 limit;

        spin_lock(&pl->pl_lock);
        slv = pl->pl_server_lock_volume;
        clv = pl->pl_client_lock_volume;
        limit = ldlm_pool_get_limit(pl);
        grant_plan = pl->pl_grant_plan;
        granted = atomic_read(&pl->pl_granted);
        grant_rate = atomic_read(&pl->pl_grant_rate);
        lvf = atomic_read(&pl->pl_lock_volume_factor);
        grant_speed = atomic_read(&pl->pl_grant_speed);
        cancel_rate = atomic_read(&pl->pl_cancel_rate);
        grant_step = ldlm_pool_t2gsp(pl->pl_recalc_period);
        spin_unlock(&pl->pl_lock);

        nr += snprintf(page + nr, count - nr, "LDLM pool state (%s):\n",
                       pl->pl_name);
        nr += snprintf(page + nr, count - nr, "  SLV: "LPU64"\n", slv);
        nr += snprintf(page + nr, count - nr, "  CLV: "LPU64"\n", clv);
        nr += snprintf(page + nr, count - nr, "  LVF: %d\n", lvf);

        if (ns_is_server(ldlm_pl2ns(pl))) {
                nr += snprintf(page + nr, count - nr, "  GSP: %d%%\n",
                               grant_step);
                nr += snprintf(page + nr, count - nr, "  GP:  %d\n",
                               grant_plan);
        }
        nr += snprintf(page + nr, count - nr, "  GR:  %d\n",
                       grant_rate);
        nr += snprintf(page + nr, count - nr, "  CR:  %d\n",
                       cancel_rate);
        nr += snprintf(page + nr, count - nr, "  GS:  %d\n",
                       grant_speed);
        nr += snprintf(page + nr, count - nr, "  G:   %d\n",
                       granted);
        nr += snprintf(page + nr, count - nr, "  L:   %d\n",
                       limit);
        return nr;
}

LDLM_POOL_PROC_READER(grant_plan, int);
LDLM_POOL_PROC_READER(recalc_period, int);
LDLM_POOL_PROC_WRITER(recalc_period, int);

static int ldlm_pool_proc_init(struct ldlm_pool *pl)
{
        struct ldlm_namespace *ns = ldlm_pl2ns(pl);
        struct proc_dir_entry *parent_ns_proc;
        struct lprocfs_vars pool_vars[2];
        char *var_name = NULL;
        int rc = 0;
        ENTRY;

        OBD_ALLOC(var_name, MAX_STRING_SIZE + 1);
        if (!var_name)
                RETURN(-ENOMEM);

        parent_ns_proc = lprocfs_srch(ldlm_ns_proc_dir, ns->ns_name);
        if (parent_ns_proc == NULL) {
                CERROR("%s: proc entry is not initialized\n",
                       ns->ns_name);
                GOTO(out_free_name, rc = -EINVAL);
        }
        pl->pl_proc_dir = lprocfs_register("pool", parent_ns_proc,
                                           NULL, NULL);
        if (IS_ERR(pl->pl_proc_dir)) {
                CERROR("LProcFS failed in ldlm-pool-init\n");
                rc = PTR_ERR(pl->pl_proc_dir);
                GOTO(out_free_name, rc);
        }

        var_name[MAX_STRING_SIZE] = '\0';
        memset(pool_vars, 0, sizeof(pool_vars));
        pool_vars[0].name = var_name;

        snprintf(var_name, MAX_STRING_SIZE, "server_lock_volume");
        pool_vars[0].data = &pl->pl_server_lock_volume;
        pool_vars[0].read_fptr = lprocfs_rd_u64;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "limit");
        pool_vars[0].data = &pl->pl_limit;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        pool_vars[0].write_fptr = lprocfs_wr_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "granted");
        pool_vars[0].data = &pl->pl_granted;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "grant_speed");
        pool_vars[0].data = &pl->pl_grant_speed;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "cancel_rate");
        pool_vars[0].data = &pl->pl_cancel_rate;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "grant_rate");
        pool_vars[0].data = &pl->pl_grant_rate;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "grant_plan");
        pool_vars[0].data = pl;
        pool_vars[0].read_fptr = lprocfs_rd_grant_plan;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "recalc_period");
        pool_vars[0].data = pl;
        pool_vars[0].read_fptr = lprocfs_rd_recalc_period;
        pool_vars[0].write_fptr = lprocfs_wr_recalc_period;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "lock_volume_factor");
        pool_vars[0].data = &pl->pl_lock_volume_factor;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        pool_vars[0].write_fptr = lprocfs_wr_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "state");
        pool_vars[0].data = pl;
        pool_vars[0].read_fptr = lprocfs_rd_pool_state;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        pl->pl_stats = lprocfs_alloc_stats(LDLM_POOL_LAST_STAT -
                                           LDLM_POOL_FIRST_STAT, 0);
        if (!pl->pl_stats)
                GOTO(out_free_name, rc = -ENOMEM);

        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "granted", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_CANCEL_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "cancel", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "cancel_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_plan", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SLV_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "slv", "slv");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SHRINK_REQTD_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "shrink_request", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SHRINK_FREED_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "shrink_freed", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_RECALC_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "recalc_freed", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_TIMING_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "recalc_timing", "sec");
        lprocfs_register_stats(pl->pl_proc_dir, "stats", pl->pl_stats);

        EXIT;
out_free_name:
        OBD_FREE(var_name, MAX_STRING_SIZE + 1);
        return rc;
}

static void ldlm_pool_proc_fini(struct ldlm_pool *pl)
{
        if (pl->pl_stats != NULL) {
                lprocfs_free_stats(&pl->pl_stats);
                pl->pl_stats = NULL;
        }
        if (pl->pl_proc_dir != NULL) {
                lprocfs_remove(&pl->pl_proc_dir);
                pl->pl_proc_dir = NULL;
        }
}
#else /* !__KERNEL__*/
#define ldlm_pool_proc_init(pl) (0)
#define ldlm_pool_proc_fini(pl) while (0) {}
#endif

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
                   int idx, ldlm_side_t client)
{
        int rc;
        ENTRY;

        spin_lock_init(&pl->pl_lock);
        atomic_set(&pl->pl_granted, 0);
        pl->pl_recalc_time = cfs_time_current_sec();
        atomic_set(&pl->pl_lock_volume_factor, 1);

        atomic_set(&pl->pl_grant_rate, 0);
        atomic_set(&pl->pl_cancel_rate, 0);
        atomic_set(&pl->pl_grant_speed, 0);
        pl->pl_grant_plan = LDLM_POOL_GP(LDLM_POOL_HOST_L);

        snprintf(pl->pl_name, sizeof(pl->pl_name), "ldlm-pool-%s-%d",
                 ns->ns_name, idx);

        if (client == LDLM_NAMESPACE_SERVER) {
                pl->pl_ops = &ldlm_srv_pool_ops;
                ldlm_pool_set_limit(pl, LDLM_POOL_HOST_L);
                pl->pl_recalc_period = LDLM_POOL_SRV_DEF_RECALC_PERIOD;
                pl->pl_server_lock_volume = ldlm_pool_slv_max(LDLM_POOL_HOST_L);
        } else {
                ldlm_pool_set_limit(pl, 1);
                pl->pl_server_lock_volume = 1;
                pl->pl_ops = &ldlm_cli_pool_ops;
                pl->pl_recalc_period = LDLM_POOL_CLI_DEF_RECALC_PERIOD;
        }
        pl->pl_client_lock_volume = 0;
        rc = ldlm_pool_proc_init(pl);
        if (rc)
                RETURN(rc);

        CDEBUG(D_DLMTRACE, "Lock pool %s is initialized\n", pl->pl_name);

        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_pool_init);

void ldlm_pool_fini(struct ldlm_pool *pl)
{
        ENTRY;
        ldlm_pool_proc_fini(pl);

        /*
         * Pool should not be used after this point. We can't free it here as
         * it lives in struct ldlm_namespace, but still interested in catching
         * any abnormal using cases.
         */
        POISON(pl, 0x5a, sizeof(*pl));
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_fini);

/**
 * Add new taken ldlm lock \a lock into pool \a pl accounting.
 */
void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        /*
         * FLOCK locks are special in a sense that they are almost never
         * cancelled, instead special kind of lock is used to drop them.
         * also there is no LRU for flock locks, so no point in tracking
         * them anyway.
         */
        if (lock->l_resource->lr_type == LDLM_FLOCK)
                return;
        ENTRY;

        atomic_inc(&pl->pl_granted);
        atomic_inc(&pl->pl_grant_rate);
        atomic_inc(&pl->pl_grant_speed);

        lprocfs_counter_incr(pl->pl_stats, LDLM_POOL_GRANT_STAT);

        /*
         * Do not do pool recalc for client side as all locks which
         * potentially may be canceled has already been packed into
         * enqueue/cancel rpc. Also we do not want to run out of stack
         * with too long call paths.
         */
        if (ns_is_server(ldlm_pl2ns(pl)))
                ldlm_pool_recalc(pl);
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_add);

/**
 * Remove ldlm lock \a lock from pool \a pl accounting.
 */
void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        /*
         * Filter out FLOCK locks. Read above comment in ldlm_pool_add().
         */
        if (lock->l_resource->lr_type == LDLM_FLOCK)
                return;
        ENTRY;

        LASSERT(atomic_read(&pl->pl_granted) > 0);
        atomic_dec(&pl->pl_granted);
        atomic_inc(&pl->pl_cancel_rate);
        atomic_dec(&pl->pl_grant_speed);

        lprocfs_counter_incr(pl->pl_stats, LDLM_POOL_CANCEL_STAT);

        if (ns_is_server(ldlm_pl2ns(pl)))
                ldlm_pool_recalc(pl);
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_del);

/**
 * Returns current \a pl SLV.
 *
 * \pre ->pl_lock is not locked.
 */
__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
        __u64 slv;
        spin_lock(&pl->pl_lock);
        slv = pl->pl_server_lock_volume;
        spin_unlock(&pl->pl_lock);
        return slv;
}
EXPORT_SYMBOL(ldlm_pool_get_slv);

/**
 * Sets passed \a slv to \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
        spin_lock(&pl->pl_lock);
        pl->pl_server_lock_volume = slv;
        spin_unlock(&pl->pl_lock);
}
EXPORT_SYMBOL(ldlm_pool_set_slv);

/**
 * Returns current \a pl CLV.
 *
 * \pre ->pl_lock is not locked.
 */
__u64 ldlm_pool_get_clv(struct ldlm_pool *pl)
{
        __u64 slv;
        spin_lock(&pl->pl_lock);
        slv = pl->pl_client_lock_volume;
        spin_unlock(&pl->pl_lock);
        return slv;
}
EXPORT_SYMBOL(ldlm_pool_get_clv);

/**
 * Sets passed \a clv to \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
void ldlm_pool_set_clv(struct ldlm_pool *pl, __u64 clv)
{
        spin_lock(&pl->pl_lock);
        pl->pl_client_lock_volume = clv;
        spin_unlock(&pl->pl_lock);
}
EXPORT_SYMBOL(ldlm_pool_set_clv);

/**
 * Returns current \a pl limit.
 */
__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
        return atomic_read(&pl->pl_limit);
}
EXPORT_SYMBOL(ldlm_pool_get_limit);

/**
 * Sets passed \a limit to \a pl.
 */
void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
        atomic_set(&pl->pl_limit, limit);
}
EXPORT_SYMBOL(ldlm_pool_set_limit);

/**
 * Returns current LVF from \a pl.
 */
__u32 ldlm_pool_get_lvf(struct ldlm_pool *pl)
{
        return atomic_read(&pl->pl_lock_volume_factor);
}
EXPORT_SYMBOL(ldlm_pool_get_lvf);

#ifdef __KERNEL__
static int ldlm_pool_granted(struct ldlm_pool *pl)
{
        return atomic_read(&pl->pl_granted);
}

static struct ptlrpc_thread *ldlm_pools_thread;
static struct shrinker *ldlm_pools_srv_shrinker;
static struct shrinker *ldlm_pools_cli_shrinker;
static struct completion ldlm_pools_comp;

/*
 * Cancel \a nr locks from all namespaces (if possible). Returns number of
 * cached locks after shrink is finished. All namespaces are asked to
 * cancel approximately equal amount of locks to keep balancing.
 */
static int ldlm_pools_shrink(ldlm_side_t client, int nr,
                             unsigned int gfp_mask)
{
        int total = 0, cached = 0, nr_ns;
        struct ldlm_namespace *ns;

        if (nr != 0 && !(gfp_mask & __GFP_FS))
                return -1;

        if (nr != 0)
                CDEBUG(D_DLMTRACE, "Request to shrink %d %s locks\n",
                       nr, client == LDLM_NAMESPACE_CLIENT ? "client":"server");

        /*
         * Find out how many resources we may release.
         */
        for (nr_ns = atomic_read(ldlm_namespace_nr(client));
             nr_ns > 0; nr_ns--)
        {
                mutex_down(ldlm_namespace_lock(client));
                if (list_empty(ldlm_namespace_list(client))) {
                        mutex_up(ldlm_namespace_lock(client));
                        return 0;
                }
                ns = ldlm_namespace_first_locked(client);
                ldlm_namespace_get(ns);
                ldlm_namespace_move_locked(ns, client);
                mutex_up(ldlm_namespace_lock(client));
                total += ldlm_pool_shrink(&ns->ns_pool, 0, gfp_mask);
                ldlm_namespace_put(ns, 1);
        }

        if (nr == 0 || total == 0)
                return total;

        /*
         * Shrink at least ldlm_namespace_nr(client) namespaces.
         */
        for (nr_ns = atomic_read(ldlm_namespace_nr(client));
             nr_ns > 0; nr_ns--)
        {
                int cancel, nr_locks;

                /*
                 * Do not call shrink under ldlm_namespace_lock(client)
                 */
                mutex_down(ldlm_namespace_lock(client));
                if (list_empty(ldlm_namespace_list(client))) {
                        mutex_up(ldlm_namespace_lock(client));
                        /*
                         * If list is empty, we can't return any @cached > 0,
                         * that probably would cause needless shrinker
                         * call.
                         */
                        cached = 0;
                        break;
                }
                ns = ldlm_namespace_first_locked(client);
                ldlm_namespace_get(ns);
                ldlm_namespace_move_locked(ns, client);
                mutex_up(ldlm_namespace_lock(client));

                nr_locks = ldlm_pool_granted(&ns->ns_pool);
                cancel = 1 + nr_locks * nr / total;
                ldlm_pool_shrink(&ns->ns_pool, cancel, gfp_mask);
                cached += ldlm_pool_granted(&ns->ns_pool);
                ldlm_namespace_put(ns, 1);
        }
        return cached;
}

static int ldlm_pools_srv_shrink(int nr, unsigned int gfp_mask)
{
        return ldlm_pools_shrink(LDLM_NAMESPACE_SERVER, nr, gfp_mask);
}

static int ldlm_pools_cli_shrink(int nr, unsigned int gfp_mask)
{
        return ldlm_pools_shrink(LDLM_NAMESPACE_CLIENT, nr, gfp_mask);
}

void ldlm_pools_recalc(ldlm_side_t client)
{
        __u32 nr_l = 0, nr_p = 0, l;
        struct ldlm_namespace *ns;
        int nr, equal = 0;

        /*
         * No need to setup pool limit for client pools.
         */
        if (client == LDLM_NAMESPACE_SERVER) {
                /*
                 * Check all modest namespaces first.
                 */
                mutex_down(ldlm_namespace_lock(client));
                list_for_each_entry(ns, ldlm_namespace_list(client),
                                    ns_list_chain)
                {
                        if (ns->ns_appetite != LDLM_NAMESPACE_MODEST)
                                continue;

                        l = ldlm_pool_granted(&ns->ns_pool);
                        if (l == 0)
                                l = 1;

                        /*
                         * Set the modest pools limit equal to their avg granted
                         * locks + 5%.
                         */
                        l += dru(l * LDLM_POOLS_MODEST_MARGIN, 100);
                        ldlm_pool_setup(&ns->ns_pool, l);
                        nr_l += l;
                        nr_p++;
                }

                /*
                 * Make sure that modest namespaces did not eat more that 2/3
                 * of limit.
                 */
                if (nr_l >= 2 * (LDLM_POOL_HOST_L / 3)) {
                        CWARN("\"Modest\" pools eat out 2/3 of server locks "
                              "limit (%d of %lu). This means that you have too "
                              "many clients for this amount of server RAM. "
                              "Upgrade server!\n", nr_l, LDLM_POOL_HOST_L);
                        equal = 1;
                }

                /*
                 * The rest is given to greedy namespaces.
                 */
                list_for_each_entry(ns, ldlm_namespace_list(client),
                                    ns_list_chain)
                {
                        if (!equal && ns->ns_appetite != LDLM_NAMESPACE_GREEDY)
                                continue;

                        if (equal) {
                                /*
                                 * In the case 2/3 locks are eaten out by
                                 * modest pools, we re-setup equal limit
                                 * for _all_ pools.
                                 */
                                l = LDLM_POOL_HOST_L /
                                        atomic_read(ldlm_namespace_nr(client));
                        } else {
                                /*
                                 * All the rest of greedy pools will have
                                 * all locks in equal parts.
                                 */
                                l = (LDLM_POOL_HOST_L - nr_l) /
                                        (atomic_read(ldlm_namespace_nr(client)) -
                                         nr_p);
                        }
                        ldlm_pool_setup(&ns->ns_pool, l);
                }
                mutex_up(ldlm_namespace_lock(client));
        }

        /*
         * Recalc at least ldlm_namespace_nr(client) namespaces.
         */
        for (nr = atomic_read(ldlm_namespace_nr(client)); nr > 0; nr--) {
                /*
                 * Lock the list, get first @ns in the list, getref, move it
                 * to the tail, unlock and call pool recalc. This way we avoid
                 * calling recalc under @ns lock what is really good as we get
                 * rid of potential deadlock on client nodes when canceling
                 * locks synchronously.
                 */
                mutex_down(ldlm_namespace_lock(client));
                if (list_empty(ldlm_namespace_list(client))) {
                        mutex_up(ldlm_namespace_lock(client));
                        break;
                }
                ns = ldlm_namespace_first_locked(client);
                ldlm_namespace_get(ns);
                ldlm_namespace_move_locked(ns, client);
                mutex_up(ldlm_namespace_lock(client));

                /*
                 * After setup is done - recalc the pool.
                 */
                ldlm_pool_recalc(&ns->ns_pool);
                ldlm_namespace_put(ns, 1);
        }
}
EXPORT_SYMBOL(ldlm_pools_recalc);

static int ldlm_pools_thread_main(void *arg)
{
        struct ptlrpc_thread *thread = (struct ptlrpc_thread *)arg;
        char *t_name = "ldlm_poold";
        ENTRY;

        cfs_daemonize(t_name);
        thread->t_flags = SVC_RUNNING;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        CDEBUG(D_DLMTRACE, "%s: pool thread starting, process %d\n",
               t_name, cfs_curproc_pid());

        while (1) {
                struct l_wait_info lwi;

                /*
                 * Recal all pools on this tick.
                 */
                ldlm_pools_recalc(LDLM_NAMESPACE_SERVER);
                ldlm_pools_recalc(LDLM_NAMESPACE_CLIENT);

                /*
                 * Wait until the next check time, or until we're
                 * stopped.
                 */
                lwi = LWI_TIMEOUT(cfs_time_seconds(LDLM_POOLS_THREAD_PERIOD),
                                  NULL, NULL);
                l_wait_event(thread->t_ctl_waitq, (thread->t_flags &
                                                   (SVC_STOPPING|SVC_EVENT)),
                             &lwi);

                if (thread->t_flags & SVC_STOPPING) {
                        thread->t_flags &= ~SVC_STOPPING;
                        break;
                } else if (thread->t_flags & SVC_EVENT) {
                        thread->t_flags &= ~SVC_EVENT;
                }
        }

        thread->t_flags = SVC_STOPPED;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        CDEBUG(D_DLMTRACE, "%s: pool thread exiting, process %d\n",
               t_name, cfs_curproc_pid());

        complete_and_exit(&ldlm_pools_comp, 0);
}

static int ldlm_pools_thread_start(void)
{
        struct l_wait_info lwi = { 0 };
        int rc;
        ENTRY;

        if (ldlm_pools_thread != NULL)
                RETURN(-EALREADY);

        OBD_ALLOC_PTR(ldlm_pools_thread);
        if (ldlm_pools_thread == NULL)
                RETURN(-ENOMEM);

        init_completion(&ldlm_pools_comp);
        cfs_waitq_init(&ldlm_pools_thread->t_ctl_waitq);

        /*
         * CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in ptlrpc_daemonize() right away.
         */
        rc = cfs_kernel_thread(ldlm_pools_thread_main, ldlm_pools_thread,
                               CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("Can't start pool thread, error %d\n",
                       rc);
                OBD_FREE(ldlm_pools_thread, sizeof(*ldlm_pools_thread));
                ldlm_pools_thread = NULL;
                RETURN(rc);
        }
        l_wait_event(ldlm_pools_thread->t_ctl_waitq,
                     (ldlm_pools_thread->t_flags & SVC_RUNNING), &lwi);
        RETURN(0);
}

static void ldlm_pools_thread_stop(void)
{
        ENTRY;

        if (ldlm_pools_thread == NULL) {
                EXIT;
                return;
        }

        ldlm_pools_thread->t_flags = SVC_STOPPING;
        cfs_waitq_signal(&ldlm_pools_thread->t_ctl_waitq);

        /*
         * Make sure that pools thread is finished before freeing @thread.
         * This fixes possible race and oops due to accessing freed memory
         * in pools thread.
         */
        wait_for_completion(&ldlm_pools_comp);
        OBD_FREE_PTR(ldlm_pools_thread);
        ldlm_pools_thread = NULL;
        EXIT;
}

int ldlm_pools_init(void)
{
        int rc;
        ENTRY;

        rc = ldlm_pools_thread_start();
        if (rc == 0) {
                ldlm_pools_srv_shrinker = set_shrinker(DEFAULT_SEEKS,
                                                       ldlm_pools_srv_shrink);
                ldlm_pools_cli_shrinker = set_shrinker(DEFAULT_SEEKS,
                                                       ldlm_pools_cli_shrink);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_pools_init);

void ldlm_pools_fini(void)
{
        if (ldlm_pools_srv_shrinker != NULL) {
                remove_shrinker(ldlm_pools_srv_shrinker);
                ldlm_pools_srv_shrinker = NULL;
        }
        if (ldlm_pools_cli_shrinker != NULL) {
                remove_shrinker(ldlm_pools_cli_shrinker);
                ldlm_pools_cli_shrinker = NULL;
        }
        ldlm_pools_thread_stop();
}
EXPORT_SYMBOL(ldlm_pools_fini);
#endif /* __KERNEL__ */

#else /* !HAVE_LRU_RESIZE_SUPPORT */
int ldlm_pool_setup(struct ldlm_pool *pl, int limit)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_setup);

int ldlm_pool_recalc(struct ldlm_pool *pl)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_recalc);

int ldlm_pool_shrink(struct ldlm_pool *pl,
                     int nr, unsigned int gfp_mask)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_shrink);

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
                   int idx, ldlm_side_t client)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_init);

void ldlm_pool_fini(struct ldlm_pool *pl)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_fini);

void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_add);

void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_del);

__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
        return 1;
}
EXPORT_SYMBOL(ldlm_pool_get_slv);

void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_set_slv);

__u64 ldlm_pool_get_clv(struct ldlm_pool *pl)
{
        return 1;
}
EXPORT_SYMBOL(ldlm_pool_get_clv);

void ldlm_pool_set_clv(struct ldlm_pool *pl, __u64 clv)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_set_clv);

__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_get_limit);

void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_set_limit);

__u32 ldlm_pool_get_lvf(struct ldlm_pool *pl)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_get_lvf);

int ldlm_pools_init(void)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pools_init);

void ldlm_pools_fini(void)
{
        return;
}
EXPORT_SYMBOL(ldlm_pools_fini);

void ldlm_pools_recalc(ldlm_side_t client)
{
        return;
}
EXPORT_SYMBOL(ldlm_pools_recalc);
#endif /* HAVE_LRU_RESIZE_SUPPORT */

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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * cl_object implementation for VVP layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#ifndef __KERNEL__
# error This file is kernel only.
#endif

#include <libcfs/libcfs.h>

#include <obd.h>
#include <lustre_lite.h>

#include "vvp_internal.h"

/*****************************************************************************
 *
 * Object operations.
 *
 */

static int vvp_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *o)
{
        struct ccc_object    *obj   = lu2ccc(o);
        struct inode         *inode = obj->cob_inode;
        struct ll_inode_info *lli;

        (*p)(env, cookie, "(%s %i %i) inode: %p ",
             cfs_list_empty(&obj->cob_pending_list) ? "-" : "+",
             obj->cob_transient_pages, cfs_atomic_read(&obj->cob_mmap_cnt),
             inode);
        if (inode) {
                lli = ll_i2info(inode);
                (*p)(env, cookie, "%lu/%u %o %u %i %p "DFID,
                     inode->i_ino, inode->i_generation, inode->i_mode,
                     inode->i_nlink, atomic_read(&inode->i_count),
                     lli->lli_clob, PFID(&lli->lli_fid));
        }
        return 0;
}

static int vvp_attr_get(const struct lu_env *env, struct cl_object *obj,
                        struct cl_attr *attr)
{
        struct inode *inode = ccc_object_inode(obj);

        /*
         * lov overwrites most of these fields in
         * lov_attr_get()->...lov_merge_lvb_kms(), except when inode
         * attributes are newer.
         */

        attr->cat_size = i_size_read(inode);
        attr->cat_mtime = LTIME_S(inode->i_mtime);
        attr->cat_atime = LTIME_S(inode->i_atime);
        attr->cat_ctime = LTIME_S(inode->i_ctime);
        attr->cat_blocks = inode->i_blocks;
        attr->cat_uid = inode->i_uid;
        attr->cat_gid = inode->i_gid;
        /* KMS is not known by this layer */
        return 0; /* layers below have to fill in the rest */
}

static int vvp_attr_set(const struct lu_env *env, struct cl_object *obj,
                        const struct cl_attr *attr, unsigned valid)
{
        struct inode *inode = ccc_object_inode(obj);

        if (valid & CAT_UID)
                inode->i_uid = attr->cat_uid;
        if (valid & CAT_GID)
                inode->i_gid = attr->cat_gid;
        if (0 && valid & CAT_SIZE)
                cl_isize_write_nolock(inode, attr->cat_size);
        /* not currently necessary */
        if (0 && valid & (CAT_UID|CAT_GID|CAT_SIZE))
                mark_inode_dirty(inode);
        return 0;
}

static const struct cl_object_operations vvp_ops = {
        .coo_page_init = vvp_page_init,
        .coo_lock_init = vvp_lock_init,
        .coo_io_init   = vvp_io_init,
        .coo_attr_get  = vvp_attr_get,
        .coo_attr_set  = vvp_attr_set,
        .coo_conf_set  = ccc_conf_set,
        .coo_glimpse   = ccc_object_glimpse
};

static const struct lu_object_operations vvp_lu_obj_ops = {
        .loo_object_init  = ccc_object_init,
        .loo_object_free  = ccc_object_free,
        .loo_object_print = vvp_object_print
};

struct ccc_object *cl_inode2ccc(struct inode *inode)
{
        struct cl_inode_info *lli = cl_i2info(inode);
        struct cl_object     *obj = lli->lli_clob;
        struct lu_object     *lu;

        LASSERT(obj != NULL);
        lu = lu_object_locate(obj->co_lu.lo_header, &vvp_device_type);
        LASSERT(lu != NULL);
        return lu2ccc(lu);
}

struct lu_object *vvp_object_alloc(const struct lu_env *env,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *dev)
{
        return ccc_object_alloc(env, hdr, dev, &vvp_ops, &vvp_lu_obj_ops);
}

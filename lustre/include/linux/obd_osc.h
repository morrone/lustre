/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <braam@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Data structures for object storage targets and client: OST & OSC's
 * 
 * See also lustre_idl.h for wire formats of requests.
 *
 */

#ifndef _LUSTRE_OST_H
#define _LUSTRE_OST_H

#include <linux/obd_support.h>

#define OST_EXIT 1
#define LUSTRE_OST_NAME "ost"

struct ost_obd {
	struct obd_device *ost_tgt;
	struct obd_conn ost_conn;
	struct task_struct *ost_thread;
	wait_queue_head_t ost_waitq;
	wait_queue_head_t ost_done_waitq;
	int ost_flags;
	spinlock_t ost_lock;
	struct list_head ost_reqs;
};

struct osc_obd {
	struct obd_device *ost_tgt;
};

struct ost_request { 
	struct list_head rq_list;
	struct ost_obd *rq_obd;
	int rq_status;

	char *rq_reqbuf;
	int rq_reqlen;
	struct ost_req_hdr *rq_reqhdr;
	struct ost_req *rq_req;

	char *rq_repbuf;
	int rq_replen;
	struct ost_rep_hdr *rq_rephdr;
	struct ost_rep *rq_rep;

        void *rq_reply_handle;
	wait_queue_head_t rq_wait_for_rep;
};

struct ost_req { 
	__u32   connid;
	__u32   cmd; 
	struct obdo oa;
	__u32   buflen1;
	__u32   buflen2;
	char *   buf1;
	char *   buf2;
};

struct ost_rep {
	__u32   result;
	__u32   connid;
	struct obdo oa;
	__u32   buflen1;
	__u32   buflen2;
	char *   buf1;
	char *   buf2;
};


/* ost/ost_pack.c */
int ost_pack_req(char *buf1, int buflen1, char *buf2, int buflen2, struct ost_req_hdr **hdr, struct ost_req **req, int *len, char **buf);
int ost_unpack_req(char *buf, int len, struct ost_req_hdr **hdr, struct ost_req **req);
int ost_pack_rep(void *buf1, __u32 buflen1, void *buf2, __u32 buflen2, struct ost_rep_hdr **hdr, struct ost_rep **rep, int *len, char **buf);
int ost_unpack_rep(char *buf, int len, struct ost_rep_hdr **hdr, struct ost_rep **rep);

#endif



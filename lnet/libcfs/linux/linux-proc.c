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
 * lnet/libcfs/linux/linux-proc.c
 *
 * Author: Zach Brown <zab@zabbo.net>
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <asm/uaccess.h>

#include <linux/proc_fs.h>
#include <linux/sysctl.h>

# define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/kp30.h>
#include <asm/div64.h>
#include "tracefile.h"

static cfs_sysctl_table_header_t *lnet_table_header = NULL;
extern char lnet_upcall[1024];
/**
 * The path of debug log dump upcall script.
 */
extern char lnet_debug_log_upcall[1024];

#ifndef HAVE_SYSCTL_UNNUMBERED
#define CTL_LNET        (0x100)
enum {
        PSDEV_DEBUG = 1,          /* control debugging */
        PSDEV_SUBSYSTEM_DEBUG,    /* control debugging */
        PSDEV_PRINTK,             /* force all messages to console */
        PSDEV_CONSOLE_RATELIMIT,  /* ratelimit console messages */
        PSDEV_CONSOLE_MAX_DELAY_CS, /* maximum delay over which we skip messages */
        PSDEV_CONSOLE_MIN_DELAY_CS, /* initial delay over which we skip messages */
        PSDEV_CONSOLE_BACKOFF,    /* delay increase factor */
        PSDEV_DEBUG_PATH,         /* crashdump log location */
        PSDEV_DEBUG_DUMP_PATH,    /* crashdump tracelog location */
        PSDEV_LNET_UPCALL,        /* User mode upcall script  */
        PSDEV_LNET_MEMUSED,       /* bytes currently PORTAL_ALLOCated */
        PSDEV_LNET_CATASTROPHE,   /* if we have LBUGged or panic'd */
        PSDEV_LNET_PANIC_ON_LBUG, /* flag to panic on LBUG */
        PSDEV_LNET_DUMP_KERNEL,   /* snapshot kernel debug buffer to file */
        PSDEV_LNET_DAEMON_FILE,   /* spool kernel debug buffer to file */
        PSDEV_LNET_DEBUG_MB,      /* size of debug buffer */
        PSDEV_LNET_DEBUG_LOG_UPCALL, /* debug log upcall script */
};
#else
#define CTL_LNET                        CTL_UNNUMBERED
#define PSDEV_DEBUG                     CTL_UNNUMBERED
#define PSDEV_SUBSYSTEM_DEBUG           CTL_UNNUMBERED
#define PSDEV_PRINTK                    CTL_UNNUMBERED
#define PSDEV_CONSOLE_RATELIMIT         CTL_UNNUMBERED
#define PSDEV_CONSOLE_MAX_DELAY_CS      CTL_UNNUMBERED
#define PSDEV_CONSOLE_MIN_DELAY_CS      CTL_UNNUMBERED
#define PSDEV_CONSOLE_BACKOFF           CTL_UNNUMBERED
#define PSDEV_DEBUG_PATH                CTL_UNNUMBERED
#define PSDEV_DEBUG_DUMP_PATH           CTL_UNNUMBERED
#define PSDEV_LNET_UPCALL               CTL_UNNUMBERED
#define PSDEV_LNET_MEMUSED              CTL_UNNUMBERED
#define PSDEV_LNET_CATASTROPHE          CTL_UNNUMBERED
#define PSDEV_LNET_PANIC_ON_LBUG        CTL_UNNUMBERED
#define PSDEV_LNET_DUMP_KERNEL          CTL_UNNUMBERED
#define PSDEV_LNET_DAEMON_FILE          CTL_UNNUMBERED
#define PSDEV_LNET_DEBUG_MB             CTL_UNNUMBERED
#define PSDEV_LNET_DEBUG_LOG_UPCALL     CTL_UNNUMBERED
#endif


static int
proc_call_handler(void *data, int write,
                  loff_t *ppos, void *buffer, size_t *lenp,
                  int (*handler)(void *data, int write,
                                 loff_t pos, void *buffer, int len))
{
        int rc = handler(data, write, *ppos, buffer, *lenp);

        if (rc < 0)
                return rc;

        if (write) {
                *ppos += *lenp;
        } else {
                *lenp = rc;
                *ppos += rc;
        }
        return 0;
}
EXPORT_SYMBOL(proc_call_handler);

static int __proc_dobitmasks(void *data, int write,
                             loff_t pos, void *buffer, int nob)
{
        const int     tmpstrlen = 512;
        char         *tmpstr;
        int           rc;
        unsigned int *mask = data;
        int           is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;
        int           is_printk = (mask == &libcfs_printk) ? 1 : 0;

        rc = trace_allocate_string_buffer(&tmpstr, tmpstrlen);
        if (rc < 0)
                return rc;

        if (!write) {
                libcfs_debug_mask2str(tmpstr, tmpstrlen, *mask, is_subsys);
                rc = strlen(tmpstr);

                if (pos >= rc) {
                        rc = 0;
                } else {
                        rc = trace_copyout_string(buffer, nob,
                                                  tmpstr + pos, "\n");
                }
        } else {
                rc = trace_copyin_string(tmpstr, tmpstrlen, buffer, nob);
                if (rc < 0)
                        return rc;

                rc = libcfs_debug_str2mask(mask, tmpstr, is_subsys);
                /* Always print LBUG/LASSERT to console, so keep this mask */
                if (is_printk)
                        *mask |= D_EMERG;
        }

        trace_free_string_buffer(tmpstr, tmpstrlen);
        return rc;
}

DECLARE_PROC_HANDLER(proc_dobitmasks)

static int __proc_dump_kernel(void *data, int write,
                              loff_t pos, void *buffer, int nob)
{
        if (!write)
                return 0;

        return trace_dump_debug_buffer_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_dump_kernel)

static int __proc_daemon_file(void *data, int write,
                              loff_t pos, void *buffer, int nob)
{
        if (!write) {
                int len = strlen(tracefile);

                if (pos >= len)
                        return 0;

                return trace_copyout_string(buffer, nob,
                                            tracefile + pos, "\n");
        }

        return trace_daemon_command_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_daemon_file)

static int __proc_debug_mb(void *data, int write,
                           loff_t pos, void *buffer, int nob)
{
        if (!write) {
                char tmpstr[32];
                int  len = snprintf(tmpstr, sizeof(tmpstr), "%d",
                                    trace_get_debug_mb());

                if (pos >= len)
                        return 0;

                return trace_copyout_string(buffer, nob, tmpstr + pos, "\n");
        }

        return trace_set_debug_mb_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_debug_mb)

int LL_PROC_PROTO(proc_console_max_delay_cs)
{
        int rc, max_delay_cs;
        cfs_sysctl_table_t dummy = *table;
        cfs_duration_t d;

        dummy.data = &max_delay_cs;
        dummy.proc_handler = &proc_dointvec;

        if (!write) { /* read */
                max_delay_cs = cfs_duration_sec(libcfs_console_max_delay * 100);
                rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
                return rc;
        }

        /* write */
        max_delay_cs = 0;
        rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
        if (rc < 0)
                return rc;
        if (max_delay_cs <= 0)
                return -EINVAL;

        d = cfs_time_seconds(max_delay_cs) / 100;
        if (d == 0 || d < libcfs_console_min_delay)
                return -EINVAL;
        libcfs_console_max_delay = d;

        return rc;
}

int LL_PROC_PROTO(proc_console_min_delay_cs)
{
        int rc, min_delay_cs;
        cfs_sysctl_table_t dummy = *table;
        cfs_duration_t d;

        dummy.data = &min_delay_cs;
        dummy.proc_handler = &proc_dointvec;

        if (!write) { /* read */
                min_delay_cs = cfs_duration_sec(libcfs_console_min_delay * 100);
                rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
                return rc;
        }

        /* write */
        min_delay_cs = 0;
        rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
        if (rc < 0)
                return rc;
        if (min_delay_cs <= 0)
                return -EINVAL;

        d = cfs_time_seconds(min_delay_cs) / 100;
        if (d == 0 || d > libcfs_console_max_delay)
                return -EINVAL;
        libcfs_console_min_delay = d;

        return rc;
}

int LL_PROC_PROTO(proc_console_backoff)
{
        int rc, backoff;
        cfs_sysctl_table_t dummy = *table;

        dummy.data = &backoff;
        dummy.proc_handler = &proc_dointvec;

        if (!write) { /* read */
                backoff= libcfs_console_backoff;
                rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
                return rc;
        }

        /* write */
        backoff = 0;
        rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);
        if (rc < 0)
                return rc;
        if (backoff <= 0)
                return -EINVAL;

        libcfs_console_backoff = backoff;

        return rc;
}

static cfs_sysctl_table_t lnet_table[] = {
        /*
         * NB No .strategy entries have been provided since sysctl(8) prefers
         * to go via /proc for portability.
         */
        {
                .ctl_name = PSDEV_DEBUG,
                .procname = "debug",
                .data     = &libcfs_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks,
        },
        {
                .ctl_name = PSDEV_SUBSYSTEM_DEBUG,
                .procname = "subsystem_debug",
                .data     = &libcfs_subsystem_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks,
        },
        {
                .ctl_name = PSDEV_PRINTK,
                .procname = "printk",
                .data     = &libcfs_printk,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks,
        },
        {
                .ctl_name = PSDEV_CONSOLE_RATELIMIT,
                .procname = "console_ratelimit",
                .data     = &libcfs_console_ratelimit,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_CONSOLE_MAX_DELAY_CS,
                .procname = "console_max_delay_centisecs",
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_console_max_delay_cs
        },
        {
                .ctl_name = PSDEV_CONSOLE_MIN_DELAY_CS,
                .procname = "console_min_delay_centisecs",
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_console_min_delay_cs
        },
        {
                .ctl_name = PSDEV_CONSOLE_BACKOFF,
                .procname = "console_backoff",
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_console_backoff
        },
        {
                .ctl_name = PSDEV_DEBUG_PATH,
                .procname = "debug_path",
                .data     = debug_file_path_arr,
                .maxlen   = sizeof(debug_file_path_arr),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
        },
        {
                .ctl_name = PSDEV_LNET_UPCALL,
                .procname = "upcall",
                .data     = lnet_upcall,
                .maxlen   = sizeof(lnet_upcall),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
        },
        {
                .ctl_name = PSDEV_LNET_DEBUG_LOG_UPCALL,
                .procname = "debug_log_upcall",
                .data     = lnet_debug_log_upcall,
                .maxlen   = sizeof(lnet_debug_log_upcall),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
        },
        {
                .ctl_name = PSDEV_LNET_MEMUSED,
                .procname = "memused",
                .data     = (int *)&libcfs_kmemory.counter,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec,
                .strategy = &sysctl_intvec,
        },
        {
                .ctl_name = PSDEV_LNET_CATASTROPHE,
                .procname = "catastrophe",
                .data     = &libcfs_catastrophe,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec,
                .strategy = &sysctl_intvec,
        },
        {
                .ctl_name = PSDEV_LNET_PANIC_ON_LBUG,
                .procname = "panic_on_lbug",
                .data     = &libcfs_panic_on_lbug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec,
                .strategy = &sysctl_intvec,
        },
        {
                .ctl_name = PSDEV_LNET_DUMP_KERNEL,
                .procname = "dump_kernel",
                .maxlen   = 256,
                .mode     = 0200,
                .proc_handler = &proc_dump_kernel,
        },
        {
                .ctl_name = PSDEV_LNET_DAEMON_FILE,
                .procname = "daemon_file",
                .mode     = 0644,
                .maxlen   = 256,
                .proc_handler = &proc_daemon_file,
        },
        {
                .ctl_name = PSDEV_LNET_DEBUG_MB,
                .procname = "debug_mb",
                .mode     = 0644,
                .proc_handler = &proc_debug_mb,
        },
        {0}
};

static cfs_sysctl_table_t top_table[] = {
        {
                .ctl_name = CTL_LNET,
                .procname = "lnet",
                .mode     = 0555,
                .data     = NULL,
                .maxlen   = 0,
                .child    = lnet_table,
        },
        {
                .ctl_name = 0
        }
};

int insert_proc(void)
{
#ifdef CONFIG_SYSCTL
	printk("call register\n");
        if (lnet_table_header == NULL)
                lnet_table_header = cfs_register_sysctl_table(top_table, 0);
#endif
        return 0;
}

void remove_proc(void)
{
#ifdef CONFIG_SYSCTL
        if (lnet_table_header != NULL)
                cfs_unregister_sysctl_table(lnet_table_header);

        lnet_table_header = NULL;
#endif
}

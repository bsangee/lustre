/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/cmm/mdc_object.c
 *  Lustre Cluster Metadata Manager (cmm)
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Mike Pershin <tappro@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_MDS
#include <obd_support.h>
#include <lustre_lib.h>
#include <obd_class.h>
#include <lustre_mdc.h>
#include "cmm_internal.h"
#include "mdc_internal.h"

static struct md_object_operations mdc_mo_ops;
static struct md_dir_operations mdc_dir_ops;
static struct lu_object_operations mdc_obj_ops;

extern struct lu_context_key mdc_thread_key;

struct lu_object *mdc_object_alloc(const struct lu_context *ctx,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *ld)
{
	struct mdc_object *mco;
        ENTRY;

	OBD_ALLOC_PTR(mco);
	if (mco != NULL) {
		struct lu_object *lo;

		lo = &mco->mco_obj.mo_lu;
                lu_object_init(lo, NULL, ld);
                mco->mco_obj.mo_ops = &mdc_mo_ops;
                mco->mco_obj.mo_dir_ops = &mdc_dir_ops;
                lo->lo_ops = &mdc_obj_ops;
                RETURN(lo);
	} else
		RETURN(NULL);
}

static void mdc_object_free(const struct lu_context *ctx, struct lu_object *lo)
{
        struct mdc_object *mco = lu2mdc_obj(lo);
	lu_object_fini(lo);
        OBD_FREE_PTR(mco);
}

static int mdc_object_init(const struct lu_context *ctx, struct lu_object *lo)
{
        ENTRY;
        lo->lo_header->loh_attr |= LOHA_REMOTE;
        RETURN(0);
}

static int mdc_object_print(const struct lu_context *ctx, void *cookie,
                            lu_printer_t p, const struct lu_object *lo)
{
	return (*p)(ctx, cookie, LUSTRE_CMM_MDC_NAME"-object@%p", lo);
}

static struct lu_object_operations mdc_obj_ops = {
        .loo_object_init    = mdc_object_init,
        .loo_object_free    = mdc_object_free,
	.loo_object_print   = mdc_object_print,
};

/* md_object_operations */
static  
struct mdc_thread_info *mdc_info_get(const struct lu_context *ctx)
{
        struct mdc_thread_info *mci;

        mci = lu_context_key_get(ctx, &mdc_thread_key);
        LASSERT(mci);
        return mci;
}

static 
struct mdc_thread_info *mdc_info_init(const struct lu_context *ctx)
{
        struct mdc_thread_info *mci;

        mci = mdc_info_get(ctx);

        memset(mci, 0, sizeof(*mci));

        return mci;
}

static void mdc_body2attr(struct mdt_body *body, struct md_attr *ma)
{
        struct lu_attr *la = &ma->ma_attr;
        /* update time */
        if (body->valid & OBD_MD_FLCTIME && body->ctime >= la->la_ctime) {
                la->la_ctime = body->ctime;
                if (body->valid & OBD_MD_FLMTIME)
                        la->la_mtime = body->mtime;
        }

        if (body->valid & OBD_MD_FLMODE)
                la->la_mode = body->mode;
        if (body->valid & OBD_MD_FLSIZE)
                la->la_size = body->size;
        if (body->valid & OBD_MD_FLBLOCKS)
                la->la_blocks = body->blocks;
        if (body->valid & OBD_MD_FLUID)
                la->la_uid = body->uid;
        if (body->valid & OBD_MD_FLGID)
                la->la_gid = body->gid;
        if (body->valid & OBD_MD_FLFLAGS)
                la->la_flags = body->flags;
        if (body->valid & OBD_MD_FLNLINK)
                la->la_nlink = body->nlink;
        if (body->valid & OBD_MD_FLRDEV)
                la->la_rdev = body->rdev;

        ma->ma_valid = MA_INODE;
}

static void mdc_req2attr_update(const struct lu_context *ctx,
                                struct md_attr *ma)
{
        struct mdc_thread_info *mci;
        struct ptlrpc_request *req;
        struct mdt_body *body;
        
        mci = mdc_info_get(ctx);
        req = mci->mci_req;
        LASSERT(req);
        body = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF, sizeof(*body));
        LASSERT(body);
        mdc_body2attr(body, ma);
}

static int mdc_attr_get(const struct lu_context *ctx, struct md_object *mo,
                        struct md_attr *ma)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_key_get(ctx, &mdc_thread_key);
        LASSERT(mci);

        memset(&mci->mci_opdata, 0, sizeof(mci->mci_opdata));

        rc = md_getattr(mc->mc_desc.cl_exp, lu_object_fid(&mo->mo_lu),
                        OBD_MD_FLMODE | OBD_MD_FLUID | OBD_MD_FLGID,
                        0, &mci->mci_req);

        if (rc == 0) {
                /* get attr from request */
                mdc_req2attr_update(ctx, ma);
        }

        ptlrpc_req_finished(mci->mci_req);

        RETURN(rc);
}


static int mdc_object_create(const struct lu_context *ctx,
                             struct md_object *mo, 
                             const struct md_create_spec *spec,
                             struct md_attr *ma)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo));
        struct lu_attr *la = &ma->ma_attr;
        struct mdc_thread_info *mci;
        const void *symname;
        int rc, symlen;
        ENTRY;

        LASSERT(spec->u.sp_pfid != NULL);
        mci = mdc_info_init(ctx);
        mci->mci_opdata.fid2 = *lu_object_fid(&mo->mo_lu);
        /* parent fid is needed to create dotdot on the remote node */
        mci->mci_opdata.fid1 = *(spec->u.sp_pfid);
        mci->mci_opdata.mod_time = la->la_mtime;

        /* get data from spec */
        if (spec->sp_cr_flags & MDS_CREATE_SLAVE_OBJ) {
                symname = spec->u.sp_ea.eadata;
                symlen = spec->u.sp_ea.eadatalen;
                mci->mci_opdata.fid1 = *(spec->u.sp_ea.fid);
                mci->mci_opdata.flags |= MDS_CREATE_SLAVE_OBJ;
        } else {
                symname = spec->u.sp_symname;
                symlen = symname ? strlen(symname) + 1 : 0;
        }
        
        rc = md_create(mc->mc_desc.cl_exp, &mci->mci_opdata,
                       symname, symlen,
                       la->la_mode, la->la_uid, la->la_gid, 0, la->la_rdev,
                       &mci->mci_req);

        if (rc == 0) {
                /* get attr from request */
                mdc_req2attr_update(ctx, ma);
        }

        ptlrpc_req_finished(mci->mci_req);

        RETURN(rc);
}

static int mdc_ref_add(const struct lu_context *ctx, struct md_object *mo)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_key_get(ctx, &mdc_thread_key);
        LASSERT(mci);

        memset(&mci->mci_opdata, 0, sizeof(mci->mci_opdata));
        mci->mci_opdata.fid1 = *lu_object_fid(&mo->mo_lu);
        //mci->mci_opdata.mod_time = la->la_ctime;
        //mci->mci_opdata.fsuid = la->la_uid;
        //mci->mci_opdata.fsgid = la->la_gid;

        rc = md_link(mc->mc_desc.cl_exp, &mci->mci_opdata, &mci->mci_req);

        ptlrpc_req_finished(mci->mci_req);

        RETURN(rc);
}

static int mdc_ref_del(const struct lu_context *ctx, struct md_object *mo,
                       struct md_attr *ma)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo));
        struct lu_attr *la = &ma->ma_attr;
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = mdc_info_init(ctx);
        mci->mci_opdata.fid1 = *lu_object_fid(&mo->mo_lu);
        mci->mci_opdata.create_mode = la->la_mode;
        mci->mci_opdata.mod_time = la->la_ctime;
        mci->mci_opdata.fsuid = la->la_uid;
        mci->mci_opdata.fsgid = la->la_gid;
        rc = md_unlink(mc->mc_desc.cl_exp, &mci->mci_opdata, &mci->mci_req);
        if (rc == 0) {
                /* get attr from request */
                mdc_req2attr_update(ctx, ma);
        }

        ptlrpc_req_finished(mci->mci_req);

        RETURN(rc);
}

#ifdef HAVE_SPLIT_SUPPORT
int mdc_send_page(struct cmm_device *cm, const struct lu_context *ctx,
                  struct md_object *mo, struct page *page, __u32 end)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo));
        struct lu_dirpage *dp;
        struct lu_dirent  *ent;
        int rc, offset = 0, rc1 = 0;
        ENTRY;

        kmap(page);
        dp = page_address(page);

        ent = lu_dirent_start(dp);
        if (ent->lde_hash > end)
                RETURN(-E2BIG);

        for (ent = lu_dirent_start(dp); ent != NULL;
             ent = lu_dirent_next(ent)) {
                if (ent->lde_hash > end) {
                        offset = (int)((__u32)ent - (__u32)dp);
                        rc1 = -E2BIG;
                        goto send_page;
                }

                /* allocate new fid for each obj */
                rc = obd_fid_alloc(mc->mc_desc.cl_exp, &ent->lde_fid, NULL);
                if (rc > 0) {
                        struct lu_site *ls;

                        ls = cm->cmm_md_dev.md_lu_dev.ld_site;
                        rc = fld_client_create(ls->ls_client_fld,
                                               fid_seq(&ent->lde_fid),
                                               mc->mc_num, ctx);
                }

                if (rc < 0) {
                        kunmap(page);
                        RETURN(rc);
                }
        }
        kunmap(page);
        offset = CFS_PAGE_SIZE;
send_page:
        if (offset > 0) {
                rc = mdc_sendpage(mc->mc_desc.cl_exp, lu_object_fid(&mo->mo_lu),
                                  page, offset);
                CDEBUG(D_INFO, "send page %p  offset %d fid "DFID" rc %d \n",
                                page, offset, PFID(lu_object_fid(&mo->mo_lu)),
                                rc);
        }
        if (rc == 0)
                rc = rc1;
        RETURN(rc);
}
#endif

static struct md_object_operations mdc_mo_ops = {
        .moo_attr_get       = mdc_attr_get,
        .moo_object_create  = mdc_object_create,
        .moo_ref_add        = mdc_ref_add,
        .moo_ref_del        = mdc_ref_del,
};

/* md_dir_operations */
static int mdc_rename_tgt(const struct lu_context *ctx,
                          struct md_object *mo_p, struct md_object *mo_t,
                          const struct lu_fid *lf, const char *name,
                          struct md_attr *ma)
{
        struct mdc_device *mc = md2mdc_dev(md_obj2dev(mo_p));
        struct lu_attr *la = &ma->ma_attr;
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = mdc_info_init(ctx);
        mci->mci_opdata.fid1 = *lu_object_fid(&mo_p->mo_lu);
        mci->mci_opdata.fid2 = *lf;
        mci->mci_opdata.mod_time = la->la_ctime;
        mci->mci_opdata.fsuid = la->la_uid;
        mci->mci_opdata.fsgid = la->la_gid;

        rc = md_rename(mc->mc_desc.cl_exp, &mci->mci_opdata, NULL, 0,
                       name, strlen(name), &mci->mci_req);

        ptlrpc_req_finished(mci->mci_req);

        RETURN(rc);
}

static struct md_dir_operations mdc_dir_ops = {
        .mdo_rename_tgt  = mdc_rename_tgt,
};


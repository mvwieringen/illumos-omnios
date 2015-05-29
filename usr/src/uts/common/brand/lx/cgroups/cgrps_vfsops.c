/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2015 Joyent, Inc.
 */

/*
 * The cgroup file system implements a subset of the Linux cgroup functionality
 * for use by lx-branded zones. Cgroups are a generic process grouping
 * mechanism which is used to apply various behaviors to the processes within
 * the group, although it's primary purpose is for resource management.
 *
 * This file system is similar to tmpfs in that directories only exist in
 * memory. Each subdirectory represents a different cgroup. Within the cgroup
 * there are pseudo files with well-defined names which control the
 * configuration and behavior of the cgroup. The primary file within a cgroup
 * is named 'tasks' and it is used to list which processes belong to the
 * cgroup. However, there can be additional files in the cgroup which define
 * additional behavior.
 *
 * Linux defines a mounted instance of cgroups as a hierarchy:
 *
 * 1) A set of cgroups arranged in a tree, such that every task in the system
 *    is in exactly one of the cgroups in the hierarchy.
 * 2) A set of subsystems; each subsystem has system-specific state attached to
 *    each cgroup in the hierarchy.
 * 3) Each hierarchy has an instance of the cgroup virtual filesystem
 *    associated with it.
 *
 * For example, it is common to see cgroup mounts for systemd, cpuset, memory,
 * etc. Each of these mounts would be used for a different subsystem. Within
 * each mount there is at least one tasks file listing the processes within
 * that group although there could be subdirectories which define new cgroups
 * that contain a subset of the processes.
 *
 * An overview of the behavior for the various vnode operations is:
 * - no hardlinks or symlinks
 * - no file create (the subsystem-specific files are a fixed list of
 *   pseudo-files accessible within the directory)
 * - no file remove
 * - no file rename, but a directory (i.e. a cgroup) can be renamed within the
 *   containing directory, but not into a different directory
 * - can mkdir and rmdir to create/destroy cgroups
 * - cannot rmdir while it contains a subdir (i.e. a sub-cgroup)
 * - open, read/write, close on the subsytem-specific pseudo files is
 *   allowed as this is the interface to configure and report on the cgroup.
 *   The pseudo file's mode controls write access and cannot be changed.
 *
 * When adding support for a new subsystem, be sure to also update the
 * lxpr_read_cgroups function in lx_procfs so that the subsystem is reported
 * by proc.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/time.h>
#include <sys/pathname.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/cred.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/mntent.h>
#include <sys/policy.h>

#include "cgrps.h"

/* Module level parameters */
static int	cgrp_fstype;
static dev_t	cgrp_dev;

/*
 * cgrp_mountcount is used to prevent module unloads while there is still
 * state from a former mount hanging around. The filesystem module must not be
 * allowed to go away before the last VFS_FREEVFS() call has been made. Since
 * this is just an atomic counter, there's no need for locking.
 */
static uint32_t cgrp_mountcount;

/*
 * cgrp_minfree is the minimum amount of swap space that cgroups leaves for
 * the rest of the zone. In other words, if the amount of free swap space
 * in the zone drops below cgrp_minfree, cgroup anon allocations will fail.
 * This number is only likely to become factor when DRAM and swap have both
 * been capped low to allow for maximum tenancy.
 */
size_t cgrp_minfree = 0;

/*
 * CGMINFREE -- the value from which cgrp_minfree is derived -- should be
 * configured to a value that is roughly the smallest practical value for
 * memory + swap minus the largest reasonable size for cgroups in such
 * a configuration. As of this writing, the smallest practical memory + swap
 * configuration is 128MB, and it seems reasonable to allow cgroups to consume
 * no more than half of this, yielding a CGMINFREE of 64MB.
 */
#define	CGMINFREE	64 * 1024 * 1024	/* 64 Megabytes */

extern pgcnt_t swapfs_minfree;

/*
 * cgroup vfs operations.
 */
static int cgrp_init(int, char *);
static int cgrp_mount(struct vfs *, struct vnode *,
	struct mounta *, struct cred *);
static int cgrp_unmount(struct vfs *, int, struct cred *);
static int cgrp_root(struct vfs *, struct vnode **);
static int cgrp_statvfs(struct vfs *, struct statvfs64 *);
static void cgrp_freevfs(vfs_t *vfsp);

/*
 * Loadable module wrapper
 */
#include <sys/modctl.h>

static vfsdef_t vfw = {
	VFSDEF_VERSION,
	"lx_cgroup",
	cgrp_init,
	VSW_ZMOUNT,
	NULL
};

/*
 * Module linkage information
 */
static struct modlfs modlfs = {
	&mod_fsops, "lx brand cgroups", &vfw
};

static struct modlinkage modlinkage = {
	MODREV_1, &modlfs, NULL
};

int
_init()
{
	return (mod_install(&modlinkage));
}

int
_fini()
{
	int error;

	if (cgrp_mountcount)
		return (EBUSY);

	if ((error = mod_remove(&modlinkage)) != 0)
		return (error);

	/*
	 * Tear down the operations vectors
	 */
	(void) vfs_freevfsops_by_type(cgrp_fstype);
	vn_freevnodeops(cgrp_vnodeops);
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*
 * Initialize global locks, etc. Called when loading cgroup module.
 */
static int
cgrp_init(int fstype, char *name)
{
	static const fs_operation_def_t cgrp_vfsops_template[] = {
		VFSNAME_MOUNT,		{ .vfs_mount = cgrp_mount },
		VFSNAME_UNMOUNT,	{ .vfs_unmount = cgrp_unmount },
		VFSNAME_ROOT,		{ .vfs_root = cgrp_root },
		VFSNAME_STATVFS,	{ .vfs_statvfs = cgrp_statvfs },
		VFSNAME_FREEVFS,	{ .vfs_freevfs = cgrp_freevfs },
		NULL,			NULL
	};
	extern const struct fs_operation_def cgrp_vnodeops_template[];
	int error;
	extern  void    cgrp_hash_init();
	major_t dev;

	cgrp_hash_init();
	cgrp_fstype = fstype;
	ASSERT(cgrp_fstype != 0);

	error = vfs_setfsops(fstype, cgrp_vfsops_template, NULL);
	if (error != 0) {
		cmn_err(CE_WARN, "cgrp_init: bad vfs ops template");
		return (error);
	}

	error = vn_make_ops(name, cgrp_vnodeops_template, &cgrp_vnodeops);
	if (error != 0) {
		(void) vfs_freevfsops_by_type(fstype);
		cmn_err(CE_WARN, "cgrp_init: bad vnode ops template");
		return (error);
	}

	/*
	 * cgrp_minfree doesn't need to be some function of configured
	 * swap space since it really is an absolute limit of swap space
	 * which still allows other processes to execute.
	 */
	if (cgrp_minfree == 0) {
		/* Set if not patched */
		cgrp_minfree = btopr(CGMINFREE);
	}

	if ((dev = getudev()) == (major_t)-1) {
		cmn_err(CE_WARN, "cgrp_init: Can't get unique device number.");
		dev = 0;
	}

	/*
	 * Make the pseudo device
	 */
	cgrp_dev = makedevice(dev, 0);

	return (0);
}

static int
cgrp_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, cred_t *cr)
{
	cgrp_mnt_t *cgm = NULL;
	struct cgrp_node *cp;
	struct pathname dpn;
	int error;
	struct vattr rattr;
	char *argstr;
	cgrp_ssid_t ssid = CG_SSID_GENERIC;

	if ((error = secpolicy_fs_mount(cr, mvp, vfsp)) != 0)
		return (error);

	if (mvp->v_type != VDIR)
		return (ENOTDIR);

	/*
	 * Ensure we don't allow overlaying mounts
	 */
	mutex_enter(&mvp->v_lock);
	if ((uap->flags & MS_OVERLAY) == 0 &&
	    (mvp->v_count > 1 || (mvp->v_flag & VROOT))) {
		mutex_exit(&mvp->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mvp->v_lock);

	/*
	 * Having the resource be anything but "swap" doesn't make sense.
	 */
	vfs_setresource(vfsp, "swap", 0);

	/* cgroups don't support read-only mounts */
	if (vfs_optionisset(vfsp, MNTOPT_RO, NULL)) {
		error = EINVAL;
		goto out;
	}

	/*
	 * If provided set the subsystem.
	 * XXX These subsystems are temporary placeholders to stub out the
	 * concept of different cgroup subsystem mounts.
	 */
	if (vfs_optionisset(vfsp, "cpuset", &argstr)) {
		if (ssid != CG_SSID_GENERIC) {
			error = EINVAL;
			goto out;
		}
		ssid = CG_SSID_CPUSET;
	}
	if (vfs_optionisset(vfsp, "memory", &argstr)) {
		if (ssid != CG_SSID_GENERIC) {
			error = EINVAL;
			goto out;
		}
		ssid = CG_SSID_MEMORY;
	}

	error = pn_get(uap->dir,
	    (uap->flags & MS_SYSSPACE) ? UIO_SYSSPACE : UIO_USERSPACE, &dpn);
	if (error != 0)
		goto out;

	cgm = kmem_zalloc(sizeof (*cgm), KM_SLEEP);

	/* Set but don't bother entering the mutex (not on mount list yet) */
	mutex_init(&cgm->cg_contents, NULL, MUTEX_DEFAULT, NULL);

	cgm->cg_vfsp = vfsp;
	cgm->cg_ssid = ssid;
	cgm->cg_gen++;		/* start at 1 */

	vfsp->vfs_data = (caddr_t)cgm;
	vfsp->vfs_fstype = cgrp_fstype;
	vfsp->vfs_dev = cgrp_dev;
	vfsp->vfs_bsize = PAGESIZE;
	vfsp->vfs_flag |= VFS_NOTRUNC;
	vfs_make_fsid(&vfsp->vfs_fsid, cgrp_dev, cgrp_fstype);
	cgm->cg_mntpath = kmem_zalloc(dpn.pn_pathlen + 1, KM_SLEEP);
	(void) strcpy(cgm->cg_mntpath, dpn.pn_path);

	/* allocate and initialize root cgrp_node structure */
	bzero(&rattr, sizeof (struct vattr));
	rattr.va_mode = (mode_t)(S_IFDIR | 0755);
	rattr.va_type = VDIR;
	rattr.va_rdev = 0;
	cp = kmem_zalloc(sizeof (struct cgrp_node), KM_SLEEP);
	cgrp_node_init(cgm, cp, &rattr, cr);

	rw_enter(&cp->cgn_rwlock, RW_WRITER);
	CGNTOV(cp)->v_flag |= VROOT;

	/*
	 * initialize linked list of cgrp_nodes so that the back pointer of
	 * the root cgrp_node always points to the last one on the list
	 * and the forward pointer of the last node is null
	 */
	cp->cgn_back = cp;
	cp->cgn_forw = NULL;
	cp->cgn_nlink = 0;
	cgm->cg_rootnode = cp;

	cp->cgn_type = CG_CGROUP_DIR;
	cp->cgn_nodeid = cgrp_inode(ssid, cgm->cg_gen);
	cgrp_dirinit(cp, cp, cr);

	rw_exit(&cp->cgn_rwlock);

	pn_free(&dpn);
	error = 0;
	atomic_inc_32(&cgrp_mountcount);

out:
	if (error == 0)
		vfs_set_feature(vfsp, VFSFT_SYSATTR_VIEWS);

	return (error);
}

static int
cgrp_unmount(struct vfs *vfsp, int flag, struct cred *cr)
{
	cgrp_mnt_t *cgm = (cgrp_mnt_t *)VFSTOCGM(vfsp);
	cgrp_node_t *cgnp, *cancel;
	struct vnode	*vp;
	int error;
	uint_t cnt;

	if ((error = secpolicy_fs_unmount(cr, vfsp)) != 0)
		return (error);

	mutex_enter(&cgm->cg_contents);

	/*
	 * In the normal unmount case, if there are no
	 * open files, only the root node should have a reference count.
	 *
	 * With cg_contents held, nothing can be added or removed.
	 * There may be some dirty pages.  To prevent fsflush from
	 * disrupting the unmount, put a hold on each node while scanning.
	 * If we find a previously referenced node, undo the holds we have
	 * placed and fail EBUSY.
	 */
	cgnp = cgm->cg_rootnode;

	vp = CGNTOV(cgnp);
	mutex_enter(&vp->v_lock);

	if (flag & MS_FORCE) {
		mutex_exit(&vp->v_lock);
		mutex_exit(&cgm->cg_contents);
		return (EINVAL);
	}

	cnt = vp->v_count;
	if (cnt > 1) {
		mutex_exit(&vp->v_lock);
		mutex_exit(&cgm->cg_contents);
		return (EBUSY);
	}

	mutex_exit(&vp->v_lock);

	/*
	 * Check for open files. An open file causes everything to unwind.
	 */
	for (cgnp = cgnp->cgn_forw; cgnp; cgnp = cgnp->cgn_forw) {
		vp = CGNTOV(cgnp);
		mutex_enter(&vp->v_lock);
		cnt = vp->v_count;
		if (cnt > 0) {
			/* An open file; unwind the holds we've been adding. */
			mutex_exit(&vp->v_lock);
			cancel = cgm->cg_rootnode->cgn_forw;
			while (cancel != cgnp) {
				vp = CGNTOV(cancel);
				ASSERT(vp->v_count > 0);
				VN_RELE(vp);
				cancel = cancel->cgn_forw;
			}
			mutex_exit(&cgm->cg_contents);
			return (EBUSY);
		} else {
			/* directly add a VN_HOLD since we have the lock */
			vp->v_count++;
			mutex_exit(&vp->v_lock);
		}
	}

	/*
	 * We can drop the mutex now because
	 * no one can find this mount anymore
	 */
	vfsp->vfs_flag |= VFS_UNMOUNTED;
	mutex_exit(&cgm->cg_contents);

	return (0);
}

/*
 * Implementation of VFS_FREEVFS(). This is called by the vfs framework after
 * umount and the last VFS_RELE, to trigger the release of any resources still
 * associated with the given vfs_t. This is normally called immediately after
 * cgrp_umount.
 */
void
cgrp_freevfs(vfs_t *vfsp)
{
	cgrp_mnt_t *cgm = (cgrp_mnt_t *)VFSTOCGM(vfsp);
	cgrp_node_t *cn;
	struct vnode	*vp;

	/*
	 * Free all kmemalloc'd and anonalloc'd memory associated with
	 * this filesystem.  To do this, we go through the file list twice,
	 * once to remove all the directory entries, and then to remove
	 * all the pseudo files.
	 */

	/*
	 * Now that we are tearing ourselves down we need to remove the
	 * UNMOUNTED flag. If we don't, we'll later hit a VN_RELE when we remove
	 * files from the system causing us to have a negative value. Doing this
	 * seems a bit better than trying to set a flag on the tmount that says
	 * we're tearing down.
	 */
	vfsp->vfs_flag &= ~VFS_UNMOUNTED;

	/*
	 * Remove all directory entries
	 */
	for (cn = cgm->cg_rootnode; cn; cn = cn->cgn_forw) {
		rw_enter(&cn->cgn_rwlock, RW_WRITER);
		if (cn->cgn_type == CG_CGROUP_DIR)
			cgrp_dirtrunc(cn);
		rw_exit(&cn->cgn_rwlock);
	}

	ASSERT(cgm->cg_rootnode);

	/*
	 * All links are gone, v_count is keeping nodes in place.
	 * VN_RELE should make the node disappear, unless somebody
	 * is holding pages against it.  Nap and retry until it disappears.
	 *
	 * We re-acquire the lock to prevent others who have a HOLD on
	 * a cgrp_node via its pages or anon slots from blowing it away
	 * (in cgrp_inactive) while we're trying to get to it here. Once
	 * we have a HOLD on it we know it'll stick around.
	 *
	 */
	mutex_enter(&cgm->cg_contents);

	/* Remove all the files (except the rootnode) backwards. */
	while ((cn = cgm->cg_rootnode->cgn_back) != cgm->cg_rootnode) {
		mutex_exit(&cgm->cg_contents);
		/*
		 * All nodes will be released here. Note we handled the link
		 * count above.
		 */
		vp = CGNTOV(cn);
		VN_RELE(vp);
		mutex_enter(&cgm->cg_contents);
		/*
		 * It's still there after the RELE. Someone else like pageout
		 * has a hold on it so wait a bit and then try again - we know
		 * they'll give it up soon.
		 */
		if (cn == cgm->cg_rootnode->cgn_back) {
			VN_HOLD(vp);
			mutex_exit(&cgm->cg_contents);
			delay(hz / 4);
			mutex_enter(&cgm->cg_contents);
		}
	}
	mutex_exit(&cgm->cg_contents);

	VN_RELE(CGNTOV(cgm->cg_rootnode));

	ASSERT(cgm->cg_mntpath);

	kmem_free(cgm->cg_mntpath, strlen(cgm->cg_mntpath) + 1);

	mutex_destroy(&cgm->cg_contents);
	mutex_destroy(&cgm->cg_renamelck);
	kmem_free(cgm, sizeof (cgrp_mnt_t));

	/* Allow _fini() to succeed now */
	atomic_dec_32(&cgrp_mountcount);
}

/*
 * return root cgnode for given vnode
 */
static int
cgrp_root(struct vfs *vfsp, struct vnode **vpp)
{
	cgrp_mnt_t *cgm = (cgrp_mnt_t *)VFSTOCGM(vfsp);
	cgrp_node_t *cp = cgm->cg_rootnode;
	struct vnode *vp;

	ASSERT(cp);

	vp = CGNTOV(cp);
	VN_HOLD(vp);
	*vpp = vp;
	return (0);
}

static int
cgrp_statvfs(struct vfs *vfsp, struct statvfs64 *sbp)
{
	cgrp_mnt_t *cgm = (cgrp_mnt_t *)VFSTOCGM(vfsp);
	ulong_t	blocks;
	dev32_t d32;
	zoneid_t eff_zid;
	struct zone *zp;

	zp = cgm->cg_vfsp->vfs_zone;

	if (zp == NULL)
		eff_zid = GLOBAL_ZONEUNIQID;
	else
		eff_zid = zp->zone_id;

	sbp->f_bsize = PAGESIZE;
	sbp->f_frsize = PAGESIZE;

	/*
	 * Find the amount of available physical and memory swap
	 */
	mutex_enter(&anoninfo_lock);
	ASSERT(k_anoninfo.ani_max >= k_anoninfo.ani_phys_resv);
	blocks = (ulong_t)CURRENT_TOTAL_AVAILABLE_SWAP;
	mutex_exit(&anoninfo_lock);

	if (blocks > cgrp_minfree)
		sbp->f_bfree = blocks - cgrp_minfree;
	else
		sbp->f_bfree = 0;

	sbp->f_bavail = sbp->f_bfree;

	/*
	 * Total number of blocks is just what's available
	 */
	sbp->f_blocks = (fsblkcnt64_t)(sbp->f_bfree);

	if (eff_zid != GLOBAL_ZONEUNIQID &&
	    zp->zone_max_swap_ctl != UINT64_MAX) {
		/*
		 * If the fs is used by a zone with a swap cap,
		 * then report the capped size.
		 */
		rctl_qty_t cap, used;
		pgcnt_t pgcap, pgused;

		mutex_enter(&zp->zone_mem_lock);
		cap = zp->zone_max_swap_ctl;
		used = zp->zone_max_swap;
		mutex_exit(&zp->zone_mem_lock);

		pgcap = btop(cap);
		pgused = btop(used);

		sbp->f_bfree = MIN(pgcap - pgused, sbp->f_bfree);
		sbp->f_bavail = sbp->f_bfree;
		sbp->f_blocks = MIN(pgcap, sbp->f_blocks);
	}

	/*
	 * The maximum number of files available is approximately the number
	 * of cgrp_nodes we can allocate from the remaining kernel memory
	 * available to cgroups.  This is fairly inaccurate since it doesn't
	 * take into account the names stored in the directory entries.
	 */
	sbp->f_ffree = sbp->f_files = ptob(availrmem) /
	    (sizeof (cgrp_node_t) + sizeof (cgrp_dirent_t));
	sbp->f_favail = (fsfilcnt64_t)(sbp->f_ffree);
	(void) cmpldev(&d32, vfsp->vfs_dev);
	sbp->f_fsid = d32;
	(void) strcpy(sbp->f_basetype, vfssw[cgrp_fstype].vsw_name);
	(void) strncpy(sbp->f_fstr, cgm->cg_mntpath, sizeof (sbp->f_fstr));
	/* ensure null termination */
	sbp->f_fstr[sizeof (sbp->f_fstr) - 1] = '\0';
	sbp->f_flag = vf_to_stf(vfsp->vfs_flag);
	sbp->f_namemax = MAXNAMELEN - 1;
	return (0);
}

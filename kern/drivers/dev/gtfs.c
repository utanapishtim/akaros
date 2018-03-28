/* Copyright (c) 2018 Google Inc
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * #gtfs, generic tree file system frontend that hooks to a backend 9p device
 */

#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <pmap.h>
#include <smp.h>
#include <tree_file.h>

struct dev gtfs_devtab;

static char *devname(void)
{
	return gtfs_devtab.name;
}

struct gtfs {
	struct tree_filesystem		tfs;
	struct kref					users;
};

// XXX
static struct gtfs *recent;
/* Blob hanging off the fs_file->priv.  The backend chans are only accessed,
 * (changed or used) with the corresponding fs_file qlock held.
 *
 * The walk chan is never opened - it's basically just the walked fid, from
 * which we can do other walks or get the I/O chans.  The read and write chans
 * are opened on demand and closed on LRU.  That means once all the frontend
 * chans are closed, the TF becomes LRU, then we close the backend I/O chans -
 * somewhat similar to how #mnt operates on a normal walk-open-read-close.  Also
 * note that several frontend gtfs chans are muxed by the same TF and gtfs_priv
 * onto the set of backend chans.
 *
 * XXX - clarify/pick one (LRU callback or pruning) and fix the preceding
 * paragraph
 *
 * TODO: we might need to close be_read and be_write on occasion.  For instance,
 * the 9p server might complain about "too many open".  We have a couple
 * options:
 * - Get an LRU callback (as mentioned above)
 * - Trigger LRU pruning.  That should cause the closes. */
struct gtfs_priv {
	struct chan					*be_walk;	/* never opened */
	struct chan					*be_read;
	struct chan					*be_write;
	bool						was_removed;
};

/* Helper.  Clones the chan (walks to itself) and then opens with omode. */
static struct chan *cclone_and_open(struct chan *c, int omode)
{
	ERRSTACK(1);
	struct chan *new;

	new = cclone(c);
	if (waserror()) {
		cclose(new);
		nexterror();
	}
	new = devtab[new->type].open(new, omode);
	poperror();
	return new;
}

static inline struct gtfs_priv *fsf_to_gtfs_priv(struct fs_file *f)
{
	return f->priv;
}

static inline struct gtfs_priv *tf_to_gtfs_priv(struct tree_file *tf)
{
	return fsf_to_gtfs_priv(&tf->file);
}

static void gtfs_destroy(struct tree_filesystem *tfs)
{
	struct gtfs *gtfs = (struct gtfs*)tfs;

// XXX
I_AM_HERE;
	assert(kref_refcnt(&gtfs->users) == 0);
	kfree(gtfs);
}

struct tree_filesystem_ops gtfs_tfs_ops = {
	.destroy = gtfs_destroy,
};

static void gtfs_release(struct kref *kref)
{
	struct gtfs *gtfs = container_of(kref, struct gtfs, users);

	// XXX  we're going to need this to work.  but also sync in the CB for
	// anything e.g. written (file data.  metadata (wstat) was probably
	// write-through, so we can e.g. update permissions and get the benefit of
	// those perm changes)
I_AM_HERE;
	//tfs_purge_frontend(&tmpfs->tfs, purge_cb);
}

static struct gtfs *chan_to_gtfs(struct chan *c)
{
	struct tree_file *tf = chan_to_tree_file(c);

	return (struct gtfs*)(tf->tfs);
}

static void incref_gtfs_chan(struct chan *c)
{
	kref_get(&chan_to_gtfs(c)->users, 1);
}

static void decref_gtfs_chan(struct chan *c)
{
	kref_put(&chan_to_gtfs(c)->users);
}

static struct walkqid *gtfs_walk(struct chan *c, struct chan *nc, char **name,
                                 unsigned int nname)
{
	struct walkqid *wq;

	wq = tree_chan_walk(c, nc, name, nname);
	if (wq && wq->clone && (wq->clone != c))
		incref_gtfs_chan(wq->clone);
	return wq;
}

static void gtfs_close(struct chan *c)
{
	tree_chan_close(c);
	decref_gtfs_chan(c);
}

static void gtfs_remove(struct chan *c)
{
	ERRSTACK(1);
	struct gtfs *gtfs = chan_to_gtfs(c);

	if (waserror()) {
		/* Same old pain-in-the-ass for remove */
		kref_put(&gtfs->users);
		nexterror();
	}
	tree_chan_remove(c);
	kref_put(&gtfs->users);
	poperror();
}

/* Caller holds the file's qlock. */
// XXX might not need the helper
static size_t __gtfs_fsf_read(struct fs_file *f, void *ubuf, size_t n,
                              off64_t off)
{
	struct gtfs_priv *gp = fsf_to_gtfs_priv(f);

	if (!gp->be_read)
		gp->be_read = cclone_and_open(gp->be_walk, O_READ);
	return devtab[gp->be_read->type].read(gp->be_read, ubuf, n, off);
}

/* Reads a file from its backend chan */
static size_t gtfs_fsf_read(struct fs_file *f, void *ubuf, size_t n,
                            off64_t off)
{
	ERRSTACK(1);
	size_t ret;

	qlock(&f->qlock);
	if (waserror()) {
		qunlock(&f->qlock);
		nexterror();
	}
	ret = __gtfs_fsf_read(f, ubuf, n, off);
	qunlock(&f->qlock);
	poperror();
	return ret;
}

static size_t gtfs_read(struct chan *c, void *ubuf, size_t n, off64_t off)
{
	struct tree_file *tf = chan_to_tree_file(c);

	if (!tree_file_is_dir(tf))
		return fs_file_read(&tf->file, ubuf, n, off);
	return gtfs_fsf_read(&tf->file, ubuf, n, off);
}

/* Given a file (with dir->name set), couple it and sync to the backend chan.
 * This will store/consume the ref for backend, in the TF (freed with
 * gtfs_tf_free), even on error, unless you zero out the be_walk field. */
static void gtfs_tf_couple_backend(struct tree_file *tf, struct chan *backend)
{
	struct dir *dir;
	struct gtfs_priv *gp = kzmalloc(sizeof(struct gtfs_priv), MEM_WAIT);

	tf->file.priv = gp;
	tf->file.dir.qid = backend->qid;
	gp->be_walk = backend;
	dir = chandirstat(backend);
	if (!dir)
		error(ENOMEM, "chandirstat failed");
	fs_file_copy_from_dir(&tf->file, dir);
	kfree(dir);
}

static void gtfs_tf_free(struct tree_file *tf)
{
	struct gtfs_priv *gp = tf_to_gtfs_priv(tf);

	/* Might have some partially / never constructed tree files */
	if (!gp)
		return;
	if (gp->was_removed) {
		gp->be_walk->type = -1;
		/* sanity */
		assert(kref_refcnt(&gp->be_walk->ref) == 1);
	}
	cclose(gp->be_walk);
	/* I/O chans can be NULL */
	cclose(gp->be_read);
	cclose(gp->be_write);
	kfree(gp);
}

static void gtfs_tf_unlink(struct tree_file *parent, struct tree_file *child)
{
	struct gtfs_priv *gp = tf_to_gtfs_priv(child);
	struct chan *be_walk = gp->be_walk;

	/* Remove clunks the be_walk chan/fid.  if it succeeded (and I think even if
	 * it didn't), we shouldn't close that fid again, which is what will happen
	 * soon after this function.  The TF code calls unlink, then when the last
	 * ref closes the TF, it'll get freed and we'll call back to gtfs_tf_free().
	 *
	 * This is the same issue we run into with all of the device remove ops
	 * where we want to refcnt something hanging off e.g. c->aux.  In 9p, you're
	 * not supposed to close a chan/fid that was already removed.
	 *
	 * Now here's the weird thing.  We can close the be_walk chan after remove,
	 * but it's possible that someone has walked and perhaps opened a frontend
	 * chan + TF, but hasn't done a read yet.  So someone might want to set up
	 * be_read, but they can't due to be_walk being closed.  We could give them
	 * a 'phase error' (one of 9p's errors for I/O on a removed file).
	 *
	 * Alternatively, we can mark the gtfs_priv so that when we do free it, we
	 * skip the dev.remove, similar to what sysremove() does.  That's probably
	 * easier.  This is technically racy, but we know that the release/free
	 * method won't be called until we return. */
	gp->was_removed = true;
	devtab[be_walk->type].remove(be_walk);
}

/* Caller sets the name, but doesn't know if it exists or not.  It's our job to
 * find out if it exists and fill in the child stucture appropriately.  For
 * negative entries, just flagging it is fine.  Otherwise, we fill in the dir.
 * We should throw on error. */
static void gtfs_tf_lookup(struct tree_file *parent, struct tree_file *child)
{
	struct walkqid *wq;
	struct chan *be_walk = tf_to_gtfs_priv(parent)->be_walk;
	struct chan *child_be_walk;

	wq = devtab[be_walk->type].walk(be_walk, NULL, &child->file.dir.name, 1);
	if (!wq || !wq->clone) {
		kfree(wq);
		/* This isn't racy, since the child isn't linked to the tree yet */
		child->flags |= TF_F_NEGATIVE | TF_F_HAS_BEEN_USED;
		return;
	}
	/* walk shouldn't give us the same chan struct since we gave it a name and a
	 * NULL nc. */
	assert(wq->clone != be_walk);
	/* only gave it one name, and it didn't fail. */
	assert(wq->nqid == 1);
	/* sanity */
	assert(wq->clone->qid.path == wq->qid[wq->nqid - 1].path);
	child_be_walk = wq->clone;
	kfree(wq);
	gtfs_tf_couple_backend(child, child_be_walk);
}

static void gtfs_tf_create(struct tree_file *parent, struct tree_file *child,
                           int perm)
{
	ERRSTACK(1);
	struct chan *c = cclone(tf_to_gtfs_priv(parent)->be_walk);

	if (waserror()) {
		cclose(c);
		nexterror();
	}
	devtab[c->type].create(c, tree_file_to_name(child), 0, perm,
	                       child->file.dir.ext);
	/* The chan c is opened, which we don't want.  We can't cclone it either
	 * (since it is opened).  All we can do is have the parent walk again so we
	 * can get the child's unopened be_walk chan.  Conveniently, that's
	 * basically a lookup, so create is really two things: make it, then look it
	 * up from the backend. */
	cclose(c);
	poperror();
	if (waserror()) {
		warn("File %s was created in the backend, but unable to look it up!",
		     tree_file_to_name(child));
		nexterror();
	}
	gtfs_tf_lookup(parent, child);
	poperror();
}

static void gtfs_tf_rename(struct tree_file *tf, struct tree_file *old_parent,
                           struct tree_file *new_parent, const char *name,
                           int flags)
{
	struct chan *tf_c = tf_to_gtfs_priv(tf)->be_walk;
	struct chan *np_c = tf_to_gtfs_priv(new_parent)->be_walk;

	if (!devtab[tf_c->type].rename)
		error(EXDEV, "%s: %s doesn't support rename", devname(),
		      devtab[tf_c->type].name);
	devtab[tf_c->type].rename(tf_c, np_c, name, flags);
}

static bool gtfs_tf_has_children(struct tree_file *parent)
{
	struct dir dir[1];

	assert(!tree_file_is_dir(parent));	/* TF bug */
	/* any read should work, but there might be issues asking for something
	 * smaller than a dir. */
	return gtfs_fsf_read(&parent->file, dir, sizeof(struct dir), 0) > 0;
}

struct tree_file_ops gtfs_tf_ops = {
	.free = gtfs_tf_free,
	.unlink = gtfs_tf_unlink,
	.lookup = gtfs_tf_lookup,
	.create = gtfs_tf_create,
	.rename = gtfs_tf_rename,
	.has_children = gtfs_tf_has_children,
};

/* Fills page with its contents from its backing store file.
 *
 * Note the page/offset might be beyond the current file length, based on the
 * current pagemap code.
 *
 * Also note we hold the fs_file's qlock. */
static int gtfs_pm_readpage(struct page_map *pm, struct page *pg)
{
	ERRSTACK(1);
	void *kva = page2kva(pg);
	off64_t offset = pg->pg_index << PGSHIFT;
	size_t ret;

	if (waserror()) {
		poperror();
		return -get_errno();
	}
	/* If offset is beyond the length of the file, the 9p device/server should
	 * return 0.  That's fine - we'll just init an empty page.  The length on
	 * the frontend (in the fsf->dir.length) will be adjusted.  The backend will
	 * hear about it on the next sync. */
	ret = gtfs_fsf_read(pm->pm_file, kva, PGSIZE, offset);
	poperror();
	if (ret < PGSIZE)
		memset(kva + ret, 0, PGSIZE - ret);
	atomic_or(&pg->pg_flags, PG_UPTODATE);
	return 0;
}

/* Meant to take the page from PM and flush to backing store. */
static int gtfs_pm_writepage(struct page_map *pm, struct page *pg)
{
	// XXX
	// 	can make this work, but it'll be hard to test, since we need to kick the
	// 	pruner or other WB (like pm destroy, fs sync, etc)
	//
	// 	can have a helper for __gtfs_tf_write, page at a time, etc
	return 0;
}

// XXX we're probably going to what an 'int which', to separate metadata from
// data.  though there might be some ordering issues (like changing len before
// or after data is added)
// 		the metadata is easier, can have a helper for that maybe
//
static void gtfs_fs_sync_file(struct fs_file *f)
{
	// XXX
}

static void gtfs_fs_punch_hole(struct fs_file *f, off64_t begin, off64_t end)
{
	// XXX
	// the TF code already attempted the PM op.  this is just whatever we need
	// to do to tell the backend that we don't have shit anymore
	// 		probably need to sync the begin and end page, if not aligned.  then
	// 		write zeros for the intermediate.
	// 			might be able to skip this if we know the backend has nothing
	// 			there yet.  like len is < end or something
}

static bool gtfs_fs_can_grow_to(struct fs_file *f, size_t len)
{
	// XXX - what are the limits of 'seek' in 9p? (or write's offset)
	return true;
}

struct fs_file_ops gtfs_fs_ops = {
	.readpage = gtfs_pm_readpage,
	.writepage = gtfs_pm_writepage,
	.sync = gtfs_fs_sync_file,
	.punch_hole = gtfs_fs_punch_hole,
	.can_grow_to = gtfs_fs_can_grow_to,
};

/* We're passed a backend chan, usually of type #mnt, used for an uncached
 * mount.  We call it 'backend.'  It is the result of an attach, e.g. mntattach.
 * In the case of #mnt, this chan is different than the one that has the 9p
 * server on the other side, called 'mchan'.  That chan is at backend->mchan,
 * and also the struct mnt->c.  The struct mnt is shared by all mounts talking
 * to the 9p server over the mchan, and is stored at mchan->mux.  Backend chans
 * have a strong (counted) ref on the mchan.
 *
 * We create and return a chan of #gtfs, suitable for attaching to the
 * namespace.  This chan will have the root TF hanging off aux, just like how
 * any other attached TFS has a root TF.  #gtfs manages the linkage between a TF
 * and the backend, which is the purpose of gtfs_priv.
 *
 * A note on refcounts: in the normal, uncached operation, the 'backend' chan
 * has a ref (actually a chan kref, which you cclose) on the comms chan (mchan).
 * We get one ref at mntattach time, and every distinct mntwalk gets another
 * ref.  Those actually get closed in chanfree(), since they are stored at
 * mchan.
 *
 * All gtfs *tree_files* have at least one refcounted chan corresponding to the
 * file/FID on the backend server.  Think of it as a 1:1 connection, even though
 * there is more than one chan.  The gtfs device can have many chans pointing to
 * the same TF, which is kreffed.  That TF is 1:1 on a backend object.
 *
 * All walks from this attach point will get chans with TFs from this TFS and
 * will incref the struct gtfs.
 */
static struct chan *gtfs_attach(char *arg)
{
	ERRSTACK(2);
	struct chan *backend = (struct chan*)arg;
	struct chan *frontend;
	struct tree_filesystem *tfs;
	struct gtfs *gtfs;

	frontend = devattach(devname(), 0);
	if (waserror()) {
		/* same as #mnt - don't cclose, since we don't want to devtab close, and
		 * we know the ref == 1 here. */
		chanfree(frontend);
		nexterror();
	}
	gtfs = kzmalloc(sizeof(struct gtfs), MEM_WAIT);
// XXX
recent = gtfs;
	kref_init(&gtfs->users, gtfs_release, 1);
	tfs = (struct tree_filesystem*)gtfs;
	/* This gives us one ref on root, stored below.  name is set to "." */
	tfs_init(tfs);
	if (waserror()) {
		/* don't consume the backend ref on error, caller expects to have it */
		tf_to_gtfs_priv(tfs->root)->be_walk = NULL;
		gtfs_tf_free(tfs->root);
		tfs_destroy(tfs);
		nexterror();
	}
	/* stores the ref for backend inside tfs->root */
	gtfs_tf_couple_backend(tfs->root, backend);
	poperror();
	tfs->tfs_ops = gtfs_tfs_ops;
	tfs->tf_ops = gtfs_tf_ops;
	tfs->fs_ops = gtfs_fs_ops;
	/* storing the ref from tfs_init */
	chan_set_tree_file(frontend, tfs->root);
	poperror();
	return frontend;
}

struct dev gtfs_devtab __devtab = {
	.name = "gtfs",

	.reset = devreset,
	.init = devinit,
	.shutdown = devshutdown,
	.attach = gtfs_attach,
	.walk = gtfs_walk,
	.stat = tree_chan_stat,
	.open = tree_chan_open,
	.create = tree_chan_create,
	.close = gtfs_close,
	.read = gtfs_read,
	.bread = devbread,
	.write = tree_chan_write,
	.bwrite = devbwrite,
	.remove = gtfs_remove,
	.wstat = tree_chan_wstat,
	.power = devpower,
	.chaninfo = devchaninfo,
	.mmap = tree_chan_mmap,
};

// XXX
static bool cb(struct tree_file *tf)
{
	printk("LRU TF %s, ref %d\n", tree_file_to_name(tf), kref_refcnt(&tf->kref));
	return true;
}

int xme(int op)
{
	if (!recent)
		return -1;
	switch (op) {
	case 1:
		printk("dumping GTFS (Qidpath, Ref)\n-----------------\n");
		__tfs_dump(&recent->tfs);
		break;
	case 2:
		tfs_lru_prune_neg(&recent->tfs);
		break;
	case 3:
		tfs_lru_for_each(&recent->tfs, cb, -1);
		break;
	case 4:
		break;
	default:
		printk("give me an op\n");
		return -1;
	}
	return 0;
}

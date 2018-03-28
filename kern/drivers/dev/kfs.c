/* Copyright (c) 2018 Google Inc
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * #kfs, in-memory ram filesystem, pulling from the kernel's embedded CPIO
 */

#include <ns.h>
#include <kmalloc.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <tree_file.h>
#include <pmap.h>
#include <cpio.h>

struct dev kfs_devtab;

struct kfs {
	struct tree_filesystem		tfs;
	atomic_t					qid;
} kfs;

static uint64_t kfs_get_qid_path(void)
{
	return atomic_fetch_and_add(&kfs.qid, 1);
}

static char *devname(void)
{
	return kfs_devtab.name;
}

static void kfs_tfs_destroy(struct tree_filesystem *tfs)
{
	panic("Should never destroy KFS!");
}

struct tree_filesystem_ops kfs_tfs_ops = {
	.destroy = kfs_tfs_destroy,
};

static void kfs_tf_free(struct tree_file *tf)
{
	/* We have nothing special hanging off the TF */
}

static void kfs_tf_unlink(struct tree_file *parent, struct tree_file *child)
{
	/* This is the "+1 for existing" ref. */
	tf_kref_put(child);
}

static void __kfs_tf_init(struct tree_file *tf, int dir_type, int dir_dev,
                          struct username *user, int perm)
{
	struct dir *dir = &tf->file.dir;

	fs_file_init_dir(&tf->file, dir_type, dir_dev, user, perm);
	dir->qid.path = kfs_get_qid_path();
	dir->qid.vers = 0;
	/* This is the "+1 for existing" ref.  There is no backing store for the FS,
	 * such as a disk or 9p, so we can't get rid of a file until it is unlinked
	 * and decreffed.  Note that KFS doesn't use pruners or anything else. */
	__kref_get(&tf->kref, 1);
}

/* Note: If your TFS doesn't support symlinks, you need to error out */
static void kfs_tf_create(struct tree_file *parent, struct tree_file *child,
                          int perm)
{
	__kfs_tf_init(child, parent->file.dir.type, parent->file.dir.dev, &eve,
	              perm);
}

static void kfs_tf_rename(struct tree_file *tf, struct tree_file *old_parent,
                          struct tree_file *new_parent, const char *name,
                          int flags)
{
	/* We don't have a backend, so we don't need to do anything additional for
	 * rename. */
}

static bool kfs_tf_has_children(struct tree_file *parent)
{
	/* The tree_file parent list is complete and not merely a cache for a real
	 * backend. */
	return !list_empty(&parent->children);
}

struct tree_file_ops kfs_tf_ops = {
	.free = kfs_tf_free,
	.unlink = kfs_tf_unlink,
	.lookup = NULL,
	.create = kfs_tf_create,
	.rename = kfs_tf_rename,
	.has_children = kfs_tf_has_children,
};

/* Fills page with its contents from its backing store file.  For KFS, that
 * means we're creating or extending a file, and the contents are 0.  Note the
 * page/offset might be beyond the current file length, based on the current
 * pagemap code. */
static int kfs_pm_readpage(struct page_map *pm, struct page *pg)
{
	memset(page2kva(pg), 0, PGSIZE);
	atomic_or(&pg->pg_flags, PG_UPTODATE);
	/* Pretend that we blocked while filing this page.  This catches a lot of
	 * bugs.  It does slightly slow down the kernel, but it's only when filling
	 * the page cache, and considering we are using a RAMFS, you shouldn't
	 * measure things that actually rely on KFS's performance. */
	kthread_usleep(1);
	return 0;
}

/* Meant to take the page from PM and flush to backing store.  There is no
 * backing store. */
static int kfs_pm_writepage(struct page_map *pm, struct page *pg)
{
	return 0;
}

static void kfs_fs_sync_file(struct fs_file *f)
{
}

static void kfs_fs_punch_hole(struct fs_file *f, off64_t begin, off64_t end)
{
}

static bool kfs_fs_can_grow_to(struct fs_file *f, size_t len)
{
	/* TODO: implement some sort of memory limit */
	return true;
}

struct fs_file_ops kfs_fs_ops = {
	.readpage = kfs_pm_readpage,
	.writepage = kfs_pm_writepage,
	.sync = kfs_fs_sync_file,
	.punch_hole = kfs_fs_punch_hole,
	.can_grow_to = kfs_fs_can_grow_to,
};

/* Consumes root's chan, even on error. */
static struct chan *__add_kfs_dir(struct chan *root, char *path,
                                  struct cpio_bin_hdr *c_bhdr)
{
	ERRSTACK(1);
	struct chan *c;

	if (waserror()) {
		warn("failed to add %s", path);
		cclose(root);
		poperror();
		return NULL;
	}
	c = namec_from(root, path, Acreate, O_EXCL, DMDIR | c_bhdr->c_mode, NULL);
	poperror();
	return c;
}

static struct chan *__add_kfs_symlink(struct chan *root, char *path,
                                      struct cpio_bin_hdr *c_bhdr)
{
	ERRSTACK(1);
	struct chan *c;
	char target[c_bhdr->c_filesize + 1];

	if (waserror()) {
		warn("failed to add %s", path);
		cclose(root);
		poperror();
		return NULL;
	}
	strncpy(target, c_bhdr->c_filestart, c_bhdr->c_filesize);
	target[c_bhdr->c_filesize] = 0;
	c = namec_from(root, path, Acreate, O_EXCL,
	               DMSYMLINK | S_IRWXU | S_IRWXG | S_IRWXO, target);
	poperror();
	return c;
}

static struct chan *__add_kfs_file(struct chan *root, char *path,
                                   struct cpio_bin_hdr *c_bhdr)
{
	ERRSTACK(1);
	struct chan *c;
	off64_t offset = 0;
	size_t ret, amt = c_bhdr->c_filesize;
	void *buf = c_bhdr->c_filestart;

	if (waserror()) {
		warn("failed to add %s", path);
		cclose(root);
		poperror();
		return NULL;
	}
	c = namec_from(root, path, Acreate, O_EXCL | O_RDWR, c_bhdr->c_mode, NULL);
	poperror();
	if (waserror()) {
		warn("failed to modify %s", path);
		cclose(c);
		poperror();
		return NULL;
	}
	while (amt) {
		ret = devtab[c->type].write(c, buf + offset, amt, offset);
		amt -= ret;
		offset += ret;
	}
	poperror();
	return c;
}

static int add_kfs_entry(struct cpio_bin_hdr *c_bhdr, void *cb_arg)
{
	struct tree_file *root = cb_arg;
	char *path = c_bhdr->c_filename;
	struct chan *c;
	struct tree_file *tf;
	struct timespec ts;

	/* Root of the FS, already part of KFS */
	if (!strcmp(path, "."))
		return 0;
	c = tree_file_alloc_chan(root, &kfs_devtab, "#kfs");
	switch (c_bhdr->c_mode & CPIO_FILE_MASK) {
	case (CPIO_DIRECTORY):
		c = __add_kfs_dir(c, path, c_bhdr);
		break;
	case (CPIO_SYMLINK):
		c = __add_kfs_symlink(c, path, c_bhdr);
		break;
	case (CPIO_REG_FILE):
		c = __add_kfs_file(c, path, c_bhdr);
		break;
	default:
		cclose(c);
		printk("Unknown file type %d in the CPIO!",
		       c_bhdr->c_mode & CPIO_FILE_MASK);
		return -1;
	}
	if (!c)
		return -1;
	tf = chan_to_tree_file(c);
	ts.tv_sec = c_bhdr->c_mtime;
	ts.tv_nsec = 0;
	/* Lockless */
	__set_acmtime_to(&tf->file, FSF_ATIME | FSF_BTIME | FSF_CTIME | FSF_MTIME,
	                 &ts);
	/* TODO: consider UID/GID.  Right now, everything is owned by eve. */
	cclose(c);
	return 0;
}

static void kfs_extract_cpio(void)
{
	extern uint8_t _binary_obj_kern_initramfs_cpio_size[];
	extern uint8_t _binary_obj_kern_initramfs_cpio_start[];

	void *cpio_base = (void*)_binary_obj_kern_initramfs_cpio_start;
	size_t cpio_sz = (size_t)_binary_obj_kern_initramfs_cpio_size;

	parse_cpio_entries(cpio_base, cpio_sz, add_kfs_entry, kfs.tfs.root);
}

static void kfs_init(void)
{
	struct tree_filesystem *tfs = &kfs.tfs;

	/* This gives us one ref on tfs->root. */
	tfs_init(tfs);
	tfs->tfs_ops = kfs_tfs_ops;
	tfs->tf_ops = kfs_tf_ops;
	tfs->fs_ops = kfs_fs_ops;
	/* Note this gives us the "+1 for existing" ref on tfs->root. */
	__kfs_tf_init(tfs->root, &kfs_devtab - devtab, 0, &eve, DMDIR | 0777);
	/* Other devices might want to create things like kthreads that run the LRU
	 * pruner or PM sweeper. */
	kfs_extract_cpio();
	// XXX give the cpio back to the base arena!
	/* This has another kref.  Note that each attach gets a ref and each new
	 * process gets a ref. */
	kern_slash = tree_file_alloc_chan(kfs.tfs.root, &kfs_devtab, "/");
}

static struct chan *kfs_attach(char *spec)
{
	/* The root TF has a new kref for the attach chan */
	return tree_file_alloc_chan(kfs.tfs.root, &kfs_devtab, "#kfs");
}

struct dev kfs_devtab __devtab = {
	.name = "kfs",
	.reset = devreset,
	.init = kfs_init,
	.shutdown = devshutdown,
	.attach = kfs_attach,
	.walk = tree_chan_walk,
	.stat = tree_chan_stat,
	.open = tree_chan_open,
	.create = tree_chan_create,
	.close = tree_chan_close,
	.read = tree_chan_read,
	.bread = devbread,
	.write = tree_chan_write,
	.bwrite = devbwrite,
	.remove = tree_chan_remove,
	.rename = tree_chan_rename,
	.wstat = tree_chan_wstat,
	.power = devpower,
	.chaninfo = devchaninfo,
	.mmap = tree_chan_mmap,
};


// XXX misc TODO
// --------------------------------------------------
// bash doesn't give us errstr...
// e.g. 
// 		bash-4.3$ echo ffff  >> /prog/goo
// 		bash: /prog/goo: Operation not permitted
// 		bash-4.3$ ash
// 		/ $ echo ffff >> /prog/goo
// 		ash: can't create /prog/goo: devpermcheck(goo, 0644, 03102) failed
// 			that's a little weird.  it was already created...  could be an ash
// 			thing
// 		/ $ write_to /prog/goo fff
// 		Can't open path: Operation not permitted, devpermcheck(goo, 0644, 03)
// 		failed
//
// 		a little better.
// 		why are the perms fucked?  that was umask, and the owner is eve, but our
// 		username is nanwan or something.  maybe nothing.  but not eve.
// 		need umask 0002 or just 0, so we don't make a file 644 that we can't
// 		write
//
// bash when tabbing out cd, shows us all files, not just directories.
// 		not ash.  they do the readdir, then stat everything
// 		some difference with stat, they can't tell it's (not) a dir?
// 		not sure - bash does the readdir, but doesn't do the stat right away.
// 		the function it is in (rl_filename_completion_function) doesn't seem to
// 		care about directories vs files.  maybe it's not getting the right comp
// 		code?  bash does do a stat, but only after printing the name
// 		rmdir doesn't do it either.  also doesn't do it on busybox.
//
//
// our have_perm checks seem to occur without locks
// 		perm changes are atomic, and have the lock
// 		do all of our have_perm checks hold the qlock?  if so, we could use
// 		that if it turns out there is a race.  look into this.
//
//  our linux list.h could use some safety shit, like WRITE_ONCE.  update to the
//  most recent one, perhaps?
//
// hashing
// - consider storing the hash in the tf.  might only be done twice
// - might be harder to resize, esp with RCU readers.  might need a seq.
// - consider hashing on the parent QID too.
// - consider bucket locks
// - consider exclusivity checks on insert (or caller's responsibility)
//
// sync and limits: can do our best on the front end and tell them on sync/other
// op.  NFS does this
//
// tree_file_is_root
//	if we bind part way into a device into a namespace, we want to not climb up. 
//		does namec handle that?  it might, with its mount logic
//
// ns devtab func signature issue
// 		qbwrite and whatnot is ssize_t, and some cases can return -1.
// 			who calls that?
// 			how do we call devtab.bread (e.g. pipe)
// 			these funcs don't always throw
// 			ipbwrite just says it wrote it all.
// 		prob should review the functions like pipebread 
//
// 		convD2M is unsigned int
//
// 		netifbread/bwrite/read/write
//
// have waserror() check for irq/trap depth
//
//
// XXX bigger shit
//
// also, maybe use chan_ctl for debugging (print from this chan on down)
// 		- chan_ctl is basically setting chan flags, but no feedback on the chan
// 		flags itself (see fd_setfl).  and no device has a chan_ctl op
// 		basically you can turn on an external flag.  will need more.
// 			all you can change now is APPEND and NONBLOCK
// 		maybe related: some sort of chan op that returns an FD for a ctl FS
// 			imagine you want to get access to a control plane for a mounted
// 			device, such as a #gtfs.  you want to fuck with various settings.
//
// - also, when we sync a chan, is it just the file, or the entire file system?
// 		- if file, just that one.  if dir, then entire FS?  (or everything
// 		under).  if the latter, is it harder to use?  (userspace needs to know
// 		the mount point)
// 			syncfs(2) does the entire FS, not just a dir and children
// 		if we want to sync /dir, but it's a union mount, how do we sync all of
// 		them?  might need better support at the 9ns layer (meaning you sync a
// 		path, not a chan)
//
//	thinking:
//		- FSF_F_META_DIRTY
//			gets set on wstat,acmtime,len change, anything in the DIR
//				basically splitting out  FSF_DIRTY
//			need F_DIRTY, and set it on write mmaps, any type of write calls
//				- idea being if someone only read it, then we don't need to scan
//				the PM and look for dirty pages
//				- clear it, then WB everything.  not a snapshot btw, and could
//				get new shit
//
//			XXX - do we want to PM unmap from existing mmaps?
//				tempted to say 'fuck it'.  can even have a pruner that just
//				skips VMRs.
//					do we want to flush out memory that is mmapped and was
//					used?  (maybe it stops being used, etc)
//
//			so the only case for concurrent removal+users is PM pruner
//					which is remove_contig
//				- not LRU, since we can handle that at the TF level
//				- trunc will try, but can fail and zero
//				- all the others that have no users ever can just WB and
//				destroy
//				- if we do this for PM pruner, does that help LRU?
//					- prob not - the current CB doesn't care
//
//		lru_foreach
//			- mnt/gtfs will need an LRU walker, but not pruner, that closes
//			backend be_read & be_write chans, esp once we get e.g. 100 opens
//				ktask for this, sleeps maybe on rendez timeout, gets killed on
//				unmount, etc
//			- pruner can do it with a CB that writes out dirty files/pages
//				can always drop unused ones (neither meta nor file dirty)
//				if it was never used since cleaning, it's still clean for
//				removal
// 				- can imagine severity with the CB, like don't bother reaping a
// 				file that has no pages in its cache, since it's just the TF
// 				entry
//
// 	will need to come up with an expanded list of pm ops and how to use them for
// 	the following scenarios.  we're talking about PMs, meaning not directories,
// 	though some large op might involve directories and file metadata.
// 		we'll have TF ops that call the right PM helper
// 			overall:
// 				classify for:
// 					users (mmaps, reads, writes)
// 						if users, we might fail at something.
// 							fail to remove a page
// 							did a dirty sweep, but user might re-dirty
// 						users + WB is the harder case
// 					writeback of dirty pages
// 						for the ones that want to remove pages, if they are dirty
// 					whole file or not
// 					TF/FSF stays in TFS tree/cache or not
// 						we never remove the TF if there is a user
// 				most of these want to remove pages from the PM
// 				failure cases:
// 					can't remove a page. (pinning, concurrent pm_op)
// 					something got re-dirtied (if doing sweeps)
// 			truncate/hole_punch (remove, don't care or 0, just a range, users)
// 				users
// 					on failure, can zero
// 				no writeback
// 				part of file
// 				TF stays
// 				XXX DONE
// 			file destruction (removed, don't care, no users)
// 				no users
// 				no writeback
// 				entire file
// 				TF drops
// 				XXX DONE
// 			fs_file LRU pruning (dropping FSF from the cache, no users, but care)
// 				users
// 					none at the start, and can detect new users with TF flags
// 					failure: a user can abort the final TF dropping or any page
// 				writeback
// 				entire file
// 				TF drops (ideally)
// 					we'll need to know that the PM is cleaned and no one came in
// 				XXX NEXT
// 					question about whether or not we want to yank pages (maybe
// 					just clean one) from a VMR
// 			fs_file PM prune (memory pressure, maybe on the TF list)
// 				users
// 					on failure, can skip WB
// 				writeback dirties (or skip/fail dirties)
// 				entire file
// 				TF stays
// 			syncing file or tree to backend (sync /mnt)
// 				users
// 					failure is irrelevant, since we don't remove any pages
// 				writeback
// 				entire file
// 				TF stays
// 				NO REMOVAL FROM PM (the only one that doesn't do removal)
// 				this is a helper, want to hit the entire tree
// 					worried about traversal: DFS / postorder
// 					picture ext flushing thread - metadata/journal stuff, etc
// 					what about metadata? (before or after data)
// 						note all trees need to sync dirs too (meta only)
// 			flushing the entire FS (e.g. unmount)
// 				no users (ignoring the 'hey, blow it all away during use' case)
// 				writeback
// 				entire file
// 				TF drops
// 				helper, want to hit the entire tree
// 				
// 			read/write/mmap/munmap
// 				preventing removal of pages
// 					read/write might look similar to mmap (refcnt); just no VMR
// 				changing len (with qlock)
// 				dirtying the overall file and single pages
// 				munmap needs to propagate dirty info back to the PM
// 					currently done when mmap disconnects via pml walkers
//
// 		a real FS might need a bit more, though in theory any 9p dev should work
// 			thinking about metadata syncing and whatnot (30 sec ktask)
// 			also, a real FS might want to work with the page cache/TFS
//
// 	XXX for each scenario, come up with the rough alg and PM ops needed
// 		XXX which scenarios require the fs_file lock?
// 			e.g. the atomic swap on pm_removal
// 		pm_remove_or_zero
// 			for each page
// 				if in a VMR, just zero it
// 				else yank it
// 				done
//
// 	current problem: XXX HERE
// 		- i want to qlock during these pm ops
// 		- can't make the spinlock a qlock, since at least lookups happen with
// 		spinlock from mm
// 		- could make it a qlock for global writers with RCU readers, but then if
// 		we're doing a WB on pg 3, all readpages also block for e.g. pg 5.
// 			- basically global writer, but now with longer latencies
// 			- though it's not a huge deal, since readpage will block too
// 			- maybe just suck it up.
// 			- this means the CBs without qlock can't delete items.  careful!
// 		- maybe allow concurrent adds to the tree
// 			- given rcu read lookups, it's a small extra step
// 			- with just a spinlock sync on the depth
// 			- removal is a little harder then (grabbing refs like TFS code)
// 			- getting into radix vm land
// 			- could also just not remove the interior nodes.  how much memory is
// 			that anyways?  esp when we can destroy the PM eventually.
// 				- means the CBs can never remove items
//
//	currently
//		load_page, nowait
//			load gets a temp ref on a pg_slot, then puts a weak ref in the PTE
//			and that weak ref is used for mmaps etc
//		put_page
//		add_vmr
//		remove_vmr
//		remove_contig
//		destroy
//
// 	freeing file contents (PM work)
//
// 	memory pressure CBs
// 		LRU pruner
// 		PM pusher
//
//
// 	glibc uses the old convD2M and friends (also grep STATFIXLEN)
//
// 	RCU
//
// 	better mmap infrastructure
// 			get rid of file_or_chan once we sort this out.  right now, it has
// 			both chan and fs_file
//
// 	mmap notes
//
//		when we talk to 9ns, we want to handle things other than PMs.  like
//		arbitrary physical memory
//			optional callback for unmapping (i.e. when the device wants to
//			revoke the VMR connection, e.g. PM flusher)
//
//			instead of PM, maybe something a little higher
//				like the fs_file
//				or the PM itself points to those pages.  not quite a PM, in that
//				it doesn't allocate pages.  we just want to know where to point.
//
// 				tempted to have __vm_foc be an fs_file, though sometimes we need
// 				its absolute path (perf), which is a chan feature.
//
//		what's the connection from VMR to file and from PM back to VMR?
//			IIRC, PM has weak refs on the VMRs, VMRs have refs on file -> PM
//			VMRs have refs on files/chan: the mmap lasts beyond the FD closing
//				though it might not need to be the chan.  could be fs_file
//				depends on what the interface is - everything with chans and
//				whatnot, multiplexed through a devtab[c->type].mmap op.
//					9p mmap op? probably not
//						say you want to access a NIC on another machine
//						9p mnt - can you do that?  it'll fake it with a mmap on
//						the frontend, implemented with reads to the backend
//
//  fs_file is doing some nasty things with usernames.  everyone is eve,
//  basically, and there's no real accounting.
//  	could change e.g. dir->uid to refcnts on struct user
//  		refcnting is a bit nasty, want something like 'users never go away'
//  	also need to interpet 9p's usernames. 
//  		like lookup, given name, hook in
//  		need something for unknown users.  eve?  mount owner?
//  	also, sort out any other rules for the dir->strings.  e.g. ext can be 0
//
//	missing chown
//		that, and glibc set errno, but has an old errstr
//			bash-4.3$ mv /prog/file /prog/f2
//			mv: can't preserve ownership of '/prog/f2': Function not implemented, could not find name f2, dev root
//		
//
// 	tempted to make 'kref-something' a namec 'service'. 
// 		if chan->priv_kref, decref it.
// 		just like mchan and all the other stuff.
// 		all that saves us is the decref on close
// 			and it also means we don't need to special-case remove, since the
// 			type = -1 bypasses dev.close, but not chanfree, which decrefs
// 		and don't forget remove - that's an easy one to lose refs, since it
// 		never closes
// 			and probably a source of bugs in other devices
// 					yes - eventfd leaks.  can just keep removing and increffing
// 			can we change that?  prob issues with repeated closing and deadlock
//
//
// 	XXX VM shit
// 		can we move all the PG_ flags out of struct page?
// 			we have PG_REMOVAL and PM_REMOVAL.  ffs.
// 				PG_REMOVAL is used to communicate through the mem_walk
// 				callbacks
// 				PG_DIRTY is the response from the CB for a particular
// 				page too.  so it's bidirectional
// 			there's a giant sem in there too, for load_page
// 			can we have the radix point to something other than a page?
// 			like some on-demand struct that has all the flags
// 				we'll need a way for vmr_for_each to communicate back to
// 				us.
// 			do we want a pml walk?  slightly better than a
// 			foreach-pte_walk, since we don't have to go up and down.
// 			but the downside is we don't know the radix slot / PM info
// 			for a specific PTE.
// 				is there something we could pass that they can quickly
// 				find it? (rewalking the radix isn't 'quickly').  if so,
// 				we'd just do another PTE
//
// 				seems like we have two structures that are both radix
// 				trees: PMLs and pm_tree.  would be nice to merge.  can
// 				we walk them in sync?  or use the same one? 
// 					no to most, since a proc's KPT has many unrelated VMRs
//
// 				also, munmap is making a pass to mark not present
// 				anyways. (in regards to the for-each-pte-walk shit)
//
// 		maybe make all VMRs point to a "PM", even anon ones, instead of using
// 		the PTEs to track pages. 
// 			- then replace all of it with the radixvm trees
// 			- and this thing can track whatever paddrs we're pointing to
// 			- PTEs become weak refs, unlike the weird shit mm does now
// 			- fs files or pms?  (separate issues)
// 			- and to some extent, all of anon mem is really one giant PM, not N
// 			separate ones, and the VMRs are windows into that PM.
// 			- revisit locking the 'fs_file' and len check.  anon won't have len.
//
//
// side note: whenever we free pages, they stay in the slab layers, so it's hard
// to tell we're actually freeing them

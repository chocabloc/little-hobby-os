#include "vfs.h"
#include "assert.h"
#include "klog.h"
#include "kmalloc.h"
#include "lock.h"
#include "memutils.h"
#include "mm/pmm.h"
#include "proc/sched/sched.h"
#include "ramfs/ramfs.h"
#include "random.h"
#include "vector.h"

#define IS_TRAVERSABLE(x) ((x)->type == VFS_NODE_FOLDER || (x)->type == VFS_NODE_MOUNTPOINT)

// TODO: more granular locking
static lock_t vfs_lock;

// the root node
static vfs_tnode_t root;

static struct {
    vfs_fsinfo_t* head;
    vfs_fsinfo_t* tail;
} fslist;

static vfs_fsinfo_t* get_fs(char* name)
{
    for (vfs_fsinfo_t* i = fslist.head; i; i = i->next)
        if (strncmp(name, i->name, sizeof(i->name)) == 0)
            return i;

    klog_err("get_fs(): filesystem %s not found\n", name);
    return NULL;
}

static void dumpnodes_rec(vfs_tnode_t* from, int lvl)
{
    for (int i = 0; i < 7 + lvl; i++)
        klog_putchar(' ');
    klog_printf("%d: %s -> %x inode, (%d refs)\n", lvl, from->name, from->inode, from->inode->refcount);

    if (IS_TRAVERSABLE(from->inode))
        for (vfs_tnode_t* t = from->inode->child; t; t = t->sibling)
            dumpnodes_rec(t, lvl + 1);
}

static vfs_node_desc_t* handle_to_fd(vfs_handle_t handle)
{
    task_t* curr = sched_get_current();
    if (handle >= curr->openfiles.len || !(curr->openfiles.data[handle])) {
        klog_err("handle_to_fd(): invalid handle %d\n", handle);
        return NULL;
    }
    return curr->openfiles.data[handle];
}

void vfs_debug()
{
    klog_info("VFS nodes dump:\n");
    dumpnodes_rec(&root, 0);
}

// converts a path to a node, creates the node if required
#define NO_CREATE 0b0001
#define CREATE 0b0010
#define ERR_ON_EXIST 0b0100
static vfs_tnode_t* path_to_node(char* path, int64_t mode, vfs_node_type_t create_type)
{
    static char tmpbuff[VFS_MAX_PATH_LEN];
    vfs_tnode_t* curr = &root;

    // we only work with absolute paths
    if (path[0] != '/') {
        klog_err("path_to_node(): '%s' is not an absolute path\n", path);
        return NULL;
    }
    path++; // skip the leading slash

    size_t pathlen = strlen(path), curr_index;
    bool foundnode = true;
    for (curr_index = 0; curr_index < pathlen;) {
        // extract next token from the path
        size_t i;
        for (i = 0; curr_index + i < pathlen; i++) {
            if (path[curr_index + i] == '/')
                break;
            tmpbuff[i] = path[curr_index + i];
        }
        tmpbuff[i] = '\0';
        curr_index += i + 1;

        // search for token in children of current node
        bool foundnext = false;
        if (IS_TRAVERSABLE(curr->inode)) {
            for (vfs_tnode_t* t = curr->inode->child; t; t = t->sibling) {
                if (strncmp(t->name, tmpbuff, sizeof(t->name)) == 0) {
                    foundnext = true;
                    curr = t;
                    break;
                }
            }
        }

        // did not find the node, break
        // TODO: call refresh() and try again
        if (!foundnext) {
            foundnode = false;
            break;
        }
    }

    // should we create the node
    if (!foundnode) {
        if ((mode & CREATE) && (curr_index > pathlen)) {
            // create the new tnode
            vfs_tnode_t* new_tnode = curr->inode->fs->mknode(curr->inode, tmpbuff, create_type);

            // add to list of children
            if (curr->inode->child == NULL) {
                curr->inode->child = new_tnode;
                curr->inode->child->sibling = NULL;
                return new_tnode;
            }
            new_tnode->sibling = curr->inode->child->sibling;
            curr->inode->child->sibling = new_tnode;
            return new_tnode;

        }
        // CREATE was not specified and node didn't exist
        else {
            klog_err("path_to_node(): '%s' doesn't exist\n", path);
            return NULL;
        }
    }
    // the node should not have existed
    else if (mode & ERR_ON_EXIST) {
        klog_err("path_to_node(): '%s' already exists\n", path);
        return NULL;
    }

    // found the node, return it
    return curr;
}

void vfs_register_fs(vfs_fsinfo_t* fs)
{
    if (!fslist.head) {
        fslist.head = fs;
        fslist.tail = fs;
        fs->next = NULL;
        return;
    }
    fs->next = fslist.head->next;
    fslist.head->next = fs;
}

vfs_handle_t vfs_open(char* path, vfs_openmode_t mode)
{
    lock_wait(&vfs_lock);

    // find the node
    vfs_tnode_t* req = path_to_node(path, NO_CREATE, 0);
    if (!req)
        goto fail;
    req->inode->refcount++;

    // create node descriptor
    vfs_node_desc_t* nd = (vfs_node_desc_t*)kmalloc(sizeof(vfs_node_desc_t));
    nd->tnode = req;
    nd->inode = req->inode;
    nd->file_pos = 0;
    nd->mode = mode;

    // add to current task
    task_t* curr = sched_get_current();
    vector_push_back(&(curr->openfiles), nd);

    // return the handle
    lock_release(&vfs_lock);
    return (curr->openfiles.len - 1);
fail:
    lock_release(&vfs_lock);
    return -1;
}

int64_t vfs_close(vfs_handle_t handle)
{
    lock_wait(&vfs_lock);

    // get current task
    task_t* curr = sched_get_current();
    if (handle >= curr->openfiles.len || !(curr->openfiles.data[handle]))
        goto fail;

    // get the file descriptor and free it
    vfs_node_desc_t* fd = curr->openfiles.data[handle];
    fd->inode->refcount--;
    kmfree(fd);
    curr->openfiles.data[handle] = NULL;

    // success
    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

int64_t vfs_create(char* path, vfs_node_type_t type)
{
    int64_t status = 0;
    lock_wait(&vfs_lock);

    // try creating the node
    vfs_tnode_t* node = path_to_node(path, CREATE | ERR_ON_EXIST, type);
    if (!node)
        status = -1;

    lock_release(&vfs_lock);
    return status;
}

int64_t vfs_seek(vfs_handle_t handle, size_t pos)
{
    vfs_node_desc_t* fd = handle_to_fd(handle);
    if (!fd)
        return -1;

    // seek position is out of bounds and mode is read only
    if (pos >= fd->inode->size && fd->mode == VFS_MODE_READ) {
        klog_err("vfs_seek(): seek position out of bounds\n");
        return -1;
    }

    fd->file_pos = pos;
    return 0;
}

int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms)
{
    vfs_node_desc_t* fd = handle_to_fd(handle);
    if (!fd)
        return -1;

    // mode is read only
    if (fd->mode == VFS_MODE_READ) {
        klog_err("vfs_chmod(): file opened read-only\n");
        return -1;
    }

    // set new permissions and sync
    fd->inode->perms = newperms;
    fd->inode->fs->sync(fd->inode);
    return 0;
}

int64_t vfs_mount(char* device, char* path, char* fsname)
{
    lock_wait(&vfs_lock);

    // get the fs info
    vfs_fsinfo_t* fs = get_fs(fsname);
    if (!fs)
        goto fail;

    // get the block device if needed
    vfs_tnode_t* dev = NULL;
    if (!fs->istemp) {
        dev = path_to_node(device, NO_CREATE, 0);
        if (!dev)
            goto fail;
        if (dev->inode->type != VFS_NODE_BLOCK_DEVICE) {
            klog_err("vfs_mount(): %s is not a block device\n", device);
            goto fail;
        }
    }

    // get the node where it is to be mounted (should be an empty folder)
    vfs_tnode_t* at = path_to_node(path, NO_CREATE, 0);
    if (!at)
        goto fail;
    if (at->inode) {
        if (at->inode->type != VFS_NODE_FOLDER || at->inode->child) {
            klog_err("vfs_mount(): '%s' is not an empty folder\n", path);
            goto fail;
        }
        kmfree(at->inode);
    }

    // mount the fs
    at->inode = fs->mount(dev ? dev->inode : NULL);
    at->inode->refcount = 1;
    at->inode->mountpoint = at;

    klog_info("vfs_mount(): mounted %s at %s as %s\n", device ? device : "<no-device>", path, fsname);
    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

// TODO: bounds checking in read and write
int64_t vfs_read(vfs_handle_t handle, size_t len, void* buff)
{
    vfs_node_desc_t* fd = handle_to_fd(handle);
    if (!fd)
        return -1;

    lock_wait(&vfs_lock);
    int status = fd->inode->fs->read(fd->inode, fd->file_pos, len, buff);
    if (status == -1)
        klog_err("vfs_read(): read (%d:%d) failed\n", fd->file_pos, fd->file_pos + len);
    lock_release(&vfs_lock);

    return status;
}

int64_t vfs_write(vfs_handle_t handle, size_t len, const void* buff)
{
    vfs_node_desc_t* fd = handle_to_fd(handle);
    if (!fd)
        return -1;

    if (fd->mode == VFS_MODE_READ) {
        klog_err("vfs_write(): file is read only\n");
        return -1;
    }

    lock_wait(&vfs_lock);
    if (fd->file_pos + len > fd->inode->size) {
        fd->inode->size = fd->file_pos + len;
        fd->inode->fs->sync(fd->inode);
    }

    int status = fd->inode->fs->write(fd->inode, fd->file_pos, len, buff);
    if (status == -1)
        klog_err("vfs_write(): write (%d:%d) failed\n", fd->file_pos, fd->file_pos + len);
    lock_release(&vfs_lock);

    return status;
}

int64_t vfs_link(char* oldpath, char* newpath)
{
    lock_wait(&vfs_lock);
    vfs_tnode_t* old_tnode = path_to_node(oldpath, NO_CREATE, 0);
    if (!old_tnode)
        goto fail;

    vfs_tnode_t* new_tnode = path_to_node(newpath, CREATE | ERR_ON_EXIST, old_tnode->inode->type);
    if (!new_tnode)
        goto fail;

    if (new_tnode->inode->mountpoint != old_tnode->inode->mountpoint) {
        klog_err("vfs_link(): mountpoints do not match\n");
        new_tnode->inode->fs->setlink(new_tnode, NULL);
        goto fail;
    }

    // we dont want the new inode
    vfs_inode_t* new_inode = new_tnode->inode;
    new_inode->refcount = 0;

    // increase refcount of link target
    old_tnode->inode->refcount++;
    new_tnode->inode->fs->setlink(new_tnode, old_tnode->inode);

    // free the new inode
    kmfree(new_inode);

    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

int64_t vfs_unlink(char* path)
{
    lock_wait(&vfs_lock);
    vfs_tnode_t* tnode = path_to_node(path, NO_CREATE, 0);
    if (!tnode)
        goto fail;

    if (tnode->inode->child) {
        klog_err("vfs_unlink(): target not an empty folder\n");
        goto fail;
    }

    // decrease refcount of old inode
    vfs_inode_t* old_inode = tnode->inode;
    old_inode->refcount--;

    int64_t status = tnode->inode->fs->setlink(tnode, NULL);

    // free the old inode, if required
    if (old_inode->refcount == 0)
        kmfree(old_inode);

    // now remove the tnode from the parent
    // TODO: do this more elegantly
    vfs_inode_t* parent = tnode->parent;
    for (vfs_tnode_t *t = parent->child, *p = NULL; t; p = t, t = t->sibling) {
        if (t == tnode) {
            if (!p)
                parent->child = t->sibling;
            else
                p->sibling = t->sibling;
            kmfree(tnode);
            break;
        }
    }

    lock_release(&vfs_lock);
    return status;
fail:
    lock_release(&vfs_lock);
    return -1;
}

// TODO: implement vfs_getdents

void vfs_init()
{
    // register ramfs and mount it at root
    vfs_register_fs(&ramfs);
    vfs_mount(NULL, "/", "ramfs");

    klog_warn("vfs_init(): stub\n");
}

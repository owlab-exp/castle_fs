#include <linux/kernel.h>
#include <linux/capability.h>
#include <linux/blkdev.h>
#include <linux/kref.h>

#include "castle_public.h"
#include "castle.h"
#include "castle_events.h"
#include "castle_sysfs.h"
#include "castle_debug.h"
#include "castle_versions.h"
#include "castle_freespace.h"
#include "castle_da.h"
#include "castle_utils.h"
#include "castle_btree.h"

static wait_queue_head_t castle_sysfs_kobj_release_wq;
static struct kobject    double_arrays_kobj;
static struct kobject    filesystem_kobj;
struct castle_sysfs_versions {
    struct kobject kobj;
    struct list_head version_list;
};
static struct castle_sysfs_versions castle_sysfs_versions;

struct castle_sysfs_entry {
    struct attribute attr;
    ssize_t (*show) (struct kobject *kobj, struct attribute *attr, char *buf);
    ssize_t (*store)(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count);
    void *private;
};

struct castle_sysfs_version {
    c_ver_t version;
    char name[10];
    struct castle_sysfs_entry csys_entry;
    struct list_head list;
};

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
    #define fs_kobject  (&fs_subsys.kset.kobj)
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
    #define fs_kobject  (&fs_subsys.kobj)
#else /* KERNEL_VERSION(2,6,24+) */
    #define fs_kobject  (fs_kobj)
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
/* Helper function which mimicks newer sysfs interfaces */
#define kobject_tree_add(_kobj, _parent, _ktype, _fmt, _a...)                    \
({                                                                               \
    int _ret = 0;                                                                \
                                                                                 \
    (_kobj)->ktype = (_ktype);                                                   \
    (_kobj)->parent = (_parent);                                                 \
    _ret = kobject_set_name(_kobj, _fmt, ##_a);                                  \
    if(!_ret)                                                                    \
        _ret = kobject_register(_kobj);                                          \
    _ret;                                                                        \
})

#define kobject_remove(_kobj)                                                    \
    kobject_unregister(_kobj)

#else /* KERNEL_VERSION(2,6,24+) */

#define kobject_tree_add(_kobj, _parent, _ktype, _fmt, _a...)                    \
({                                                                               \
    int _ret;                                                                    \
                                                                                 \
    kobject_init(_kobj, _ktype);                                                 \
    _ret = kobject_add(_kobj, _parent, _fmt, ##_a);                              \
    _ret;                                                                        \
})

#define kobject_remove(_kobj)                                                    \
    kobject_del(_kobj)

#endif

static ssize_t versions_list_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct castle_sysfs_entry *csys_entry =
                container_of(attr, struct castle_sysfs_entry, attr);
    struct castle_sysfs_version *v =
                container_of(csys_entry, struct castle_sysfs_version, csys_entry);
    c_ver_t live_parent;
    c_byte_off_t size;
    ssize_t len;
    int leaf;
    int ret;
    c_da_t da_id;

    ret = castle_version_read(v->version, &da_id, NULL, &live_parent, &size, &leaf);
    if(ret == 0)
    {
        cv_nonatomic_stats_t stats;
        struct timeval creation_timestamp;

        stats = castle_version_consistent_stats_get(v->version);
        creation_timestamp = castle_version_creation_timestamp_get(v->version);
        len = sprintf(buf,
                "Id: 0x%x\n"
                "VertreeId: 0x%x\n"
                "ParentId: 0x%x\n"
                "LogicalSize: %llu\n"
                "IsLeaf: %d\n"
                "Keys: %ld\n"
                "Tombstones: %ld\n"
                "TombstoneDeletes: %ld\n"
                "VersionDeletes: %ld\n"
                "KeyReplaces: %ld\n"
                "CreationTimestamp: %ld.%.6ld\n",
                 v->version,
                 castle_version_da_id_get(v->version),
                 live_parent,
                 size,
                 leaf,
                 stats.keys,
                 stats.tombstones,
                 stats.tombstone_deletes,
                 stats.version_deletes,
                 stats.key_replaces,
                 creation_timestamp.tv_sec,
                 creation_timestamp.tv_usec);

        return len;
    }

    return sprintf(buf, "Could not read the version, err %d\n", ret);
}

static ssize_t versions_list_store(struct kobject *kobj,
                                   struct attribute *attr,
                                   const char *buf,
                                   size_t count)
{
    castle_printk(LOG_INFO, "Got write to volumes: %s\n", buf);
    return count;
}

static void castle_sysfs_versions_fini(void)
{
    struct castle_sysfs_version *v;
    struct list_head *l, *t;

    kobject_remove(&castle_sysfs_versions.kobj);
    list_for_each_safe(l, t, &castle_sysfs_versions.version_list)
    {
        list_del(l);
        v = list_entry(l, struct castle_sysfs_version, list);
        castle_free(v);
    }
}

int castle_sysfs_version_add(c_ver_t version)
{
    struct castle_sysfs_version *v;
    int ret;

    /* We've got 10 chars for the name, 'ver-%d'. This means
       version has to be less than 100000 */
    if(version >= 100000)
    {
        castle_printk(LOG_INFO, "ERROR: version number > 100000. Not adding to sysfs.\n");
        return -E2BIG;
    }
    v = castle_malloc(sizeof(struct castle_sysfs_version), GFP_KERNEL);
    if(!v) return -ENOMEM;

    v->version = version;
    sprintf(v->name, "%x", version);
    v->csys_entry.attr.name  = v->name;
    v->csys_entry.attr.mode  = S_IRUGO|S_IWUSR;
    v->csys_entry.attr.owner = THIS_MODULE;
    v->csys_entry.show  = versions_list_show;
    v->csys_entry.store = versions_list_store;

    ret = sysfs_create_file(&castle_sysfs_versions.kobj, &v->csys_entry.attr);
    if(ret)
    {
        castle_printk(LOG_WARN, "Warning: could not create a version file in sysfs.\n");
        castle_free(v);
    } else
    {
        /* Succeeded at adding the version, add it to the list, so that it gets cleaned up */
        INIT_LIST_HEAD(&v->list);
        list_add(&v->list, &castle_sysfs_versions.version_list);
    }

    return ret;
}

int castle_sysfs_version_del(c_ver_t version)
{
    struct castle_sysfs_version *v = NULL, *k;
    struct list_head *pos, *tmp;

    list_for_each_safe(pos, tmp, &castle_sysfs_versions.version_list)
    {
        k = list_entry(pos, struct castle_sysfs_version, list);
        if (k->version == version)
        {
            v = k;
            break;
        }
    }
    if (!v)
        return -1;

    sysfs_remove_file(&castle_sysfs_versions.kobj, &v->csys_entry.attr);

    return 0;
}

/* Double Array functions could race with DA deletion. */
static ssize_t double_array_number_show(struct kobject *kobj,
                                        struct attribute *attr,
                                        char *buf)
{
    return sprintf(buf, "%d\n", castle_da_count());
}

static ssize_t da_version_show(struct kobject *kobj,
                               struct attribute *attr,
                               char *buf)
{
    struct castle_double_array *da = container_of(kobj, struct castle_double_array, kobj);

    return sprintf(buf, "0x%x\n", da->root_version);
}

static ssize_t da_compacting_show(struct kobject *kobj,
                                  struct attribute *attr,
                                  char *buf)
{
    struct castle_double_array *da = container_of(kobj, struct castle_double_array, kobj);

    return sprintf(buf, "%u\n", castle_da_compacting(da));
}

static ssize_t da_size_show(struct kobject *kobj,
                            struct attribute *attr,
                            char *buf)
{
    struct castle_double_array *da = container_of(kobj, struct castle_double_array, kobj);
    int i;
    uint32_t size = 0;

    /* Get READ lock on DA, to make sure DA doesnt disappear while printing stats. */
    read_lock(&da->lock);

    for(i=0; i<=da->top_level; i++)
    {
        struct castle_component_tree *ct;
        struct list_head *lh;

        list_for_each(lh, &da->levels[i].trees)
        {
            ct = list_entry(lh, struct castle_component_tree, da_list);

            size += CHUNK(ct->tree_ext_free.ext_size) +
                    CHUNK(ct->data_ext_free.ext_size) +
                    CHUNK(ct->internal_ext_free.ext_size) +
                    ((ct->bloom_exists)?ct->bloom.num_chunks:0) +
                    atomic64_read(&ct->large_ext_chk_cnt);
        }
    }

    read_unlock(&da->lock);

    return sprintf(buf, "%u\n", size);
}
/**
 * Show statistics for a given Doubling Array.
 *
 * Format:
 * ------
 *
 * " One row for DA stats
 * <nr of trees in DA> <Height of DA>
 *
 * " One row for each level contains #trees and one entry for each tree in the level
 * <nr of trees in level> [<item count> <leaf node size> <internal node size> <tree depth> <size of the tree(in chunks)>] [] []
 */
static ssize_t da_tree_list_show(struct kobject *kobj,
                                 struct attribute *attr,
                                 char *buf)
{
    struct castle_double_array *da = container_of(kobj, struct castle_double_array, kobj);
    int i;
    int ret = 0;

    /* Get READ lock on DA, to make sure DA doesnt disappear while printing stats. */
    read_lock(&da->lock);

    /* Total number of trees. */
    sprintf(buf, "%u %u\n", da->nr_trees, da->top_level);

    for(i=0; i<=da->top_level; i++)
    {
        struct castle_component_tree *ct;
        struct list_head *lh;

        /* Number of trees in each level. */
        ret = snprintf(buf, PAGE_SIZE, "%s%u ", buf,
                       da->levels[i].nr_trees + da->levels[i].nr_compac_trees);
        /* Buffer is of size one PAGE. MAke sure we are not overflowing buffer. */
        if (ret >= PAGE_SIZE)
            goto err;

        list_for_each(lh, &da->levels[i].trees)
        {
            struct castle_btree_type *btree;

            ct = list_entry(lh, struct castle_component_tree, da_list);
            btree = castle_btree_type_get(ct->btree_type);
            ret = snprintf(buf, PAGE_SIZE,
                           "%s[%lu %u %u %u %u] ",
                           buf,
                           atomic64_read(&ct->item_count),       /* Item count*/
                           (uint32_t)btree->node_size(ct, 0),    /* Leaf node size */
                           ct->tree_depth > 1 ?                  /* Internal node size */
                               (uint32_t)btree->node_size(ct, 1) : 0,
                           (uint32_t)ct->tree_depth,             /* Depth of tree */
                           (uint32_t)
                           (CHUNK(ct->tree_ext_free.ext_size) +
                            CHUNK(ct->data_ext_free.ext_size) +
                            CHUNK(ct->internal_ext_free.ext_size) +
                            ((ct->bloom_exists)?ct->bloom.num_chunks:0) +
                            atomic64_read(&ct->large_ext_chk_cnt)));           /* Tree size */
            if (ret >= PAGE_SIZE)
                goto err;
        }
        ret = snprintf(buf, PAGE_SIZE, "%s\n", buf);
        if (ret >= PAGE_SIZE)
            goto err;
    }
    ret = 0;

err:
    read_unlock(&da->lock);

    if (ret) sprintf(buf + PAGE_SIZE - 20, "Overloaded...\n");

    return strlen(buf);
}

static ssize_t slaves_number_show(struct kobject *kobj,
                                  struct attribute *attr,
                                  char *buf)
{
    struct castle_slaves *slaves =
                container_of(kobj, struct castle_slaves, kobj);
    struct list_head *lh;
    int nr_slaves = 0;

    rcu_read_lock();
    list_for_each_rcu(lh, &slaves->slaves)
        nr_slaves++;
    rcu_read_unlock();

    return sprintf(buf, "%d\n", nr_slaves);
}

static ssize_t slave_uuid_show(struct kobject *kobj,
                               struct attribute *attr,
                               char *buf)
{
    struct castle_slave *slave = container_of(kobj, struct castle_slave, kobj);

    return sprintf(buf, "0x%x\n", slave->uuid);
}

static ssize_t slave_size_show(struct kobject *kobj,
                               struct attribute *attr,
                               char *buf)
{
    struct castle_slave *slave = container_of(kobj, struct castle_slave, kobj);
    struct castle_slave_superblock *sb;
    uint64_t size;

    if (!test_bit(CASTLE_SLAVE_GHOST_BIT, &slave->flags))
    {
        sb = castle_slave_superblock_get(slave);
        size = sb->pub.size;
        castle_slave_superblock_put(slave, 0);
        size *= C_BLK_SIZE;
    }
    else
        size = 0;

    return sprintf(buf, "%lld\n", size);
}

static ssize_t slave_used_show(struct kobject *kobj,
                               struct attribute *attr,
                               char *buf)
{
    struct castle_slave *slave = container_of(kobj, struct castle_slave, kobj);
    c_chk_cnt_t free_chunks;
    c_chk_cnt_t size_chunks;
    uint64_t used;

    if (!test_bit(CASTLE_SLAVE_GHOST_BIT, &slave->flags))
    {
        castle_freespace_summary_get(slave, &free_chunks, &size_chunks);

        used = (uint64_t)(size_chunks - free_chunks) * C_CHK_SIZE;
    }
    else
        used = 0;

    return sprintf(buf, "%llu\n", used);
}

static ssize_t slave_ssd_show(struct kobject *kobj,
                              struct attribute *attr,
                              char *buf)
{
    struct castle_slave *slave = container_of(kobj, struct castle_slave, kobj);
    struct castle_slave_superblock *sb;
    int ssd;

    if (!test_bit(CASTLE_SLAVE_GHOST_BIT, &slave->flags))
    {
        sb = castle_slave_superblock_get(slave);
        ssd = !!(sb->pub.flags & CASTLE_SLAVE_SSD);
        castle_slave_superblock_put(slave, 0);
    }
    else
        ssd = -1;

    return sprintf(buf, "%d\n", ssd);
}

/* Display rebuild state for this slave. See the castle_slave flags bits in castle.h.  */
static ssize_t slave_rebuild_state_show(struct kobject *kobj,
                              struct attribute *attr,
                              char *buf)
{
    struct castle_slave *slave = container_of(kobj, struct castle_slave, kobj);

    return sprintf(buf, "0x%lx\n", slave->flags);
}

/* Display the fs version (checkpoint number). */
extern uint32_t castle_filesystem_fs_version;
static ssize_t filesystem_version_show(struct kobject *kobj,
                                   struct attribute *attr,
                                   char *buf)
{
    struct castle_fs_superblock *fs_sb;
    uint32_t                    fs_version;

    fs_sb = castle_fs_superblocks_get();
    fs_version = fs_sb->fs_version;
    castle_fs_superblocks_put(fs_sb, 0);

    return sprintf(buf, "%u\n", fs_version);
}

/* Display the number of blocks that have been remapped. */
extern long castle_extents_chunks_remapped;
static ssize_t slaves_rebuild_chunks_remapped_show(struct kobject *kobj,
                              struct attribute *attr,
                              char *buf)
{
    return sprintf(buf, "%ld\n", castle_extents_chunks_remapped);
}

static ssize_t devices_number_show(struct kobject *kobj,
                                   struct attribute *attr,
                                   char *buf)
{
    struct castle_attachments *devices =
                container_of(kobj, struct castle_attachments, devices_kobj);
    struct castle_attachment *device;
    struct list_head *lh;
    int nr_devices = 0;

    list_for_each(lh, &devices->attachments)
    {
        device = list_entry(lh, struct castle_attachment, list);
        if(!device->device)
            continue;
        nr_devices++;
    }

    return sprintf(buf, "%d\n", nr_devices);
}

static ssize_t device_version_show(struct kobject *kobj,
                                   struct attribute *attr,
                                   char *buf)
{
    struct castle_attachment *device = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf, "0x%x\n", device->version);
}

static ssize_t device_id_show(struct kobject *kobj,
                              struct attribute *attr,
                              char *buf)
{
    struct castle_attachment *device = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf, "0x%x\n", new_encode_dev(MKDEV(device->dev.gd->major, device->dev.gd->first_minor)));
}

static ssize_t collections_number_show(struct kobject *kobj,
                                       struct attribute *attr,
                                       char *buf)
{
    struct castle_attachments *collections =
                container_of(kobj, struct castle_attachments, collections_kobj);
    struct castle_attachment *collection;
    struct list_head *lh;
    int nr_collections = 0;

    list_for_each(lh, &collections->attachments)
    {
        collection = list_entry(lh, struct castle_attachment, list);
        if(collection->device)
            continue;
        nr_collections++;
    }

    return sprintf(buf, "%d\n", nr_collections);
}

static ssize_t collection_version_show(struct kobject *kobj,
                                       struct attribute *attr,
                                       char *buf)
{
    struct castle_attachment *collection = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf, "0x%x\n", collection->version);
}

static ssize_t collection_stats_show(struct kobject *kobj,
                                     struct attribute *attr,
                                     char *buf)
{
    struct castle_attachment *collection = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf,
                   "Gets: %lu\n"
                   "GetsSize: %lu\n"
                   "Puts: %lu\n"
                   "PutsSize: %lu\n"
                   "BigGets: %lu\n"
                   "BigGetsSize: %lu\n"
                   "BigPuts: %lu\n"
                   "BigPutsSize: %lu\n"
                   "RangeQueries: %lu\n"
                   "RangeQueriesSize: %lu\n"
                   "RangeQueriesKeys: %lu\n",
                   atomic64_read(&collection->get.ios),
                   atomic64_read(&collection->get.bytes),
                   atomic64_read(&collection->put.ios),
                   atomic64_read(&collection->put.bytes),
                   atomic64_read(&collection->big_get.ios),
                   atomic64_read(&collection->big_get.bytes),
                   atomic64_read(&collection->big_put.ios),
                   atomic64_read(&collection->big_put.bytes),
                   atomic64_read(&collection->rq.ios),
                   atomic64_read(&collection->rq.bytes),
                   atomic64_read(&collection->rq_nr_keys));
}

static ssize_t collection_id_show(struct kobject *kobj,
                                  struct attribute *attr,
                                  char *buf)
{
    struct castle_attachment *collection = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf, "0x%x\n", collection->col.id);
}

static ssize_t collection_name_show(struct kobject *kobj,
                                    struct attribute *attr,
                                    char *buf)
{
    struct castle_attachment *collection = container_of(kobj, struct castle_attachment, kobj);

    return sprintf(buf, "%s\n", collection->col.name);
}

static ssize_t castle_attr_show(struct kobject *kobj,
                                struct attribute *attr,
                                char *page)
{
    struct castle_sysfs_entry *entry =
                container_of(attr, struct castle_sysfs_entry, attr);

    if (!entry->show)
        return -EIO;

    return entry->show(kobj, attr, page);
}

static ssize_t castle_attr_store(struct kobject *kobj,
                                 struct attribute *attr,
                                 const char *page,
                                 size_t length)
{
    struct castle_sysfs_entry *entry =
                container_of(attr, struct castle_sysfs_entry, attr);

    if (!entry->store)
        return -EIO;
    if (!capable(CAP_SYS_ADMIN))
        return -EACCES;
    return entry->store(kobj, attr, page, length);
}

static void castle_sysfs_kobj_release(struct kobject *kobj)
{
    wake_up(&castle_sysfs_kobj_release_wq);
}

static void castle_sysfs_kobj_release_wait(struct kobject *kobj)
{
    wait_event(castle_sysfs_kobj_release_wq, atomic_read(&kobj->kref.refcount) == 0);
}

static struct sysfs_ops castle_sysfs_ops = {
    .show   = castle_attr_show,
    .store  = castle_attr_store,
};

static struct attribute *castle_root_attrs[] = {
    NULL,
};

static struct kobj_type castle_root_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_root_attrs,
};

static struct attribute *castle_versions_attrs[] = {
    NULL,
};

static struct kobj_type castle_versions_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_versions_attrs,
};

/* Definition of Double array sysfs directory attributes */
static struct castle_sysfs_entry double_array_number =
__ATTR(number, S_IRUGO|S_IWUSR, double_array_number_show, NULL);

static struct attribute *castle_double_array_attrs[] = {
    &double_array_number.attr,
    NULL,
};

static struct kobj_type castle_double_array_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_double_array_attrs,
};

/* Definition of each da sysfs directory attributes */
static struct castle_sysfs_entry da_version =
__ATTR(version, S_IRUGO|S_IWUSR, da_version_show, NULL);

static struct castle_sysfs_entry da_size =
__ATTR(size, S_IRUGO|S_IWUSR, da_size_show, NULL);

static struct castle_sysfs_entry da_compacting =
__ATTR(compacting, S_IRUGO|S_IWUSR, da_compacting_show, NULL);

static struct castle_sysfs_entry da_tree_list =
__ATTR(component_trees, S_IRUGO|S_IWUSR, da_tree_list_show, NULL);

static struct attribute *castle_da_attrs[] = {
    &da_version.attr,
    &da_size.attr,
    &da_compacting.attr,
    &da_tree_list.attr,
    NULL,
};

static struct kobj_type castle_da_ktype = {
    .release        = castle_sysfs_kobj_release,
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_da_attrs,
};

int castle_sysfs_da_add(struct castle_double_array *da)
{
    int ret;

    memset(&da->kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&da->kobj,
                           &double_arrays_kobj,
                           &castle_da_ktype,
                           "%x", da->id);

    if (ret < 0)
        return ret;

    return 0;
}

void castle_sysfs_da_del(struct castle_double_array *da)
{
    kobject_remove(&da->kobj);
    castle_sysfs_kobj_release_wait(&da->kobj);
}

/* Definition of slaves sysfs directory attributes */
static struct castle_sysfs_entry slaves_number =
__ATTR(number, S_IRUGO|S_IWUSR, slaves_number_show, NULL);

static struct castle_sysfs_entry slaves_rebuild_chunks_remapped =
__ATTR(rebuild_chunks_remapped, S_IRUGO|S_IWUSR, slaves_rebuild_chunks_remapped_show, NULL);

static struct attribute *castle_slaves_attrs[] = {
    &slaves_number.attr,
    &slaves_rebuild_chunks_remapped.attr,
    NULL,
};

static struct kobj_type castle_slaves_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_slaves_attrs,
};

/* Definition of each slave sysfs directory attributes */
static struct castle_sysfs_entry slave_uuid =
__ATTR(uuid, S_IRUGO|S_IWUSR, slave_uuid_show, NULL);

static struct castle_sysfs_entry slave_size =
__ATTR(size, S_IRUGO|S_IWUSR, slave_size_show, NULL);

static struct castle_sysfs_entry slave_used =
__ATTR(used, S_IRUGO|S_IWUSR, slave_used_show, NULL);

static struct castle_sysfs_entry slave_ssd =
__ATTR(ssd, S_IRUGO|S_IWUSR, slave_ssd_show, NULL);

static struct castle_sysfs_entry slave_rebuild_state =
__ATTR(rebuild_state, S_IRUGO|S_IWUSR, slave_rebuild_state_show, NULL);

static struct attribute *castle_slave_attrs[] = {
    &slave_uuid.attr,
    &slave_size.attr,
    &slave_used.attr,
    &slave_ssd.attr,
    &slave_rebuild_state.attr,
    NULL,
};

static struct kobj_type castle_slave_ktype = {
    .release        = castle_sysfs_kobj_release,
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_slave_attrs,
};

int castle_sysfs_slave_add(struct castle_slave *slave)
{
    int ret;

    memset(&slave->kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&slave->kobj,
                           &castle_slaves.kobj,
                           &castle_slave_ktype,
                           "%x", slave->uuid);
    if(ret < 0)
        return ret;
    /* TODO: do we need a link for >32?. If so, how do we get hold of the right kobj */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
    /* There is no bdev for ghost slaves. */
    if (!test_bit(CASTLE_SLAVE_GHOST_BIT, &slave->flags))
    {
        /* If this is a partition, link to the partition. */
        if(slave->bdev->bd_part)
            ret = sysfs_create_link(&slave->kobj, &slave->bdev->bd_part->kobj, "dev");
        /* Otherwise link to the device. */
        else
            ret = sysfs_create_link(&slave->kobj, &slave->bdev->bd_disk->kobj, "dev");
        if (ret < 0)
            return ret;
    }
#endif

    return 0;
}

void castle_sysfs_slave_del(struct castle_slave *slave)
{
    /* If slave is not claimed, the sysfs 'dev' link will already have been removed. */
    if (!test_bit(CASTLE_SLAVE_BDCLAIMED_BIT, &slave->flags))
        sysfs_remove_link(&slave->kobj, "dev");
    kobject_remove(&slave->kobj);
    castle_sysfs_kobj_release_wait(&slave->kobj);
}

/* Definition of devices sysfs directory attributes */
static struct castle_sysfs_entry devices_number =
__ATTR(number, S_IRUGO|S_IWUSR, devices_number_show, NULL);

static struct attribute *castle_devices_attrs[] = {
    &devices_number.attr,
    NULL,
};

static struct kobj_type castle_devices_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_devices_attrs,
};

/* Definition of collections sysfs directory attributes */
static struct castle_sysfs_entry collections_number =
__ATTR(number, S_IRUGO|S_IWUSR, collections_number_show, NULL);

static struct attribute *castle_collections_attrs[] = {
    &collections_number.attr,
    NULL,
};

static struct kobj_type castle_collections_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_collections_attrs,
};

/* Definition of filesystem sysfs directory attributes */
static struct castle_sysfs_entry filesystem_version =
__ATTR(filesystem_version, S_IRUGO|S_IWUSR, filesystem_version_show, NULL);

static struct attribute *castle_filesystem_attrs[] = {
    &filesystem_version.attr,
    NULL,
};

static struct kobj_type castle_filesystem_ktype = {
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_filesystem_attrs,
};

/* Definition of each device sysfs directory attributes */
static struct castle_sysfs_entry device_version =
__ATTR(version, S_IRUGO|S_IWUSR, device_version_show, NULL);

static struct castle_sysfs_entry device_id =
__ATTR(id, S_IRUGO|S_IWUSR, device_id_show, NULL);

static struct attribute *castle_device_attrs[] = {
    &device_id.attr,
    &device_version.attr,
    NULL,
};

static struct kobj_type castle_device_ktype = {
    .release        = castle_sysfs_kobj_release,
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_device_attrs,
};

int castle_sysfs_device_add(struct castle_attachment *device)
{
    int ret;

    memset(&device->kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&device->kobj,
                           &castle_attachments.devices_kobj,
                           &castle_device_ktype,
                           "%x",
                           new_encode_dev(MKDEV(device->dev.gd->major,
                                                device->dev.gd->first_minor)));
    if(ret < 0)
        return ret;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
    ret = sysfs_create_link(&device->kobj, &device->dev.gd->kobj, "dev");
    if (ret < 0)
        return ret;
#endif

    return 0;
}

void castle_sysfs_device_del(struct castle_attachment *device)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
    sysfs_remove_link(&device->kobj, "dev");
#endif
    kobject_remove(&device->kobj);
    castle_sysfs_kobj_release_wait(&device->kobj);
}

/* Definition of each collection sysfs directory attributes */
static struct castle_sysfs_entry collection_version =
__ATTR(version, S_IRUGO|S_IWUSR, collection_version_show, NULL);

static struct castle_sysfs_entry collection_id =
__ATTR(id, S_IRUGO|S_IWUSR, collection_id_show, NULL);

static struct castle_sysfs_entry collection_name =
__ATTR(name, S_IRUGO|S_IWUSR, collection_name_show, NULL);

static struct castle_sysfs_entry collection_stats =
__ATTR(stats, S_IRUGO|S_IWUSR, collection_stats_show, NULL);

static struct attribute *castle_collection_attrs[] = {
    &collection_id.attr,
    &collection_version.attr,
    &collection_name.attr,
    &collection_stats.attr,
    NULL,
};

static struct kobj_type castle_collection_ktype = {
    .release        = castle_sysfs_kobj_release,
    .sysfs_ops      = &castle_sysfs_ops,
    .default_attrs  = castle_collection_attrs,
};

int castle_sysfs_collection_add(struct castle_attachment *collection)
{
    int ret;

    memset(&collection->kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&collection->kobj,
                           &castle_attachments.collections_kobj,
                           &castle_collection_ktype,
                           "%x",
                           collection->col.id);
    if(ret < 0)
        return ret;

    return 0;
}

void castle_sysfs_collection_del(struct castle_attachment *collection)
{
    kobject_remove(&collection->kobj);
    castle_sysfs_kobj_release_wait(&collection->kobj);
}

/* Initialisation of sysfs dirs == kobjs registration */
int castle_sysfs_init(void)
{
    int ret;

    init_waitqueue_head(&castle_sysfs_kobj_release_wq);
    memset(&castle.kobj, 0, sizeof(struct kobject));

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
    kobj_set_kset_s(&castle, fs_subsys);
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24)
    /* TODO should probably be kobj_set_kset_s(&castle, fs_subsys); */
    castle.kobj.kset   = &fs_subsys;
#endif

    ret = kobject_tree_add(&castle.kobj,
                            fs_kobject,
                           &castle_root_ktype,
                           "%s", "castle-fs");
    if(ret < 0) goto out1;

    memset(&castle_sysfs_versions.kobj, 0, sizeof(struct kobject));
    INIT_LIST_HEAD(&castle_sysfs_versions.version_list);
    ret = kobject_tree_add(&castle_sysfs_versions.kobj,
                           &castle.kobj,
                           &castle_versions_ktype,
                           "%s", "versions");
    if(ret < 0) goto out2;

    memset(&castle_slaves.kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&castle_slaves.kobj,
                           &castle.kobj,
                           &castle_slaves_ktype,
                           "%s", "slaves");
    if(ret < 0) goto out3;

    memset(&castle_attachments.devices_kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&castle_attachments.devices_kobj,
                           &castle.kobj,
                           &castle_devices_ktype,
                           "%s", "devices");
    if(ret < 0) goto out4;

    memset(&castle_attachments.collections_kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&castle_attachments.collections_kobj,
                           &castle.kobj,
                           &castle_collections_ktype,
                           "%s", "collections");
    if(ret < 0) goto out5;

    memset(&double_arrays_kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&double_arrays_kobj,
                           &castle.kobj,
                           &castle_double_array_ktype,
                           "%s", "vertrees");
    if(ret < 0) goto out6;

    memset(&filesystem_kobj, 0, sizeof(struct kobject));
    ret = kobject_tree_add(&filesystem_kobj,
                           &castle.kobj,
                           &castle_filesystem_ktype,
                           "%s", "filesystem");
    if(ret < 0) goto out7;

    return 0;

    kobject_remove(&filesystem_kobj); /* Unreachable */
out7:
    kobject_remove(&double_arrays_kobj);
out6:
    kobject_remove(&castle_attachments.collections_kobj);
out5:
    kobject_remove(&castle_attachments.devices_kobj);
out4:
    kobject_remove(&castle_slaves.kobj);
out3:
    kobject_remove(&castle_sysfs_versions.kobj);
out2:
    kobject_remove(&castle.kobj);
out1:

    return ret;
}

void castle_sysfs_fini(void)
{
    kobject_remove(&filesystem_kobj);
    kobject_remove(&double_arrays_kobj);
    kobject_remove(&castle_attachments.collections_kobj);
    kobject_remove(&castle_attachments.devices_kobj);
    kobject_remove(&castle_slaves.kobj);
    castle_sysfs_versions_fini();
    kobject_remove(&castle.kobj);
}



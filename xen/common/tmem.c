/******************************************************************************
 * tmem.c
 *
 * Transcendent memory
 *
 * Copyright (c) 2009, Dan Magenheimer, Oracle Corp.
 */

/* TODO list: 090129
   - improve on reclamation policy
   - use different tlsf pools for each client (maybe each pool)
   - implement page accounting and minimal QoS limits
   - test shared access more completely (need pv cluster fs)
   - add feedback-driven compression (not for persistent pools though!)
   - add data-structure total bytes overhead stats
 */

#ifdef __XEN__
#include <xen/tmem_xen.h> /* host-specific (eg Xen) code goes here */
#endif

#include <xen/tmem.h>
#include <xen/rbtree.h>
#include <xen/radix-tree.h>
#include <xen/list.h>

#define EXPORT /* indicates code other modules are dependent upon */
#define FORWARD

/************  INTERFACE TO TMEM HOST-DEPENDENT (tmh) CODE ************/

#define CLI_ID_NULL TMH_CLI_ID_NULL
#define cli_id_str  tmh_cli_id_str
#define client_str  tmh_client_str

/************ DEBUG and STATISTICS (+ some compression testing) *******/

#ifndef NDEBUG
#define SENTINELS
#define NOINLINE noinline
#else
#define NOINLINE
#endif

#ifdef SENTINELS
#define DECL_SENTINEL unsigned long sentinel;
#define SET_SENTINEL(_x,_y) _x->sentinel = _y##_SENTINEL
#define INVERT_SENTINEL(_x,_y) _x->sentinel = ~_y##_SENTINEL
#define ASSERT_SENTINEL(_x,_y) \
    ASSERT(_x->sentinel != ~_y##_SENTINEL);ASSERT(_x->sentinel == _y##_SENTINEL)
#ifdef __i386__
#define POOL_SENTINEL 0x87658765
#define OBJ_SENTINEL 0x12345678
#define OBJNODE_SENTINEL 0xfedcba09
#define PGD_SENTINEL  0x43214321
#else
#define POOL_SENTINEL 0x8765876587658765
#define OBJ_SENTINEL 0x1234567812345678
#define OBJNODE_SENTINEL 0xfedcba0987654321
#define PGD_SENTINEL  0x4321432143214321
#endif
#else
#define DECL_SENTINEL
#define SET_SENTINEL(_x,_y) do { } while (0)
#define ASSERT_SENTINEL(_x,_y) do { } while (0)
#define INVERT_SENTINEL(_x,_y) do { } while (0)
#endif

/* global statistics (none need to be locked) */
static unsigned long total_tmem_ops = 0;
static unsigned long errored_tmem_ops = 0;
static unsigned long total_flush_pool = 0;
static unsigned long alloc_failed = 0, alloc_page_failed = 0;
static unsigned long evicted_pgs = 0, evict_attempts = 0;
static unsigned long relinq_pgs = 0, relinq_attempts = 0;
static unsigned long max_evicts_per_relinq = 0;
static unsigned long low_on_memory = 0;
static int global_obj_count_max = 0;
static int global_pgp_count_max = 0;
static int global_page_count_max = 0;
static int global_rtree_node_count_max = 0;
static long global_eph_count_max = 0;
static unsigned long failed_copies;

DECL_CYC_COUNTER(succ_get);
DECL_CYC_COUNTER(succ_put);
DECL_CYC_COUNTER(non_succ_get);
DECL_CYC_COUNTER(non_succ_put);
DECL_CYC_COUNTER(flush);
DECL_CYC_COUNTER(flush_obj);
#ifdef COMPARE_COPY_PAGE_SSE2
EXTERN_CYC_COUNTER(pg_copy1);
EXTERN_CYC_COUNTER(pg_copy2);
EXTERN_CYC_COUNTER(pg_copy3);
EXTERN_CYC_COUNTER(pg_copy4);
#else
EXTERN_CYC_COUNTER(pg_copy);
#endif
DECL_CYC_COUNTER(compress);
DECL_CYC_COUNTER(decompress);

/************ CORE DATA STRUCTURES ************************************/

#define MAX_POOLS_PER_DOMAIN 16
#define MAX_GLOBAL_SHARED_POOLS  16

struct tm_pool;
struct client {
    struct list_head client_list;
    struct tm_pool *pools[MAX_POOLS_PER_DOMAIN];
    tmh_client_t *tmh;
    struct list_head ephemeral_page_list;
    long eph_count, eph_count_max;
    cli_id_t cli_id;
    uint32_t weight;
    uint32_t cap;
    bool_t compress;
    bool_t frozen;
    unsigned long compress_poor, compress_nomem;
    unsigned long compressed_pages;
    uint64_t compressed_sum_size;
};
typedef struct client client_t;

struct share_list {
    struct list_head share_list;
    client_t *client;
};
typedef struct share_list sharelist_t;

#define OBJ_HASH_BUCKETS 256 /* must be power of two */
#define OBJ_HASH_BUCKETS_MASK (OBJ_HASH_BUCKETS-1)
#define OBJ_HASH(_oid) (tmh_hash(_oid, BITS_PER_LONG) & OBJ_HASH_BUCKETS_MASK)

struct tm_pool {
    bool_t shared;
    bool_t persistent;
    struct list_head pool_list; /* FIXME do we need this anymore? */
    client_t *client;
    uint64_t uuid[2]; /* 0 for private, non-zero for shared */
    uint32_t pool_id;
    rwlock_t pool_rwlock;
    struct rb_root obj_rb_root[OBJ_HASH_BUCKETS]; /* protected by pool_rwlock */
    struct list_head share_list; /* valid if shared */
    DECL_SENTINEL
    int shared_count; /* valid if shared */
    atomic_t pgp_count;
    int pgp_count_max;
    long obj_count;  /* atomicity depends on pool_rwlock held for write */
    long obj_count_max;  
    unsigned long objnode_count, objnode_count_max;
    uint64_t sum_life_cycles;
    uint64_t sum_evicted_cycles;
    unsigned long puts, good_puts, no_mem_puts;
    unsigned long dup_puts_flushed, dup_puts_replaced;
    unsigned long gets, found_gets;
    unsigned long flushs, flushs_found;
    unsigned long flush_objs, flush_objs_found;
};
typedef struct tm_pool pool_t;

#define is_persistent(_p)  (_p->persistent)
#define is_ephemeral(_p)   (!(_p->persistent))
#define is_shared(_p)      (_p->shared)
#define is_private(_p)     (!(_p->shared))

struct tmem_object_root {
    DECL_SENTINEL
    uint64_t oid;
    struct rb_node rb_tree_node; /* protected by pool->pool_rwlock */
    unsigned long objnode_count; /* atomicity depends on obj_spinlock */
    long pgp_count; /* atomicity depends on obj_spinlock */
    struct radix_tree_root tree_root; /* tree of pages within object */
    pool_t *pool;
    cli_id_t last_client;
    spinlock_t obj_spinlock;
    bool_t no_evict; /* if globally locked, pseudo-locks against eviction */
};
typedef struct tmem_object_root obj_t;

typedef struct radix_tree_node rtn_t;
struct tmem_object_node {
    obj_t *obj;
    DECL_SENTINEL
    rtn_t rtn;
};
typedef struct tmem_object_node objnode_t;

struct tmem_page_descriptor {
    struct list_head global_eph_pages;
    struct list_head client_eph_pages;
    obj_t *obj;
    uint32_t index;
    size_t size; /* 0 == PAGE_SIZE (pfp), else compressed data (cdata) */
    union {
        pfp_t *pfp;  /* page frame pointer */
        char *cdata; /* compressed data */
    };
    uint64_t timestamp;
    DECL_SENTINEL
};
typedef struct tmem_page_descriptor pgp_t;

static LIST_HEAD(global_ephemeral_page_list); /* all pages in ephemeral pools */

static LIST_HEAD(global_client_list);
static LIST_HEAD(global_pool_list);

static pool_t *global_shared_pools[MAX_GLOBAL_SHARED_POOLS] = { 0 };
static atomic_t client_weight_total = ATOMIC_INIT(0);
static int tmem_initialized = 0;

/************ CONCURRENCY  ***********************************************/

EXPORT DEFINE_SPINLOCK(tmem_spinlock);  /* used iff tmh_lock_all */
EXPORT DEFINE_RWLOCK(tmem_rwlock);      /* used iff !tmh_lock_all */
static DEFINE_SPINLOCK(eph_lists_spinlock); /* protects global AND clients */

#define tmem_spin_lock(_l)  do {if (!tmh_lock_all) spin_lock(_l);}while(0)
#define tmem_spin_unlock(_l)  do {if (!tmh_lock_all) spin_unlock(_l);}while(0)
#define tmem_read_lock(_l)  do {if (!tmh_lock_all) read_lock(_l);}while(0)
#define tmem_read_unlock(_l)  do {if (!tmh_lock_all) read_unlock(_l);}while(0)
#define tmem_write_lock(_l)  do {if (!tmh_lock_all) write_lock(_l);}while(0)
#define tmem_write_unlock(_l)  do {if (!tmh_lock_all) write_unlock(_l);}while(0)
#define tmem_write_trylock(_l)  ((tmh_lock_all)?1:write_trylock(_l))
#define tmem_spin_trylock(_l)  (tmh_lock_all?1:spin_trylock(_l))

#define ASSERT_SPINLOCK(_l) ASSERT(tmh_lock_all || spin_is_locked(_l))
#define ASSERT_WRITELOCK(_l) ASSERT(tmh_lock_all || rw_is_write_locked(_l))

/* global counters (should use long_atomic_t access) */
static long global_eph_count = 0; /* atomicity depends on eph_lists_spinlock */
static atomic_t global_obj_count = ATOMIC_INIT(0);
static atomic_t global_pgp_count = ATOMIC_INIT(0);
static atomic_t global_page_count = ATOMIC_INIT(0);
static atomic_t global_rtree_node_count = ATOMIC_INIT(0);

#define atomic_inc_and_max(_c) do { \
    atomic_inc(&_c); \
    if ( _atomic_read(_c) > _c##_max ) \
        _c##_max = _atomic_read(_c); \
} while (0)

#define atomic_dec_and_assert(_c) do { \
    atomic_dec(&_c); \
    ASSERT(_atomic_read(_c) >= 0); \
} while (0)


/************ MEMORY ALLOCATION INTERFACE *****************************/

#define tmem_malloc(_type,_pool) \
       _tmem_malloc(sizeof(_type), __alignof__(_type), _pool)

#define tmem_malloc_bytes(_size,_pool) \
       _tmem_malloc(_size, 1, _pool)

static NOINLINE void *_tmem_malloc(size_t size, size_t align, pool_t *pool)
{
    void *v;

    if ( (pool != NULL) && is_persistent(pool) )
        v = tmh_alloc_subpage_thispool(pool,size,align);
    else
        v = tmh_alloc_subpage(pool, size, align);
    if ( v == NULL )
        alloc_failed++;
    return v;
}

static NOINLINE void tmem_free(void *p, size_t size, pool_t *pool)
{
    if ( pool == NULL || !is_persistent(pool) )
        tmh_free_subpage(p,size);
    else
        tmh_free_subpage_thispool(pool,p,size);
}

static NOINLINE pfp_t *tmem_page_alloc(pool_t *pool)
{
    pfp_t *pfp = NULL;

    if ( pool != NULL && is_persistent(pool) )
        pfp = tmh_alloc_page_thispool(pool);
    else
        pfp = tmh_alloc_page(pool,0);
    if ( pfp == NULL )
        alloc_page_failed++;
    else
        atomic_inc_and_max(global_page_count);
    return pfp;
}

static NOINLINE void tmem_page_free(pool_t *pool, pfp_t *pfp)
{
    ASSERT(pfp);
    if ( pool == NULL || !is_persistent(pool) )
        tmh_free_page(pfp);
    else
        tmh_free_page_thispool(pool,pfp);
    atomic_dec_and_assert(global_page_count);
}

/************ PAGE DESCRIPTOR MANIPULATION ROUTINES *******************/

/* allocate a pgp_t and associate it with an object */
static NOINLINE pgp_t *pgp_alloc(obj_t *obj)
{
    pgp_t *pgp;
    pool_t *pool;

    ASSERT(obj != NULL);
    ASSERT(obj->pool != NULL);
    pool = obj->pool;
    if ( (pgp = tmem_malloc(pgp_t, pool)) == NULL )
        return NULL;
    pgp->obj = obj;
    INIT_LIST_HEAD(&pgp->global_eph_pages);
    INIT_LIST_HEAD(&pgp->client_eph_pages);
    pgp->pfp = NULL;
    pgp->size = -1;
    pgp->index = -1;
    pgp->timestamp = get_cycles();
    SET_SENTINEL(pgp,PGD);
    atomic_inc_and_max(global_pgp_count);
    atomic_inc_and_max(pool->pgp_count);
    return pgp;
}

static pgp_t *pgp_lookup_in_obj(obj_t *obj, uint32_t index)
{
    ASSERT(obj != NULL);
    ASSERT_SPINLOCK(&obj->obj_spinlock);
    ASSERT_SENTINEL(obj,OBJ);
    ASSERT(obj->pool != NULL);
    ASSERT_SENTINEL(obj->pool,POOL);
    return radix_tree_lookup(&obj->tree_root, index);
}

static NOINLINE void pgp_free_data(pgp_t *pgp, pool_t *pool)
{
    if ( pgp->pfp == NULL )
        return;
    if ( !pgp->size )
        tmem_page_free(pgp->obj->pool,pgp->pfp);
    else
    {
        tmem_free(pgp->cdata,pgp->size,pool);
        if ( pool != NULL )
        {
            pool->client->compressed_pages--;
            pool->client->compressed_sum_size -= pgp->size;
        }
    }
    pgp->pfp = NULL;
    pgp->size = -1;
}

static NOINLINE void pgp_free(pgp_t *pgp, int from_delete)
{
    pool_t *pool = NULL;

    ASSERT_SENTINEL(pgp,PGD);
    ASSERT(pgp->obj != NULL);
    ASSERT_SENTINEL(pgp->obj,OBJ);
    ASSERT_SENTINEL(pgp->obj->pool,POOL);
    ASSERT(list_empty(&pgp->global_eph_pages));
    ASSERT(list_empty(&pgp->client_eph_pages));
    if ( from_delete )
        ASSERT(pgp_lookup_in_obj(pgp->obj,pgp->index) == NULL);
    ASSERT(pgp->obj->pool != NULL);
    pool = pgp->obj->pool;
    pgp_free_data(pgp, pool);
    INVERT_SENTINEL(pgp,PGD);
    pgp->obj = NULL;
    pgp->index = -1;
    pgp->size = -1;
    atomic_dec_and_assert(global_pgp_count);
    atomic_dec_and_assert(pool->pgp_count);
    tmem_free(pgp,sizeof(pgp_t),pool);
}

/* remove the page from appropriate lists but not from parent object */
static void pgp_delist(pgp_t *pgp, bool_t no_eph_lock)
{
    ASSERT(pgp != NULL);
    ASSERT(pgp->obj != NULL);
    ASSERT(pgp->obj->pool != NULL);
    ASSERT(pgp->obj->pool->client != NULL);
    if ( is_ephemeral(pgp->obj->pool) )
    {
        if ( !no_eph_lock )
            tmem_spin_lock(&eph_lists_spinlock);
        if ( !list_empty(&pgp->client_eph_pages) )
            pgp->obj->pool->client->eph_count--;
        ASSERT(pgp->obj->pool->client->eph_count >= 0);
        list_del_init(&pgp->client_eph_pages);
        if ( !list_empty(&pgp->global_eph_pages) )
            global_eph_count--;
        ASSERT(global_eph_count >= 0);
        list_del_init(&pgp->global_eph_pages);
        if ( !no_eph_lock )
            tmem_spin_unlock(&eph_lists_spinlock);
    }
}

/* remove page from lists (but not from parent object) and free it */
static NOINLINE void pgp_delete(pgp_t *pgp, bool_t no_eph_lock)
{
    uint64_t life;

    ASSERT(pgp != NULL);
    ASSERT(pgp->obj != NULL);
    ASSERT(pgp->obj->pool != NULL);
    life = get_cycles() - pgp->timestamp;
    pgp->obj->pool->sum_life_cycles += life;
    pgp_delist(pgp, no_eph_lock);
    pgp_free(pgp,1);
}

/* called only indirectly by radix_tree_destroy */
static NOINLINE void pgp_destroy(void *v)
{
    pgp_t *pgp = (pgp_t *)v;

    ASSERT_SPINLOCK(&pgp->obj->obj_spinlock);
    pgp_delist(pgp,0);
    ASSERT(pgp->obj != NULL);
    pgp->obj->pgp_count--;
    ASSERT(pgp->obj->pgp_count >= 0);
    pgp_free(pgp,0);
}

FORWARD static rtn_t *rtn_alloc(void *arg);
FORWARD static void rtn_free(rtn_t *rtn);

static int pgp_add_to_obj(obj_t *obj, uint32_t index, pgp_t *pgp)
{
    int ret;

    ASSERT_SPINLOCK(&obj->obj_spinlock);
    ret = radix_tree_insert(&obj->tree_root, index, pgp, rtn_alloc, obj);
    if ( !ret )
        obj->pgp_count++;
    return ret;
}

static NOINLINE pgp_t *pgp_delete_from_obj(obj_t *obj, uint32_t index)
{
    pgp_t *pgp;

    ASSERT(obj != NULL);
    ASSERT_SPINLOCK(&obj->obj_spinlock);
    ASSERT_SENTINEL(obj,OBJ);
    ASSERT(obj->pool != NULL);
    ASSERT_SENTINEL(obj->pool,POOL);
    pgp = radix_tree_delete(&obj->tree_root, index, rtn_free);
    if ( pgp != NULL )
        obj->pgp_count--;
    ASSERT(obj->pgp_count >= 0);

    return pgp;
}

/************ RADIX TREE NODE MANIPULATION ROUTINES *******************/

/* called only indirectly from radix_tree_insert */
static NOINLINE rtn_t *rtn_alloc(void *arg)
{
    objnode_t *objnode;
    obj_t *obj = (obj_t *)arg;

    ASSERT_SENTINEL(obj,OBJ);
    ASSERT(obj->pool != NULL);
    ASSERT_SENTINEL(obj->pool,POOL);
    objnode = tmem_malloc(objnode_t,obj->pool);
    if (objnode == NULL)
        return NULL;
    objnode->obj = obj;
    SET_SENTINEL(objnode,OBJNODE);
    memset(&objnode->rtn, 0, sizeof(rtn_t));
    if (++obj->pool->objnode_count > obj->pool->objnode_count_max)
        obj->pool->objnode_count_max = obj->pool->objnode_count;
    atomic_inc_and_max(global_rtree_node_count);
    obj->objnode_count++;
    return &objnode->rtn;
}

/* called only indirectly from radix_tree_delete/destroy */
static void rtn_free(rtn_t *rtn)
{
    pool_t *pool;
    objnode_t *objnode;
    int i;

    ASSERT(rtn != NULL);
    for (i = 0; i < RADIX_TREE_MAP_SIZE; i++)
        ASSERT(rtn->slots[i] == NULL);
    objnode = container_of(rtn,objnode_t,rtn);
    ASSERT_SENTINEL(objnode,OBJNODE);
    INVERT_SENTINEL(objnode,OBJNODE);
    ASSERT(objnode->obj != NULL);
    ASSERT_SPINLOCK(&objnode->obj->obj_spinlock);
    ASSERT_SENTINEL(objnode->obj,OBJ);
    pool = objnode->obj->pool;
    ASSERT(pool != NULL);
    ASSERT_SENTINEL(pool,POOL);
    pool->objnode_count--;
    objnode->obj->objnode_count--;
    objnode->obj = NULL;
    tmem_free(objnode,sizeof(objnode_t),pool);
    atomic_dec_and_assert(global_rtree_node_count);
}

/************ POOL OBJECT COLLECTION MANIPULATION ROUTINES *******************/

/* searches for object==oid in pool, returns locked object if found */
static NOINLINE obj_t * obj_find(pool_t *pool, uint64_t oid)
{
    struct rb_node *node;
    obj_t *obj;

restart_find:
    tmem_read_lock(&pool->pool_rwlock);
    node = pool->obj_rb_root[OBJ_HASH(oid)].rb_node;
    while ( node )
    {
        obj = container_of(node, obj_t, rb_tree_node);
        if ( obj->oid == oid )
        {
            if ( tmh_lock_all )
                obj->no_evict = 1;
            else
            {
                if ( !tmem_spin_trylock(&obj->obj_spinlock) )
                {
                    tmem_read_unlock(&pool->pool_rwlock);
                    goto restart_find;
                }
                tmem_read_unlock(&pool->pool_rwlock);
            }
            return obj;
        }
        else if ( oid < obj->oid )
            node = node->rb_left;
        else
            node = node->rb_right;
    }
    tmem_read_unlock(&pool->pool_rwlock);
    return NULL;
}

/* free an object that has no more pgps in it */
static NOINLINE void obj_free(obj_t *obj, int no_rebalance)
{
    pool_t *pool;
    uint64_t old_oid;

    ASSERT_SPINLOCK(&obj->obj_spinlock);
    ASSERT(obj != NULL);
    ASSERT_SENTINEL(obj,OBJ);
    ASSERT(obj->pgp_count == 0);
    pool = obj->pool;
    ASSERT(pool != NULL);
    ASSERT_WRITELOCK(&pool->pool_rwlock);
    if ( obj->tree_root.rnode != NULL ) /* may be a "stump" with no leaves */
        radix_tree_destroy(&obj->tree_root, pgp_destroy, rtn_free);
    ASSERT((long)obj->objnode_count == 0);
    ASSERT(obj->tree_root.rnode == NULL);
    pool->obj_count--;
    ASSERT(pool->obj_count >= 0);
    INVERT_SENTINEL(obj,OBJ);
    obj->pool = NULL;
    old_oid = obj->oid;
    obj->oid = -1;
    obj->last_client = CLI_ID_NULL;
    atomic_dec_and_assert(global_obj_count);
    /* use no_rebalance only if all objects are being destroyed anyway */
    if ( !no_rebalance )
        rb_erase(&obj->rb_tree_node,&pool->obj_rb_root[OBJ_HASH(old_oid)]);
    tmem_free(obj,sizeof(obj_t),pool);
}

static NOINLINE void obj_rb_destroy_node(struct rb_node *node)
{
    obj_t * obj;

    if ( node == NULL )
        return;
    obj_rb_destroy_node(node->rb_left);
    obj_rb_destroy_node(node->rb_right);
    obj = container_of(node, obj_t, rb_tree_node);
    tmem_spin_lock(&obj->obj_spinlock);
    ASSERT(obj->no_evict == 0);
    radix_tree_destroy(&obj->tree_root, pgp_destroy, rtn_free);
    obj_free(obj,1);
}

static NOINLINE int obj_rb_insert(struct rb_root *root, obj_t *obj)
{
    struct rb_node **new, *parent = NULL;
    obj_t *this;

    new = &(root->rb_node);
    while ( *new )
    {
        this = container_of(*new, obj_t, rb_tree_node);
        parent = *new;
        if ( obj->oid < this->oid )
            new = &((*new)->rb_left);
        else if ( obj->oid > this->oid )
            new = &((*new)->rb_right);
        else
            return 0;
    }
    rb_link_node(&obj->rb_tree_node, parent, new);
    rb_insert_color(&obj->rb_tree_node, root);
    return 1;
}

/*
 * allocate, initialize, and insert an tmem_object_root
 * (should be called only if find failed)
 */
static NOINLINE obj_t * obj_new(pool_t *pool, uint64_t oid)
{
    obj_t *obj;

    ASSERT(pool != NULL);
    ASSERT_WRITELOCK(&pool->pool_rwlock);
    if ( (obj = tmem_malloc(obj_t,pool)) == NULL )
        return NULL;
    pool->obj_count++;
    if (pool->obj_count > pool->obj_count_max)
        pool->obj_count_max = pool->obj_count;
    atomic_inc_and_max(global_obj_count);
    INIT_RADIX_TREE(&obj->tree_root,0);
    spin_lock_init(&obj->obj_spinlock);
    obj->pool = pool;
    obj->oid = oid;
    obj->objnode_count = 0;
    obj->pgp_count = 0;
    obj->last_client = CLI_ID_NULL;
    SET_SENTINEL(obj,OBJ);
    tmem_spin_lock(&obj->obj_spinlock);
    obj_rb_insert(&pool->obj_rb_root[OBJ_HASH(oid)], obj);
    obj->no_evict = 1;
    ASSERT_SPINLOCK(&obj->obj_spinlock);
    return obj;
}

/* free an object after destroying any pgps in it */
static NOINLINE void obj_destroy(obj_t *obj)
{
    ASSERT_WRITELOCK(&obj->pool->pool_rwlock);
    radix_tree_destroy(&obj->tree_root, pgp_destroy, rtn_free);
    obj_free(obj,0);
}

/* destroy all objects in a pool */
static NOINLINE void obj_rb_destroy_all(pool_t *pool)
{
    int i;

    tmem_write_lock(&pool->pool_rwlock);
    for (i = 0; i < OBJ_HASH_BUCKETS; i++)
        obj_rb_destroy_node(pool->obj_rb_root[i].rb_node);
    tmem_write_unlock(&pool->pool_rwlock);
}

/* destroys all objects in a pool that have last_client set to cli_id */
static void obj_free_selective(pool_t *pool, cli_id_t cli_id)
{
    struct rb_node *node;
    obj_t *obj;
    int i;

    tmem_write_lock(&pool->pool_rwlock);
    for (i = 0; i < OBJ_HASH_BUCKETS; i++)
    {
        node = rb_first(&pool->obj_rb_root[i]);
        while ( node != NULL )
        {
            obj = container_of(node, obj_t, rb_tree_node);
            tmem_spin_lock(&obj->obj_spinlock);
            node = rb_next(node);
            if ( obj->last_client == cli_id )
                obj_destroy(obj);
            else
                tmem_spin_unlock(&obj->obj_spinlock);
        }
    }
    tmem_write_unlock(&pool->pool_rwlock);
}


/************ POOL MANIPULATION ROUTINES ******************************/

static pool_t * pool_alloc(void)
{
    pool_t *pool;
    int i;

    if ( (pool = tmem_malloc(pool_t,NULL)) == NULL )
        return NULL;
    for (i = 0; i < OBJ_HASH_BUCKETS; i++)
        pool->obj_rb_root[i] = RB_ROOT;
    INIT_LIST_HEAD(&pool->pool_list);
    rwlock_init(&pool->pool_rwlock);
    pool->pgp_count_max = pool->obj_count_max = 0;
    pool->objnode_count = pool->objnode_count_max = 0;
    atomic_set(&pool->pgp_count,0);
    pool->obj_count = 0;
    pool->good_puts = pool->puts = pool->dup_puts_flushed = 0;
    pool->dup_puts_replaced = pool->no_mem_puts = 0;
    pool->found_gets = pool->gets = 0;
    pool->flushs_found = pool->flushs = 0;
    pool->flush_objs_found = pool->flush_objs = 0;
    SET_SENTINEL(pool,POOL);
    return pool;
}

static NOINLINE void pool_free(pool_t *pool)
{
    ASSERT_SENTINEL(pool,POOL);
    INVERT_SENTINEL(pool,POOL);
    pool->client = NULL;
    list_del(&pool->pool_list);
    tmem_free(pool,sizeof(pool_t),NULL);
}

/* register new_client as a user of this shared pool and return new
   total number of registered users */
static int shared_pool_join(pool_t *pool, client_t *new_client)
{
    sharelist_t *sl;

    ASSERT(is_shared(pool));
    if ( (sl = tmem_malloc(sharelist_t,NULL)) == NULL )
        return -1;
    sl->client = new_client;
    list_add_tail(&sl->share_list, &pool->share_list);
    printk("adding new %s %d to shared pool owned by %s %d\n",
        client_str, new_client->cli_id, client_str, pool->client->cli_id);
    return ++pool->shared_count;
}

/* reassign "ownership" of the pool to another client that shares this pool */
static NOINLINE void shared_pool_reassign(pool_t *pool)
{
    sharelist_t *sl;
    int poolid;
    client_t *old_client = pool->client, *new_client;

    ASSERT(is_shared(pool));
    if ( list_empty(&pool->share_list) )
    {
        ASSERT(pool->shared_count == 0);
        return;
    }
    old_client->pools[pool->pool_id] = NULL;
    sl = list_entry(pool->share_list.next, sharelist_t, share_list);
    ASSERT(sl->client != old_client);
    pool->client = new_client = sl->client;
    for (poolid = 0; poolid < MAX_POOLS_PER_DOMAIN; poolid++)
        if (new_client->pools[poolid] == pool)
            break;
    ASSERT(poolid != MAX_POOLS_PER_DOMAIN);
    printk("reassigned shared pool from %s=%d to %s=%d pool_id=%d\n",
        cli_id_str, old_client->cli_id, cli_id_str, new_client->cli_id, poolid);
    pool->pool_id = poolid;
}

/* destroy all objects with last_client same as passed cli_id,
   remove pool's cli_id from list of sharers of this pool */
static NOINLINE int shared_pool_quit(pool_t *pool, cli_id_t cli_id)
{
    sharelist_t *sl;
    int s_poolid;

    ASSERT(is_shared(pool));
    ASSERT(pool->client != NULL);
    
    obj_free_selective(pool,cli_id);
    list_for_each_entry(sl,&pool->share_list, share_list)
    {
        if (sl->client->cli_id != cli_id)
            continue;
        list_del(&sl->share_list);
        tmem_free(sl,sizeof(sharelist_t),pool);
        --pool->shared_count;
        if (pool->client->cli_id == cli_id)
            shared_pool_reassign(pool);
        if (pool->shared_count)
            return pool->shared_count;
        for (s_poolid = 0; s_poolid < MAX_GLOBAL_SHARED_POOLS; s_poolid++)
            if ( (global_shared_pools[s_poolid]) == pool )
            {
                global_shared_pools[s_poolid] = NULL;
                break;
            }
        return 0;
    }
    printk("tmem: no match unsharing pool, %s=%d\n",
        cli_id_str,pool->client->cli_id);
    return -1;
}

/* flush all data (owned by cli_id) from a pool and, optionally, free it */
static void pool_flush(pool_t *pool, cli_id_t cli_id, bool_t destroy)
{
    ASSERT(pool != NULL);
    if ( (is_shared(pool)) && (shared_pool_quit(pool,cli_id) > 0) )
    {
        printk("tmem: unshared shared pool %d from %s=%d\n",
           pool->pool_id, cli_id_str,pool->client->cli_id);
        return;
    }
    printk("%s %s-%s tmem pool ",destroy?"destroying":"flushing",
        is_persistent(pool) ? "persistent" : "ephemeral" ,
        is_shared(pool) ? "shared" : "private");
    printk("%s=%d pool_id=%d\n", cli_id_str,pool->client->cli_id,pool->pool_id);
    obj_rb_destroy_all(pool);
    if ( destroy )
    {
        pool->client->pools[pool->pool_id] = NULL;
        pool_free(pool);
    }
}

/************ CLIENT MANIPULATION OPERATIONS **************************/

static client_t *client_create(void)
{
    client_t *client = tmem_malloc(client_t,NULL);
    cli_id_t cli_id = tmh_get_cli_id_from_current();

    printk("tmem: initializing tmem capability for %s=%d...",cli_id_str,cli_id);
    if ( client == NULL )
    {
        printk("failed... out of memory\n");
        return NULL;
    }
    memset(client,0,sizeof(client_t));
    if ( (client->tmh = tmh_client_init()) == NULL )
    {
        printk("failed... can't allocate host-dependent part of client\n");
        if ( client )
            tmem_free(client,sizeof(client_t),NULL);
        return NULL;
    }
    tmh_set_current_client(client);
    client->cli_id = cli_id;
#ifdef __i386__
    client->compress = 0;
#else
    client->compress = tmh_compression_enabled();
#endif
    list_add_tail(&client->client_list, &global_client_list);
    INIT_LIST_HEAD(&client->ephemeral_page_list);
    client->eph_count = client->eph_count_max = 0;
    printk("ok\n");
    return client;
}

static void client_free(client_t *client)
{
    list_del(&client->client_list);
    tmh_client_destroy(client->tmh);
    tmh_set_current_client(NULL);
    tmem_free(client,sizeof(client_t),NULL);
}

/* flush all data from a client and, optionally, free it */
static void client_flush(client_t *client, bool_t destroy)
{
    int i;
    pool_t *pool;

    for  (i = 0; i < MAX_POOLS_PER_DOMAIN; i++)
    {
        if ( (pool = client->pools[i]) == NULL )
            continue;
        pool_flush(pool,client->cli_id,destroy);
        if ( destroy )
            client->pools[i] = NULL;
    }
    if ( destroy )
        client_free(client);
}

static bool_t client_over_quota(client_t *client)
{
    int total = _atomic_read(client_weight_total);

    ASSERT(client != NULL);
    if ( (total == 0) || (client->weight == 0) || 
          (client->eph_count == 0) )
        return 0;
    return ( ((global_eph_count*100L) / client->eph_count ) >
             ((total*100L) / client->weight) );
}

/************ MEMORY REVOCATION ROUTINES *******************************/

static int tmem_evict(void)
{
    client_t *client = tmh_client_from_current();
    pgp_t *pgp = NULL, *pgp_del;
    obj_t *obj;
    pool_t *pool;
    int ret = 0;
    bool_t hold_pool_rwlock = 0;

    evict_attempts++;
    tmem_spin_lock(&eph_lists_spinlock);
    if ( (client != NULL) && client_over_quota(client) &&
         !list_empty(&client->ephemeral_page_list) )
    {
        list_for_each_entry(pgp,&client->ephemeral_page_list,client_eph_pages)
        {
            obj = pgp->obj;
            pool = obj->pool;
            if ( tmh_lock_all && !obj->no_evict )
                goto found;
            if ( tmem_spin_trylock(&obj->obj_spinlock) )
            {
                if ( obj->pgp_count > 1 )
                    goto found;
                if ( tmem_write_trylock(&pool->pool_rwlock) )
                {
                    hold_pool_rwlock = 1;
                    goto found;
                }
                tmem_spin_unlock(&obj->obj_spinlock);
            }
        }
    } else if ( list_empty(&global_ephemeral_page_list) ) {
        goto out;
    } else {
        list_for_each_entry(pgp,&global_ephemeral_page_list,global_eph_pages)
        {
            obj = pgp->obj;
            pool = obj->pool;
            if ( tmh_lock_all && !obj->no_evict )
                goto found;
            if ( tmem_spin_trylock(&obj->obj_spinlock) )
            {
                if ( obj->pgp_count > 1 )
                    goto found;
                if ( tmem_write_trylock(&pool->pool_rwlock) )
                {
                    hold_pool_rwlock = 1;
                    goto found;
                }
                tmem_spin_unlock(&obj->obj_spinlock);
            }
        }
    }

    ret = 0;
    goto out;

found:
    ASSERT(pgp != NULL);
    ASSERT_SENTINEL(pgp,PGD);
    obj = pgp->obj;
    ASSERT(obj != NULL);
    ASSERT(obj->no_evict == 0);
    ASSERT(obj->pool != NULL);
    ASSERT_SENTINEL(obj,OBJ);

    ASSERT_SPINLOCK(&obj->obj_spinlock);
    pgp_del = pgp_delete_from_obj(obj, pgp->index);
    ASSERT(pgp_del == pgp);
    pgp_delete(pgp,1);
    if ( obj->pgp_count == 0 )
    {
        ASSERT_WRITELOCK(&pool->pool_rwlock);
        obj_free(obj,0);
    }
    else
        tmem_spin_unlock(&obj->obj_spinlock);
    if ( hold_pool_rwlock )
        tmem_write_unlock(&pool->pool_rwlock);
    evicted_pgs++;
    ret = 1;

out:
    tmem_spin_unlock(&eph_lists_spinlock);
    return ret;
}

static unsigned long tmem_relinquish_npages(unsigned long n)
{
    unsigned long avail_pages = 0;

    while ( (avail_pages = tmh_avail_pages()) < n )
    {
        if (  !tmem_evict() )
            break;
    }
    if ( avail_pages )
        tmh_release_avail_pages_to_host();
    return avail_pages;
}

/************ TMEM CORE OPERATIONS ************************************/

static NOINLINE int do_tmem_put_compress(pgp_t *pgp, tmem_cli_mfn_t cmfn)
{
    void *dst, *p;
    size_t size;
    int ret = 0;
    DECL_LOCAL_CYC_COUNTER(compress);
    
    ASSERT(pgp != NULL);
    ASSERT(pgp->obj != NULL);
    ASSERT_SPINLOCK(&pgp->obj->obj_spinlock);
    ASSERT(pgp->obj->pool != NULL);
    ASSERT(pgp->obj->pool->client != NULL);
#ifdef __i386__
    return -ENOMEM;
#endif
    if ( pgp->pfp != NULL )
        pgp_free_data(pgp, pgp->obj->pool);  /* FIXME... is this right? */
    START_CYC_COUNTER(compress);
    ret = tmh_compress_from_client(cmfn, &dst, &size);
    if ( (ret == -EFAULT) || (ret == 0) )
        goto out;
    else if ( (size == 0) || (size >= tmem_subpage_maxsize()) )
        ret = 0;
    else if ( (p = tmem_malloc_bytes(size,pgp->obj->pool)) == NULL )
        ret = -ENOMEM;
    else
    {
        memcpy(p,dst,size);
        pgp->cdata = p;
        pgp->size = size;
        pgp->obj->pool->client->compressed_pages++;
        pgp->obj->pool->client->compressed_sum_size += size;
        ret = 1;
    }

out:
    END_CYC_COUNTER(compress);
    return ret;
}

static NOINLINE int do_tmem_dup_put(pgp_t *pgp, tmem_cli_mfn_t cmfn,
              uint32_t tmem_offset, uint32_t pfn_offset, uint32_t len)
{
    pool_t *pool;
    obj_t *obj;
    client_t *client;
    pgp_t *pgpfound = NULL;
    int ret;

    /* if we can successfully manipulate pgp to change out the data, do so */
    ASSERT(pgp != NULL);
    ASSERT(pgp->pfp != NULL);
    ASSERT(pgp->size != -1);
    obj = pgp->obj;
    ASSERT_SPINLOCK(&obj->obj_spinlock);
    ASSERT(obj != NULL);
    pool = obj->pool;
    ASSERT(pool != NULL);
    client = pool->client;
    if ( len != 0 && tmh_compression_enabled() &&
         client->compress && pgp->size != 0 )
    {
        ret = do_tmem_put_compress(pgp,cmfn);
        if ( ret == 1 )
            goto done;
        else if ( ret == 0 )
            goto copy_uncompressed;
        else if ( ret == -ENOMEM )
            goto failed_dup;
        else if ( ret == -EFAULT )
            goto bad_copy;
    }

copy_uncompressed:
    if ( pgp->pfp )
        pgp_free_data(pgp, pool);
    if ( ( pgp->pfp = tmem_page_alloc(pool) ) == NULL )
        goto failed_dup;
    /* tmh_copy_from_client properly handles len==0 and offsets != 0 */
    ret = tmh_copy_from_client(pgp->pfp,cmfn,tmem_offset,pfn_offset,len);
    if ( ret == -EFAULT )
        goto bad_copy;
    pgp->size = 0;

done:
    /* successfully replaced data, clean up and return success */
    if ( is_shared(pool) )
        obj->last_client = client->cli_id;
    obj->no_evict = 0;
    tmem_spin_unlock(&obj->obj_spinlock);
    pool->dup_puts_replaced++;
    pool->good_puts++;
    return 1;

bad_copy:
    /* this should only happen if the client passed a bad mfn */
    failed_copies++;
ASSERT(0);
    return -EFAULT;

failed_dup:
   /* couldn't change out the data, flush the old data and return
    * -ENOSPC instead of -ENOMEM to differentiate failed _dup_ put */
    pgpfound = pgp_delete_from_obj(obj, pgp->index);
    ASSERT(pgpfound == pgp);
    pgp_delete(pgpfound,0);
    if ( obj->pgp_count == 0 )
    {
        tmem_write_lock(&pool->pool_rwlock);
        obj_free(obj,0);
        tmem_write_unlock(&pool->pool_rwlock);
    } else {
        obj->no_evict = 0;
        tmem_spin_unlock(&obj->obj_spinlock);
    }
    pool->dup_puts_flushed++;
    return -ENOSPC;
}


static NOINLINE int do_tmem_put(pool_t *pool, uint64_t oid, uint32_t index,
              tmem_cli_mfn_t cmfn, uint32_t tmem_offset,
              uint32_t pfn_offset, uint32_t len)
{
    obj_t *obj = NULL, *objfound = NULL, *objnew = NULL;
    pgp_t *pgp = NULL, *pgpdel = NULL;
    client_t *client = pool->client;
    int ret = client->frozen ? -EFROZEN : -ENOMEM;

    ASSERT(pool != NULL);
    pool->puts++;
    /* does page already exist (dup)?  if so, handle specially */
    if ( (obj = objfound = obj_find(pool,oid)) != NULL )
    {
        ASSERT_SPINLOCK(&objfound->obj_spinlock);
        if ((pgp = pgp_lookup_in_obj(objfound, index)) != NULL)
            return do_tmem_dup_put(pgp,cmfn,tmem_offset,pfn_offset,len);
    }

    /* no puts allowed into a frozen pool (except dup puts) */
    if ( client->frozen )
        goto free;

    if ( (objfound == NULL) )
    {
        tmem_write_lock(&pool->pool_rwlock);
        if ( (obj = objnew = obj_new(pool,oid)) == NULL )
        {
            tmem_write_unlock(&pool->pool_rwlock);
            return -ENOMEM;
        }
        ASSERT_SPINLOCK(&objnew->obj_spinlock);
        tmem_write_unlock(&pool->pool_rwlock);
    }

    ASSERT((obj != NULL)&&((objnew==obj)||(objfound==obj))&&(objnew!=objfound));
    ASSERT_SPINLOCK(&obj->obj_spinlock);
    if ( (pgp = pgp_alloc(obj)) == NULL )
        goto free;

    ret = pgp_add_to_obj(obj, index, pgp);
    if ( ret == -ENOMEM  )
        /* warning, may result in partially built radix tree ("stump") */
        goto free;
    ASSERT(ret != -EEXIST);
    pgp->index = index;

    if ( len != 0 && tmh_compression_enabled() && client->compress )
    {
        ASSERT(pgp->pfp == NULL);
        ret = do_tmem_put_compress(pgp,cmfn);
        if ( ret == 1 )
            goto insert_page;
        if ( ret == -ENOMEM )
        {
            client->compress_nomem++;
            goto delete_and_free;
        }
        if ( ret == 0 )
        {
            client->compress_poor++;
            goto copy_uncompressed;
        }
        if ( ret == -EFAULT )
            goto bad_copy;
    }

copy_uncompressed:
    if ( ( pgp->pfp = tmem_page_alloc(pool) ) == NULL )
    {
        ret == -ENOMEM;
        goto delete_and_free;
    }
    /* tmh_copy_from_client properly handles len==0 (TMEM_NEW_PAGE) */
    ret = tmh_copy_from_client(pgp->pfp,cmfn,tmem_offset,pfn_offset,len);
    if ( ret == -EFAULT )
        goto bad_copy;
    pgp->size = 0;

insert_page:
    if ( is_ephemeral(pool) )
    {
        tmem_spin_lock(&eph_lists_spinlock);
        list_add_tail(&pgp->global_eph_pages,
            &global_ephemeral_page_list);
        if (++global_eph_count > global_eph_count_max)
            global_eph_count_max = global_eph_count;
        list_add_tail(&pgp->client_eph_pages,
            &client->ephemeral_page_list);
        if (++client->eph_count > client->eph_count_max)
            client->eph_count_max = client->eph_count;
        tmem_spin_unlock(&eph_lists_spinlock);
    }
    ASSERT( ((objnew==obj)||(objfound==obj)) && (objnew!=objfound));
    if ( is_shared(pool) )
        obj->last_client = client->cli_id;
    obj->no_evict = 0;
    tmem_spin_unlock(&obj->obj_spinlock);
    pool->good_puts++;
    return 1;

delete_and_free:
    ASSERT((obj != NULL) && (pgp != NULL) && (pgp->index != -1));
    pgpdel = pgp_delete_from_obj(obj, pgp->index);
    ASSERT(pgp == pgpdel);

free:
    if ( pgp )
        pgp_delete(pgp,0);
    if ( objfound )
    {
        objfound->no_evict = 0;
        tmem_spin_unlock(&objfound->obj_spinlock);
    }
    if ( objnew )
    {
        tmem_write_lock(&pool->pool_rwlock);
        obj_free(objnew,0);
        tmem_write_unlock(&pool->pool_rwlock);
    }
    pool->no_mem_puts++;
    return ret;

bad_copy:
    /* this should only happen if the client passed a bad mfn */
    failed_copies++;
ASSERT(0);
    goto free;
}

static NOINLINE int do_tmem_get(pool_t *pool, uint64_t oid, uint32_t index,
              tmem_cli_mfn_t cmfn, uint32_t tmem_offset,
              uint32_t pfn_offset, uint32_t len)
{
    obj_t *obj;
    pgp_t *pgp;
    client_t *client = pool->client;
    DECL_LOCAL_CYC_COUNTER(decompress);

    if ( !_atomic_read(pool->pgp_count) )
        return -EEMPTY;

    pool->gets++;
    obj = obj_find(pool,oid);
    if ( obj == NULL )
        return 0;

    ASSERT_SPINLOCK(&obj->obj_spinlock);
    if (is_shared(pool) || is_persistent(pool) )
        pgp = pgp_lookup_in_obj(obj, index);
    else
        pgp = pgp_delete_from_obj(obj, index);
    if ( pgp == NULL )
    {
        obj->no_evict = 0;
        tmem_spin_unlock(&obj->obj_spinlock);
        return 0;
    }
    ASSERT(pgp->size != -1);
    if ( pgp->size != 0 )
    {
        START_CYC_COUNTER(decompress);
        if ( tmh_decompress_to_client(cmfn, pgp->cdata, pgp->size) == -EFAULT )
            goto bad_copy;
        END_CYC_COUNTER(decompress);
    }
    else if ( tmh_copy_to_client(cmfn, pgp->pfp, tmem_offset,
                                 pfn_offset, len) == -EFAULT)
        goto bad_copy;
    if ( is_ephemeral(pool) )
    {
        if ( is_private(pool) )
        {
            pgp_delete(pgp,0);
            if ( obj->pgp_count == 0 )
            {
                tmem_write_lock(&pool->pool_rwlock);
                obj_free(obj,0);
                obj = NULL;
                tmem_write_unlock(&pool->pool_rwlock);
            }
        } else {
            tmem_spin_lock(&eph_lists_spinlock);
            list_del(&pgp->global_eph_pages);
            list_add_tail(&pgp->global_eph_pages,&global_ephemeral_page_list);
            list_del(&pgp->client_eph_pages);
            list_add_tail(&pgp->client_eph_pages,&client->ephemeral_page_list);
            tmem_spin_unlock(&eph_lists_spinlock);
            ASSERT(obj != NULL);
            obj->last_client = tmh_get_cli_id_from_current();
        }
    }
    if ( obj != NULL )
    {
        obj->no_evict = 0;
        tmem_spin_unlock(&obj->obj_spinlock);
    }
    pool->found_gets++;
    return 1;

bad_copy:
    /* this should only happen if the client passed a bad mfn */
    failed_copies++;
ASSERT(0);
    return -EFAULT;

}

static NOINLINE int do_tmem_flush_page(pool_t *pool, uint64_t oid, uint32_t index)
{
    obj_t *obj;
    pgp_t *pgp;

    pool->flushs++;
    obj = obj_find(pool,oid);
    if ( obj == NULL )
        goto out;
    pgp = pgp_delete_from_obj(obj, index);
    if ( pgp == NULL )
    {
        obj->no_evict = 0;
        tmem_spin_unlock(&obj->obj_spinlock);
        goto out;
    }
    pgp_delete(pgp,0);
    if ( obj->pgp_count == 0 )
    {
        tmem_write_lock(&pool->pool_rwlock);
        obj_free(obj,0);
        tmem_write_unlock(&pool->pool_rwlock);
    } else {
        obj->no_evict = 0;
        tmem_spin_unlock(&obj->obj_spinlock);
    }
    pool->flushs_found++;

out:
    if ( pool->client->frozen )
        return -EFROZEN;
    else
        return 1;
}

static NOINLINE int do_tmem_flush_object(pool_t *pool, uint64_t oid)
{
    obj_t *obj;

    pool->flush_objs++;
    obj = obj_find(pool,oid);
    if ( obj == NULL )
        goto out;
    tmem_write_lock(&pool->pool_rwlock);
    obj_destroy(obj);
    pool->flush_objs_found++;
    tmem_write_unlock(&pool->pool_rwlock);

out:
    if ( pool->client->frozen )
        return -EFROZEN;
    else
        return 1;
}

static NOINLINE int do_tmem_destroy_pool(uint32_t pool_id)
{
    client_t *client = tmh_client_from_current();
    pool_t *pool;

    if ( client->pools == NULL )
        return 0;
    if ( (pool = client->pools[pool_id]) == NULL )
        return 0;
    client->pools[pool_id] = NULL;
    pool_flush(pool,client->cli_id,1);
    return 1;
}

static NOINLINE int do_tmem_new_pool(uint32_t flags, uint64_t uuid_lo, uint64_t uuid_hi)
{
    client_t *client = tmh_client_from_current();
    cli_id_t cli_id = tmh_get_cli_id_from_current();
    int persistent = flags & TMEM_POOL_PERSIST;
    int shared = flags & TMEM_POOL_SHARED;
    int pagebits = (flags >> TMEM_POOL_PAGESIZE_SHIFT)
         & TMEM_POOL_PAGESIZE_MASK;
    int specversion = (flags >> TMEM_POOL_VERSION_SHIFT)
         & TMEM_POOL_VERSION_MASK;
    pool_t *pool, *shpool;
    int s_poolid, d_poolid, first_unused_s_poolid;

    ASSERT(client != NULL);
    printk("tmem: allocating %s-%s tmem pool for %s=%d...",
        persistent ? "persistent" : "ephemeral" ,
        shared ? "shared" : "private", cli_id_str, cli_id);
    if ( specversion != 0 )
    {
        printk("failed... unsupported spec version\n");
        return -EPERM;
    }
    if ( pagebits != (PAGE_SHIFT - 12) )
    {
        printk("failed... unsupported pagesize %d\n",1<<(pagebits+12));
        return -EPERM;
    }
    if ( (pool = pool_alloc()) == NULL )
    {
        printk("failed... out of memory\n");
        return -ENOMEM;
    }
    for ( d_poolid = 0; d_poolid < MAX_POOLS_PER_DOMAIN; d_poolid++ )
        if ( client->pools[d_poolid] == NULL )
            break;
    if ( d_poolid == MAX_POOLS_PER_DOMAIN )
    {
        printk("failed... no more pool slots available for this %s\n",
            client_str);
        goto fail;
    }
    pool->shared = shared;
    pool->client = client;
    if ( shared )
    {
        first_unused_s_poolid = MAX_GLOBAL_SHARED_POOLS;
        for ( s_poolid = 0; s_poolid < MAX_GLOBAL_SHARED_POOLS; s_poolid++ )
        {
            if ( (shpool = global_shared_pools[s_poolid]) != NULL )
            {
                if ( shpool->uuid[0] == uuid_lo && shpool->uuid[1] == uuid_hi )
                {
                    printk("(matches shared pool uuid=%"PRIx64".%"PRIu64") ",
                        uuid_hi, uuid_lo);
                    printk("pool_id=%d\n",d_poolid);
                    client->pools[d_poolid] = global_shared_pools[s_poolid];
                    shared_pool_join(global_shared_pools[s_poolid], client);
                    pool_free(pool);
                    return d_poolid;
                }
            }
            else if ( first_unused_s_poolid == MAX_GLOBAL_SHARED_POOLS )
                first_unused_s_poolid = s_poolid;
        }
        if ( first_unused_s_poolid == MAX_GLOBAL_SHARED_POOLS )
        {
            printk("tmem: failed... no global shared pool slots available\n");
            goto fail;
        }
        else
        {
            INIT_LIST_HEAD(&pool->share_list);
            pool->shared_count = 0;
            global_shared_pools[first_unused_s_poolid] = pool;
            (void)shared_pool_join(pool,client);
        }
    }
    client->pools[d_poolid] = pool;
    list_add_tail(&pool->pool_list, &global_pool_list);
    pool->pool_id = d_poolid;
    pool->persistent = persistent;
    pool->uuid[0] = uuid_lo; pool->uuid[1] = uuid_hi;
    printk("pool_id=%d\n",d_poolid);
    return d_poolid;

fail:
    pool_free(pool);
    return -EPERM;
}

/************ TMEM CONTROL OPERATIONS ************************************/

/* freeze/thaw all pools belonging to client cli_id (all domains if -1) */
static int tmemc_freeze_pools(int cli_id, int arg)
{
    client_t *client;
    bool_t freeze = (arg == TMEMC_FREEZE) ? 1 : 0;
    bool_t destroy = (arg == TMEMC_DESTROY) ? 1 : 0;
    char *s;

    s = destroy ? "destroyed" : ( freeze ? "frozen" : "thawed" );
    if ( cli_id == CLI_ID_NULL )
    {
        list_for_each_entry(client,&global_client_list,client_list)
        {
            client->frozen = freeze;
            printk("tmem: all pools %s for all %ss\n",s,client_str);
        }
    }
    else
    {
        if ( (client = tmh_client_from_cli_id(cli_id)) == NULL)
            return -1;
        client->frozen = freeze;
        printk("tmem: all pools %s for %s=%d\n",s,cli_id_str,cli_id);
    }
    return 0;
}

static int tmemc_flush_mem(int cli_id, uint32_t kb)
{
    uint32_t npages, flushed_pages, flushed_kb;

    if ( cli_id != CLI_ID_NULL )
    {
        printk("tmem: %s-specific flush not supported yet, use --all\n",
           client_str);
        return -1;
    }
    /* convert kb to pages, rounding up if necessary */
    npages = (kb + ((1 << (PAGE_SHIFT-10))-1)) >> (PAGE_SHIFT-10);
    flushed_pages = tmem_relinquish_npages(npages);
    flushed_kb = flushed_pages << (PAGE_SHIFT-10);
    return flushed_kb;
}

/*
 * These tmemc_list* routines output lots of stats in a format that is
 *  intended to be program-parseable, not human-readable. Further, by
 *  tying each group of stats to a line format indicator (e.g. G= for
 *  global stats) and each individual stat to a two-letter specifier
 *  (e.g. Ec:nnnnn in the G= line says there are nnnnn pages in the
 *  global ephemeral pool), it should allow the stats reported to be
 *  forward and backwards compatible as tmem evolves.
 */
#define BSIZE 1024

static int tmemc_list_client(client_t *c, tmem_cli_va_t buf, int off, 
                             uint32_t len, bool_t use_long)
{
    char info[BSIZE];
    int i, n = 0, sum = 0;
    pool_t *p;
    bool_t s;

    n = scnprintf(info,BSIZE,"C=CI:%d,ww:%d,ca:%d,co:%d,fr:%d%c",
        c->cli_id, c->weight, c->cap, c->compress,
        c->frozen, use_long ? ',' : '\n');
    if (use_long)
        n += scnprintf(info+n,BSIZE-n,
             "Ec:%ld,Em:%ld,cp:%ld,cb:%lld,cn:%ld,cm:%ld\n",
             c->eph_count, c->eph_count_max,
             c->compressed_pages, (long long)c->compressed_sum_size,
             c->compress_poor, c->compress_nomem);
    tmh_copy_to_client_buf_offset(buf,off+sum,info,n+1);
    sum += n;
    for ( i = 0; i < MAX_POOLS_PER_DOMAIN; i++ )
    {
        if ( (p = c->pools[i]) == NULL )
            continue;
        s = is_shared(p);
        n = scnprintf(info,BSIZE,"P=CI:%d,PI:%d,PT:%c%c,U0:%llx,U1:%llx%c",
             c->cli_id, p->pool_id,
             is_persistent(p) ? 'P' : 'E', s ? 'S' : 'P',
             s ? p->uuid[0] : 0LL, s ? p->uuid[1] : 0LL,
             use_long ? ',' : '\n');
        if (use_long)
            n += scnprintf(info+n,BSIZE-n,
             "Pc:%d,Pm:%d,Oc:%ld,Om:%ld,Nc:%lu,Nm:%lu,"
             "ps:%lu,pt:%lu,pd:%lu,pr:%lu,px:%lu,gs:%lu,gt:%lu,"
             "fs:%lu,ft:%lu,os:%lu,ot:%lu\n",
             _atomic_read(p->pgp_count), p->pgp_count_max,
             p->obj_count, p->obj_count_max,
             p->objnode_count, p->objnode_count_max,
             p->good_puts, p->puts,p->dup_puts_flushed, p->dup_puts_replaced,
             p->no_mem_puts, 
             p->found_gets, p->gets,
             p->flushs_found, p->flushs, p->flush_objs_found, p->flush_objs);
        if ( sum + n >= len )
            return sum;
        tmh_copy_to_client_buf_offset(buf,off+sum,info,n+1);
        sum += n;
    }
    return sum;
}

static int tmemc_list_shared(tmem_cli_va_t buf, int off, uint32_t len,
                              bool_t use_long)
{
    char info[BSIZE];
    int i, n = 0, sum = 0;
    pool_t *p;
    sharelist_t *sl;

    for ( i = 0; i < MAX_GLOBAL_SHARED_POOLS; i++ )
    {
        if ( (p = global_shared_pools[i]) == NULL )
            continue;
        n = scnprintf(info+n,BSIZE-n,"S=SI:%d,PT:%c%c,U0:%llx,U1:%llx",
            i, is_persistent(p) ? 'P' : 'E', is_shared(p) ? 'S' : 'P',
             (unsigned long long)p->uuid[0], (unsigned long long)p->uuid[1]);
        list_for_each_entry(sl,&p->share_list, share_list)
            n += scnprintf(info+n,BSIZE-n,",SC:%d",sl->client->cli_id);
        n += scnprintf(info+n,BSIZE-n,"%c", use_long ? ',' : '\n');
        if (use_long)
            n += scnprintf(info+n,BSIZE-n,
             "Pc:%d,Pm:%d,Oc:%ld,Om:%ld,Nc:%lu,Nm:%lu,"
             "ps:%lu,pt:%lu,pd:%lu,pr:%lu,px:%lu,gs:%lu,gt:%lu,"
             "fs:%lu,ft:%lu,os:%lu,ot:%lu\n",
             _atomic_read(p->pgp_count), p->pgp_count_max,
             p->obj_count, p->obj_count_max,
             p->objnode_count, p->objnode_count_max,
             p->good_puts, p->puts,p->dup_puts_flushed, p->dup_puts_replaced,
             p->no_mem_puts, 
             p->found_gets, p->gets,
             p->flushs_found, p->flushs, p->flush_objs_found, p->flush_objs);
        if ( sum + n >= len )
            return sum;
        tmh_copy_to_client_buf_offset(buf,off+sum,info,n+1);
        sum += n;
    }
    return sum;
}

#ifdef TMEM_PERF
static int tmemc_list_global_perf(tmem_cli_va_t buf, int off, uint32_t len,
                              bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    n = scnprintf(info+n,BSIZE-n,"T=");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,succ_get,"G");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,succ_put,"P");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,non_succ_get,"g");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,non_succ_put,"p");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,flush,"F");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,flush_obj,"O");
#ifdef COMPARE_COPY_PAGE_SSE2
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,pg_copy1,"1");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,pg_copy2,"2");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,pg_copy3,"3");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,pg_copy4,"4");
#else
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,pg_copy,"C");
#endif
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,compress,"c");
    n += SCNPRINTF_CYC_COUNTER(info+n,BSIZE-n,decompress,"d");
    n--; /* overwrite trailing comma */
    n += scnprintf(info+n,BSIZE-n,"\n");
    if ( sum + n >= len )
        return sum;
    tmh_copy_to_client_buf_offset(buf,off+sum,info,n+1);
    sum += n;
    return sum;
}
#else
#define tmemc_list_global_perf(_buf,_off,_len,_use) (0)
#endif

static int tmemc_list_global(tmem_cli_va_t buf, int off, uint32_t len,
                              bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = off;

    n += scnprintf(info,BSIZE,"G="
      "Tt:%lu,Te:%lu,Cf:%lu,Af:%lu,Pf:%lu,Ta:%lu,"
      "Lm:%lu,Et:%lu,Ea:%lu,Rt:%lu,Ra:%lu,Rx:%lu,Fp:%lu%c",
      total_tmem_ops, errored_tmem_ops, failed_copies,
      alloc_failed, alloc_page_failed, tmh_avail_pages(),
      low_on_memory, evicted_pgs,
      evict_attempts, relinq_pgs, relinq_attempts, max_evicts_per_relinq,
      total_flush_pool, use_long ? ',' : '\n');
    if (use_long)
        n += scnprintf(info+n,BSIZE-n,
          "Ec:%ld,Em:%ld,Oc:%d,Om:%d,Nc:%d,Nm:%d,Pc:%d,Pm:%d\n",
          global_eph_count, global_eph_count_max,
          _atomic_read(global_obj_count), global_obj_count_max,
          _atomic_read(global_rtree_node_count), global_rtree_node_count_max,
          _atomic_read(global_pgp_count), global_pgp_count_max);
    if ( sum + n >= len )
        return sum;
    tmh_copy_to_client_buf_offset(buf,off+sum,info,n+1);
    sum += n;
    return sum;
}

static int tmemc_list(int cli_id, tmem_cli_va_t buf, uint32_t len,
                               bool_t use_long)
{
    client_t *client;
    int off = 0;

    if ( cli_id == CLI_ID_NULL ) {
        off = tmemc_list_global(buf,0,len,use_long);
        off += tmemc_list_shared(buf,off,len-off,use_long);
        list_for_each_entry(client,&global_client_list,client_list)
            off += tmemc_list_client(client, buf, off, len-off, use_long);
        off += tmemc_list_global_perf(buf,off,len-off,use_long);
    }
    else if ( (client = tmh_client_from_cli_id(cli_id)) == NULL)
        return -1;
    else
        off = tmemc_list_client(client, buf, 0, len, use_long);


    return 0;
}

static int tmemc_set_var_one(client_t *client, uint32_t subop, uint32_t arg1)
{
    cli_id_t cli_id = client->cli_id;
    uint32_t old_weight;

    switch (subop)
    {
    case TMEMC_SET_WEIGHT:
        old_weight = client->weight;
        client->weight = arg1;
        printk("tmem: weight set to %d for %s=%d\n",arg1,cli_id_str,cli_id);
        atomic_sub(old_weight,&client_weight_total);
        atomic_add(client->weight,&client_weight_total);
        break;
    case TMEMC_SET_CAP:
        client->cap = arg1;
        printk("tmem: cap set to %d for %s=%d\n",arg1,cli_id_str,cli_id);
        break;
    case TMEMC_SET_COMPRESS:
        client->compress = arg1 ? 1 : 0;
        printk("tmem: compression %s for %s=%d\n",
            arg1 ? "enabled" : "disabled",cli_id_str,cli_id);
        break;
    default:
        printk("tmem: unknown subop %d for tmemc_set_var\n",subop);
        return -1;
    }
    return 0;
}

static int tmemc_set_var(int cli_id, uint32_t subop, uint32_t arg1)
{
    client_t *client;

    if ( cli_id == CLI_ID_NULL )
        list_for_each_entry(client,&global_client_list,client_list)
            tmemc_set_var_one(client, subop, arg1);
    else if ( (client = tmh_client_from_cli_id(cli_id)) == NULL)
        return -1;
    else
            tmemc_set_var_one(client, subop, arg1);
    return 0;
}

static int do_tmem_control(uint32_t subop, uint32_t cli_id32,
   uint32_t arg1, uint32_t arg2, tmem_cli_va_t buf)
{
    int ret;
    cli_id_t cli_id = (cli_id_t)cli_id32;

    if (!tmh_current_is_privileged())
    {
        /* don't fail... mystery: sometimes dom0 fails here */
        /* return -EPERM; */
    }
    switch(subop)
    {
    case TMEMC_THAW:
    case TMEMC_FREEZE:
    case TMEMC_DESTROY:
        ret = tmemc_freeze_pools(cli_id,subop);
        break;
    case TMEMC_FLUSH:
        ret = tmemc_flush_mem(cli_id,arg1);
        break;
    case TMEMC_LIST:
        ret = tmemc_list(cli_id,buf,arg1,arg2);
        break;
    case TMEMC_SET_WEIGHT:
    case TMEMC_SET_CAP:
    case TMEMC_SET_COMPRESS:
        ret = tmemc_set_var(cli_id,subop,arg1);
        break;
    default:
        ret = -1;
    }
    return ret;
}

/************ EXPORTed FUNCTIONS **************************************/

EXPORT long do_tmem_op(tmem_cli_op_t uops)
{
    struct tmem_op op;
    client_t *client = tmh_client_from_current();
    pool_t *pool = NULL;
    int rc = 0;
    bool_t succ_get = 0, succ_put = 0;
    bool_t non_succ_get = 0, non_succ_put = 0;
    bool_t flush = 0, flush_obj = 0;
    bool_t tmem_write_lock_set = 0, tmem_read_lock_set = 0;
    static bool_t warned = 0;
    DECL_LOCAL_CYC_COUNTER(succ_get);
    DECL_LOCAL_CYC_COUNTER(succ_put);
    DECL_LOCAL_CYC_COUNTER(non_succ_get);
    DECL_LOCAL_CYC_COUNTER(non_succ_put);
    DECL_LOCAL_CYC_COUNTER(flush);
    DECL_LOCAL_CYC_COUNTER(flush_obj);

    if ( !tmem_initialized )
    {
        if ( !warned )
            printk("tmem: must specify tmem parameter on xen boot line\n");
        warned = 1;
        return -ENODEV;
    }

    total_tmem_ops++;

    if ( tmh_lock_all )
    {
        if ( tmh_lock_all > 1 )
            spin_lock_irq(&tmem_spinlock);
        else
            spin_lock(&tmem_spinlock);
    }

    START_CYC_COUNTER(succ_get);
    DUP_START_CYC_COUNTER(succ_put,succ_get);
    DUP_START_CYC_COUNTER(non_succ_get,succ_get);
    DUP_START_CYC_COUNTER(non_succ_put,succ_get);
    DUP_START_CYC_COUNTER(flush,succ_get);
    DUP_START_CYC_COUNTER(flush_obj,succ_get);

    if ( unlikely(tmh_get_tmemop_from_client(&op, uops) != 0) )
    {
        printk("tmem: can't get tmem struct from %s\n",client_str);
        rc = -EFAULT;
        goto out;
    }

    if ( op.cmd == TMEM_CONTROL )
    {
        tmem_write_lock(&tmem_rwlock);
        tmem_write_lock_set = 1;
        rc = do_tmem_control(op.subop, op.cli_id, op.arg1, op.arg2, op.buf);
        goto out;
    }

    /* create per-client tmem structure dynamically on first use by client */
    if ( client == NULL )
    {
        tmem_write_lock(&tmem_rwlock);
        tmem_write_lock_set = 1;
        if ( (client = client_create()) == NULL )
        {
            printk("tmem: can't create tmem structure for %s\n",client_str);
            rc = -ENOMEM;
            goto out;
        }
    }

    if ( op.cmd == TMEM_NEW_POOL )
    {
        if ( !tmem_write_lock_set )
        {
            tmem_write_lock(&tmem_rwlock);
            tmem_write_lock_set = 1;
        }
    }
    else
    {
        if ( !tmem_write_lock_set )
        {
            tmem_read_lock(&tmem_rwlock);
            tmem_read_lock_set = 1;
        }
        if ( ((uint32_t)op.pool_id >= MAX_POOLS_PER_DOMAIN) ||
             ((pool = client->pools[op.pool_id]) == NULL) )
        {
            rc = -ENODEV;
            printk("tmem: operation requested on uncreated pool\n");
            goto out;
        }
        ASSERT_SENTINEL(pool,POOL);
    }

    switch ( op.cmd )
    {
    case TMEM_NEW_POOL:
        rc = do_tmem_new_pool(op.flags,op.uuid[0],op.uuid[1]);
        break;
    case TMEM_NEW_PAGE:
        rc = do_tmem_put(pool, op.object, op.index, op.cmfn, 0, 0, 0);
        break;
    case TMEM_PUT_PAGE:
        rc = do_tmem_put(pool, op.object, op.index, op.cmfn, 0, 0, PAGE_SIZE);
        if (rc == 1) succ_put = 1;
        else non_succ_put = 1;
        break;
    case TMEM_GET_PAGE:
        rc = do_tmem_get(pool, op.object, op.index, op.cmfn, 0, 0, PAGE_SIZE);
        if (rc == 1) succ_get = 1;
        else non_succ_get = 1;
        break;
    case TMEM_FLUSH_PAGE:
        flush = 1;
        rc = do_tmem_flush_page(pool, op.object, op.index);
        break;
    case TMEM_FLUSH_OBJECT:
        rc = do_tmem_flush_object(pool, op.object);
        flush_obj = 1;
        break;
    case TMEM_DESTROY_POOL:
        flush = 1;
        rc = do_tmem_destroy_pool(op.pool_id);
        break;
    case TMEM_READ:
        rc = do_tmem_get(pool, op.object, op.index, op.cmfn,
                         op.tmem_offset, op.pfn_offset, op.len);
        break;
    case TMEM_WRITE:
        rc = do_tmem_put(pool, op.object, op.index, op.cmfn,
                         op.tmem_offset, op.pfn_offset, op.len);
        break;
    case TMEM_XCHG:
        /* need to hold global lock to ensure xchg is atomic */
        printk("tmem_xchg op not implemented yet\n");
        rc = 0;
        break;
    default:
        printk("tmem: op %d not implemented\n", op.cmd);
        rc = 0;
        break;
    }

out:
    if ( rc < 0 )
        errored_tmem_ops++;
    if ( succ_get )
        END_CYC_COUNTER(succ_get);
    else if ( succ_put )
        END_CYC_COUNTER(succ_put);
    else if ( non_succ_get )
        END_CYC_COUNTER(non_succ_get);
    else if ( non_succ_put )
        END_CYC_COUNTER(non_succ_put);
    else if ( flush )
        END_CYC_COUNTER(flush);
    else
        END_CYC_COUNTER(flush_obj);

    if ( tmh_lock_all )
    {
        if ( tmh_lock_all > 1 )
            spin_unlock_irq(&tmem_spinlock);
        else
            spin_unlock(&tmem_spinlock);
    } else {
        if ( tmem_write_lock_set )
            write_unlock(&tmem_rwlock);
        else if ( tmem_read_lock_set )
            read_unlock(&tmem_rwlock);
        else 
            ASSERT(0);
    }

    return rc;
}

/* this should be called when the host is destroying a client */
EXPORT void tmem_destroy(void *v)
{
    client_t *client = (client_t *)v;

    if ( tmh_lock_all )
        spin_lock(&tmem_spinlock);
    else
        write_lock(&tmem_rwlock);

    if ( client == NULL )
        printk("tmem: can't destroy tmem pools for %s=%d\n",
               cli_id_str,client->cli_id);
    else
    {
        printk("tmem: flushing tmem pools for %s=%d\n",
               cli_id_str,client->cli_id);
        client_flush(client,1);
    }

    if ( tmh_lock_all )
        spin_unlock(&tmem_spinlock);
    else
        write_unlock(&tmem_rwlock);
}

/* freezing all pools guarantees that no additional memory will be consumed */
EXPORT void tmem_freeze_all(unsigned char key)
{
    static int freeze = 0;
 
    if ( tmh_lock_all )
        spin_lock(&tmem_spinlock);
    else
        write_lock(&tmem_rwlock);

    freeze = !freeze;
    tmemc_freeze_pools(CLI_ID_NULL,freeze);

    if ( tmh_lock_all )
        spin_unlock(&tmem_spinlock);
    else
        write_unlock(&tmem_rwlock);
}

#define MAX_EVICTS 10  /* should be variable or set via TMEMC_ ?? */

EXPORT void *tmem_relinquish_pages(unsigned int order, unsigned int memflags)
{
    pfp_t *pfp;
    unsigned long evicts_per_relinq = 0;
    int max_evictions = 10;

    if (!tmh_enabled())
        return NULL;
#ifdef __i386__
    return NULL;
#endif

    relinq_attempts++;
    if ( order > 0 )
    {
        printk("tmem_relinquish_page: failing order=%d\n", order);
        return NULL;
    }

    if ( tmh_called_from_tmem(memflags) )
    {
        if ( tmh_lock_all )
            spin_lock(&tmem_spinlock);
        else
            read_lock(&tmem_rwlock);
    }

    while ( (pfp = tmh_alloc_page(NULL,1)) == NULL )
    {
        if ( (max_evictions-- <= 0) || !tmem_evict())
            break;
        evicts_per_relinq++;
    }
    if ( evicts_per_relinq > max_evicts_per_relinq )
        max_evicts_per_relinq = evicts_per_relinq;
    tmh_scrub_page(pfp, memflags);
    if ( pfp != NULL )
        relinq_pgs++;

    if ( tmh_called_from_tmem(memflags) )
    {
        if ( tmh_lock_all )
            spin_unlock(&tmem_spinlock);
        else
            read_unlock(&tmem_rwlock);
    }

    return pfp;
}

/* called at hypervisor startup */
EXPORT void init_tmem(void)
{
    if ( !tmh_enabled() )
        return;

    radix_tree_init();
    if ( tmh_init() )
    {
        printk("tmem: initialized comp=%d global-lock=%d\n",
            tmh_compression_enabled(), tmh_lock_all);
        tmem_initialized = 1;
    }
    else
        printk("tmem: initialization FAILED\n");
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

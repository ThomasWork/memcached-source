/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

//首先定义了三个互斥锁
static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_items_counter_lock = PTHREAD_MUTEX_INITIALIZER;//在更新item的数量时加锁

typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */


/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;//初始值为16

#define hashsize(n) ((ub4)1<<(n))
#define hashmask(n) (hashsize(n)-1)

//主哈希表，这是我们查找的地方除非是在扩展过程中
static item** primary_hashtable = 0;//二维数组，每一元素是一个桶，
 //旧的哈希表。在扩展过程中，从这里查找还没有被移动到主表的键值
static item** old_hashtable = 0;
//hash表中有多少元素
static unsigned int hash_items = 0;//assoc_insert中调用
//标记: 指示是否正处于扩展过程中
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how//间隔尺寸，粒度
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
 //在扩展过程中，我们使用桶(bucket)的粒度来进行迁
 //具体地从0一直到hashsize(hashpower-1)-1;，这里使用hashpower减1，是因为已经+1了�
static unsigned int expand_bucket = 0;

void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;//初始默认值为16
    }
    primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
    STATS_LOCK();
    stats_state.hash_power_level = hashpower;
    stats_state.hash_bytes = hashsize(hashpower) * sizeof(void *);//
    STATS_UNLOCK();
}

//因为可能处于扩容状态中，所以需要先定位到相应的桶，然后才能向下寻找
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

	//下面开始根据hash值查找链表，首先hash值相等，然后判断key相等
    item *ret = NULL;
    int depth = 0;
    while (it) {
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */
//返回在key前面的item的指针的地址
//如果该地址中保存的值为0，表示没有找到
static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {//首先比较长度，然后再比较内容
        pos = &(*pos)->h_next;
    }
    return pos;
}

//扩展哈希表到2的下一个指数
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;//让旧表指针指向当前指针

//calloc最大的不同时分配空间过后会初始化为0
    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));//将主表扩长，hashpower还没有变大
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;//这里变大了
        expanding = true;//表示正在扩容
        expand_bucket = 0;
        STATS_LOCK();
        stats_state.hash_power_level = hashpower;
        stats_state.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats_state.hash_is_expanding = true;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;//分配空间失败，仍然指向之前的地址
        /* Bad news, but we can keep running. */
    }
}

//在增加item计数锁时，该函数也在锁中，所以可以保证一致性
static void assoc_start_expand(void) {
    if (started_expanding)
        return;

    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);//设置条件变量，开始扩容
}

//这不是assoc_update，当调用这个函数时，key必须不存在
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;//加入到旧的hash表中
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    pthread_mutex_lock(&hash_items_counter_lock);
    hash_items++;
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {//达到1.5倍时开始扩展
        assoc_start_expand();
    }
    pthread_mutex_unlock(&hash_items_counter_lock);

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

//从hash 表中删除
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);//找到在该元素之前的元素的地址

    if (*before) {//如果找到了
        item *nxt;
        pthread_mutex_lock(&hash_items_counter_lock);
        hash_items--;
        pthread_mutex_unlock(&hash_items_counter_lock);
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}


static volatile int do_run_maintenance_thread = 1;

#define DEFAULT_HASH_BULK_MOVE 1//每次移动的桶的数量
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;


//这个线程是用来进行扩容的
static void *assoc_maintenance_thread(void *arg) {

    mutex_lock(&maintenance_lock);//这个锁是做什么用的，什么时间解开呢�
    while (do_run_maintenance_thread) {//这是一个死循环，只有将该值修改才不会跳出
        int ii = 0;

		//由于仅仅有一个扩展线程，所以不需要全局锁
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
            void *item_lock = NULL;

            /* bucket = hv & hashmask(hashpower) =>the bucket of hash table
             * is the lowest N bits of the hv, and the bucket of item_locks is
             *  also the lowest M bits of hv, and N is greater than M.
             *  So we can process expanding with only one item_lock. cool! */
             //bucket=hv & hashmask(hashpower)，因此哈希表的桶代表了哈希值最低的N bit
             // 由于item锁的桶也是哈希值的最低M 位，并且N 大于M，
             //所以我们可以仅仅使用一个item锁进行扩容，(一个item锁对应多个哈希桶)
            if ((item_lock = item_trylock(expand_bucket))) {//取得item锁并且这个锁已经加上了(pthread_mutex_trylock)
                    for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {//这里expand_bucket是全局变量，初始值为0
                        next = it->h_next;
                        bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);//注意这里使用的hashpower和锁的power不同
                        it->h_next = primary_hashtable[bucket];//放入新的桶中
                        primary_hashtable[bucket] = it;
                    }

                    old_hashtable[expand_bucket] = NULL;

                    expand_bucket++;
                    if (expand_bucket == hashsize(hashpower - 1)) {
                        expanding = false;//非正在扩容
                        free(old_hashtable);//释放旧的哈希表的空间
                        STATS_LOCK();
                        stats_state.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                        stats_state.hash_is_expanding = false;
                        STATS_UNLOCK();
                        if (settings.verbose > 1)
                            fprintf(stderr, "Hash table expansion done\n");
                    }

            } else {
                usleep(10*1000);//睡眠10秒钟，这里是否会有一直等待的情况出现?即锁一直 被占用
            }

            if (item_lock) {//如果锁定成功
                item_trylock_unlock(item_lock);//这里实际上调用的是unlock
                item_lock = NULL;
            }
        }//如果睡眠的话，因为有一个线程专门负责扩容，所以继续循环

        if (!expanding) {//扩容结束
            /* We are done expanding.. just wait for next invocation */
            started_expanding = false;
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);//在这里等待下一次通知
            /* assoc_expand() swaps out the hash table entirely, so we need
             * all threads to not hold any references related to the hash
             * table while this happens.
             * This is instead of a more complex, possibly slower algorithm to
             * allow dynamic hash table expansion without causing significant
             * wait times.
             */
             //assoc_expand 交换出整个哈希表，因此我们需要确保在此过程中
             //没有任何线程持有哈希表的引用
            pause_threads(PAUSE_ALL_THREADS);
            assoc_expand();//在这个函数中将expending设置为true，因为知道条件变量允许才会运行到这里
            pause_threads(RESUME_ALL_THREADS);
        }
    }
    return NULL;
}

static pthread_t maintenance_tid;

//扩容线程
int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;//设置默认移动几个hash桶
        }
    }
    pthread_mutex_init(&maintenance_lock, NULL);//初始化锁
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_assoc_maintenance_thread() {
    mutex_lock(&maintenance_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    mutex_unlock(&maintenance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}


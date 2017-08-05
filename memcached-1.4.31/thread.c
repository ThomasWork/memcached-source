/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "memcached.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __sun
#include <atomic.h>
#endif

#define ITEMS_PER_ALLOC 64

/* An item in the connection queue. */
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int               sfd;
    enum conn_states  init_state;//����״̬
    int               event_flags;
    int               read_buffer_size;
    enum network_transport     transport;//�������ͣ�TCP/UDP/����
    conn *c;
    CQ_ITEM          *next;
};

//���Ӷ���
typedef struct conn_queue CQ;
struct conn_queue {//ÿ�����Ӷ��ж���һ��������
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

// LRU ��
/* Locks for cache LRU operations */
pthread_mutex_t lru_locks[POWER_LARGEST];//lru ����ʱ�ӵ���


//�����������������Ƿ�����µ�����
pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;


//���û�ж������������� (֧��ԭ�Ӳ���) ��ô����һ��ԭ��������������ԭ�Ӳ���
//Ŀǰ��֪���Ƕ�item��refcount�����޸�
#if !defined(HAVE_GCC_ATOMICS) && !defined(__sun)
pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif


//Ϊȫ��״̬����������
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

//�ڹ����̱߳�����֮�������������߳�
static pthread_mutex_t worker_hang_lock;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;//���е�CQ_ITEM�ṹ
static pthread_mutex_t cqi_freelist_lock;

static pthread_mutex_t *item_locks;//�����̵߳����������������������item_lock_count
static uint32_t item_lock_count;//����hashsize(item_lock_hashpower)  2 ��item_lock_hashpower�η�
unsigned int item_lock_hashpower;//���Ϊ13�������߳���������
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)//���ֵ����& ������Ѹ�λ��0

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
 //ÿһ��libeventʵ����һ�����ѹܵ��������߳̿�������
 //��������һ���źŸ����������Ѿ������Ķ��������һ���µ�����
static LIBEVENT_THREAD *threads;//�����̹߳��ɵ����飬���ʾ����߳�ʱ��ʹ��threads+ƫ��������

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;//��ʼ�����������ȴ������߳�ע��
static pthread_cond_t init_cond;//��������


static void thread_libevent_process(int fd, short which, void *arg);

unsigned short refcount_incr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_inc_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

unsigned short refcount_decr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_dec_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

/* item_lock() must be held for an item before any modifications to either its
 * associated hash bucket, or the structure itself.
 * LRU modifications must hold the item lock, and the LRU lock.
 * LRU's accessing items must item_trylock() before modifying an item.
 * Items accessible from an LRU must not be freed or modified
 * without first locking and removing from the LRU.
 */
 //item_lock���뱻�����������Ƕ�����������Ͱ�����Ǹýṹ����
 //LRU �����������item �����Լ�LRU ��
 //���LRU ��Ҫ����һ��item������Ҫʹ��item_trylock�����޸���֮ǰ
 //��LRU ���ʵõ���item���ܱ��ͷŻ����޸ģ������Ѿ��������������Ѿ���
 //LRU ���Ƴ�

void item_lock(uint32_t hv) {
    mutex_lock(&item_locks[hv & hashmask(item_lock_hashpower)]);//�õ���Ӧ�����������������ס�˶��Ͱ
}

void *item_trylock(uint32_t hv) {
    pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
    if (pthread_mutex_trylock(lock) == 0) {
        return lock;
    }
    return NULL;
}

void item_trylock_unlock(void *lock) {
    mutex_unlock((pthread_mutex_t *) lock);
}

void item_unlock(uint32_t hv) {
    mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);

	//ǿ�ȹ����߳�pile up (�ѷš��ѻ�) ���������Ҫ�����Ļ�
    pthread_mutex_lock(&worker_hang_lock);
    pthread_mutex_unlock(&worker_hang_lock);
}

/* Must not be called with any deeper locks held */
//��������κ�deeper ������ֹ���øú���
void pause_threads(enum pause_thread_types type) {
    char buf[1];
    int i;

    buf[0] = 0;
    switch (type) {
        case PAUSE_ALL_THREADS:
            slabs_rebalancer_pause();
            lru_crawler_pause();
            lru_maintainer_pause();
        case PAUSE_WORKER_THREADS:
            buf[0] = 'p';
            pthread_mutex_lock(&worker_hang_lock);
            break;
        case RESUME_ALL_THREADS:
            slabs_rebalancer_resume();
            lru_crawler_resume();
            lru_maintainer_resume();
        case RESUME_WORKER_THREADS:
            pthread_mutex_unlock(&worker_hang_lock);
            break;
        default:
            fprintf(stderr, "Unknown lock type: %d\n", type);
            assert(1 == 0);
            break;
    }

    /* Only send a message if we have one. */
    if (buf[0] == 0) {
        return;
    }

    pthread_mutex_lock(&init_lock);
    init_count = 0;
    for (i = 0; i < settings.num_threads; i++) {
        if (write(threads[i].notify_send_fd, buf, 1) != 1) {
            perror("Failed writing to notify pipe");
            /* TODO: This is a fatal problem. Can it ever happen temporarily? */
        }
    }
    wait_for_thread_registration(settings.num_threads);
    pthread_mutex_unlock(&init_lock);
}

//��ʼ�����Ӷ���
static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

 //��һ�����Ӷ����в���һ��item
 //����ҵ����ظ�Ԫ�أ����򷵻�NULL
static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

 //�����Ӷ��������һ��Ԫ��
static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void) {
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item) {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}


/*
 * Frees a connection queue item (adds it to the freelist.)
 */
static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}


/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&((LIBEVENT_THREAD*)arg)->thread_id, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

/*
 * Sets whether or not we accept new connections.
 */
void accept_new_conns(const bool do_accept) {
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(do_accept);
    pthread_mutex_unlock(&conn_lock);
}
/****************************** LIBEVENT THREADS *****************************/

 //�����̵߳���Ϣ
static void setup_thread(LIBEVENT_THREAD *me) {
    me->base = event_init();//�µ�eventbase��
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);//���ó����ģ��ɶ�����
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL) {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);//��ʼ�����Ӷ��У���Ҫ�ǳ�ʼ�����������Լ���ָ��ΪNULL��

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {//��ʼ���߳�״̬�ֶ��е���
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

    me->suffix_cache = cache_create("suffix", SUFFIX_SIZE, sizeof(char*),
                                    NULL, NULL);//���
    if (me->suffix_cache == NULL) {
        fprintf(stderr, "Failed to create suffix cache\n");
        exit(EXIT_FAILURE);
    }
}

 //�����̵߳���ѭ��
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = arg;

    /* Any per-thread setup can happen here; memcached_thread_init() will block until
     * all threads have finished initializing.
     */
     //memcached_thread_init����������ֱ�������߳��Ѿ���ɳ�ʼ��
    me->l = logger_create();
    if (me->l == NULL) {
        abort();
    }

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}


/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
 //�����뵽��libevent�Ļ��ѹܵ�ʱ���øú���
 //����һ������������ "����һ�������� " item
static void thread_libevent_process(int fd, short which, void *arg) {
    LIBEVENT_THREAD *me = arg;
    CQ_ITEM *item;
    char buf[1];
    unsigned int timeout_fd;

    if (read(fd, buf, 1) != 1) {
        if (settings.verbose > 0)
            fprintf(stderr, "Can't read from libevent pipe\n");
        return;
    }

    switch (buf[0]) {
    case 'c':
        item = cq_pop(me->new_conn_queue);

        if (NULL != item) {
            conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
                               item->read_buffer_size, item->transport,
                               me->base);//���ﴴ��һ���µ����ӣ����Ѹ����ӵļ����¼���ӵ����̵߳�base��
            if (c == NULL) {
                if (IS_UDP(item->transport)) {
                    fprintf(stderr, "Can't listen for events on UDP socket\n");
                    exit(1);
                } else {
                    if (settings.verbose > 0) {
                        fprintf(stderr, "Can't listen for events on fd %d\n",
                            item->sfd);
                    }
                    close(item->sfd);
                }
            } else {
                c->thread = me;
            }
            cqi_free(item);
        }
        break;
    case 'r':
        item = cq_pop(me->new_conn_queue);

        if (NULL != item) {
            conn_worker_readd(item->c);
            cqi_free(item);
        }
        break;
    /* we were told to pause and report in */
    case 'p':
        register_thread_initialized();
        break;
    /* a client socket timed out */
    case 't':
        if (read(fd, &timeout_fd, sizeof(timeout_fd)) != sizeof(timeout_fd)) {
            if (settings.verbose > 0)
                fprintf(stderr, "Can't read timeout fd from libevent pipe\n");
            return;
        }
        conn_close_idle(conns[timeout_fd]);
        break;
    }
}

/* Which thread we assigned a connection to most recently. */
//����������ĸ��̷߳���������
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
 //�ַ�һ���µ����Ӹ���һ���߳�
 //�������ֻ�����̵߳��ã�Ҫô�ڶ� UDP ��ʼ�������У�
 //Ҫô����һ���µ����ӵ���ʱ
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size, enum network_transport transport) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        close(sfd);
        /* given that malloc failed this may also fail, but let's try */
        fprintf(stderr, "Failed to allocate memory for connection object\n");
        return ;
    }

    int tid = (last_thread + 1) % settings.num_threads;
    LIBEVENT_THREAD *thread = threads + tid;
    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;

    cq_push(thread->new_conn_queue, item);

    MEMCACHED_CONN_DISPATCH(sfd, thread->thread_id);
    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

/*
 * Re-dispatches a connection back to the original thread. Can be called from
 * any side thread borrowing a connection.
 */
 //���·ַ�һ�����Ӹ����߳�
 //���Ա��κν���һ�����ӵ�worker�̵߳���
void redispatch_conn(conn *c) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        /* Can't cleanly redispatch connection. close it forcefully. */
        c->state = conn_closed;
        close(c->sfd);
        return;
    }
    LIBEVENT_THREAD *thread = c->thread;
    item->sfd = c->sfd;
    item->init_state = conn_new_cmd;
    item->c = c;

    cq_push(thread->new_conn_queue, item);

    buf[0] = 'r';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

/* This misses the allow_new_conns flag :( */
void sidethread_conn_close(conn *c) {
    c->state = conn_closed;
    if (settings.verbose > 1)
        fprintf(stderr, "<%d connection closed from side thread.\n", c->sfd);
    close(c->sfd);

    STATS_LOCK();
    stats_state.curr_conns--;
    STATS_UNLOCK();

    return;
}

/********************************* ITEM ACCESS *******************************/

/*
 * Allocates a new item.
 */
 //����һ���µ�item��ע������ʹ�������ݵĳ��ȣ�����û��ʵ�ʵ�����
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes) {
    item *it;
    /* do_item_alloc handles its own locks */
    it = do_item_alloc(key, nkey, flags, exptime, nbytes);
    return it;
}

/*
 * Returns an item if it hasn't been marked as expired,
 * lazy-expiring as needed.
 */
 //�������ȼ�����Ȼ���ҵ�Ԫ���ٽ���
 //���Զ���߳̿���ͬʱ����һ��Ԫ��
item *item_get(const char *key, const size_t nkey, conn *c) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);//ִ�зֶμ���
    it = do_item_get(key, nkey, hv, c);//���Ҹ�Ԫ�أ�����ҵ��ж��Ƿ�ˢ�������ڣ����򷵻�Ԫ��
    item_unlock(hv);
    return it;
}

item *item_touch(const char *key, size_t nkey, uint32_t exptime, conn *c) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_touch(key, nkey, exptime, hv, c);
    item_unlock(hv);
    return it;
}

/*
 * Links an item into the LRU and hashtable.
 */
 //��һ��item�ŵ�LRU�͹�ϣ����
int item_link(item *item) {
    int ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_item_link(item, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
 //���ٶ�һ��item��refcount�������Ҫ��������뵽freelist
void item_remove(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_remove(item);
    item_unlock(hv);
}

/*
 * Replaces one item with another in the hashtable.
 * Unprotected by a mutex lock since the core server does not require
 * it to be thread-safe.
 */

//����һ��item�滻��hash���е�һ��item
//�������û�м�������Ϊ���ķ�����Ҫ
//����û�ж�item����
int item_replace(item *old_it, item *new_it, const uint32_t hv) {
    return do_item_replace(old_it, new_it, hv);
}

/*
 * Unlinks an item from the LRU and hashtable.
 */
 //
void item_unlink(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    do_item_unlink(item, hv);
    item_unlock(hv);
}

/*
 * Moves an item to the back of the LRU queue.
 */
 //��item�ƶ���LRU���е�ͷ�������Ƶ��������ܾ�����
void item_update(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_update(item);
    item_unlock(hv);
}

/*
 * Does arithmetic on a numeric item value.
 */
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, int incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas) {
    enum delta_result_type ret;
    uint32_t hv;

    hv = hash(key, nkey);
    item_lock(hv);
    ret = do_add_delta(c, key, nkey, incr, delta, buf, cas, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Stores an item in the cache (high level, obeys set/add/replace semantics)
 */
 //��cache�д洢item   (high level����ѭset/ add/ replace ����)
enum store_item_type store_item(item *item, int comm, conn* c) {
    enum store_item_type ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_store_item(item, comm, c, hv);
    item_unlock(hv);
    return ret;
}

/******************************* GLOBAL STATS ******************************/

void STATS_LOCK() {
    pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK() {
    pthread_mutex_unlock(&stats_lock);
}

void threadlocal_stats_reset(void) {
    int ii;
    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);
#define X(name) threads[ii].stats.name = 0;
        THREAD_STATS_FIELDS
#undef X

        memset(&threads[ii].stats.slab_stats, 0,
                sizeof(threads[ii].stats.slab_stats));

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void threadlocal_stats_aggregate(struct thread_stats *stats) {
    int ii, sid;

    /* The struct has a mutex, but we can safely set the whole thing
     * to zero since it is unused when aggregating. */
    memset(stats, 0, sizeof(*stats));

    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);
#define X(name) stats->name += threads[ii].stats.name;
        THREAD_STATS_FIELDS
#undef X

        for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
#define X(name) stats->slab_stats[sid].name += \
            threads[ii].stats.slab_stats[sid].name;
            SLAB_STATS_FIELDS
#undef X
        }

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out) {
    int sid;

    memset(out, 0, sizeof(*out));

    for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
#define X(name) out->name += stats->slab_stats[sid].name;
        SLAB_STATS_FIELDS
#undef X
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 */
 //��ʼ���߳���ϵͳ���������ָ����Ĺ����߳�

void memcached_thread_init(int nthreads) {
    int         i;
    int         power;

    for (i = 0; i < POWER_LARGEST; i++) {
        pthread_mutex_init(&lru_locks[i], NULL);
    }
    pthread_mutex_init(&worker_hang_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    /* Want a wide lock table, but don't waste memory */
    if (nthreads < 3) {
        power = 10;
    } else if (nthreads < 4) {
        power = 11;
    } else if (nthreads < 5) {
        power = 12;
    } else {
        /* 8192 buckets, and central locks don't scale much past 5 threads */
        power = 13;
    }

    if (power >= hashpower) {//��֤һ������ס��Ͱ����������1
        fprintf(stderr, "Hash table power size (%d) cannot be equal to or less than item lock table (%d)\n", hashpower, power);
        fprintf(stderr, "Item lock table grows with `-t N` (worker threadcount)\n");
        fprintf(stderr, "Hash table grows with `-o hashpower=N` \n");
        exit(1);
    }

    item_lock_count = hashsize(power);//2��power�η�
    item_lock_hashpower = power;

//calloc �� malloc������֮һ����Ҫ�����������ֱ�ָ��Ԫ�ظ�����Ԫ��size����������Է���Ŀռ�����
    item_locks = calloc(item_lock_count, sizeof(pthread_mutex_t));//����������������ռ�
    if (! item_locks) {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (i = 0; i < item_lock_count; i++) {
        pthread_mutex_init(&item_locks[i], NULL);//��ʼ����
    }

    threads = calloc(nthreads, sizeof(LIBEVENT_THREAD));//����n���̵߳Ŀռ�
    if (! threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    for (i = 0; i < nthreads; i++) {//����ÿһ���߳�
        int fds[2];
        if (pipe(fds)) {//���������ܵ���[0]��ʾ����[1]��ʾд
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);//�����̵߳�״̬
        /* Reserve three fds for the libevent base, and two for the pipe */
        stats_state.reserved_fds += 5;//�������ļ������� 3������base��2�����������ܵ�
    }

	//�����������libevent����֮�󴴽��߳�
    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i]);//�����̣߳���ѭ����worker_libevent��
    }

	//�ڷ���֮ǰ�ȴ����е��߳�����
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);//�����ʼ������û�дﵽָ��ֵ����ȴ�
    pthread_mutex_unlock(&init_lock);
}


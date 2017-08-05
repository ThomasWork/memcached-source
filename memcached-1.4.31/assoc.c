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

//���ȶ���������������
static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_items_counter_lock = PTHREAD_MUTEX_INITIALIZER;//�ڸ���item������ʱ����

typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */


/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;//��ʼֵΪ16

#define hashsize(n) ((ub4)1<<(n))
#define hashmask(n) (hashsize(n)-1)

//����ϣ���������ǲ��ҵĵط�����������չ������
static item** primary_hashtable = 0;//��ά���飬ÿһԪ����һ��Ͱ��
 //�ɵĹ�ϣ������չ�����У���������һ�û�б��ƶ�������ļ�ֵ
static item** old_hashtable = 0;
//hash�����ж���Ԫ��
static unsigned int hash_items = 0;//assoc_insert�е���
//���: ָʾ�Ƿ���������չ������
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how//����ߴ磬����
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
 //����չ�����У�����ʹ��Ͱ(bucket)������������Ǩ
 //����ش�0һֱ��hashsize(hashpower-1)-1;������ʹ��hashpower��1������Ϊ�Ѿ�+1���
static unsigned int expand_bucket = 0;

void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;//��ʼĬ��ֵΪ16
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

//��Ϊ���ܴ�������״̬�У�������Ҫ�ȶ�λ����Ӧ��Ͱ��Ȼ���������Ѱ��
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

	//���濪ʼ����hashֵ������������hashֵ��ȣ�Ȼ���ж�key���
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
//������keyǰ���item��ָ��ĵ�ַ
//����õ�ַ�б����ֵΪ0����ʾû���ҵ�
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

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {//���ȱȽϳ��ȣ�Ȼ���ٱȽ�����
        pos = &(*pos)->h_next;
    }
    return pos;
}

//��չ��ϣ��2����һ��ָ��
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;//�þɱ�ָ��ָ��ǰָ��

//calloc���Ĳ�ͬʱ����ռ������ʼ��Ϊ0
    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));//������������hashpower��û�б��
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;//��������
        expanding = true;//��ʾ��������
        expand_bucket = 0;
        STATS_LOCK();
        stats_state.hash_power_level = hashpower;
        stats_state.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats_state.hash_is_expanding = true;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;//����ռ�ʧ�ܣ���Ȼָ��֮ǰ�ĵ�ַ
        /* Bad news, but we can keep running. */
    }
}

//������item������ʱ���ú���Ҳ�����У����Կ��Ա�֤һ����
static void assoc_start_expand(void) {
    if (started_expanding)
        return;

    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);//����������������ʼ����
}

//�ⲻ��assoc_update���������������ʱ��key���벻����
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;//���뵽�ɵ�hash����
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    pthread_mutex_lock(&hash_items_counter_lock);
    hash_items++;
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {//�ﵽ1.5��ʱ��ʼ��չ
        assoc_start_expand();
    }
    pthread_mutex_unlock(&hash_items_counter_lock);

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

//��hash ����ɾ��
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);//�ҵ��ڸ�Ԫ��֮ǰ��Ԫ�صĵ�ַ

    if (*before) {//����ҵ���
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

#define DEFAULT_HASH_BULK_MOVE 1//ÿ���ƶ���Ͱ������
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;


//����߳��������������ݵ�
static void *assoc_maintenance_thread(void *arg) {

    mutex_lock(&maintenance_lock);//���������ʲô�õģ�ʲôʱ��⿪���
    while (do_run_maintenance_thread) {//����һ����ѭ����ֻ�н���ֵ�޸ĲŲ�������
        int ii = 0;

		//���ڽ�����һ����չ�̣߳����Բ���Ҫȫ����
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
            void *item_lock = NULL;

            /* bucket = hv & hashmask(hashpower) =>the bucket of hash table
             * is the lowest N bits of the hv, and the bucket of item_locks is
             *  also the lowest M bits of hv, and N is greater than M.
             *  So we can process expanding with only one item_lock. cool! */
             //bucket=hv & hashmask(hashpower)����˹�ϣ���Ͱ�����˹�ϣֵ��͵�N bit
             // ����item����ͰҲ�ǹ�ϣֵ�����M λ������N ����M��
             //�������ǿ��Խ���ʹ��һ��item���������ݣ�(һ��item����Ӧ�����ϣͰ)
            if ((item_lock = item_trylock(expand_bucket))) {//ȡ��item������������Ѿ�������(pthread_mutex_trylock)
                    for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {//����expand_bucket��ȫ�ֱ�������ʼֵΪ0
                        next = it->h_next;
                        bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);//ע������ʹ�õ�hashpower������power��ͬ
                        it->h_next = primary_hashtable[bucket];//�����µ�Ͱ��
                        primary_hashtable[bucket] = it;
                    }

                    old_hashtable[expand_bucket] = NULL;

                    expand_bucket++;
                    if (expand_bucket == hashsize(hashpower - 1)) {
                        expanding = false;//����������
                        free(old_hashtable);//�ͷžɵĹ�ϣ��Ŀռ�
                        STATS_LOCK();
                        stats_state.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                        stats_state.hash_is_expanding = false;
                        STATS_UNLOCK();
                        if (settings.verbose > 1)
                            fprintf(stderr, "Hash table expansion done\n");
                    }

            } else {
                usleep(10*1000);//˯��10���ӣ������Ƿ����һֱ�ȴ����������?����һֱ ��ռ��
            }

            if (item_lock) {//��������ɹ�
                item_trylock_unlock(item_lock);//����ʵ���ϵ��õ���unlock
                item_lock = NULL;
            }
        }//���˯�ߵĻ�����Ϊ��һ���߳�ר�Ÿ������ݣ����Լ���ѭ��

        if (!expanding) {//���ݽ���
            /* We are done expanding.. just wait for next invocation */
            started_expanding = false;
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);//������ȴ���һ��֪ͨ
            /* assoc_expand() swaps out the hash table entirely, so we need
             * all threads to not hold any references related to the hash
             * table while this happens.
             * This is instead of a more complex, possibly slower algorithm to
             * allow dynamic hash table expansion without causing significant
             * wait times.
             */
             //assoc_expand ������������ϣ�����������Ҫȷ���ڴ˹�����
             //û���κ��̳߳��й�ϣ�������
            pause_threads(PAUSE_ALL_THREADS);
            assoc_expand();//����������н�expending����Ϊtrue����Ϊ֪��������������Ż����е�����
            pause_threads(RESUME_ALL_THREADS);
        }
    }
    return NULL;
}

static pthread_t maintenance_tid;

int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;//����Ĭ���ƶ�����hashͰ
        }
    }
    pthread_mutex_init(&maintenance_lock, NULL);//��ʼ����
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


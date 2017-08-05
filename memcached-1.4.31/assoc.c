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

//Ê×ÏÈ¶¨ÒåÁËÈı¸ö»¥³âËø
static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_items_counter_lock = PTHREAD_MUTEX_INITIALIZER;//ÔÚ¸üĞÂitemµÄÊıÁ¿Ê±¼ÓËø

typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */


/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;//³õÊ¼ÖµÎª16

#define hashsize(n) ((ub4)1<<(n))
#define hashmask(n) (hashsize(n)-1)

//Ö÷¹şÏ£±í£¬ÕâÊÇÎÒÃÇ²éÕÒµÄµØ·½³ı·ÇÊÇÔÚÀ©Õ¹¹ı³ÌÖĞ
static item** primary_hashtable = 0;//¶şÎ¬Êı×é£¬Ã¿Ò»ÔªËØÊÇÒ»¸öÍ°£¬
 //¾ÉµÄ¹şÏ£±í¡£ÔÚÀ©Õ¹¹ı³ÌÖĞ£¬´ÓÕâÀï²éÕÒ»¹Ã»ÓĞ±»ÒÆ¶¯µ½Ö÷±íµÄ¼üÖµ
static item** old_hashtable = 0;
//hash±íÖĞÓĞ¶àÉÙÔªËØ
static unsigned int hash_items = 0;//assoc_insertÖĞµ÷ÓÃ
//±ê¼Ç: Ö¸Ê¾ÊÇ·ñÕı´¦ÓÚÀ©Õ¹¹ı³ÌÖĞ
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how//¼ä¸ô³ß´ç£¬Á£¶È
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
 //ÔÚÀ©Õ¹¹ı³ÌÖĞ£¬ÎÒÃÇÊ¹ÓÃÍ°(bucket)µÄÁ£¶ÈÀ´½øĞĞÇ¨
 //¾ßÌåµØ´Ó0Ò»Ö±µ½hashsize(hashpower-1)-1;£¬ÕâÀïÊ¹ÓÃhashpower¼õ1£¬ÊÇÒòÎªÒÑ¾­+1ÁËå
static unsigned int expand_bucket = 0;

void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;//³õÊ¼Ä¬ÈÏÖµÎª16
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

//ÒòÎª¿ÉÄÜ´¦ÓÚÀ©Èİ×´Ì¬ÖĞ£¬ËùÒÔĞèÒªÏÈ¶¨Î»µ½ÏàÓ¦µÄÍ°£¬È»ºó²ÅÄÜÏòÏÂÑ°ÕÒ
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

	//ÏÂÃæ¿ªÊ¼¸ù¾İhashÖµ²éÕÒÁ´±í£¬Ê×ÏÈhashÖµÏàµÈ£¬È»ºóÅĞ¶ÏkeyÏàµÈ
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
//·µ»ØÔÚkeyÇ°ÃæµÄitemµÄÖ¸ÕëµÄµØÖ·
//Èç¹û¸ÃµØÖ·ÖĞ±£´æµÄÖµÎª0£¬±íÊ¾Ã»ÓĞÕÒµ½
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

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {//Ê×ÏÈ±È½Ï³¤¶È£¬È»ºóÔÙ±È½ÏÄÚÈİ
        pos = &(*pos)->h_next;
    }
    return pos;
}

//À©Õ¹¹şÏ£±íµ½2µÄÏÂÒ»¸öÖ¸Êı
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;//ÈÃ¾É±íÖ¸ÕëÖ¸Ïòµ±Ç°Ö¸Õë

//calloc×î´óµÄ²»Í¬Ê±·ÖÅä¿Õ¼ä¹ıºó»á³õÊ¼»¯Îª0
    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));//½«Ö÷±íÀ©³¤£¬hashpower»¹Ã»ÓĞ±ä´ó
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;//ÕâÀï±ä´óÁË
        expanding = true;//±íÊ¾ÕıÔÚÀ©Èİ
        expand_bucket = 0;
        STATS_LOCK();
        stats_state.hash_power_level = hashpower;
        stats_state.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats_state.hash_is_expanding = true;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;//·ÖÅä¿Õ¼äÊ§°Ü£¬ÈÔÈ»Ö¸ÏòÖ®Ç°µÄµØÖ·
        /* Bad news, but we can keep running. */
    }
}

//ÔÚÔö¼Óitem¼ÆÊıËøÊ±£¬¸Ãº¯ÊıÒ²ÔÚËøÖĞ£¬ËùÒÔ¿ÉÒÔ±£Ö¤Ò»ÖÂĞÔ
static void assoc_start_expand(void) {
    if (started_expanding)
        return;

    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);//ÉèÖÃÌõ¼ş±äÁ¿£¬¿ªÊ¼À©Èİ
}

//Õâ²»ÊÇassoc_update£¬µ±µ÷ÓÃÕâ¸öº¯ÊıÊ±£¬key±ØĞë²»´æÔÚ
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;//¼ÓÈëµ½¾ÉµÄhash±íÖĞ
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    pthread_mutex_lock(&hash_items_counter_lock);
    hash_items++;
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {//´ïµ½1.5±¶Ê±¿ªÊ¼À©Õ¹
        assoc_start_expand();
    }
    pthread_mutex_unlock(&hash_items_counter_lock);

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

//´Óhash ±íÖĞÉ¾³ı
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);//ÕÒµ½ÔÚ¸ÃÔªËØÖ®Ç°µÄÔªËØµÄµØÖ·

    if (*before) {//Èç¹ûÕÒµ½ÁË
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

#define DEFAULT_HASH_BULK_MOVE 1//Ã¿´ÎÒÆ¶¯µÄÍ°µÄÊıÁ¿
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;


//Õâ¸öÏß³ÌÊÇÓÃÀ´½øĞĞÀ©ÈİµÄ
static void *assoc_maintenance_thread(void *arg) {

    mutex_lock(&maintenance_lock);//Õâ¸öËøÊÇ×öÊ²Ã´ÓÃµÄ£¬Ê²Ã´Ê±¼ä½â¿ªÄØå
    while (do_run_maintenance_thread) {//ÕâÊÇÒ»¸öËÀÑ­»·£¬Ö»ÓĞ½«¸ÃÖµĞŞ¸Ä²Å²»»áÌø³ö
        int ii = 0;

		//ÓÉÓÚ½ö½öÓĞÒ»¸öÀ©Õ¹Ïß³Ì£¬ËùÒÔ²»ĞèÒªÈ«¾ÖËø
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
            void *item_lock = NULL;

            /* bucket = hv & hashmask(hashpower) =>the bucket of hash table
             * is the lowest N bits of the hv, and the bucket of item_locks is
             *  also the lowest M bits of hv, and N is greater than M.
             *  So we can process expanding with only one item_lock. cool! */
             //bucket=hv & hashmask(hashpower)£¬Òò´Ë¹şÏ£±íµÄÍ°´ú±íÁË¹şÏ£Öµ×îµÍµÄN bit
             // ÓÉÓÚitemËøµÄÍ°Ò²ÊÇ¹şÏ£ÖµµÄ×îµÍM Î»£¬²¢ÇÒN ´óÓÚM£¬
             //ËùÒÔÎÒÃÇ¿ÉÒÔ½ö½öÊ¹ÓÃÒ»¸öitemËø½øĞĞÀ©Èİ£¬(Ò»¸öitemËø¶ÔÓ¦¶à¸ö¹şÏ£Í°)
            if ((item_lock = item_trylock(expand_bucket))) {//È¡µÃitemËø²¢ÇÒÕâ¸öËøÒÑ¾­¼ÓÉÏÁË(pthread_mutex_trylock)
                    for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {//ÕâÀïexpand_bucketÊÇÈ«¾Ö±äÁ¿£¬³õÊ¼ÖµÎª0
                        next = it->h_next;
                        bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);//×¢ÒâÕâÀïÊ¹ÓÃµÄhashpowerºÍËøµÄpower²»Í¬
                        it->h_next = primary_hashtable[bucket];//·ÅÈëĞÂµÄÍ°ÖĞ
                        primary_hashtable[bucket] = it;
                    }

                    old_hashtable[expand_bucket] = NULL;

                    expand_bucket++;
                    if (expand_bucket == hashsize(hashpower - 1)) {
                        expanding = false;//·ÇÕıÔÚÀ©Èİ
                        free(old_hashtable);//ÊÍ·Å¾ÉµÄ¹şÏ£±íµÄ¿Õ¼ä
                        STATS_LOCK();
                        stats_state.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                        stats_state.hash_is_expanding = false;
                        STATS_UNLOCK();
                        if (settings.verbose > 1)
                            fprintf(stderr, "Hash table expansion done\n");
                    }

            } else {
                usleep(10*1000);//Ë¯Ãß10ÃëÖÓ£¬ÕâÀïÊÇ·ñ»áÓĞÒ»Ö±µÈ´ıµÄÇé¿ö³öÏÖ?¼´ËøÒ»Ö± ±»Õ¼ÓÃ
            }

            if (item_lock) {//Èç¹ûËø¶¨³É¹¦
                item_trylock_unlock(item_lock);//ÕâÀïÊµ¼ÊÉÏµ÷ÓÃµÄÊÇunlock
                item_lock = NULL;
            }
        }//Èç¹ûË¯ÃßµÄ»°£¬ÒòÎªÓĞÒ»¸öÏß³Ì×¨ÃÅ¸ºÔğÀ©Èİ£¬ËùÒÔ¼ÌĞøÑ­»·

        if (!expanding) {//À©Èİ½áÊø
            /* We are done expanding.. just wait for next invocation */
            started_expanding = false;
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);//ÔÚÕâÀïµÈ´ıÏÂÒ»´ÎÍ¨Öª
            /* assoc_expand() swaps out the hash table entirely, so we need
             * all threads to not hold any references related to the hash
             * table while this happens.
             * This is instead of a more complex, possibly slower algorithm to
             * allow dynamic hash table expansion without causing significant
             * wait times.
             */
             //assoc_expand ½»»»³öÕû¸ö¹şÏ£±í£¬Òò´ËÎÒÃÇĞèÒªÈ·±£ÔÚ´Ë¹ı³ÌÖĞ
             //Ã»ÓĞÈÎºÎÏß³Ì³ÖÓĞ¹şÏ£±íµÄÒıÓÃ
            pause_threads(PAUSE_ALL_THREADS);
            assoc_expand();//ÔÚÕâ¸öº¯ÊıÖĞ½«expendingÉèÖÃÎªtrue£¬ÒòÎªÖªµÀÌõ¼ş±äÁ¿ÔÊĞí²Å»áÔËĞĞµ½ÕâÀï
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
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;//ÉèÖÃÄ¬ÈÏÒÆ¶¯¼¸¸öhashÍ°
        }
    }
    pthread_mutex_init(&maintenance_lock, NULL);//³õÊ¼»¯Ëø
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


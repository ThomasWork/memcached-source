/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/** \file
 * The main memcached header holding commonly used data
 * structures and function prototypes.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <event.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#include "protocol_binary.h"
#include "cache.h"
#include "logger.h"

#include "sasl_defs.h"

/** Maximum length of a key. */
#define KEY_MAX_LENGTH 250

/** Size of an incr buf. */
#define INCR_MAX_STORAGE_LEN 24

#define DATA_BUFFER_SIZE 2048
#define UDP_READ_BUFFER_SIZE 65536
#define UDP_MAX_PAYLOAD_SIZE 1400
#define UDP_HEADER_SIZE 8
#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)
/* I'm told the max length of a 64-bit num converted to string is 20 bytes.
 * Plus a few for spaces, \r\n, \0 */
#define SUFFIX_SIZE 24

/** Initial size of list of items being returned by "get". */
#define ITEM_LIST_INITIAL 200

/** Initial size of list of CAS suffixes appended to "gets" lines. */
#define SUFFIX_LIST_INITIAL 20

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400

/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10

/** High water marks for buffer shrinking */
#define READ_BUFFER_HIGHWAT 8192
#define ITEM_LIST_HIGHWAT 400
#define IOV_LIST_HIGHWAT 600
#define MSG_LIST_HIGHWAT 100

/* Binary protocol stuff */
#define MIN_BIN_PKT_LENGTH 16
#define BIN_PKT_HDR_WORDS (MIN_BIN_PKT_LENGTH/sizeof(uint32_t))

/* Initial power multiplier for the hash table */
#define HASHPOWER_DEFAULT 16

/*
 * We only reposition items in the LRU queue if they haven't been repositioned
 * in this many seconds. That saves us from churning on frequently-accessed
 * items.
 */
 //ÎÒÃÇ½ö½öÔÚitem ÔÚ60 ÃëÄÚÃ»ÓÐ±»ÖØÐÂ°²ÅÅ¹ýÎ»ÖÃµÄÇé¿öÏÂ£¬
 //²ÅÖØÐÂ°²ÅÅËüµÄÎ»ÖÃ£¬·ÀÖ¹ÎÒÃÇÆµ·±µØ·ÃÎÊitem
#define ITEM_UPDATE_INTERVAL 60

/* unistd.h is here */
#if HAVE_UNISTD_H
# include <unistd.h>
#endif

/* Slab sizing definitions. */
#define POWER_SMALLEST 1
#define POWER_LARGEST  256 /* actual cap is 255 */
#define SLAB_GLOBAL_PAGE_POOL 0 /* magic slab class for storing pages for reassignment */
#define CHUNK_ALIGN_BYTES 8
/* slab class max is a 6-bit number, -1. */
#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)

/** How long an object can reasonably be assumed to be locked before
    harvesting it on a low memory condition. Default: disabled. */
#define TAIL_REPAIR_TIME_DEFAULT 0

/* warning: don't use these macros with a function, as it evals its arg twice */
#define ITEM_get_cas(i) (((i)->it_flags & ITEM_CAS) ? \
        (i)->data->cas : (uint64_t)0)

#define ITEM_set_cas(i,v) { \
    if ((i)->it_flags & ITEM_CAS) { \
        (i)->data->cas = v; \
    } \
}

#define ITEM_key(item) (((char*)&((item)->data)) \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_suffix(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_data(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (item)->nsuffix \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_ntotal(item) (sizeof(struct _stritem) + (item)->nkey + 1 \
         + (item)->nsuffix + (item)->nbytes \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_clsid(item) ((item)->slabs_clsid & ~(3<<6))

#define STAT_KEY_LEN 128
#define STAT_VAL_LEN 128

/** Append a simple stat with a stat name, value format and value */
#define APPEND_STAT(name, fmt, val) \
    append_stat(name, add_stats, c, fmt, val);

/** Append an indexed stat with a stat name (with format), value format
    and value */
#define APPEND_NUM_FMT_STAT(name_fmt, num, name, fmt, val)          \
    klen = snprintf(key_str, STAT_KEY_LEN, name_fmt, num, name);    \
    vlen = snprintf(val_str, STAT_VAL_LEN, fmt, val);               \
    add_stats(key_str, klen, val_str, vlen, c);

/** Common APPEND_NUM_FMT_STAT format. */
#define APPEND_NUM_STAT(num, name, fmt, val) \
    APPEND_NUM_FMT_STAT("%d:%s", num, name, fmt, val)

/**
 * Callback for any function producing stats.
 *
 * @param key the stat's key
 * @param klen length of the key
 * @param val the stat's value in an ascii form (e.g. text form of a number)
 * @param vlen length of the value
 * @parm cookie magic callback cookie
 */
 //ÈÎºÎ²úÉú×´Ì¬µÄº¯ÊýµÄ»Øµ÷º¯Êý
typedef void (*ADD_STAT)(const char *key, const uint16_t klen,
                         const char *val, const uint32_t vlen,
                         const void *cookie);

/*
 * NOTE: If you modify this table you _MUST_ update the function state_text
 */
 //Ò»¸öÁ¬½ÓµÄ¿ÉÄÜ×´Ì¬
enum conn_states {
    conn_listening,  //ÕýÔÚ¼àÌýÁ¬½Ó
    conn_new_cmd,    //ÎªÏÂÒ»¸öÃüÁî×¼±¸Á¬½Ó/**< Prepare connection for next command */
    conn_waiting,    //ÕýÔÚµÈ´ýÒ»¸ö¿É¶ÁµÄÌ×½Ó×Ö
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      //Ð´Ò»¸ö¼òµ¥µÄ»ØÓ¦
    conn_nread,      //¶ÁÈë¹Ì¶¨ÊýÁ¿µÄ×Ö½Ú
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    //ÕýÔÚ¹Ø±ÕÕâ¸öÁ¬½Ó
    conn_mwrite,     //ÕýÔÚË³ÐòµØÐ´¶à¸öitem
    conn_closed,     //Á¬½Ó±»¹Ø±ÕÁË
    conn_watch,      //±»ÈÕÖ¾Ïß³Ì³ÖÓÐ×÷Îª¹Û²ìÆ÷
    conn_max_state   //×î´óµÄ×´Ì¬Êý£¬±»assertionº¯ÊýÊ¹ÓÃ
};

enum bin_substates {
    bin_no_state,
    bin_reading_set_header,
    bin_reading_cas_header,
    bin_read_set_value,
    bin_reading_get_key,
    bin_reading_stat,
    bin_reading_del_header,
    bin_reading_incr_header,
    bin_read_flush_exptime,
    bin_reading_sasl_auth,
    bin_reading_sasl_auth_data,
    bin_reading_touch_key,
};

enum protocol {
    ascii_prot = 3, /* arbitrary value. */
    binary_prot,
    negotiating_prot /* Discovering the protocol */
};

enum network_transport {
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

enum pause_thread_types {
    PAUSE_WORKER_THREADS = 0,
    PAUSE_ALL_THREADS,
    RESUME_ALL_THREADS,
    RESUME_WORKER_THREADS
};

#define IS_TCP(x) (x == tcp_transport)
#define IS_UDP(x) (x == udp_transport)

#define NREAD_ADD 1
#define NREAD_SET 2
#define NREAD_REPLACE 3
#define NREAD_APPEND 4
#define NREAD_PREPEND 5
#define NREAD_CAS 6

enum store_item_type {
    NOT_STORED=0, STORED, EXISTS, NOT_FOUND, TOO_LARGE, NO_MEMORY
};

enum delta_result_type {
    OK, NON_NUMERIC, EOM, DELTA_ITEM_NOT_FOUND, DELTA_ITEM_CAS_MISMATCH
};

/** Time relative to server start. Smaller than time_t on 64-bit systems. */
typedef unsigned int rel_time_t;

/** Use X macros to avoid iterating over the stats fields during reset and
 * aggregation. No longer have to add new stats in 3+ places.
 */
 //Ê¹ÓÃX ºêÀ´±ÜÃâÔÚreset ºÍaggregationÖÐ
 //µü´ú(ÖØ¸´å)) ¡£²»ÐèÒªÔÚ³¬¹ý3¸öµÄµØ·½Ìí¼ÓÐÂµÄ×´Ì¬

#define SLAB_STATS_FIELDS \
    X(set_cmds) \
    X(get_hits) \
    X(touch_hits) \
    X(delete_hits) \
    X(cas_hits) \
    X(cas_badval) \
    X(incr_hits) \
    X(decr_hits)

/** Stats stored per slab (and per thread). */
struct slab_stats {
#define X(name) uint64_t    name;
    SLAB_STATS_FIELDS
#undef X
};

#define THREAD_STATS_FIELDS \
    X(get_cmds) \
    X(get_misses) \
    X(get_expired) \
    X(get_flushed) \
    X(touch_cmds) \
    X(touch_misses) \
    X(delete_misses) \
    X(incr_misses) \
    X(decr_misses) \
    X(cas_misses) \
    X(bytes_read) \   //¶ÁÁË¶àÉÙ×Ö½Ú
    X(bytes_written) \
    X(flush_cmds) \
    X(conn_yields) /* # of yields for connections (-R option)*/ \
    X(auth_cmds) \
    X(auth_errors) \
    X(idle_kicks) /* idle connections killed */

/**
 * Stats stored per-thread.
 */

//////////////////////////////////

//////////////////////////////////////////


struct thread_stats {
			pthread_mutex_t   mutex;
#define X(name) uint64_t    name;
			THREAD_STATS_FIELDS
#undef X
			struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES];

};

/**
 * Global stats. Only resettable stats should go into this structure.
 */
struct stats {
    uint64_t      total_items;
    uint64_t      total_conns;
    uint64_t      rejected_conns;//¾Ü¾øµÄÁ¬½ÓµÄÊýÁ¿£¬ÔÚÃèÊö·ûÊýÁ¿²»¹»µÄÊ±ºò»á¾Ü¾øÁ¬½Ó
    uint64_t      malloc_fails;//·ÖÅä¿Õ¼äÊ§°Ü
    uint64_t      listen_disabled_num;
    uint64_t      slabs_moved;       /* times slabs were moved around */
    uint64_t      slab_reassign_rescues; /* items rescued during slab move */
    uint64_t      slab_reassign_evictions_nomem; /* valid items lost during slab move */
    uint64_t      slab_reassign_inline_reclaim; /* valid items lost during slab move */
    uint64_t      slab_reassign_chunk_rescues; /* chunked-item chunks recovered */
    uint64_t      slab_reassign_busy_items; /* valid temporarily unmovable */
    uint64_t      lru_crawler_starts; /* Number of item crawlers kicked off */
    uint64_t      lru_maintainer_juggles; /* number of LRU bg pokes */
    uint64_t      time_in_listen_disabled_us;  /* elapsed time in microseconds while server unable to process new connections */
    uint64_t      log_worker_dropped; /* logs dropped by worker threads */
    uint64_t      log_worker_written; /* logs written by worker threads */
    uint64_t      log_watcher_skipped; /* logs watchers missed */
    uint64_t      log_watcher_sent; /* logs sent to watcher buffers */
    struct timeval maxconns_entered;  /* last time maxconns entered */
};

/**
 * Global "state" stats. Reflects state that shouldn't be wiped ever.
 * Ordered for some cache line locality for commonly updated counters.
 */
 //È«¾Ö×´Ì¬£¬´ú±íÁË²»Ó¦¸Ã±»²Á³ýµÄ×´Ì¬
 //¶ÔÐèÒª¾­³£¸üÐÂµÄ¼ÆÊýÆ÷£¬¸ù¾Ýcache line ¾Ö²¿ÐÔ½øÐÐÅÅÐò¬
struct stats_state {
    uint64_t      curr_items;
    uint64_t      curr_bytes;
    uint64_t      curr_conns;
    uint64_t      hash_bytes;       //¹þÏ£±íÊ¹ÓÃÁË¶àÉÙ×Ö½Ú
    unsigned int  conn_structs;
    unsigned int  reserved_fds;//Õâ¸ö±äÁ¿ÓÐÊ²Ã´ÓÃ£¬ÔÚmemcached_thread_initÖÐ·ÃÎÊ¹ý
    unsigned int  hash_power_level; //µ±Ç°hash±íµÄ³¤¶È´ú±í2µÄ¶àÉÙ´Î·½
    bool          hash_is_expanding; //µ±Ç°¹þÏ£±íÊÇ·ñÕýÔÚÀ©ÈÝ
    bool          accepting_conns;  //Ä¿Ç°ÊÇ·ñÔÚ½ÓÊÕÁ¬½Ó
    bool          slab_reassign_running; /* slab reassign in progress */
    bool          lru_crawler_running; /* crawl in progress */
};

#define MAX_VERBOSITY_LEVEL 2

/* When adding a setting, be sure to update process_stat_settings */
/**
 * Globally accessible settings as derived from the commandline.
 */
struct settings {
    size_t maxbytes;
    int maxconns;
    int port;
    int udpport;
    char *inter;
    int verbose;//Õâ¸öÊÇµ÷ÊÔÐÅÏ¢å
    rel_time_t oldest_live; //ºöÂÔ±È¸ÃÊ±¼äÀÏµÄÇÒ´æÔÚµÄitem                                      /* ignore existing items older than this */
    uint64_t oldest_cas; //ºöÂÔCAS Öµ±È¸ÃÖµÐ¡µÄÇÒ´æÔÚµÄitem   /* ignore existing items with CAS values lower than this */
    int evict_to_free;
    char *socketpath;   /* path to unix socket if using local socket */
    int access;  /* access mask (a la chmod) for unix domain socket */
    double factor;          /* chunk size growth factor */
    int chunk_size;
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    int num_threads_per_udp; /* number of worker threads serving each udp socket */
    char prefix_delimiter;  /* character that marks a key prefix (for stats) */
    int detail_enabled;     /* nonzero if we're collecting detailed stats */
    int reqs_per_event;     /* Maximum number of io to process on each
                               io-event. */
    bool use_cas;
    enum protocol binding_protocol;
    int backlog;
    int item_size_max;        /* Maximum item size */
    int slab_chunk_size_max;  /* Upper end for chunks within slab pages. */
    int slab_page_size;     /* Slab's page units. */
    bool sasl;              /* SASL on/off */
    bool maxconns_fast;     /* Whether or not to early close connections */
    bool lru_crawler;       //ÊÇ·ñÔÊÐí×Ô¶¯ÅÀ³æÏß³Ì
    bool lru_maintainer_thread; /* LRU maintainer background thread */
    bool slab_reassign;     /* Whether or not slab reassignment is allowed */
    int slab_automove;     /* Whether or not to automatically move slabs */
    int hashpower_init;     /* Starting hash power level */
    bool shutdown_command; /* allow shutdown command */
    int tail_repair_time;   /* LRU tail refcount leak repair time */
    bool flush_enabled;     /* flush_all enabled */
    char *hash_algorithm;     /* Hash algorithm in use */
    int lru_crawler_sleep;  /* Microsecond sleep between items */
    uint32_t lru_crawler_tocrawl; /* Number of items to crawl per run */
    int hot_lru_pct; /* percentage of slab space for HOT_LRU */
    int warm_lru_pct; /* percentage of slab space for WARM_LRU */
    int crawls_persleep; /* Number of LRU crawls to run before sleeping */
    bool expirezero_does_not_evict; /* exptime == 0 goes into NOEXP_LRU */
    int idle_timeout;       /* Number of seconds to let connections idle */
    unsigned int logger_watcher_buf_size; /* size of logger's per-watcher buffer */
    unsigned int logger_buf_size; /* size of per-thread logger buffer */
};

extern struct stats stats;
extern struct stats_state stats_state;
extern time_t process_started;
extern struct settings settings;

#define ITEM_LINKED 1
#define ITEM_CAS 2

/* temp */
#define ITEM_SLABBED 4

/* Item was fetched at least once in its lifetime */
#define ITEM_FETCHED 8
/* Appended on fetch, removed on LRU shuffling */
#define ITEM_ACTIVE 16

/* If an item's storage are chained chunks. */
#define ITEM_CHUNKED 32//Èç¹ûÒ»¸öitemµÄ´æ´¢·½Ê½ÊÇÁ´½ÓµÄ¿é
#define ITEM_CHUNK 64

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks */
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock */
    struct _stritem *h_next;    /* hash chain next */
    rel_time_t      time;       /* least recent access */
    rel_time_t      exptime;    /* expire time */
    int             nbytes;     /* size of data */
    unsigned short  refcount;
    uint8_t         nsuffix;    /* length of flags-and-length string */
    uint8_t         it_flags;   //item µ±Ç°µÄ×´Ì¬£¬ÊÇ·ñÊÇlinked£¬chunkedµÈµÈ
    uint8_t         slabs_clsid;/* which slab class we're in */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS. */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS */
    /* then null-terminated key */
    /* then " flags length\r\n" (no terminating null) */
    /* then data with terminating \r\n (no terminating null; it's binary!) */
} item;

// TODO: If we eventually want user loaded modules, we can't use an enum :(
enum crawler_run_type {
    CRAWLER_EXPIRED=0, CRAWLER_METADUMP
};

typedef struct {
    struct _stritem *next;
    struct _stritem *prev;
    struct _stritem *h_next;    /* hash chain next */
    rel_time_t      time;       /* least recent access */
    rel_time_t      exptime;    /* expire time */
    int             nbytes;     /* size of data */
    unsigned short  refcount;
    uint8_t         nsuffix;    /* length of flags-and-length string */
    uint8_t         it_flags;   /* ITEM_* above */
    uint8_t         slabs_clsid;/* which slab class we're in */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    uint32_t        remaining;  /* Max keys to crawl per slab per invocation */
    uint64_t        reclaimed;  /* items reclaimed during this crawl. */
    uint64_t        unfetched;  /* items reclaiemd unfetched during this crawl. */
    uint64_t        checked;    /* items examined during this crawl. */
} crawler;

/* Header when an item is actually a chunk of another item. */
typedef struct _strchunk {
    struct _strchunk *next;     /* points within its own chain. */
    struct _strchunk *prev;     /* can potentially point to the head. */
    struct _stritem  *head;     /* always points to the owner chunk */
    int              size;      /* available chunk space in bytes */
    int              used;      /* chunk space used */
    int              nbytes;    /* used. */
    unsigned short   refcount;  /* used? */
    uint8_t          nsuffix;   /* unused */
    uint8_t          it_flags;  /* ITEM_* above. */
    uint8_t          slabs_clsid; /* Same as above. */
    char data[];
} item_chunk;

typedef struct {
    pthread_t thread_id;        //Ïß³ÌµÄÎ¨Ò»id
    struct event_base *base;    /* libevent handle this thread uses */
    struct event notify_event;  //¶ÔÓÚÍ¨Öª¹ÜµÀµÄ¼àÌýÊÂ¼þ
    int notify_receive_fd;      //ÄäÃû¹ÜµÀµÄ¶Á¶Ë
    int notify_send_fd;         //ÄäÃû¹ÜµÀµÄÐ´¶Ë
    struct thread_stats stats;  //Õâ¸öÏß³ÌÉú³ÉµÄ×´Ì¬/* Stats generated by this thread */
    struct conn_queue *new_conn_queue; //ÐÂµÄ½«Òª´¦ÀíµÄÁ¬½Ó¶ÓÁÐ£¬¶ÔÓÚÀÏµÄ¸ÃÔõÃ´°ìå
    cache_t *suffix_cache;     //Õâ¸öºó×ºcache´ú±íÊ²Ã´ÒâË¼?
    logger *l;                  /* logger buffer */
} LIBEVENT_THREAD;

 //memcachedÁ¬½Ó
typedef struct conn conn;
struct conn {
    int    sfd;
    sasl_conn_t *sasl_conn;
    bool authenticated;
    enum conn_states  state;
    enum bin_substates substate;
    rel_time_t last_cmd_time; //ÉÏÒ»´ÎÖ´ÐÐÖ¸ÁîµÄÊ±¼ä
    struct event event;
    short  ev_flags;//libevent ¼àÌýµÄÊÂ¼þ±êÖ¾
    short  which;   //¸Õ¸Õ´¥·¢ÁËÄÄ¸öÊÂ¼þ /** which events were just triggered */

    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    int    rsize;   /** total allocated size of rbuf */
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;
	
    enum conn_states  write_and_go;//ÔÚÐ´Íêµ±Ç°ÄÚÈÝÖ®ºóµ½ÄÄ¸ö×´Ì¬?
    void   *write_and_free; /** free this memory after finishing writing */

    char   *ritem;  /** when we read in an item's value, it goes here */
    int    rlbytes;

    /* data for the nread state */

    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the actual
     * data. The data is read into ITEM_data(item) to avoid extra copying.
     */

    void   *item;     //ÎªÁËÃüÁî: set/add/replace  /* for commands set/add/replace  */

    /* data for the swallow state */
    int    sbytes;    /* how many bytes to swallow */

	//ÔÚmwrite ×´Ì¬Ê¹ÓÃµÄÊý¾Ý
    struct iovec *iov;
    int    iovsize;   //iovÊý×éÓÐ¶àÉÙÔªËØ
    int    iovused;   //iov Êý×éÖÐÊ¹ÓÃÁË¶àÉÙÔªËØ

    struct msghdr *msglist;//
    int    msgsize;   //msglistÊý×é×Ü¹²ÓÐ¶àÉÙ¿Õ¼ä /* number of elements allocated in msglist[] */
    int    msgused;  //msglist Ê¹ÓÃÁË¶àÉÙÔªËØ /* number of elements used in msglist[] */
    int    msgcurr;  //µ±Ç°ÕýÔÚ±»´«ÊäµÄÔªËØ /* element in msglist[] being transmitted now */
    int    msgbytes;  //µ±Ç°msgÖÐÓÐ¶àÉÙ×Ö½Ú  /* number of bytes in current msg */
		
    item   	**ilist;   //½«ÒªÏòÍâÐ´³öµÄitemµÄÁÐ±í
    int    	isize;
    item  	 **icurr;
    int  	  ileft;

    char   **suffixlist;
    int  	  suffixsize;
    char   **suffixcurr;
    int    suffixleft;

    enum protocol protocol;   //Ê¹ÓÃÄÄ¸öÐ­Òé£¬¶þ½øÖÆ»¹ÊÇASCII
    enum network_transport transport; /* what transport is used by this connection */

    /* data for UDP clients */
    int    request_id; /* Incoming UDP request ID, if this is a UDP "connection" */
    struct sockaddr_in6 request_addr; /* udp: Who sent the most recent request */
    socklen_t request_addr_size;
    unsigned char *hdrbuf; /* udp packet headers */
    int    hdrsize;   /* number of headers' worth of space is allocated */

    bool   noreply;   /* True if the reply should not be sent. */
    /* current stats command */
    struct {
        char *buffer;
        size_t size;
        size_t offset;
    } stats;

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    protocol_binary_request_header binary_header;
    uint64_t cas; /* the cas to return */
    short cmd; //Ä¿Ç°ÕýÒª´¦ÀíµÄÃüÁî
    int opaque;
    int keylen;
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread;//Ö¸Ïò·þÎñÓÚ¸ÃÁ¬½ÓµÄÏß³Ì
};

/* array of conn structures, indexed by file descriptor */
extern conn **conns;

/* current time of day (updated periodically) */
extern volatile rel_time_t current_time;

/* TODO: Move to slabs.h? */
extern volatile int slab_rebalance_signal;

struct slab_rebalance {
    void *slab_start;
    void *slab_end;
    void *slab_pos;
    int s_clsid;
    int d_clsid;
    uint32_t busy_items;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;
    uint32_t chunk_rescues;
    uint8_t done;
};

extern struct slab_rebalance slab_rebal;

/*
 * Functions
 */
void do_accept_new_conns(const bool do_accept);
enum delta_result_type do_add_delta(conn *c, const char *key,
                                    const size_t nkey, const bool incr,
                                    const int64_t delta, char *buf,
                                    uint64_t *cas, const uint32_t hv);
enum store_item_type do_store_item(item *item, int comm, conn* c, const uint32_t hv);
conn *conn_new(const int sfd, const enum conn_states init_state, const int event_flags, const int read_buffer_size, enum network_transport transport, struct event_base *base);
void conn_worker_readd(conn *c);
extern int daemonize(int nochdir, int noclose);

#define mutex_lock(x) pthread_mutex_lock(x)
#define mutex_unlock(x) pthread_mutex_unlock(x)

#include "stats.h"
#include "slabs.h"
#include "assoc.h"
#include "items.h"
#include "crawler.h"
#include "trace.h"
#include "hash.h"
#include "util.h"

/*
 * Functions such as the libevent-related calls that need to do cross-thread
 * communication in multithreaded mode (rather than actually doing the work
 * in the current thread) are called via "dispatch_" frontends, which are
 * also #define-d to directly call the underlying code in singlethreaded mode.
 */

void memcached_thread_init(int nthreads);
void redispatch_conn(conn *c);
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags, int read_buffer_size, enum network_transport transport);
void sidethread_conn_close(conn *c);

/* Lock wrappers for cache functions that are called from main loop. */
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, const int incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas);
void accept_new_conns(const bool do_accept);
conn *conn_from_freelist(void);
bool  conn_add_to_freelist(conn *c);
void  conn_close_idle(conn *c);
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes);
item *item_get(const char *key, const size_t nkey, conn *c);
item *item_touch(const char *key, const size_t nkey, uint32_t exptime, conn *c);
int   item_link(item *it);
void  item_remove(item *it);
int   item_replace(item *it, item *new_it, const uint32_t hv);
void  item_unlink(item *it);
void  item_update(item *it);

void item_lock(uint32_t hv);
void *item_trylock(uint32_t hv);
void item_trylock_unlock(void *arg);
void item_unlock(uint32_t hv);
void pause_threads(enum pause_thread_types type);
unsigned short refcount_incr(unsigned short *refcount);
unsigned short refcount_decr(unsigned short *refcount);
void STATS_LOCK(void);
void STATS_UNLOCK(void);
void threadlocal_stats_reset(void);
void threadlocal_stats_aggregate(struct thread_stats *stats);
void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out);

/* Stat processing functions */
void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...);

enum store_item_type store_item(item *item, int comm, conn *c);

#if HAVE_DROP_PRIVILEGES
extern void drop_privileges(void);
#else
#define drop_privileges()
#endif

/* If supported, give compiler hints for branch prediction. */
#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

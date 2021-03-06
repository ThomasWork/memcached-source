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
 //我们仅仅在item 在60 秒内没有被重新安排过位置的情况下，
 //才重新安排它的位置，防止我们频繁地访问item
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

#define ITEM_key(item) (((char*)&((item)->data)) \  //强制类型转换为char *
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
 //任何产生状态的函数的回调函数
typedef void (*ADD_STAT)(const char *key, const uint16_t klen,
                         const char *val, const uint32_t vlen,
                         const void *cookie);

/*
 * NOTE: If you modify this table you _MUST_ update the function state_text
 */
 //一个连接的可能状态
enum conn_states {
    conn_listening,  //正在监听连接
    conn_new_cmd,    //为下一个命令准备连接/**< Prepare connection for next command */
    conn_waiting,    //正在等待一个可读的套接字
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      //写一个简单的回应
    conn_nread,      //读入固定数量的字节
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    //正在关闭这个连接
    conn_mwrite,     //正在顺序地写多个item
    conn_closed,     //连接被关闭了
    conn_watch,      //被日志线程持有作为观察器
    conn_max_state   //最大的状态数，被assertion函数使用
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
 //使用X 宏来避免在reset 和aggregation中
 //迭代(重复�)) 。不需要在超过3个的地方添加新的状态

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
    X(bytes_read) \   //读了多少字节
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
    uint64_t      rejected_conns;//拒绝的连接的数量，在描述符数量不够的时候会拒绝连接
    uint64_t      malloc_fails;//分配空间失败
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
 //全局状态，代表了不应该被擦除的状态
 //对需要经常更新的计数器，根据cache line 局部性进行排序�
struct stats_state {
    uint64_t      curr_items;
    uint64_t      curr_bytes;
    uint64_t      curr_conns;
    uint64_t      hash_bytes;       //哈希表使用了多少字节
    unsigned int  conn_structs;
    unsigned int  reserved_fds;//这个变量有什么用，在memcached_thread_init中访问过
    unsigned int  hash_power_level; //当前hash表的长度代表2的多少次方
    bool          hash_is_expanding; //当前哈希表是否正在扩容
    bool          accepting_conns;  //目前是否在接收连接
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
    int verbose;//这个是调试信息�
    rel_time_t oldest_live; //忽略比该时间老的且存在的item                                      /* ignore existing items older than this */
    uint64_t oldest_cas; //忽略CAS 值比该值小的且存在的item   /* ignore existing items with CAS values lower than this */
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
    bool lru_crawler;       //是否允许自动爬虫线程
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
#define ITEM_CHUNKED 32//如果一个item的存储方式是链接的块
#define ITEM_CHUNK 64

/**
 * Structure for storing items within memcached.
 */
 //memcached.h
 //在memcached中存储item的结构
typedef struct _stritem {
    /* Protected by LRU locks *///被LRU 锁保护
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock *///剩下的被一个item 锁保护
    struct _stritem *h_next;   //指向hash链表中的下一个节点
    rel_time_t      time;      //最近访问时间 /* least recent access */
    rel_time_t      exptime;   //过期时间 
    int             nbytes;    //数据的大小
    unsigned short  refcount;//引用计数，这个在锁的时候好像有妙用
    uint8_t         nsuffix;    /* length of flags-and-length string */
    uint8_t         it_flags;   //item 当前的状态，是否是linked，chunked等等
    uint8_t         slabs_clsid; //处于哪个slab class 中/* which slab class we're in */
    uint8_t         nkey;     //key的长度  /* key length, w/terminating null and padding */
    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS. */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS *///如果it_flags & ITEM_CAS，那么就使用8 bytes CAS，以及以null 结尾的key
    /* then null-terminated key */                                
    /* then " flags length\r\n" (no terminating null) */ //然后是"flagg length\r\n"(没有终止符null)，然后是以\r\n 结尾的data
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
    pthread_t thread_id;        //线程的唯一id
    struct event_base *base;    /* libevent handle this thread uses */
    struct event notify_event;  //对于notify_receive_fd 的监听事件
    int notify_receive_fd;      //匿名管道的读端
    int notify_send_fd;         //匿名管道的写端
    struct thread_stats stats;  //这个线程生成的状态/* Stats generated by this thread */在memcached.h中定义
    struct conn_queue *new_conn_queue; //新的将要处理的连接队列，对于老的该怎么办�
    cache_t *suffix_cache;     //这个后缀cache?
    logger *l;            //logger 缓存      /* logger buffer */
} LIBEVENT_THREAD;

 //memcached连接
typedef struct conn conn;
struct conn {
    int    sfd;
    sasl_conn_t *sasl_conn;
    bool authenticated;
    enum conn_states  state;
    enum bin_substates substate;
    rel_time_t last_cmd_time; //上一次执行指令的时间
    struct event event;
    short  ev_flags;//libevent 监听的事件标志
    short  which;   //刚刚触发了哪个事件 /** which events were just triggered */

    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    int    rsize;   /** total allocated size of rbuf */
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;
	
    enum conn_states  write_and_go;//在写完当前内容之后到哪个状态?
    void   *write_and_free; /** free this memory after finishing writing */

    char   *ritem;  /** when we read in an item's value, it goes here */
    int    rlbytes;

    /* data for the nread state */

    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the actual
     * data. The data is read into ITEM_data(item) to avoid extra copying.
     */

    void   *item;     //为了命令: set/add/replace  /* for commands set/add/replace  */

    /* data for the swallow state */
    int    sbytes;    /* how many bytes to swallow */

	//在mwrite 状态使用的数据
    struct iovec *iov;
    int    iovsize;   //iov数组有多少元素
    int    iovused;   //iov 数组中使用了多少元素

    struct msghdr *msglist;//
    int    msgsize;   //msglist数组总共有多少空间 /* number of elements allocated in msglist[] */
    int    msgused;  //msglist 使用了多少元素 /* number of elements used in msglist[] */
    int    msgcurr;  //当前正在被传输的元素 /* element in msglist[] being transmitted now */
    int    msgbytes;  //当前msg中有多少字节  /* number of bytes in current msg */
		
    item   	**ilist;   //将要向外写出的item的列表
    int    	isize;
    item  	 **icurr;
    int  	  ileft;

    char   **suffixlist;
    int  	  suffixsize;
    char   **suffixcurr;
    int    suffixleft;

    enum protocol protocol;   //使用哪个协议，二进制还是ASCII
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
    uint64_t cas; //将要返回的cas    /* the cas to return */
    short cmd; //目前正要处理的命令
    int opaque;
    int keylen;
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread;//指向服务于该连接的线程
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

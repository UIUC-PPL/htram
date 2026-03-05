#ifndef __HTRAM_H__
#define __HTRAM_H__
//#define SRC_GROUPING
//#define PER_DESTPE_BUFFER
//#define NODE_SRC_BUFFER
//#define LOCAL_BUF
//#define IDLE_FLUSH
#ifdef IDLE_FLUSH
#define PARTIAL_FLUSH 0.2
#endif
#define ALL_BUF_TYPES
#include <queue>
#include "htram_group.decl.h"

// Application-specific data type selection.
// Pass one of -DHISTO, -DPHOLD, -DIG, -DUNION_FIND, or -DGRAPH at compile time.
#ifdef SSSP
typedef std::pair<int,int> datatype;
#endif

#ifdef HISTO
typedef int datatype;
#endif

#ifdef PHOLD
typedef double datatype;
#endif

#ifdef IG
#include "packet.h"
typedef packet1 datatype;
#endif

#ifdef UNIONFIND
#include "types.h"
typedef findBossData datatype;
#endif

#ifdef GRAPH
// The application directory must pass -DHTRAM_GRAPH_TYPES_HEADER=\"/path/to/weighted_node_struct.h\"
// using the full absolute path so that a same-named file in the htram directory is not
// accidentally picked up instead.
#include HTRAM_GRAPH_TYPES_HEADER
typedef Update datatype;
typedef std::queue<datatype>** array2d_of_queues;
#endif

#include <memory>
using namespace std;
#define SIZE_LIST (int[]){1024, 512, 2048}
#define BUFSIZE 512
#define LOCAL_BUFSIZE 16
#define NODE_COUNT 512

#define TOTAL_LATENCY 0
#define MAX_LATENCY 1
#define MIN_LATENCY 2
#define TOTAL_MSGS 3
#define STATS_COUNT 4

/**
 * Aggregation modes:
 *   WPs: per-worker buffer, sorted at destination process
 *   WsP: per-worker buffer, sorted at source
 *   PP:  per-process buffer, send to processes
 *   WW:  per-worker buffer, send to workers
 */
#define WPs 0
#define WsP 1
#define PP  2
#define WW  3

template <typename T>
struct item {
  int destPe;
  T payload;
};

typedef item<datatype> itemT;

class HTramMessage : public CMessage_HTramMessage {
  public:
    HTramMessage() { next = 0; }
    HTramMessage(HTramMessage *copy) {
      next = copy->next;
      std::copy(copy->buffer, copy->buffer + next, buffer);
    }
    int next{0};
    itemT buffer[BUFSIZE];
};

class HTramLocalMessage : public CMessage_HTramLocalMessage {
  public:
    HTramLocalMessage() { next = 0; }
    HTramLocalMessage(int size, itemT *buf) : next(size) {
      std::copy(buf, buf + size, buffer);
    }
    itemT buffer[LOCAL_BUFSIZE];
    int next;
};

class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    HTramNodeMessage() : offset(CkNodeSize(CkMyNode())) {}
    datatype buffer[BUFSIZE];
    std::vector<int> offset;
};

class HTramNodeGrp : public CBase_HTramNodeGrp {
  HTramNodeGrp_SDAG_CODE
  public:
    std::atomic_int flush_count{0};
    std::atomic_int get_idx[NODE_COUNT];
    std::atomic_int done_count[NODE_COUNT];
    HTramMessage **msgBuffers;
#ifndef BUCKETS_BY_DEST
    int num_mailboxes = 0;
    std::unique_ptr<std::atomic<int>[]> mailbox_receiver;
#endif
    HTramNodeGrp();
    HTramNodeGrp(CkMigrateMessage *msg);
};

typedef void (*callback_function)(void *, datatype);
typedef void (*callback_function_retarr)(void *, datatype *, int);
typedef int  (*destproc_function)(void *, datatype);
typedef void (*end_function)(void *);

class HTram : public CBase_HTram {
  HTram_SDAG_CODE

  private:
    callback_function cb;
    callback_function_retarr cb_retarr;
    destproc_function get_dest_proc;
    end_function tram_done;
    CkGroupID client_gid;
    CProxy_HTramRecv nodeGrpProxy;
    CProxy_HTramNodeGrp srcNodeGrpProxy;
    CkCallback endCb;
    CkCallback return_cb;
    int myPE, buf_type;
    int agg;
    int tot_recv_count, tot_send_count, local_updates;
    int histo_bucket_count, direct_threshold = 0, tram_threshold = 0;
    int num_nodes;
    float selectivity = 1.0;
    bool ret_list;
    bool request;
    double flush_time;
    double msg_stats[STATS_COUNT]{0.0};
    int local_idx[NODE_COUNT];
#ifdef BUCKETS_BY_DEST
    int *updates_in_tram;
    array2d_of_queues tram_hold;
#else
    int updates_in_tram_count = 0;
    std::queue<datatype> *tram_hold;
#endif
    void *objPtr;
    HTramNodeGrp *srcNodeGrp;
    HTramRecv *nodeGrp;
    HTramMessage **msgBuffers;
    HTramLocalMessage **local_buf;
    HTramMessage *localMsgBuffer;
    std::vector<itemT> *localBuffers;
    std::vector<std::vector<HTramMessage *>> fillerOverflowBuffers;
    std::vector<std::vector<int>> fillerOverflowBuffersBucketMin;
    std::vector<std::vector<int>> fillerOverflowBuffersBucketMax;
    int nodesize = 0;
    int *nodeOf;

  public:
    bool enable_flush;
    int bufSize;
    int prevBufSize;
    int agg_msg_count;
    int flush_msg_count;
    HTram(CkGroupID recv_ngid, CkGroupID src_ngid, int buffer_size,
          bool enable_timed_flushing, double flush_timer, bool ret_item,
          bool req, CkCallback start_cb);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage *msg);
    void set_func_ptr(void (*func)(void *, datatype), void *);
    // 2-arg form used by UNIONFIND/paratreet
    void set_func_ptr_retarr(void (*func)(void *, datatype *, int), void *);
    // 4-arg form used by GRAPH (adds get_dest_proc and done callbacks)
    void set_func_ptr_retarr(void (*func)(void *, datatype *, int),
                             int  (*func2)(void *, datatype),
                             void (*func3)(void *), void *);
    int getAggregatingPE(int dest_pe);
    void copyToNodeBuf(int destnode, int increment);
    void insertValue(datatype send_value, int dest_pe);
    void insertValueWPs(datatype send_value, int dest_pe);
    void insertToProcess(datatype item, int logicNodeNum);
    void sendItemPrioDeferredDest(datatype new_update, int neighbor_bucket);
    void reset_stats(int buf_type, int buf_size, int agtype);
    void enableIdleFlush();
    void tflush(bool idleflush = false);
    void flush_everything();
    void shareArrayOfBuckets(std::vector<datatype> *new_tram_hold,
                             int bucket_count);
#ifdef BUCKETS_BY_DEST
    void insertBucketsByDest(int, int);
#else
    void insertBuckets(int);
#endif
    void changeThreshold(int, int, float);
    void sanityCheck();
    void getTotSendCount(int);
    void getTotRecvCount(int);
    void getTotTramHCount(int);
    bool idleFlush();
    void avgLatency(CkCallback cb);
    void receivePerPE(HTramMessage *);
    void receiveOnPE(HTramMessage *msg);
    void receivePerPE(HTramNodeMessage *);
    void registercb();
    void stop_periodic_flush();
};

class HTramRecv : public CBase_HTramRecv {
  HTramRecv_SDAG_CODE
    CkCallback return_cb;
#ifndef BUCKETS_BY_DEST
    callback_function_retarr cb_retarr;
    void *objPtr;
#endif

  public:
    CProxy_HTram tram_proxy;
    double msg_stats[STATS_COUNT]{0.0};
    std::atomic_int *msgs_in_transit;
    std::atomic_int *msgs_received_from;
    HTramRecv();
    HTramRecv(CkMigrateMessage *msg);
    void setTramProxy(CkGroupID);
    void receiveOnProc(HTramMessage *);
#ifndef BUCKETS_BY_DEST
    void set_func_ptr_retarr(void (*func)(void *, datatype *, int), void *);
#endif
    void receive(HTramMessage *);
    void receive_no_sort(HTramMessage *);
    void receive_small(HTramLocalMessage *);
    void avgLatency(CkCallback cb);
};
#endif

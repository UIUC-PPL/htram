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
#define BUFSIZE 2048 //max num of items allocated in a buffer
#define LOCAL_BUFSIZE 16

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

// The payload of every htram message is a genuine varsize array, declared as
// raw bytes in the .ci and viewed through a typed accessor here. The generated
// allocator aligns each varsize field to ALIGN_DEFAULT (16 B), which covers
// the alignment of any payload type htram carries.
static_assert(alignof(itemT) <= 16, "itemT alignment exceeds message alignment");
static_assert(alignof(datatype) <= 16, "datatype alignment exceeds message alignment");

class HTramMessage : public CMessage_HTramMessage {
  public:
    // `cap` is the number of items the varsize array was allocated for. It is
    // the invariant every fill site relies on: next <= cap, and cap >= bufSize
    // for any buffer the library will fill.
    int next{0};
    int cap{0};
    char *buffer;
    itemT *items() { return reinterpret_cast<itemT *>(buffer); }
    const itemT *items() const { return reinterpret_cast<const itemT *>(buffer); }
    // Bytes actually occupied, for setUsersize on a partially filled buffer.
    size_t usedBytes() const {
      return ALIGN_DEFAULT(sizeof(HTramMessage)) + sizeof(itemT) * (size_t)next;
    }
};

// Allocate a message able to hold `capacity` items.
inline HTramMessage *newHTramMessage(int capacity) {
  HTramMessage *m = new (capacity * (int)sizeof(itemT)) HTramMessage();
  m->next = 0;
  m->cap = capacity;
  return m;
}

// Shrink the envelope to the bytes actually filled. A full buffer already
// occupies its whole allocation, but this is correct in either case, so it is
// applied uniformly on every send rather than only on the flush paths.
inline void trimHTramMessage(HTramMessage *m) {
  ((envelope *)UsrToEnv(m))->setUsersize(m->usedBytes());
}

class HTramLocalMessage : public CMessage_HTramLocalMessage {
  public:
    int next{0};
    int cap{0};
    char *buffer;
    itemT *items() { return reinterpret_cast<itemT *>(buffer); }
};

inline HTramLocalMessage *newHTramLocalMessage(int capacity) {
  HTramLocalMessage *m = new (capacity * (int)sizeof(itemT)) HTramLocalMessage();
  m->next = 0;
  m->cap = capacity;
  return m;
}

class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    // One offset per PE on the receiving node, and one payload slot per item
    // the source message actually carried -- not BUFSIZE of each.
    int noffsets{0};
    char *buffer;
    int *offset;
    datatype *items() { return reinterpret_cast<datatype *>(buffer); }
};

inline HTramNodeMessage *newHTramNodeMessage(int capacity, int noffsets) {
  HTramNodeMessage *m =
      new (capacity * (int)sizeof(datatype), noffsets) HTramNodeMessage();
  m->noffsets = noffsets;
  return m;
}

class HTramNodeGrp : public CBase_HTramNodeGrp {
  HTramNodeGrp_SDAG_CODE
  public:
    std::atomic_int flush_count{0};
    // Sized at construction from CkNumNodes(). These were fixed 512-element
    // arrays while the constructor loops to CkNumNodes(), so any run on more
    // than 512 nodes wrote past them into whatever followed -- silently, and
    // within the range of node counts this code is meant to scale to.
    std::unique_ptr<std::atomic<int>[]> get_idx;
    std::unique_ptr<std::atomic<int>[]> done_count;
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
    CkCallback quiesce_cb;
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
    std::unique_ptr<int[]> local_idx; // CkNumNodes() entries
    // Number of destinations actually in use: nodes under WPs/WsP/PP, PEs
    // under WW. Per-destination structures are sized by this, not by
    // CkNumPes(), which over-allocates by the node size in every mode but WW.
    int destCount() const { return (agg == WW) ? CkNumPes() : CkNumNodes(); }
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
    std::vector<itemT> *localBuffers;
    std::vector<std::vector<HTramMessage *>> fillerOverflowBuffers;
    std::vector<std::vector<int>> fillerOverflowBuffersBucketMin;
    std::vector<std::vector<int>> fillerOverflowBuffersBucketMax;
    int nodesize = 0;
    int *nodeOf;
    // Set the envelope size and account for the message. Every send goes
    // through here so that a run reports the bytes it actually shipped, not
    // the bytes it allocated -- the paper reports wire volume alongside wall
    // clock, and those two differed by up to 4x before messages were varsize.
    void trim(HTramMessage *m);

  public:
    bool enable_flush;
    int bufSize;
    int prevBufSize;
    int agg_msg_count;
    int flush_msg_count;
    unsigned long long bytes_sent = 0;   // envelope user bytes handed to sends
    unsigned long long bytes_alloc = 0;  // bytes allocated for those messages
    unsigned long long msgs_sent = 0;
    HTram(CkGroupID recv_ngid, CkGroupID src_ngid, int buffer_size,
          bool enable_timed_flushing, double flush_timer, bool ret_item,
          bool req, CkCallback start_cb);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage *msg);
    void setBufferSize(int new_size);
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
    void htramQuiesce(CkCallback cb);
    void onQD();
    void countBuffers();
    void onBufferCount(int total);
    void getTotSendCount(int);
    void getTotRecvCount(int);
    void getTotTramHCount(int);
    void tramStats(CkCallback cb);
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
    // Node messages are built on the receiving node, so these are allocation
    // bytes rather than wire bytes. They were a fixed BUFSIZE payload each.
    std::atomic<unsigned long long> node_msg_bytes{0};
    std::atomic<unsigned long long> node_msgs{0};
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

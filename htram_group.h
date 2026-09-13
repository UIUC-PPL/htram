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
#include "htram_combine.h"

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
    //
    // The payload offset is read back from the pointer the generated allocator
    // installed rather than recomputed from sizeof(). Restating the allocator's
    // formula here is what went wrong before: the old code wrote
    // sizeof(int) + sizeof(itemT)*next, which is the offset of the payload only
    // if the first field is followed by no padding and the message is not
    // varsize -- neither of which held. Anything derived from `buffer` itself
    // cannot drift from where the payload actually is, whatever charmc does
    // with alignment or with fields added to this class later.
    size_t payloadOffset() const {
      return (size_t)(buffer - reinterpret_cast<const char *>(this));
    }
    size_t usedBytes() const {
      return payloadOffset() + sizeof(itemT) * (size_t)next;
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

// Receive-side check that the envelope actually carried the items the message
// claims to hold. Compiled out with -DHTRAM_NO_ENVELOPE_CHECK; otherwise it is
// one comparison per received message, not per item.
//
// This exists because a sender-side size formula that undercounts the payload
// is invisible inside a process: a local send hands over the same pointer, so
// the receiver reads the whole allocation whatever the envelope says. Only a
// message that leaves the address space is truncated to its declared size, and
// then reading item next-1 runs off the end of the received buffer. That makes
// the defect a multi-node-only, silent out-of-bounds read -- so the invariant
// is asserted where it is observable, at every landing point, rather than left
// to whichever run happens to notice corrupted values.
//
// setUsersize rounds up to ALIGN_DEFAULT, so getUsersize() >= the size the
// sender asked for and the comparison is not tautological: it asks whether the
// bytes that arrived cover the items that are about to be read.
inline void checkHTramEnvelope(const void *msg, size_t need, int items,
                               const char *where) {
#ifndef HTRAM_NO_ENVELOPE_CHECK
  size_t have = ((envelope *)UsrToEnv(const_cast<void *>(msg)))->getUsersize();
  if (have < need)
    CkAbort("htram: %s got a message declaring %d items, which need %zu user "
            "bytes, in an envelope carrying only %zu. The sender under-sized "
            "it; reading the last item would run past the received buffer.",
            where, items, need, have);
#else
  (void)msg; (void)need; (void)items; (void)where;
#endif
}

class HTramLocalMessage : public CMessage_HTramLocalMessage {
  public:
    int next{0};
    int cap{0};
    char *buffer;
    itemT *items() { return reinterpret_cast<itemT *>(buffer); }
    // Same reasoning as HTramMessage::usedBytes().
    size_t payloadOffset() const {
      return (size_t)(buffer - reinterpret_cast<const char *>(this));
    }
    size_t usedBytes() const {
      return payloadOffset() + sizeof(itemT) * (size_t)next;
    }
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
    // What the allocator actually reserved: the offset it placed the last
    // varsize array at, plus that array's own aligned length. Read back from
    // the installed pointer for the same reason as HTramMessage::usedBytes().
    // Node messages never leave the node, so this is for accounting rather
    // than for an envelope, but it is the same formula either way.
    size_t allocBytes() const {
      return (size_t)(reinterpret_cast<const char *>(offset) -
                      reinterpret_cast<const char *>(this)) +
             ALIGN_DEFAULT(sizeof(int) * (size_t)noffsets);
    }
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
    // Buffers to each destination that reached bufSize and shipped on their
    // own since the last flushStale(). Zero means traffic to that destination
    // is not filling anything, so whatever is sitting in its buffer will stay
    // there until somebody flushes it.
    int *full_sends;
    void noteFullSend(int dest) { full_sends[dest]++; }
    void flushDest(int dest);
    // One per destination when combining is on; null otherwise, and then
    // nothing below is reached. With combining on, the holds replace both
    // tram_hold and the direct path into msgBuffers: every item waits in its
    // destination's hold, where a later item for the same key can fold into
    // it, until a full buffer's worth has been admitted by the threshold or
    // a flush reaches it.
    CombiningHold *holds = nullptr;
    void releaseFull(int dest);
    void shipBuffer(int dest, bool full);
    void appendHeld(HTramMessage *m, const void *item) {
      datatype value;
      std::memcpy(&value, item, sizeof(datatype));
      m->items()[m->next].payload = value;
      m->items()[m->next].destPe = get_dest_proc(objPtr, value);
      m->next++;
    }
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
    // Flush only the destinations whose buffers did not fill on their own
    // since the previous call. Meant to be called once per application round:
    // it is the adaptive half of the flush cadence, and costs nothing for a
    // destination that is already shipping full buffers.
    void flushStale();
    // Flush every destination holding admitted items. For a caller that has
    // nothing left to do: this PE cannot add to any buffer until a message
    // arrives, so waiting for one to fill is pure latency. Cheap enough to
    // call on every idle scheduler pass, and a no-op outside WPs/WW, which
    // are the only modes with the per-destination counters that make it so.
    void flushIdle();
    unsigned long long stale_flushes = 0; // destinations flushed by flushStale
    unsigned long long idle_flushes = 0;  // destinations flushed by flushIdle
    // Turn on source-side combining. Must be called before the first send.
    // `ops` must outlive the library; `client` is handed to ops->on_absorb.
    void enableCombining(const HoldOps *ops, void *client);
    bool combining() const {
#ifdef BUCKETS_BY_DEST
      return holds != nullptr;
#else
      return false;
#endif
    }
    void flush_everything();
    void setHistoBucketCount(int bucket_count);
#ifdef BUCKETS_BY_DEST
    void insertBucketsByDest(int, int);
#else
    void insertBuckets(int);
#endif
    void changeThreshold(int, int, float);
    // Merge every k adjacent priority buckets into one: bucket i becomes i / k,
    // and both thresholds follow. Exact for an application whose bucket index
    // is floor(distance / width) and which multiplies its width by k at the
    // same time, since floor(floor(x) / k) == floor(x / k). Not supported with
    // combining on.
    void coarsenBuckets(int k);
    // Recount what the admitted counters should hold -- held items at or
    // below the threshold plus everything sitting in the buffer -- and return
    // the total absolute difference. Walks every held queue: a debugging aid
    // for diagnosis builds, not for a timed path.
    long long admittedDrift() const;
    // Held / admitted / buffered item counts for this PE, for a client that
    // has stopped making progress. Walks every held queue: a stall report,
    // not a round.
    void pendingItems(long long *held, long long *admitted,
                      long long *buffered) const;
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

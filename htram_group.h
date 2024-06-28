#ifndef __HTRAM_H__
#define __HTRAM_H__
//#define SRC_GROUPING
//#define PER_DESTPE_BUFFER
//#define NODE_SRC_BUFFER
//#define LOCAL_BUF
#define ALL_BUF_TYPES
#include "htram_group.decl.h"
/* readonly */ extern CProxy_HTram tram_proxy;
///* readonly */ extern CProxy_HTramRecv nodeGrpProxy;
///* readonly */ extern CProxy_HTramNodeGrp srcNodeGrpProxy;
#include "packet.h"
using namespace std;
#define SIZE_LIST (int[]){1024, 512, 2048}
#define BUFSIZE 1024
#define BUFSIZE_SMALL 512
#define BUFSIZE_MED 1024
#define BUFSIZE_LARGE1 2048
#define BUFSIZE_LARGE2 4096
#define LOCAL_BUFSIZE 16
#define PPN_COUNT 8
#define NODE_COUNT 64

#define TOTAL_LATENCY 0
#define MAX_LATENCY 1
#define MIN_LATENCY 2
#define TOTAL_MSGS 3
#define STATS_COUNT 4

#define PNs 0
#define PsN 1
#define NNs 2
#define PP 3
template <typename T>
struct item {
//#if !defined(SRC_GROUPING) && !defined(PER_DESTPE_BUFFER)
  int destPe;
//#endif
  T payload;
};

//typedef std::pair<int,int> datatype;
//typedef int datatype;

typedef packet1 datatype;

typedef item<datatype> itemT;

class BaseMsg {
  public:
  virtual itemT* getBuffer(){};
  virtual double* getTimer(){};
  virtual int* getIndex(){};
  virtual int* getNext(){};
  virtual int* getDoTimer(){};
};

class HTramMessage : public BaseMsg, public CMessage_HTramMessage {
  public:
    HTramMessage() {next = 0;}
    HTramMessage(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[BUFSIZE];
    int index[PPN_COUNT] = {-1};
    double timer[2];
    int do_timer {1};
    int next{0}; //next available slot in buffer
    itemT* getBuffer() {return buffer;}
    double* getTimer() {return timer;}
    int* getIndex() {return index;}
    int* getNext() {return &next;}
    int* getDoTimer() {return &do_timer;}
};

class HTramMessageSmall : public BaseMsg, public CMessage_HTramMessageSmall {
  public:
    HTramMessageSmall() {next = 0;}
    HTramMessageSmall(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[BUFSIZE_SMALL];
    int index[PPN_COUNT] = {-1};
    double timer[2];
    int do_timer {1};
    int next{0}; //next available slot in buffer
    itemT* getBuffer() {return buffer;}
    double* getTimer() {return timer;}
    int* getIndex() {return index;}
    int* getNext() {return &next;}
    int* getDoTimer() {return &do_timer;}
};

class HTramMessageLarge : public BaseMsg, public CMessage_HTramMessageLarge {
  public:
    HTramMessageLarge() {next = 0;}
    HTramMessageLarge(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[BUFSIZE_LARGE1];
    int index[PPN_COUNT] = {-1};
    double timer[2];
    int do_timer {1};
    int next{0}; //next available slot in buffer
    itemT* getBuffer() {return buffer;}
    double* getTimer() {return timer;}
    int* getIndex() {return index;}
    int* getNext() {return &next;}
    int* getDoTimer() {return &do_timer;}
};

class HTramLocalMessage : public CMessage_HTramLocalMessage {
  public:
    HTramLocalMessage() {next = 0;}
    HTramLocalMessage(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[LOCAL_BUFSIZE];
    int next; //next available slot in buffer
};


class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    HTramNodeMessage() {}
    datatype buffer[BUFSIZE_LARGE1];
    int offset[PPN_COUNT];
};

class HTramNodeGrp : public CBase_HTramNodeGrp {
  HTramNodeGrp_SDAG_CODE
  public:
    std::atomic_int flush_count{0};
    std::atomic_int get_idx[NODE_COUNT];
    std::atomic_int done_count[NODE_COUNT];
//    HTramMessage
    BaseMsg **msgBuffers;
    HTramNodeGrp();
    HTramNodeGrp(CkMigrateMessage* msg);
};

typedef void (*callback_function)(void*, datatype);
typedef void (*callback_function_retarr)(void*, datatype*, int);

class HTram : public CBase_HTram {
  HTram_SDAG_CODE

  private:
    callback_function cb;
    callback_function_retarr cb_retarr;
    CkGroupID client_gid;
    CProxy_HTramRecv nodeGrpProxy;
    CProxy_HTramNodeGrp srcNodeGrpProxy;
    CkCallback endCb;
    CkCallback return_cb;
    int myPE, buf_type;
    int agg;
    bool ret_list;
    bool request;
    double flush_time;
    double msg_stats[STATS_COUNT] {0.0};
    int local_idx[NODE_COUNT];
    void* objPtr;
    HTramNodeGrp* srcNodeGrp;
    HTramRecv* nodeGrp;
//    HTramMessage
    BaseMsg **msgBuffers;
    HTramLocalMessage **local_buf;
    HTramMessage *localMsgBuffer;
    std::vector<itemT>* localBuffers;
  public:
    bool enable_flush;
    int bufSize;
    int prevBufSize;
    HTram(CkGroupID recv_ngid, CkGroupID src_ngid, int buffer_size, bool enable_timed_flushing, double flush_timer, bool ret_item, bool req, CkCallback start_cb);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage* msg);
    void set_func_ptr(void (*func)(void*, datatype), void*);
    void set_func_ptr_retarr(void (*func)(void*, datatype*, int), void*);
    int getAggregatingPE(int dest_pe);
    void copyToNodeBuf(int destnode, int increment);
    void insertValue(datatype send_value, int dest_pe);
    void reset_stats(int buf_type, int buf_size, int agtype);
    void tflush();
    void avgLatency(CkCallback cb);
//#ifdef SRC_GROUPING
    void receivePerPE(HTramMessage *);
//#elif defined PER_DESTPE_BUFFER
    void receiveOnPE(HTramMessage* msg);
    void receiveOnPESmall(HTramMessageSmall* msg);
    void receiveOnPELarge(HTramMessageLarge* msg);
//    void receiveOnPELarge(HTramMessageSmall* msg);
//#else
    void receivePerPE(HTramNodeMessage *);
//#endif
    void registercb();
    void stop_periodic_flush();
};


class HTramRecv : public CBase_HTramRecv {
  HTramRecv_SDAG_CODE
    CkCallback return_cb;
  public:
    CProxy_HTram tram_proxy;
    double msg_stats[STATS_COUNT] {0.0};
    HTramRecv();
    HTramRecv(CkMigrateMessage* msg);
    void setTramProxy(CkGroupID);
//#ifndef PER_DESTPE_BUFFER
    void receive(HTramMessage*);
    void receiveSmall(HTramMessageSmall*);
    void receiveLarge(HTramMessageLarge*);

    void receive_no_sort(HTramMessage*);
    void receive_no_sortSmall(HTramMessageSmall*);
    void receive_no_sortLarge(HTramMessageLarge*);
    void receive_small(HTramLocalMessage*);
    void avgLatency(CkCallback cb);
//#endif
};
#endif

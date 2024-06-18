#ifndef __HTRAM_H__
#define __HTRAM_H__
//#define SRC_GROUPING
//#define PER_DESTPE_BUFFER
//#define NODE_SRC_BUFFER
//#define LOCAL_BUF
#include "htram_group.decl.h"
/* readonly */ extern CProxy_HTram tram_proxy;
/* readonly */ extern CProxy_HTramRecv nodeGrpProxy;
/* readonly */ extern CProxy_HTramNodeGrp srcNodeGrpProxy;

using namespace std;
#define BUFSIZE 1024
#define LOCAL_BUFSIZE 16
#define PPN_COUNT 64
#define NODE_COUNT 32

template <typename T>
struct item {
#if !defined(SRC_GROUPING) && !defined(PER_DESTPE_BUFFER)
  int destPe;
#endif
  T payload;
};

//typedef std::pair<int,int> datatype;
typedef int datatype;

typedef item<datatype> itemT;

class HTramMessage : public CMessage_HTramMessage {
  public:
    HTramMessage() {next = 0;}
    HTramMessage(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[BUFSIZE];
#ifdef SRC_GROUPING
    int index[PPN_COUNT] = {-1};
#endif
    int next; //next available slot in buffer
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
    datatype buffer[BUFSIZE];
    int offset[PPN_COUNT];
};

class HTramNodeGrp : public CBase_HTramNodeGrp {
  HTramNodeGrp_SDAG_CODE
  public:
    std::atomic_int flush_count{0};
    std::atomic_int get_idx[NODE_COUNT];
    std::atomic_int done_count[NODE_COUNT];
    HTramMessage **msgBuffers;
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
    CkCallback endCb;
    int myPE;
    bool ret_list;
    double flush_time;
    int local_idx[NODE_COUNT];
    void* objPtr;
    HTramMessage **msgBuffers;
    HTramLocalMessage **local_buf;
    HTramMessage *localMsgBuffer;
    std::vector<itemT>* localBuffers;
  public:
    bool enable_flush;
    HTram(CkGroupID gid, int buffer_size, bool enable_timed_flushing, double flush_timer, bool ret_item);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage* msg);
    void set_func_ptr(void (*func)(void*, datatype), void*);
    void set_func_ptr_retarr(void (*func)(void*, datatype*, int), void*);
    int getAggregatingPE(int dest_pe);
    void copyToNodeBuf(int destnode, int increment);
    void insertValue(datatype send_value, int dest_pe);
    void tflush();
#ifdef SRC_GROUPING
    void receivePerPE(HTramMessage *);
#elif defined PER_DESTPE_BUFFER
    void receiveOnPE(HTramMessage* msg);
#else
    void receivePerPE(HTramNodeMessage *);
#endif
    void registercb();
    void stop_periodic_flush();
};


class HTramRecv : public CBase_HTramRecv {
  HTramRecv_SDAG_CODE

  public:
    HTramRecv();
    HTramRecv(CkMigrateMessage* msg);
#ifndef PER_DESTPE_BUFFER
    void receive(HTramMessage*);
    void receive_small(HTramLocalMessage*);
#endif
};
#endif

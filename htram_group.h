#ifndef __HTRAM_H__
#define __HTRAM_H__
//#define SRC_GROUPING
#include "htram_group.decl.h"
/* readonly */ extern CProxy_HTram tram_proxy;
/* readonly */ extern CProxy_HTramRecv nodeGrpProxy;
/* readonly */ extern CProxy_HTramNodeGrp srcNodeGrpProxy;

using namespace std;
#define BUFSIZE 1024
#define LOCAL_BUFSIZE 32
#define PPN_COUNT 64

typedef struct item {
#ifndef SRC_GROUPING
  int destPe;
#endif
  int payload;
} itemT; //make customized size

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


class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    HTramNodeMessage() {}
    int buffer[BUFSIZE];
    int offset[PPN_COUNT];
};

class HTramNodeGrp : public CBase_HTramNodeGrp {
  HTramNodeGrp_SDAG_CODE
  public:
    HTramMessage **msgBuffers;
    CmiNodeLock *locks;
    HTramNodeGrp();
    HTramNodeGrp(CkMigrateMessage* msg);
};

typedef void (*callback_function)(void*, int);

class HTram : public CBase_HTram {
  HTram_SDAG_CODE

  private:
    callback_function cb;
    CkGroupID client_gid;
    CkCallback endCb;
    int myPE;
    double flush_time;
    void* objPtr;
    HTramMessage **msgBuffers;
    std::vector<itemT>* localBuffers;
  public:
    bool enable_flush;
    HTram(CkGroupID gid, int buffer_size, bool enable_timed_flushing, double flush_timer);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage* msg);
    void set_func_ptr(void (*func)(void*, int), void*);
    int getAggregatingPE(int dest_pe);
    void insertValue(int send_value, int dest_pe);
    void tflush();
#ifdef SRC_GROUPING
    void receivePerPE(HTramMessage *);
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
    void receive(HTramMessage*);
};
#endif

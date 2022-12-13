#ifndef __HTRAM_H__
#define __HTRAM_H__
#include "htram_group.decl.h"
/* readonly */ extern CProxy_HTram tram_proxy;
/* readonly */ extern CProxy_HTramRecv nodeGrpProxy;

using namespace std;
#define BUFSIZE 4096//1024
#define PPN_COUNT 64
#define POOL_SIZE 256

typedef struct item {
  int destPe;
  int payload;
} itemT; //make customized size

class HTramMessage : public CMessage_HTramMessage {
  public:
    HTramMessage() {next = 0;}
    HTramMessage(int size, itemT *buf): next(size) {
      std::copy(buf, buf+size, buffer);
    }
    itemT buffer[BUFSIZE];
    int next; //next available slot in buffer
};


class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    HTramNodeMessage() {}
    int buffer[BUFSIZE];
    int offset[PPN_COUNT];
    int *pool_index_use;
    int poolIndex;
    std::atomic<int> refCount {0};
};

typedef void (*callback_function)(void*, int);

class HTram : public CBase_HTram {
  HTram_SDAG_CODE

  private:
    callback_function cb;
    CkGroupID client_gid;
    CkCallback endCb;
    int myPE;
    void* objPtr;
    HTramMessage **msgBuffers;
  public:
    HTram(CkGroupID gid, int buffer_size, bool enable_timed_flushing, double flush_timer);
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage* msg);
//    void set_func_ptr(void (*func)(void*, int), void*);
    int getAggregatingPE(int dest_pe);
    void insertValue(int send_value, int dest_pe);
    void tflush();
//    void receivePerPE(HTramNodeMessage *);
};


class HTramRecv : public CBase_HTramRecv {
  HTramRecv_SDAG_CODE
     HTramNodeMessage* sorted_agg_message;
    HTramNodeMessage* msg_pool[POOL_SIZE];
    int in_use[POOL_SIZE];
    callback_function cb;
    CkGroupID client_gid;
    CkCallback endCb;
    int myPE;
    void* objPtr;
  public:
    HTramRecv();
    HTramRecv(CkMigrateMessage* msg);
    void set_func_ptr(void (*func)(void*, int), void*);
    void receive(HTramMessage*);
    void receivePerPE(HTramNodeMessage *);
};
#endif

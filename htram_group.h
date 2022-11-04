#ifndef __HTRAM_H__
#define __HTRAM_H__
#include "htram_group.decl.h"
/* readonly */ extern CProxy_HTram htramProxy;
/* readonly */ extern CProxy_HTramRecv nodeGrpProxy;

using namespace std;
#define BUFSIZE 1024
#define PPN_COUNT 32
//#define SORT 1

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
#if SORT
    int lrange[PPN_COUNT];
    int urange[PPN_COUNT];
#endif
    int next; //next available slot in buffer
};


class HTramNodeMessage : public CMessage_HTramNodeMessage {
  public:
    HTramNodeMessage() {}
    int buffer[BUFSIZE];
    int offset[PPN_COUNT];
};

typedef void (*callback_function)(CkGroupID, void*, int);

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
    HTram(CkGroupID gid, CkCallback cb);
    HTram(CkMigrateMessage* msg);
    void setCb(void (*func)(CkGroupID, void*, int), void*);
    int getAggregatingPE(int dest_pe);
    void insertValue(int send_value, int dest_pe);
    void tflush();
#if SORT
    void receivePerPE(HTramMessage *);
#else
    void receivePerPE(HTramNodeMessage *);
#endif
};


class HTramRecv : public CBase_HTramRecv {
  HTramRecv_SDAG_CODE

  public:
    HTramRecv();
    HTramRecv(CkMigrateMessage* msg);
    void receive(HTramMessage*);
};
#endif

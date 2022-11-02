#ifndef __TRAM_NON_SMP_H__
#define __TRAM_NON_SMP_H__

#include "tramNonSmp.decl.h"

/* readonly */ extern CProxy_tramNonSmp tramNonSmpProxy;

#define PAYLOAD_BUFFER_SIZE 1024

struct tramNonSmpMsg : public CMessage_tramNonSmpMsg {

    tramNonSmpMsg() : next(0) {}

    tramNonSmpMsg(int size, CmiInt8* buf) : next(size) {
        std::copy(buf, buf + size, payload_buffer);
    }

    int next;
    CmiInt8 payload_buffer[PAYLOAD_BUFFER_SIZE];
};

class tramNonSmp : public CBase_tramNonSmp {
private:
    using function_ptr = void (*)(void*, CmiInt8);

    function_ptr func_ptr;
    void* obj_ptr;
    tramNonSmpMsg **msgBuffers;

public:
    tramNonSmp(CkMigrateMessage* msg);
    tramNonSmp();

    // Locally accessed function
    void set_func_ptr(function_ptr ptr, void* obj_ptr);

    // Entry methods
    void insertValue(int send_value, int dest_pe);
    void tflush();
    void receive(tramNonSmpMsg *);
};

#endif

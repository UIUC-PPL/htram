#ifndef __TRAM_NON_SMP_H__
#define __TRAM_NON_SMP_H__

#include "tramNonSmp.decl.h"

#define PAYLOAD_BUFFER_SIZE 1024

template <typename T>
struct tramNonSmpMsg : public CMessage_tramNonSmpMsg<T> {

    using value_type = T;

    tramNonSmpMsg() : next(0) {}

    tramNonSmpMsg(int size, CmiInt8* buf) : next(size) {
        std::copy(buf, buf + size, payload_buffer);
    }

    int next;
    value_type payload_buffer[PAYLOAD_BUFFER_SIZE];
};

template <typename T>
class tramNonSmp : public CBase_tramNonSmp<T> {
private:
    using value_type = T;
    using function_ptr = void (*)(void*, value_type const&);
    using buff_function_ptr = void(*)(void*, tramNonSmpMsg<value_type>*);

    function_ptr func_ptr;
    buff_function_ptr buff_func_ptr;
    void* obj_ptr;
    tramNonSmpMsg<value_type> **msgBuffers;

    bool is_itemized;

public:
    tramNonSmp(CkMigrateMessage* msg);
    
    tramNonSmp();

    // Locally accessed function
    void set_func_ptr(function_ptr fptr, void* optr);
    void set_buffered_func_ptr(buff_function_ptr fptr, void* optr);
    void set_itemized(bool value);

    // Entry methods
    void insertValue(value_type const& value, int dest_pe);
    void tflush();
    void receive(tramNonSmpMsg<value_type>* msg);
};

template <typename T>
tramNonSmp<T>::tramNonSmp(CkMigrateMessage* msg) {}

template <typename T>
tramNonSmp<T>::tramNonSmp() : func_ptr(nullptr), obj_ptr(nullptr), is_itemized(true) {
    msgBuffers = new tramNonSmpMsg<value_type>*[CkNumPes()];
    for (int i = 0; i != CkNumPes(); ++i)
        msgBuffers[i] = new tramNonSmpMsg<value_type>();
}

template <typename T>
void tramNonSmp<T>::set_func_ptr(function_ptr fptr, void* optr) {
    func_ptr = fptr;
    obj_ptr = optr;
}

template <typename T>
void tramNonSmp<T>::set_buffered_func_ptr(buff_function_ptr fptr, void* optr) {
    buff_func_ptr = fptr;
    obj_ptr = optr;
}

template <typename T>
void tramNonSmp<T>::set_itemized(bool value) {
    is_itemized = value;
}

template <typename T>
void tramNonSmp<T>::insertValue(value_type const& value, int dest_pe) {
    // Buffer the message
    tramNonSmpMsg<value_type>* destMsg = msgBuffers[dest_pe];
    destMsg->payload_buffer[destMsg->next] = value;
    destMsg->next++;

    if (destMsg->next == PAYLOAD_BUFFER_SIZE) {
        // Flush message to destination PE if its filled
        this->thisProxy[dest_pe].receive(destMsg);
        msgBuffers[dest_pe] = new tramNonSmpMsg<value_type>();
    }
}

template <typename T>
void tramNonSmp<T>::tflush() {
    for (int i = 0; i < CkNumPes(); ++i) {
        this->thisProxy[i].receive(msgBuffers[i]);
        msgBuffers[i] = new tramNonSmpMsg<value_type>();
    }
}

template <typename T>
void tramNonSmp<T>::receive(tramNonSmpMsg<T>* msg) {
    // Call the callback function
    int limit = msg->next;
    if (is_itemized)
        for (int i = 0; i != limit; ++i) {
            func_ptr(obj_ptr, msg->payload_buffer[i]);
        }
    else
        buff_func_ptr(obj_ptr, msg);
}

#define CK_TEMPLATES_ONLY
#include "tramNonSmp.def.h"
#undef CK_TEMPLATES_ONLY

#endif

#include "tramNonSmp.h"

tramNonSmp::tramNonSmp() : func_ptr(nullptr), obj_ptr(nullptr) {
    msgBuffers = new tramNonSmpMsg*[CkNumPes()];
    for (int i = 0; i != CkNumPes(); ++i)
        msgBuffers[i] = new tramNonSmpMsg();
}

tramNonSmp::tramNonSmp(CkMigrateMessage* msg) {}

void tramNonSmp::insertValue(int value, int dest_pe) {
    // Buffer the message
    tramNonSmpMsg* destMsg = msgBuffers[dest_pe];
    destMsg->payload_buffer[destMsg->next] = value;
    destMsg->next++;

    if (destMsg->next == PAYLOAD_BUFFER_SIZE) {
        // Flush message to destination PE if its filled
        thisProxy[dest_pe].receive(destMsg);
        msgBuffers[dest_pe] = new tramNonSmpMsg();
    }
}

void tramNonSmp::set_func_ptr(function_ptr func_ptr_, void* obj_ptr_) {
    func_ptr = func_ptr_;
    obj_ptr = obj_ptr_;
}

void tramNonSmp::tflush() {
    for (int i = 0; i < CkNumPes(); ++i) {
        thisProxy[i].receive(msgBuffers[i]);
        msgBuffers[i] = new tramNonSmpMsg();
    }
}

void tramNonSmp::receive(tramNonSmpMsg* msg) {
    // Call the callback function
    int limit = msg->next;
    for (int i = 0; i != limit; ++i) {
        func_ptr(obj_ptr, msg->payload_buffer[i]);
    }
}

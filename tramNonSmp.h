#ifndef __TRAM_NON_SMP_H__
#define __TRAM_NON_SMP_H__

#include "tramNonSmp.decl.h"

template <typename T, typename SingletonClass>
class singleton
{
public:
    using value_type = T;
    using class_type = SingletonClass;

    // Non-copyable, non-movable
    singleton(singleton const&) = delete;
    singleton(singleton&&) = delete;
    singleton& operator=(singleton const&) = delete;
    singleton& operator=(singleton&&) = delete;

    static const std::unique_ptr<value_type>& instance()
    {
        static std::unique_ptr<value_type> inst{new value_type()};

        return inst;
    }

protected:
    singleton() = default;
};

#define TRAM_GENERATE_SINGLETON(type, name)                                      \
    class name : public singleton<type, name>                        \
    {                                                                          \
    private:                                                                   \
        name() = default;                                                      \
    }

#define TRAM_ACCESS_SINGLETON(name) (*name::instance())

using buffer_t = int;
TRAM_GENERATE_SINGLETON(buffer_t, payload_buffer_size);

template <typename T>
struct tramNonSmpMsg : public CMessage_tramNonSmpMsg<T> {

    using value_type = T;

    tramNonSmpMsg() : next(0) {}

    tramNonSmpMsg(int size, value_type* buf) : next(size) {
        std::copy(buf, buf + size, payload_buffer);
    }

    int next;
    value_type* payload_buffer;
};

template <typename T>
tramNonSmpMsg<T>* make_tram_msg(int size_) {
    auto* msg = new (&size_) tramNonSmpMsg<T>();

    return msg;
}

template <typename T>
tramNonSmpMsg<T>* make_tram_msg(int size_, tramNonSmp<T>* buffer_) {
    auto* msg = new (&size_) tramNonSmpMsg<T>(size_, buffer_);

    return msg;
}

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

    tramNonSmp(int);

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
    buffer_t& buffer_size = TRAM_ACCESS_SINGLETON(payload_buffer_size);
    buffer_size = 1024;

    // Question: Does this also needs to be double pointer? I think not.
    msgBuffers = new tramNonSmpMsg<value_type>*[CkNumPes()];

    for (int i = 0; i != CkNumPes(); ++i)
        msgBuffers[i] = make_tram_msg<value_type>(buffer_size);
}

template <typename T>
tramNonSmp<T>::tramNonSmp(int buffer_size_) 
: func_ptr(nullptr), obj_ptr(nullptr), is_itemized(true) {

    buffer_t& buffer_size = TRAM_ACCESS_SINGLETON(payload_buffer_size);
    buffer_size = buffer_size_;

    // Question: Does this also needs to be double pointer? I think not.
    msgBuffers = new tramNonSmpMsg<value_type>*[CkNumPes()];

    for (int i = 0; i != CkNumPes(); ++i)
        msgBuffers[i] = make_tram_msg<value_type>(buffer_size);
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

    buffer_t& buffer_size = TRAM_ACCESS_SINGLETON(payload_buffer_size);

    if (destMsg->next == TRAM_ACCESS_SINGLETON(payload_buffer_size)) {
        // Flush message to destination PE if its filled
        this->thisProxy[dest_pe].receive(destMsg);
        msgBuffers[dest_pe] = make_tram_msg<value_type>(buffer_size);
    }
}

template <typename T>
void tramNonSmp<T>::tflush() {
    buffer_t& buffer_size = TRAM_ACCESS_SINGLETON(payload_buffer_size);

    for (int i = 0; i < CkNumPes(); ++i) {
        this->thisProxy[i].receive(msgBuffers[i]);
        msgBuffers[i] = make_tram_msg<value_type>(buffer_size);
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

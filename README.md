# HTram

The HTram library is a new messaging library for Charm++.

## How to use (non-smp):

1. Add the following to your .ci file:
   ```
   extern module tramNonSmp;
   group tramNonSmp<your_message_type>;
   message tramNonSmpMsg<your_message_type>;
   readonly CProxy_tramNonSmp<your_message_type> tram_proxy;
   ```
   where your_message_type is the type of the message you are sending.
2. Add ` #include "tramNonSmp.h" ` to your charm .cpp or .c file
3. (recommended) Add the following aliases for your types to your charm code, and define the tram proxy:
   ```
   using tram_proxy_t = CProxy_tramNonSmp<your_message_type>;
   using tram_t = tramNonSmp<your_message_type>;
   tram_proxy_t tram_proxy;
   ```
4. Initialize the tram library in your Main function:
   ```
   CkGroupID updater_array_gid;
	 updater_array_gid = arr.ckGetArrayID();
	 tram_proxy = tram_proxy_t::ckNew(updater_array_gid, buffer_size, enable_buffer_flushing, flush_timer);
   ```
5. Define a target function that will receive messages from htram.
   This must be a static void function that takes two arguments: a void pointer,
   and a message of type your_message_type:
   ```
   static void name_of_target_fn(void *p, your_message_type message)
   {
     //call to a C++ method to process the message
   }
   ```
6. Get the local tram library using `tram_t *tram = tram_proxy.ckLocalBranch();`
7. Set this function as the receiver with `tram->set_func_ptr`
9. Use `tram->insertValue(destpe, message)` to send messages in htram
8. Use `tram->tflush() to flush messages`
9. To compile with htram, do:
   ```
   $(CHARMC) tramNonSmp.ci
   $(CHARMC) -c tramNonSmp.C -o tramNonSmp.o -g
   $(CHARMC) tramNonSmp.o -o libtramnonsmp.a -language charm++
   $(CHARMC) mycode.ci -DTRAM_NON_SMP
   $(CHARMC) mycode.cpp libtramnonsmp.a -language charm++ -o mycode -std=c++1z -DTRAM_NON_SMP
   ```

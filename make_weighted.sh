#!/bin/bash
../charm/bin/charmc weighted_htram.ci -DTRAM_NON_SMP
../charm/bin/charmc -O3 libtramnonsmp.a -language charm++ -o weighted_htram weighted_htram.cpp -tracemode projections -std=c++1z -DTRAM_NON_SMP


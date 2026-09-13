#!/bin/bash
#SBATCH --mem-per-cpu=1GB
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=2
#SBATCH --cpus-per-task=1
#SBATCH --output=weighted_htram.out
#SBATCH --partition=cpu
#SBATCH --account=mzu-delta-cpu
#SBATCH --job-name=myjobtest
#SBATCH --time=02:00:00      # hh:mm:ss for the job
../charm/bin/charmc weighted_htram.ci -DTRAM_NON_SMP
../charm/bin/charmc -O3 libtramnonsmp.a -language charm++ -o weighted_htram weighted_htram.cpp -tracemode projections -std=c++1z -DTRAM_NON_SMP
./charmrun weighted_htram 2 100000 ../facebook_clean_data/big_graph.csv 100 1 +p2 +traceroot weighted_htram_projections
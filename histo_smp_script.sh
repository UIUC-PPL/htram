#!/bin/bash
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=32
#SBATCH --partition=cpu
#SBATCH --account=mzu-delta-cpu
#SBATCH --job-name=histo_smp
#SBATCH --output=histo_smp_reconverse_ww_2_4procs_pemap.out
#SBATCH --exclusive
#SBATCH --time=00:10:00      # hh:mm:ss for the job

export LD_LIBRARY_PATH=/u/rao1/charm_for_reconverse/charm/lib:$LD_LIBRARY_PATH
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
for i in 16 64 256 1000 4000 16000 64000
do
    srun ./histo_smp +pe 112 -n $i
done
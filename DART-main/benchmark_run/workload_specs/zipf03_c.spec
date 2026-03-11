# Workload C: Read-only, Zipfian 0.3
# 2M records, 2M operations
recordcount=2000000
operationcount=2000000
workload=site.ycsb.workloads.CoreWorkload
readallfields=true
readproportion=1.0
updateproportion=0
scanproportion=0
insertproportion=0
requestdistribution=zipfian
zipfian.constant=0.3

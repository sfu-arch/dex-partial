# Workload C: Read-only, Zipfian 0.3
# 10M records, 10M operations
recordcount=10000000
operationcount=10000000
workload=site.ycsb.workloads.CoreWorkload
readallfields=true
readproportion=1.0
updateproportion=0
scanproportion=0
insertproportion=0
requestdistribution=zipfian
zipfian.constant=0.3

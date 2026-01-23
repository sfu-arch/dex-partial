# QW1: Key Access Distribution Experiment

## Research Question
Under what key access distributions does system behavior change regime?

## Experiment Design

### Independent Variables (Knobs)
| Variable | Values |
|----------|--------|
| Key access distribution | Uniform, Zipfian |
| Zipfian skew (theta) | 0.6, 0.8, 0.9, 0.99 |

### Dependent Variables (Metrics)

| Metric | DEX | CHIME |
|--------|-----|-------|
| Tail latency | Not directly tracked | p50, p90, p99, p99.9 from us_lat/*.lat |
| Remote bytes/query | `rdma_read_size + rdma_write_size` | Not directly tracked |
| RDMA ops/query | `rdma_read_num + rdma_write_num + rdma_cas_num` | Not directly tracked |
| Cache hit rate | Sherman baseline only | `cache_hit_rate` |
| Index traversal | Not directly tracked | `try_read_leaf`, `read_leaf_retry` |
| Throughput | `Final throughput` (Mops/s) | `cluster throughput` (Mops/s) |

---

## System-Specific Configuration

### DEX Configuration

**Zipfian parameter location:** Command-line arguments in `newbench`

```
argv[10] = uniform_workload  (0 = Zipfian, 1 = Uniform)
argv[11] = zipfian_theta     (e.g., 0.6, 0.8, 0.9, 0.99)
```

**Run command format:**
```bash
./newbench $nodenum $read $insert $update $delete $range $threads $mem_threads $cache $uniform $zipfian $bulk $warmup $runnum $correct $timebase $early $idx $rpc $admit $tune $maxthread
```

**Metrics collected (from newbench.cpp output):**
- `Avg. rdma read / op` - RDMA read operations per query
- `Avg. rdma write / op` - RDMA write operations per query  
- `Avg. rdma read size/ op` - Bytes read per query
- `Avg. rdma write size / op` - Bytes written per query
- `Avg. all rdma / op` - Total RDMA operations per query
- `Avg. rdma RW size / op` - Total bytes per query

### CHIME Configuration

**Zipfian parameter:** YCSB workload specification files control distribution.
The YCSB framework (Java) generates access patterns using `requestdistribution=zipfian`.

**Standard YCSB Zipfian** uses theta ≈ 0.99 by default.

**Metrics collected (from ycsb_test.cpp output):**
- `cache hit rate` - Cache hit ratio
- `cluster throughput` - Ops/sec
- Latency percentiles written to `us_lat/epoch_*.lat` files

---

## Execution Steps

### Step 1: Build Both Systems

**DEX:**
```bash
cd dex
./script/hugepage.sh
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
cp ../script/restartMemc.sh .
```

**CHIME:**
```bash
cd CHIME
echo 36864 > /proc/sys/vm/nr_hugepages
ulimit -l unlimited
mkdir -p build && cd build
cmake -DENABLE_CACHE=on -DHOPSCOTCH_LEAF_NODE=on -DSPECULATIVE_READ=on ..
make -j
```

### Step 2: Generate CHIME Workloads

Standard YCSB workloads (theta ≈ 0.99):
```bash
cd CHIME/ycsb
python3 gen_workload.py workloada randint full
```

### Step 3: Run DEX Experiments

Use the provided `run_qw1_dex.sh` script for each skew value:

```bash
# Example for theta=0.99, read-update workload (50/50)
./newbench 2 50 0 50 0 0 18 4 256 0 0.99 50 10 50 0 1 1 0 1 0.1 0 36
```

Parameter breakdown:
- `nodenum=2`, `read=50`, `insert=0`, `update=50`, `delete=0`, `range=0`
- `threads=18`, `mem_threads=4`, `cache=256`
- `uniform=0` (use Zipfian), `zipfian=0.99`
- `bulk=50` (50M bulk load), `warmup=10` (10M warmup), `runnum=50` (50M ops)

### Step 4: Run CHIME Experiments

```bash
cd CHIME/build
./restartMemc.sh
python3 ../ycsb/split_workload.py a randint 2 18
./ycsb_test 2 18 2 randint a
```

### Step 5: Collect Latency Data

**CHIME latency aggregation:**
```bash
cd CHIME/us_lat
python3 cluster_latency.py <CN_num> <epoch_start> <epoch_num>
```

---

## Important Notes

### Matching Configurations
| Parameter | DEX | CHIME |
|-----------|-----|-------|
| Key space | `kKeySpace` (computed from bulk+ops) | `recordcount=60000000` |
| Workload A | `read=50, update=50` | `readproportion=0.5, updateproportion=0.5` |
| Cache size | `cache_mb=256` | Configured in Common.h |

### Zipfian Implementation
Both systems use the same Zipf implementation from MehCached (`zipf.h`):
- `theta = 0` → Uniform distribution
- `theta ∈ (0,1)` → Zipfian skew (higher = more skewed)
- DEX: Configured at runtime via argv[11]
- CHIME: Uses YCSB's built-in Zipfian generator (theta hardcoded ~0.99)

### Limitations for CHIME Zipfian Variation
CHIME relies on YCSB framework for workload generation. YCSB's Zipfian constant is typically not directly exposed. To vary theta in CHIME:

**Option A:** Modify YCSB source (`core/src/main/java/site/ycsb/generator/ZipfianGenerator.java`) and rebuild
**Option B:** Use `zipfconstant` property in workload spec if supported:
```properties
zipfconstant=0.6
```

---

## Output Files

### DEX Output
RDMA statistics printed to stdout at benchmark end.

### CHIME Output  
- Latency histograms: `CHIME/us_lat/epoch_*.lat`
- Format: `<latency_us>\t<count>`
- Granularity: 0.1 µs buckets

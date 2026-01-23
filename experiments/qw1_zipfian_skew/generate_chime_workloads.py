#!/usr/bin/env python3
"""
Generate YCSB workload specification files with varying Zipfian constants.

YCSB supports the 'zipfconstant' property to control skew.
This script creates workload specs for different theta values.
"""

import os
from pathlib import Path

# Base workload A specification (50% read, 50% update)
WORKLOAD_A_TEMPLATE = """# Yahoo! Cloud System Benchmark
# Workload A variant with custom Zipfian constant
#
# Read/update ratio: 50/50
# Request distribution: zipfian with theta={theta}

recordcount=60000000
operationcount=60000000
fieldcount=1
fieldlength=1

workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true

readproportion=0.5
updateproportion=0.5
scanproportion=0
insertproportion=0

requestdistribution=zipfian
zipfconstant={theta}
"""

# Uniform distribution workload
WORKLOAD_UNIFORM_TEMPLATE = """# Yahoo! Cloud System Benchmark
# Workload A variant with UNIFORM distribution
#
# Read/update ratio: 50/50
# Request distribution: uniform

recordcount=60000000
operationcount=60000000
fieldcount=1
fieldlength=1

workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true

readproportion=0.5
updateproportion=0.5
scanproportion=0
insertproportion=0

requestdistribution=uniform
"""


def generate_workload_specs(output_dir: str):
    """Generate workload spec files for varying theta values."""
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    theta_values = [0.6, 0.8, 0.9, 0.99]
    
    # Generate Zipfian variants
    for theta in theta_values:
        spec_content = WORKLOAD_A_TEMPLATE.format(theta=theta)
        # Use underscore-free naming for compatibility
        filename = f"workloada_z{str(theta).replace('.', '')}"
        spec_path = output_path / filename
        
        with open(spec_path, 'w') as f:
            f.write(spec_content)
        
        print(f"Created: {spec_path}")
    
    # Generate uniform variant
    spec_path = output_path / "workloada_uniform"
    with open(spec_path, 'w') as f:
        f.write(WORKLOAD_UNIFORM_TEMPLATE)
    print(f"Created: {spec_path}")
    
    print(f"\nGenerated {len(theta_values) + 1} workload specs in: {output_path}")
    print("\nTo use these with CHIME:")
    print("1. Copy specs to CHIME/ycsb/full_workload_spec/")
    print("2. Run: python3 gen_workload.py workloada_z099 randint full")
    print("3. Run: python3 split_workload.py a_z099 randint <CN_num> <client_num>")


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Generate YCSB workload specs with varying Zipfian')
    parser.add_argument('--output', '-o', default='./workload_specs',
                        help='Output directory for workload specs')
    args = parser.parse_args()
    
    generate_workload_specs(args.output)

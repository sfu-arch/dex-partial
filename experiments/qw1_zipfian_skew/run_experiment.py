#!/usr/bin/env python3
"""
QW1 Unified Experiment Runner

Runs the QW1 (Zipfian Skew) experiment on both DEX and CHIME systems,
collecting comparable metrics for analysis.

Usage:
    python3 run_experiment.py --system dex --theta 0.99
    python3 run_experiment.py --system chime --theta 0.99
    python3 run_experiment.py --all  # Run all configurations
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent.parent

DEX_DIR = PROJECT_ROOT / "dex"
CHIME_DIR = PROJECT_ROOT / "CHIME"

# Default experiment parameters
DEFAULT_THETA_VALUES = [0.0, 0.6, 0.8, 0.9, 0.99]  # 0.0 = uniform
DEFAULT_WORKLOAD = "a"  # YCSB-A: 50% read, 50% update

# DEX parameters
DEX_PARAMS = {
    "nodenum": 2,
    "threads": 18,
    "mem_threads": 4,
    "cache_mb": 256,
    "bulk_load": 50,
    "warmup": 10,
    "run_num": 50,
    "read": 50,
    "insert": 0,
    "update": 50,
    "delete": 0,
    "range": 0,
}

# CHIME parameters
CHIME_PARAMS = {
    "cn_num": 2,
    "client_num": 18,
    "coro_num": 2,
    "key_type": "randint",
}


# ============================================================================
# System Detection
# ============================================================================

def check_dex_built() -> bool:
    """Check if DEX is built."""
    return (DEX_DIR / "build" / "newbench").exists()


def check_chime_built() -> bool:
    """Check if CHIME is built."""
    return (CHIME_DIR / "build" / "ycsb_test").exists()


def get_available_systems() -> List[str]:
    """Get list of available systems."""
    systems = []
    if check_dex_built():
        systems.append("dex")
    if check_chime_built():
        systems.append("chime")
    return systems


# ============================================================================
# DEX Runner
# ============================================================================

def run_dex_experiment(theta: float, output_dir: Path) -> Dict:
    """Run DEX experiment with given Zipfian theta."""
    build_dir = DEX_DIR / "build"
    
    # Determine if uniform or zipfian
    uniform = 1 if theta == 0.0 else 0
    
    # Build command
    cmd = [
        str(build_dir / "newbench"),
        str(DEX_PARAMS["nodenum"]),
        str(DEX_PARAMS["read"]),
        str(DEX_PARAMS["insert"]),
        str(DEX_PARAMS["update"]),
        str(DEX_PARAMS["delete"]),
        str(DEX_PARAMS["range"]),
        str(DEX_PARAMS["threads"]),
        str(DEX_PARAMS["mem_threads"]),
        str(DEX_PARAMS["cache_mb"]),
        str(uniform),
        str(theta if theta > 0 else 0.99),  # theta value (ignored if uniform)
        str(DEX_PARAMS["bulk_load"]),
        str(DEX_PARAMS["warmup"]),
        str(DEX_PARAMS["run_num"]),
        "0",   # correct
        "1",   # timebase
        "1",   # early
        "0",   # index (0=DEX)
        "1",   # rpc
        "0.1", # admit
        "0",   # tune
        "36",  # max_thread
    ]
    
    # Restart memcached first
    restart_script = build_dir / "restartMemc.sh"
    if restart_script.exists():
        subprocess.run(["bash", str(restart_script)], cwd=build_dir, capture_output=True)
        time.sleep(2)
    
    # Run experiment
    print(f"  Running DEX with theta={theta}...")
    result = subprocess.run(
        cmd,
        cwd=build_dir,
        capture_output=True,
        text=True,
        timeout=600,  # 10 minute timeout
    )
    
    # Save raw output
    theta_str = "uniform" if theta == 0.0 else f"zipf_{theta}"
    log_file = output_dir / f"dex_{theta_str}.log"
    with open(log_file, "w") as f:
        f.write(result.stdout)
        f.write("\n--- STDERR ---\n")
        f.write(result.stderr)
    
    # Parse metrics
    metrics = parse_dex_output(result.stdout)
    metrics["theta"] = theta
    metrics["system"] = "dex"
    
    return metrics


def parse_dex_output(output: str) -> Dict:
    """Parse DEX benchmark output for metrics."""
    metrics = {}
    
    patterns = {
        "rdma_read_per_op": r"Avg\. rdma read / op = ([\d.]+)",
        "rdma_write_per_op": r"Avg\. rdma write / op = ([\d.]+)",
        "rdma_cas_per_op": r"Avg\. rdma cas / op = ([\d.]+)",
        "rdma_total_per_op": r"Avg\. all rdma / op = ([\d.]+)",
        "rdma_read_size_per_op": r"Avg\. rdma read size/ op = ([\d.]+)",
        "rdma_write_size_per_op": r"Avg\. rdma write size / op = ([\d.]+)",
        "rdma_total_size_per_op": r"Avg\. rdma RW size / op = ([\d.]+)",
        "throughput_mops": r"Final throughput = ([\d.]+)",
        "cache_hit_rate": r"cache hit rate[:\s]*([\d.]+)",
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, output)
        if match:
            metrics[key] = float(match.group(1))
    
    return metrics


# ============================================================================
# CHIME Runner
# ============================================================================

def run_chime_experiment(theta: float, output_dir: Path) -> Dict:
    """Run CHIME experiment with given Zipfian theta.
    
    Note: CHIME uses YCSB workloads. Standard YCSB Zipfian uses theta ≈ 0.99.
    For other theta values, custom workload specs must be generated.
    """
    build_dir = CHIME_DIR / "build"
    
    # For now, CHIME only supports its default Zipfian (θ ≈ 0.99)
    # TODO: Generate custom workloads for other theta values
    if theta != 0.99 and theta != 0.0:
        print(f"  Warning: CHIME currently only supports theta=0.99 (Zipfian) or theta=0.0 (uniform)")
        print(f"  Running with default YCSB Zipfian (theta≈0.99)")
    
    workload = DEFAULT_WORKLOAD
    
    # Restart memcached
    restart_script = build_dir / "restartMemc.sh"
    if restart_script.exists():
        subprocess.run(["bash", str(restart_script)], cwd=build_dir, capture_output=True)
        time.sleep(2)
    
    # Split workloads
    split_cmd = [
        "python3",
        str(CHIME_DIR / "ycsb" / "split_workload.py"),
        workload,
        CHIME_PARAMS["key_type"],
        str(CHIME_PARAMS["cn_num"]),
        str(CHIME_PARAMS["client_num"]),
    ]
    subprocess.run(split_cmd, cwd=build_dir, capture_output=True)
    
    # Build command
    cmd = [
        str(build_dir / "ycsb_test"),
        str(CHIME_PARAMS["cn_num"]),
        str(CHIME_PARAMS["client_num"]),
        str(CHIME_PARAMS["coro_num"]),
        CHIME_PARAMS["key_type"],
        workload,
    ]
    
    # Run experiment
    print(f"  Running CHIME with theta={theta}...")
    result = subprocess.run(
        cmd,
        cwd=build_dir,
        capture_output=True,
        text=True,
        timeout=600,
    )
    
    # Save raw output
    theta_str = "uniform" if theta == 0.0 else f"zipf_{theta}"
    log_file = output_dir / f"chime_{theta_str}.log"
    with open(log_file, "w") as f:
        f.write(result.stdout)
        f.write("\n--- STDERR ---\n")
        f.write(result.stderr)
    
    # Parse metrics
    metrics = parse_chime_output(result.stdout)
    metrics["theta"] = theta
    metrics["system"] = "chime"
    
    return metrics


def parse_chime_output(output: str) -> Dict:
    """Parse CHIME benchmark output for metrics."""
    metrics = {}
    
    patterns = {
        "cache_hit_rate": r"cache hit rate[:\s]*([\d.]+)",
        "throughput_mops": r"cluster throughput ([\d.]+)",
        "lock_fail_avg": r"avg\. lock/cas fail cnt[:\s]*([\d.]+)",
        "write_combining_rate": r"write combining rate[:\s]*([\d.]+)",
        "read_delegation_rate": r"read delegation rate[:\s]*([\d.]+)",
        "speculative_read_rate": r"speculative read rate[:\s]*([\d.]+)",
        "speculative_accuracy": r"correct ratio of speculative read[:\s]*([\d.]+)",
        "read_leaf_retry": r"read_leaf_retry[:\s]*([\d.]+)",
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, output, re.IGNORECASE)
        if match:
            metrics[key] = float(match.group(1))
    
    return metrics


# ============================================================================
# Unified Metrics
# ============================================================================

def normalize_metrics(metrics: Dict, system: str) -> Dict:
    """Normalize metrics to common schema for comparison."""
    normalized = {
        "system": system,
        "theta": metrics.get("theta", 0.99),
        "throughput_mops": metrics.get("throughput_mops", 0),
        "cache_hit_rate": metrics.get("cache_hit_rate", 0),
    }
    
    if system == "dex":
        normalized["rdma_ops_per_query"] = metrics.get("rdma_total_per_op", 0)
        normalized["rdma_bytes_per_query"] = metrics.get("rdma_total_size_per_op", 0)
        normalized["rdma_reads_per_query"] = metrics.get("rdma_read_per_op", 0)
        normalized["rdma_writes_per_query"] = metrics.get("rdma_write_per_op", 0)
    elif system == "chime":
        # CHIME doesn't directly expose RDMA metrics, estimate from other stats
        normalized["write_combining_rate"] = metrics.get("write_combining_rate", 0)
        normalized["read_delegation_rate"] = metrics.get("read_delegation_rate", 0)
        normalized["speculative_read_rate"] = metrics.get("speculative_read_rate", 0)
        normalized["speculative_accuracy"] = metrics.get("speculative_accuracy", 0)
    
    return normalized


# ============================================================================
# Main Entry Point
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="QW1 Unified Experiment Runner")
    parser.add_argument("--system", choices=["dex", "chime", "all"], default="all",
                        help="Which system to run")
    parser.add_argument("--theta", type=float, nargs="+", default=DEFAULT_THETA_VALUES,
                        help="Zipfian theta values (0.0 for uniform)")
    parser.add_argument("--output", type=str, default=None,
                        help="Output directory for results")
    args = parser.parse_args()
    
    # Check available systems
    available = get_available_systems()
    if not available:
        print("ERROR: No systems built. Please build DEX and/or CHIME first.")
        print(f"  DEX: cd {DEX_DIR} && mkdir build && cd build && cmake .. && make")
        print(f"  CHIME: cd {CHIME_DIR} && mkdir build && cd build && cmake .. && make")
        sys.exit(1)
    
    print(f"Available systems: {available}")
    
    # Determine which systems to run
    if args.system == "all":
        systems = available
    else:
        if args.system not in available:
            print(f"ERROR: {args.system} is not built.")
            sys.exit(1)
        systems = [args.system]
    
    # Create output directory
    if args.output:
        output_dir = Path(args.output)
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_dir = SCRIPT_DIR / f"results_qw1_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"\n=== QW1 Experiment: Zipfian Skew Impact ===")
    print(f"Systems: {systems}")
    print(f"Theta values: {args.theta}")
    print(f"Output: {output_dir}")
    print()
    
    all_results = []
    
    for system in systems:
        print(f"\n--- Running {system.upper()} experiments ---")
        system_output = output_dir / system
        system_output.mkdir(exist_ok=True)
        
        for theta in args.theta:
            try:
                if system == "dex":
                    metrics = run_dex_experiment(theta, system_output)
                else:
                    metrics = run_chime_experiment(theta, system_output)
                
                normalized = normalize_metrics(metrics, system)
                all_results.append(normalized)
                
                print(f"    theta={theta}: throughput={normalized['throughput_mops']:.2f} Mops/s, "
                      f"cache_hit={normalized['cache_hit_rate']:.2%}")
                
            except subprocess.TimeoutExpired:
                print(f"    theta={theta}: TIMEOUT")
            except Exception as e:
                print(f"    theta={theta}: ERROR - {e}")
            
            time.sleep(2)  # Brief pause between runs
    
    # Save unified results
    results_file = output_dir / "results.json"
    with open(results_file, "w") as f:
        json.dump(all_results, f, indent=2)
    
    print(f"\n=== Experiment Complete ===")
    print(f"Results saved to: {results_file}")
    print(f"\nTo generate plots:")
    print(f"  python3 parse_results.py --input {output_dir}")


if __name__ == "__main__":
    main()

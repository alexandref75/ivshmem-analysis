#!/bin/bash

# Script to run the ivshmem performance test with comprehensive sanity checks
#
# Usage:
#   ./run_test.sh [latency_count] [bandwidth_count]
#   ./run_test.sh 1              # Run 1 latency test only (no bandwidth)
#   ./run_test.sh 100 5          # Run 100 latency + 5 bandwidth tests
#   ./run_test.sh 0 10           # Run bandwidth tests only (10 iterations)
#
# CPU PINNING OPTIMIZATION:
# This script supports CPU pinning to minimize context switches and improve performance.
# The host_writer process will be pinned to specific CPU cores using taskset.
# Environment variables (inherited from setup.sh or set manually):
#   HOST_CPU_CORES="0-1"   - Pin host processes to cores 0-1 (format: "0-3" or "0,2,4")
#   VM_CPU_CORES="2-3"     - Information about VM pinning (for display only)
#
# Example usage:
#   HOST_CPU_CORES="0-1" VM_CPU_CORES="2-3" ./run_test.sh 1
#   ./run_test.sh 1000 10

set -e  # Exit on any error

newline() {
    echo ""
}
echo "=== IVSHMEM Performance Test ==="
echo ""

: ${IVSHMEM_SIZE:=64}
: ${VM_NAME:="ivshmem-vm"}
: ${VM_DISK:="ivshmem-disk.qcow2"}
: ${CLOUD_IMAGE:="debian-12-generic-amd64.qcow2"}
: ${CLOUD_IMAGE_URL:="https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-generic-amd64.qcow2"}
: ${CERTIFICATE_FILE:="temp_id_rsa"}
: ${SHMEM_FILE:="./ivshmem-shmem"}

# Configuration
IVSHMEM_SIZE=64
SHMEM_PATH="/dev/shm/ivshmem"
SHMEM_FALLBACK="./ivshmem-shmem"
SSH_OPTS="-q -i ${CERTIFICATE_FILE} -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
SCP_OPTS="-i ${CERTIFICATE_FILE} -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
VM_USER="debian@localhost"

# Detect OS
OS=$(uname -o)
ARCH_M=$(uname -m)
case ${ARCH_M} in
        x86_64)
		: ${KVM_MACHINE_TYPE:="pc"}
                ARCH_GEN=amd64
                ARCH=amd64;;
        arm64|aarch64)
		: ${KVM_MACHINE_TYPE:="virt"}
                ARCH=arm64
                ARCH_GEN=arm
                ARCH_M=aarch64;;
        *)
		: ${KVM_MACHINE_TYPE:="pc"}
                ARCH=${ARCH_M}
                ARCH_GEN=${ARCH_M};;
esac
: ${QEMU_PATH:="/usr/bin/qemu-system-${ARCH_M}"}


# Create shared memory file
if [ "$OS" = "Darwin" ]; then
  # macOS: use local file
  SHMEM_PATH=$SHMEM_FILE
else
  # Linux: use /dev/shm if available, otherwise local file
  if [ -d /dev/shm ]; then
    SHMEM_PATH=/dev/shm/ivshmem
  else
    SHMEM_PATH=$SHMEM_FILE
  fi
fi
  
# Configuration
SHMEM_FALLBACK="./ivshmem-shmem"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

error() {
    echo -e "${RED}[X] $1${NC}" >&2
    exit 1
}

warning() {
    echo -e "${YELLOW}[!] $1${NC}" >&2
}

success() {
    echo -e "${GREEN}[✓] $1${NC}"
}

info() {
    echo "[i] $1"
}

# Parse command line arguments
show_usage() {
    echo "Usage: $0 [latency_count] [bandwidth_count]"
    echo "  latency_count   - Number of latency measurements (default: 1000, 0 to skip)"
    echo "  bandwidth_count - Number of bandwidth measurements (default: 10, 0 to skip)"
    echo ""
    echo "Examples:"
    echo "  $0 1            # Run 1 latency test only (no bandwidth)"
    echo "  $0 100 5        # Run 100 latency + 5 bandwidth tests"
    echo "  $0 0 10         # Run bandwidth tests only (10 iterations)"
    echo "  $0              # Run default tests (1000 latency + 10 bandwidth)"
    exit 0
}

# Check for help flags
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_usage
fi


# Parse test counts from command line arguments
LAT_COUNT=${1:-1000}      # Default 1000 latency tests
BAND_COUNT=${2:-10}       # Default 10 bandwidth tests

# If only latency count provided and >0, skip bandwidth by default
if [[ $# -eq 1 && $LAT_COUNT -gt 0 ]]; then
    BAND_COUNT=0
    info "Single argument provided - running $LAT_COUNT latency tests only (bandwidth skipped)"
fi

# Validate arguments
if ! [[ "$LAT_COUNT" =~ ^[0-9]+$ ]] || ! [[ "$BAND_COUNT" =~ ^[0-9]+$ ]]; then
    error "Arguments must be non-negative integers. Use -h for help."
fi

if [[ $LAT_COUNT -eq 0 && $BAND_COUNT -eq 0 ]]; then
    error "At least one test type must be enabled (latency_count > 0 or bandwidth_count > 0)"
fi

# CPU Pinning Configuration (inherited from setup.sh or set manually)
# These can be overridden by environment variables
VM_CPU_CORES="${VM_CPU_CORES:-}"
HOST_CPU_CORES="${HOST_CPU_CORES:-}"

# Auto-detect CPU configuration if not set
detect_host_cpu_config() {
  if [ -z "$HOST_CPU_CORES" ] && [ -z "$VM_CPU_CORES" ]; then
    TOTAL_CORES=$(nproc)
    if [ "$TOTAL_CORES" -ge 4 ]; then
      # Use same auto-configuration as setup.sh
      if [ "$TOTAL_CORES" -ge 8 ]; then
        HOST_CPU_CORES="0-1,6-7"
      elif [ "$TOTAL_CORES" -ge 6 ]; then
        HOST_CPU_CORES="0-1,5"
      else
        HOST_CPU_CORES="0-1"
      fi
      info "Auto-configured host CPU cores: $HOST_CPU_CORES"
    fi
  fi
}

echo "=== IVSHMEM Performance Test ==="
info "Test Configuration: Latency=${LAT_COUNT}, Bandwidth=${BAND_COUNT}"
info "Includes baseline memory performance validation (Host vs VM comparison)"
newline

# Configure CPU pinning
detect_host_cpu_config

# Display CPU pinning configuration
if [ -n "$HOST_CPU_CORES" ] || [ -n "$VM_CPU_CORES" ]; then
  info "CPU Pinning Configuration:"
  [ -n "$HOST_CPU_CORES" ] && info "  Host processes pinned to cores: $HOST_CPU_CORES"
  [ -n "$VM_CPU_CORES" ] && info "  VM should be pinned to cores: $VM_CPU_CORES"
  newline
fi

# Sanity Check 1: VM is running
echo "Checking VM status..."
if ! pgrep -F pidfile.pid > /dev/null; then
    error "VM is not running! Start it first with: ./setup.sh"
fi
success "VM is running"

# Sanity Check 2: ivshmem support
echo "Checking ivshmem support..."
if ! pgrep -f "${QEMU_PATH}.*ivshmem" > /dev/null; then
    warning "VM may not have ivshmem support enabled"
    warning "Performance tests may not work correctly"
else
    success "VM has ivshmem support"
fi

# Sanity Check 3: SSH connectivity
echo "Checking SSH connectivity..."
if ! ssh $SSH_OPTS $VM_USER 'echo "SSH OK"' > /dev/null 2>&1; then
    error "Cannot connect to VM via SSH. Is the VM fully booted?"
fi
success "SSH connectivity OK"

# Sanity Check 4: VM has required tools
echo "Checking VM build environment..."
if ! ssh $SSH_OPTS $VM_USER 'command -v gcc > /dev/null'; then
    error "gcc not found in VM. The VM setup may have failed."
fi
if ! ssh $SSH_OPTS $VM_USER 'command -v make > /dev/null'; then
    error "make not found in VM. The VM setup may have failed."
fi
success "VM build environment OK"

# Sanity Check 5: ivshmem device in VM
echo "Checking /dev/dax0.0 device in VM..."
if ! ssh $SSH_OPTS $VM_USER 'test -e /dev/dax0.0'; then
    echo "/dev/dax0.0 does not exist, trying to create..."
    # Sanity Check 5: ivshmem device in VM
    if ! ssh $SSH_OPTS $VM_USER 'sudo ndctl create-namespace --mode devdax --map mem -e namespace0.0 -f'; then
	echo "Checking ivshmem device in VM..."
	if ! ssh $SSH_OPTS $VM_USER 'test -e /sys/bus/pci/devices/0000:00:03.0/resource2'; then
	    error "ivshmem PCI device not found in VM. VM may not have ivshmem support."
	fi
	success "ivshmem PCI device found in VM"
    fi
    success "/dev/dax0,0 created on VM"
fi
success "dax0.0 device found in VM"

# Sanity Check 6: Host build environment
echo "Checking host build environment..."
if ! command -v gcc > /dev/null; then
    error "gcc not found on host. Install with: apt-get install build-essential"
fi
if ! command -v make > /dev/null; then
    error "make not found on host. Install with: apt-get install build-essential"
fi
success "Host build environment OK"

# Sanity Check 7: OpenSSL development libraries
echo "Checking OpenSSL libraries..."
if ! pkg-config --exists openssl 2>/dev/null && ! test -f /usr/include/openssl/sha.h; then
    error "OpenSSL development libraries not found. Install with: apt-get install libssl-dev"
fi
success "OpenSSL libraries OK"

# Sanity Check 8: CPU pinning support
echo "Checking CPU pinning support..."
if [ -n "$HOST_CPU_CORES" ] && ! command -v taskset > /dev/null; then
    error "taskset not found but CPU pinning is requested. Install with: apt-get install util-linux"
fi
if [ -n "$HOST_CPU_CORES" ]; then
    # Test that the specified cores are valid
    if ! taskset -c "$HOST_CPU_CORES" true 2>/dev/null; then
        error "Invalid host CPU cores specified: $HOST_CPU_CORES. Use format like '0-1' or '0,2,4'"
    fi
    success "CPU pinning configuration valid"
elif [ -n "$VM_CPU_CORES" ]; then
    info "VM CPU pinning configured (taskset not needed for host)"
else
    info "CPU pinning disabled"
fi

# Sanity Check 9: Baseline memory performance comparison
echo "Running baseline memory performance comparison..."
info "This validates that VM overhead is minimal before testing ivshmem"

# Copy and compile memory baseline test
if [ ! -f "memory_baseline.c" ]; then
    warning "memory_baseline.c not found, skipping baseline performance check"
else
    # Compile on host
    if ! gcc -O2 -o memory_baseline memory_baseline.c 2>/dev/null; then
        warning "Failed to compile memory_baseline.c on host, skipping baseline check"
    else
        # Copy to VM and compile there
        if ! scp $SCP_OPTS memory_baseline.c $VM_USER:/tmp/ >/dev/null 2>&1; then
            warning "Failed to copy memory_baseline.c to VM, skipping baseline check"
        elif ! ssh $SSH_OPTS $VM_USER 'cd /tmp && gcc -O2 -o memory_baseline memory_baseline.c' >/dev/null 2>&1; then
            warning "Failed to compile memory_baseline.c on VM, skipping baseline check"
        else
            # Run baseline tests (reduced size for speed - 5MB instead of 24MB)
            info "Running host baseline (5MB test, 3 iterations)..."
            HOST_RESULT=$(./memory_baseline 5 3 2>/dev/null | grep "memcpy local (cold)" | awk '{print $6}' | sed 's/MB\/s//')
            
            info "Running VM baseline (5MB test, 3 iterations)..."
            VM_RESULT=$(ssh $SSH_OPTS $VM_USER 'cd /tmp && ./memory_baseline 5 3' 2>/dev/null | grep "memcpy local (cold)" | awk '{print $6}' | sed 's/MB\/s//')
            
            if [ -n "$HOST_RESULT" ] && [ -n "$VM_RESULT" ]; then
                # Calculate percentage (try bc first, fallback to awk)
                if command -v bc >/dev/null 2>&1; then
                    PERCENT=$(echo "scale=1; $VM_RESULT * 100 / $HOST_RESULT" | bc 2>/dev/null)
                else
                    PERCENT=$(echo "$VM_RESULT $HOST_RESULT" | awk '{printf "%.1f", $1 * 100 / $2}')
                fi
                
                if [ -n "$PERCENT" ]; then
                    info "Baseline memcpy performance: Host=${HOST_RESULT} MB/s, VM=${VM_RESULT} MB/s (${PERCENT}%)"
                    
                    # Check if performance is reasonable (should be 85-115% of host)
                    PERCENT_INT=$(echo "$PERCENT" | cut -d. -f1)
                    if [ "$PERCENT_INT" -lt 85 ]; then
                        warning "VM performance is ${PERCENT}% of host (below 85%) - VM may be under stress"
                        warning "Consider checking VM resources before interpreting ivshmem results"
                    elif [ "$PERCENT_INT" -gt 115 ]; then
                        info "VM performance is ${PERCENT}% of host (above 115%) - excellent VM optimization"
                    else
                        success "VM performance is ${PERCENT}% of host (85-115% range) - healthy VM overhead"
                        success "VM is performing normally - ivshmem results should be representative"
                    fi
                else
                    info "Baseline performance check completed (unable to calculate percentage)"
                fi
            else
                warning "Baseline performance results incomplete, continuing with tests"
            fi
            
            # Cleanup
            ssh $SSH_OPTS $VM_USER 'rm -f /tmp/memory_baseline /tmp/memory_baseline.c' >/dev/null 2>&1 || true
        fi
    fi
    rm -f memory_baseline >/dev/null 2>&1 || true
fi

# Verify shared memory setup
echo "Verifying shared memory setup..."
EXPECTED_SIZE=$((IVSHMEM_SIZE * 1024 * 1024))

# Check if shared memory file exists
if [ ! -f "$SHMEM_PATH" ]; then
    error "Shared memory file not found: $SHMEM_PATH
Please run ./setup.sh first to create the VM and shared memory."
fi

# Check if shared memory file has correct size
CURRENT_SIZE=$(stat -c%s "$SHMEM_PATH" 2>/dev/null || echo "0")
if [ "$CURRENT_SIZE" -ne "$EXPECTED_SIZE" ]; then
    error "Shared memory file has wrong size: $CURRENT_SIZE bytes (expected: $EXPECTED_SIZE bytes)
Please run ./setup.sh to recreate the shared memory with correct size."
fi

success "Shared memory file verified: $SHMEM_PATH (${IVSHMEM_SIZE}MB)"

# Clean up any existing CSV files for fresh results
echo "Cleaning up previous test results..."
rm -f latency_results.csv bandwidth_results.csv
success "Previous results cleaned"

# Compile programs
echo "Compiling programs..."
if ! make all > /dev/null 2>&1; then
    error "Failed to compile host programs. Check compiler errors with: make all"
fi
success "Host programs compiled"

# Deploy and compile guest program
echo "Deploying guest program to VM..."
if ! make deploy > /dev/null 2>&1; then
    error "Failed to deploy guest program to VM. Check with: make deploy"
fi
success "Guest program deployed and compiled"

# Clean up any existing guest processes
echo "Cleaning up existing guest processes..."
ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1 || true || true
success "Cleanup complete"

# Note: No manual shared memory reset needed!
# The host_writer.c handles proper initialization with the state machine,
# and guest_reader.c is designed to be resilient to startup conditions
info "Shared memory initialization will be handled by host_writer.c"

# Run host writer
newline
info "Starting performance tests..."
newline

# Prepare host CPU pinning command
if [ -n "$HOST_CPU_CORES" ]; then
  HOST_PINNING_CMD="taskset -c $HOST_CPU_CORES"
  info "Host writer will be pinned to cores: $HOST_CPU_CORES"
else
  HOST_PINNING_CMD=""
fi

# Run latency test if requested
if [ $LAT_COUNT -gt 0 ]; then
  newline
  info "=== RUNNING LATENCY TEST ==="
  newline

  # Start guest reader for latency test only
  echo "Starting guest reader for latency test (${LAT_COUNT} iterations)..."
  ssh $SSH_OPTS $VM_USER "sudo /tmp/guest_reader -l $LAT_COUNT" > /tmp/guest_latency.log 2>&1 &
  LATENCY_GUEST_PID=$!

  # Wait for guest to initialize
  sleep 3

  if ! kill -0 $LATENCY_GUEST_PID 2>/dev/null; then
      error "Latency guest failed to start. Check log: cat /tmp/guest_latency.log"
  fi

  # Run latency test on host (separate invocation)
  if sudo $HOST_PINNING_CMD ./host_writer -l $LAT_COUNT; then
      success "Latency test completed successfully"
  else
      warning "Latency test completed with issues"
  fi

  exit 1

  # Cleanup latency guest (handle gracefully even if process already exited)
  echo "Cleaning up latency guest process..."
  if kill -0 $LATENCY_GUEST_PID 2>/dev/null; then
      kill $LATENCY_GUEST_PID 2>/dev/null || true
      echo "  ✓ Latency guest process terminated"
  else
      echo "  ✓ Latency guest process already completed"
  fi
  ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1 || true
  sleep 2
else
  info "Latency test skipped (count = 0)"
fi

# Run bandwidth test if requested
if [ $BAND_COUNT -gt 0 ]; then
  newline
  info "=== RUNNING BANDWIDTH TEST ==="
  newline

  # Start guest reader for bandwidth test only
  echo "Starting guest reader for bandwidth test (${BAND_COUNT} iterations)..."
  ssh $SSH_OPTS $VM_USER "sudo /tmp/guest_reader -c $((BAND_COUNT * 3))" > /tmp/guest_bandwidth.log 2>&1 &
  BANDWIDTH_GUEST_PID=$!

  # Wait for guest to initialize
  sleep 3

  if ! kill -0 $BANDWIDTH_GUEST_PID 2>/dev/null; then
      error "Bandwidth guest failed to start. Check log: cat /tmp/guest_bandwidth.log"
  fi

  # Run bandwidth test on host (separate invocation)
  if echo "" | sudo $HOST_PINNING_CMD ./host_writer -b ${BAND_COUNT}; then
      success "Bandwidth test completed successfully"
  else
      warning "Bandwidth test completed with issues"
  fi

  # Cleanup bandwidth guest (handle gracefully)
  echo "Cleaning up bandwidth guest process..."
  if kill -0 $BANDWIDTH_GUEST_PID 2>/dev/null; then
      kill $BANDWIDTH_GUEST_PID 2>/dev/null || true
      echo "  ✓ Bandwidth guest process terminated"
  else
      echo "  ✓ Bandwidth guest process already completed"
  fi
else
  info "Bandwidth test skipped (count = 0)"
fi

# Final cleanup
ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1 || true

# Show results
newline
echo "=========================================="
echo "Test Results:"
echo "=========================================="
if [ -f latency_results.csv ]; then
    LATENCY_COUNT=$(tail -n +2 latency_results.csv | wc -l)
    success "Latency results: $LATENCY_COUNT measurements in latency_results.csv"
else
    warning "No latency results found"
fi

if [ -f bandwidth_results.csv ]; then
    BANDWIDTH_COUNT=$(tail -n +2 bandwidth_results.csv | wc -l)
    success "Bandwidth results: $BANDWIDTH_COUNT measurements in bandwidth_results.csv"
else
    warning "No bandwidth results found"
fi

newline
info "To analyze results, run: python3 analyze_results.py"
info "Or with uv: uv run analyze_results.py"
newline
if [ -n "$HOST_CPU_CORES" ] || [ -n "$VM_CPU_CORES" ]; then
    info "CPU Pinning was used for this test:"
    [ -n "$HOST_CPU_CORES" ] && info "  Host processes pinned to: $HOST_CPU_CORES"
    [ -n "$VM_CPU_CORES" ] && info "  VM should be pinned to: $VM_CPU_CORES"
    info "This should minimize context switches and VM exits."
else
    info "To optimize performance with CPU pinning, set environment variables:"
    info "  HOST_CPU_CORES=0-1 VM_CPU_CORES=2-3 ./run_test.sh"
fi

# Automatically run analysis if results exist
if [ -f latency_results.csv ] || [ -f bandwidth_results.csv ]; then
    newline
    info "Running automatic analysis..."
    if command -v uv &> /dev/null; then
        if uv run analyze_results.py; then
            success "Analysis completed! Check generated plots and performance_report.txt"
        else
            warning "Analysis failed - you can run it manually with: uv run analyze_results.py"
        fi
    else
        warning "uv not found. Install with: curl -LsSf https://astral.sh/uv/install.sh | sh"
        info "Then run: uv run analyze_results.py"
    fi
fi
newline

success "Test completed!"

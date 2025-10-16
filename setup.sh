#!/bin/sh

# This is a VM-based solution with shared memory to measure the host -> guest communication 
# latency, bandwidth and pressure under load. The most straightforward approach is using 
# QEMU/KVM with ivshmem (Inter-VM Shared Memory device), which provides a PCI device that 
# maps to shared memory accessible by both host and guest.

# Needs to work on both ARM64 and x86_64 architecture. Linux only, as ivshmem is not available on macOS.
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

# Variables
: ${IVSHMEM_SIZE:=64}
: ${VM_NAME:="ivshmem-vm"}
: ${VM_DISK:="ivshmem-disk.qcow2"}
: ${CLOUD_IMAGE:="debian-12-generic-${ARCH}.qcow2"}
: ${CLOUD_IMAGE_URL:="https://cloud.debian.org/images/cloud/bookworm/latest/${CLOUD_IMAGE}"}
: ${CERTIFICATE_FILE:="temp_id_rsa"}
: ${SHMEM_FILE:="./ivshmem-shmem"}

: ${QEMU_PATH:="/usr/bin/qemu-system-${ARCH_M}"}

# CPU PINNING OPTIMIZATION:
# This script supports CPU pinning to minimize VM exits and context switches by isolating
# the VM and host processes to different CPU cores. Set these environment variables:
#   VM_CPU_CORES="2-3"     - Pin VM to cores 2-3 (format: "0-3" or "0,2,4")
#   HOST_CPU_CORES="0-1"   - Pin host processes to cores 0-1
#   VM_VCPU_COUNT=2        - Number of virtual CPUs for the VM
# 
# Auto-configuration is enabled for systems with 4+ cores:
#   4-5 cores: VM=2-3, Host=0-1
#   6-7 cores: VM=2-4, Host=0-1,5
#   8+ cores:  VM=2-5, Host=0-1,6-7
VM_CPU_CORES="${VM_CPU_CORES:-}"
HOST_CPU_CORES="${HOST_CPU_CORES:-}"
VM_VCPU_COUNT="${VM_VCPU_COUNT:-2}"  # Number of virtual CPUs for the VM


# CPU topology detection and auto-configuration
detect_cpu_topology() {
  TOTAL_CORES=$(nproc)
  echo "Detected $TOTAL_CORES CPU cores"
  
  # Auto-configure CPU pinning if not manually set
  if [ -z "$VM_CPU_CORES" ] && [ -z "$HOST_CPU_CORES" ] && [ "$TOTAL_CORES" -ge 4 ]; then
    # For systems with 4+ cores, automatically separate VM and host
    if [ "$TOTAL_CORES" -ge 8 ]; then
      # 8+ cores: VM gets cores 2-5, host gets cores 0-1,6-7
      VM_CPU_CORES="2-5"
      HOST_CPU_CORES="0-1,6-7"
      VM_VCPU_COUNT=4
    elif [ "$TOTAL_CORES" -ge 6 ]; then
      # 6+ cores: VM gets cores 2-4, host gets cores 0-1,5
      VM_CPU_CORES="2-4"
      HOST_CPU_CORES="0-1,5"
      VM_VCPU_COUNT=3
    else
      # 4-5 cores: VM gets cores 2-3, host gets cores 0-1
      VM_CPU_CORES="2-3"
      HOST_CPU_CORES="0-1"
      VM_VCPU_COUNT=2
    fi
    
    echo "Auto-configured CPU pinning:"
    echo "  VM cores: $VM_CPU_CORES ($VM_VCPU_COUNT vCPUs)"
    echo "  Host cores: $HOST_CPU_CORES"
    echo "  (Set VM_CPU_CORES/HOST_CPU_CORES environment variables to override)"
  elif [ -n "$VM_CPU_CORES" ] || [ -n "$HOST_CPU_CORES" ]; then
    echo "Using manual CPU pinning configuration:"
    [ -n "$VM_CPU_CORES" ] && echo "  VM cores: $VM_CPU_CORES"
    [ -n "$HOST_CPU_CORES" ] && echo "  Host cores: $HOST_CPU_CORES"
  else
    echo "CPU pinning disabled (less than 4 cores or manually disabled)"
  fi
}

# Detect CPU topology for auto-configuration
detect_cpu_topology

# Create shared memory file
if [ "$OS" = "Darwin" ]; then
  # macOS: use local file
  dd if=/dev/zero of=$SHMEM_FILE bs=1M count=$IVSHMEM_SIZE
  ACCEL_FLAG="-accel hvf"
  CPU_FLAG="-cpu host"
  SHMEM_PATH=$SHMEM_FILE
else
  # Linux: use /dev/shm if available, otherwise local file
  if [ -d /dev/hugepages ]; then
    echo "Creating shared memory file: /dev/hugepages/ivshmem (${IVSHMEM_SIZE}MB)"
    SHMEM_PATH=/dev/hugepages/ivshmem
  elif [ -d /dev/shm ]; then
    echo "Creating shared memory file: /dev/shm/ivshmem (${IVSHMEM_SIZE}MB)"
    SHMEM_PATH=/dev/shm/ivshmem
  else
    echo "Creating shared memory file: $SHMEM_FILE (${IVSHMEM_SIZE}MB)"
    SHMEM_PATH=$SHMEM_FILE
  fi
  #sudo dd if=/dev/zero of=$SHMEM_PATH bs=1M count=$IVSHMEM_SIZE
  sudo chmod a+rw $SHMEM_PATH 
  
  # Check if KVM is accessible
  if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_FLAG="-enable-kvm"
    CPU_FLAG="-cpu host"
    echo "KVM acceleration is available"
  else
    ACCEL_FLAG="-accel tcg"
    CPU_FLAG="-cpu qemu64"
    echo "WARNING: KVM not accessible, using TCG (software emulation - slower)"
    echo "To enable KVM acceleration, add your user to the kvm group:"
    echo "  sudo usermod -aG kvm \$USER"
    echo "  Then log out and log back in"
    echo ""
  fi
fi

# Make sure we have the qemu-system-${ARCH_M} command
which $QEMU_PATH > /dev/null
if [ $? -ne 0 ]; then
  echo "qemu-system-${ARCH_M} not found"
  echo "Install with: brew install qemu (macOS) or apt-get install qemu-system (Linux)"
  exit 1
fi

# Check for build tools (needed to compile host/guest programs)
if ! command -v gcc > /dev/null; then
  echo "WARNING: gcc not found on host"
  echo "The host/guest test programs require gcc to compile."
  echo "Install with: apt-get install build-essential (Linux) or xcode-select --install (macOS)"
  echo ""
fi

# Download Debian cloud image if not present
if [ ! -f $CLOUD_IMAGE ]; then
  echo "Downloading Debian cloud image..."
  echo "This may take a few minutes..."
  if command -v wget > /dev/null; then
    wget -O $CLOUD_IMAGE $CLOUD_IMAGE_URL
  elif command -v curl > /dev/null; then
    curl -L -o $CLOUD_IMAGE $CLOUD_IMAGE_URL
  else
    echo "ERROR: Neither wget nor curl found. Please install one of them."
    exit 1
  fi
  
  if [ ! -f $CLOUD_IMAGE ]; then
    echo "ERROR: Failed to download cloud image"
    exit 1
  fi
fi

# Create VM disk from cloud image if it doesn't exist
if [ ! -f $VM_DISK ]; then
  echo "Creating VM disk from cloud image..."
  qemu-img create -f qcow2 -F qcow2 -b $CLOUD_IMAGE $VM_DISK 20G
fi

# Generate SSH key if it doesn't exist
if [ ! -f $CERTIFICATE_FILE ]; then
  echo "Generating SSH key..."
  ssh-keygen -t rsa -b 4096 -f $CERTIFICATE_FILE -N ""
fi

# Create cloud-init configuration
CLOUD_INIT_DIR="cloud-init-config"
mkdir -p $CLOUD_INIT_DIR

# Create meta-data
cat > $CLOUD_INIT_DIR/meta-data << EOF
instance-id: ivshmem-vm-001
local-hostname: ivshmem-vm
EOF

# Create user-data with SSH key
cat > $CLOUD_INIT_DIR/user-data << EOF
#cloud-config
users:
  - name: debian
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat ${CERTIFICATE_FILE}.pub)

package_update: true
package_upgrade: true
packages:
  - build-essential
  - gcc
  - make
  - libssl-dev
  - ndctl

runcmd:
  - echo "VM setup complete" > /tmp/setup-complete
EOF

# Create cloud-init ISO
CLOUD_INIT_ISO="cloud-init.iso"
if command -v genisoimage > /dev/null; then
  genisoimage -output $CLOUD_INIT_ISO -volid cidata -joliet -rock $CLOUD_INIT_DIR/user-data $CLOUD_INIT_DIR/meta-data
elif command -v mkisofs > /dev/null; then
  mkisofs -output $CLOUD_INIT_ISO -volid cidata -joliet -rock $CLOUD_INIT_DIR/user-data $CLOUD_INIT_DIR/meta-data
else
  echo "ERROR: genisoimage or mkisofs not found. Install with: apt-get install genisoimage"
  exit 1
fi

# Check if ivshmem is available
HAS_IVSHMEM=0
if $QEMU_PATH -device help 2>&1 | grep -q ivshmem; then
  HAS_IVSHMEM=1
  echo "ivshmem device is available"
else
  echo "WARNING: ivshmem device is NOT available in this QEMU build"
  echo "The VM will start without shared memory support."
  echo ""
  echo "To get ivshmem support:"
  if [ "$OS" = "Darwin" ]; then
    echo "  - Use Linux instead (ivshmem is typically not in macOS QEMU builds)"
    echo "  - Or compile QEMU from source with --enable-ivshmem"
  else
    echo "  - Install qemu-system with ivshmem: apt-get install qemu-system"
    echo "  - Or compile QEMU from source with --enable-ivshmem"
  fi
  echo ""
fi

# Check if VM is already running
if pgrep -f "qemu-system-${ARCH_M}.*ivshmem" > /dev/null; then
  echo "VM appears to already be running with ivshmem."
  echo "If you want to restart it, kill the existing process first:"
  echo "  pkill -f qemu-system-${ARCH_M}"
  echo ""
  echo "To run tests with the existing VM:"
  echo "  ./run_test.sh"
  exit 0
fi

# Prepare CPU pinning arguments
CPU_PINNING_FLAGS=""
if [ -n "$VM_CPU_CORES" ]; then
  # Create CPU pinning command prefix
  CPU_PINNING_CMD="taskset -c $VM_CPU_CORES"
  echo "VM will be pinned to CPU cores: $VM_CPU_CORES"
else
  CPU_PINNING_CMD=""
fi

# Launch QEMU VM with or without ivshmem Device
if [ $HAS_IVSHMEM -eq 1 ]; then
  # With ivshmem support - shared memory will be created by run_test.sh
  echo "Starting VM with ivshmem support..."
  $QEMU_PATH \
    -M nvdimm=on \
    -machine ${KVM_MACHINE_TYPE} \
    $CPU_FLAG \
    $ACCEL_FLAG \
    -m 4096m,slots=2,maxmem=6144m \
    -cpu host \
    -smp cpus=$VM_VCPU_COUNT,maxcpus=$VM_VCPU_COUNT \
    -pidfile pidfile.pid \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=none,format=qcow2,file=$VM_DISK,id=hd0 \
    -device virtio-blk-pci,drive=hd0 \
    -drive file=$CLOUD_INIT_ISO,format=raw,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -object memory-backend-file,id=hostmem,mem-path=$SHMEM_PATH,size=${IVSHMEM_SIZE}M,share=on \
    -device nvdimm,memdev=hostmem \
    -nographic > qemu.log 2>&1 &
    # -device nvdimm,memdev=hostmem,label-size=2M \
    #-device ivshmem-plain,memdev=hostmem \
else
  # Without ivshmem (fallback)
  echo "Starting VM without ivshmem (limited functionality)..."
  $CPU_PINNING_CMD $QEMU_PATH \
    -machine ${KVM_MACHINE_TYPE} \
    $CPU_FLAG \
    $ACCEL_FLAG \
    -pidfile pidfile.pid \
    -m 4096 \
    -smp cpus=$VM_VCPU_COUNT,maxcpus=$VM_VCPU_COUNT \
    -drive file=$VM_DISK,format=qcow2,if=virtio \
    -drive file=$CLOUD_INIT_ISO,format=raw,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -nographic > qemu.log 2>&1 &
fi

sleep 5
if [ ! -e pidfile.pid ];then
	echo "QEMU did not start"
	exit 1
fi
QEMU_PID=$(cat pidfile.pid)
echo "QEMU started with PID $QEMU_PID"
echo "Checking if VM started successfully..."

# Wait up to 10 seconds for QEMU to either fail or stabilize
for i in $(seq 1 10); do
  if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "ERROR: QEMU process died unexpectedly"
    exit 1
  fi
  sleep 1
done

echo "VM is running. Waiting for boot to complete..."
# Now wait for SSH to become available (up to 50 more seconds)
for i in $(seq 1 50); do
  if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "ERROR: QEMU process died during boot"
    exit 1
  fi
  # Check if SSH port is responding
  if nc -z localhost 2222 2>/dev/null; then
    echo "VM booted successfully! SSH is available on port 2222"
    break
  fi
  sleep 1
done

echo ""
echo "=========================================="
echo "VM is ready!"
echo "=========================================="
if [ -n "$VM_CPU_CORES" ] || [ -n "$HOST_CPU_CORES" ]; then
  echo "CPU Pinning Configuration:"
  [ -n "$VM_CPU_CORES" ] && echo "  VM pinned to cores: $VM_CPU_CORES ($VM_VCPU_COUNT vCPUs)"
  [ -n "$HOST_CPU_CORES" ] && echo "  Host processes will use cores: $HOST_CPU_CORES"
  echo ""
fi
echo "To run performance tests:"
echo "  ./run_test.sh"
echo ""
echo "To run with custom CPU pinning:"
echo "  VM_CPU_CORES=2-3 HOST_CPU_CORES=0-1 ./run_test.sh"
echo ""
echo "To connect to the VM manually:"
echo "  ssh -i $CERTIFICATE_FILE -p 2222 debian@localhost"
echo ""
echo "To stop the VM:"
echo "  kill $QEMU_PID"
echo "  # or: pkill -f qemu-system-${ARCH_M}"
echo ""


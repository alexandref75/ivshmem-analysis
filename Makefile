.PHONY: all clean host guest deploy test

CC = gcc
CFLAGS = -Wall -O2 -std=c11 -mtune=native
LDFLAGS = -lrt -lssl -lcrypto
SSHFLAGS = -i temp_id_rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SCPFLAGS = -i temp_id_rsa -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSH_PORT_FLAGS = -p 2222
VM_NAME = debian@localhost
TARGET_DIR = /tmp
GUEST_PROGRAM = guest_reader

all: host guest

host: host_writer.c
	$(CC) $(CFLAGS) -o host_writer host_writer.c $(LDFLAGS)

guest: guest_reader.c
	$(CC) $(CFLAGS) -o guest_reader guest_reader.c $(LDFLAGS)

# Deploy guest program to VM (compile source on VM)
deploy: guest
	@echo "Copying guest_reader, common.h, and performance_counters.h to VM..."
	scp $(SCPFLAGS) $(GUEST_PROGRAM).c common.h performance_counters.h $(VM_NAME):$(TARGET_DIR)/
	@echo "Compiling on VM..."
	ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'cd $(TARGET_DIR) && $(CC) $(CFLAGS) -o $(GUEST_PROGRAM) $(GUEST_PROGRAM).c $(LDFLAGS)'
	@echo "Guest program ready at $(TARGET_DIR)/guest_reader on VM"

# Deploy compiled binary to VM (faster for testing)
deploy-binary: guest
	@echo "Copying compiled binary to VM..."
	scp $(SCPFLAGS) $(GUEST_PROGRAM) $(VM_NAME):$(TARGET_DIR)/
	@echo "Binary deployed to $(TARGET_DIR)/$(GUEST_PROGRAM) on VM"

# Run test (starts guest in background via SSH, then runs host)
test: host deploy clean_guest
	@echo ""
	@echo "Starting guest reader on VM..."
	@ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'sudo $(TARGET_DIR)/$(GUEST_PROGRAM)' &
	@echo "Waiting 2 seconds for guest to initialize..."
	@sleep 2
	@echo ""
	@echo "Starting host writer..."
	@./host_writer

# Quick test with specific parameters (use deploy-binary for speed)
quick-test-latency: host deploy-binary
	@echo ""
	@echo "Starting guest reader for latency test (5 messages)..."
	@ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'sudo $(TARGET_DIR)/$(GUEST_PROGRAM) -l 5 2>&1' &
	@echo "Waiting 2 seconds for guest to initialize..."
	@sleep 2
	@echo ""
	@echo "Starting host writer for latency test..."
	@echo "" | ./host_writer -l 5

# Test with single message for debugging
debug-test: host deploy-binary
	@echo ""
	@echo "Starting guest reader for single message debug..."
	@ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'sudo $(TARGET_DIR)/$(GUEST_PROGRAM) -l 1 2>&1' &
	@echo "Waiting 2 seconds for guest to initialize..."
	@sleep 2
	@echo ""
	@echo "Starting host writer for single message..."
	@echo "" | ./host_writer -l 1

clean:
	rm -f host_writer $(GUEST_PROGRAM)
	@ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'rm -f $(TARGET_DIR)/$(GUEST_PROGRAM) $(TARGET_DIR)/$(GUEST_PROGRAM).c $(TARGET_DIR)/common.h $(TARGET_DIR)/performance_counters.h' 2>/dev/null || true

clean_guest:
	@ssh $(SSHFLAGS) $(SSH_PORT_FLAGS) $(VM_NAME) 'rm -f $(TARGET_DIR)/$(GUEST_PROGRAM) $(TARGET_DIR)/$(GUEST_PROGRAM).c $(TARGET_DIR)/common.h $(TARGET_DIR)/performance_counters.h' 2>/dev/null || true


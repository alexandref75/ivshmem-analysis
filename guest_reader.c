/*
 * guest_reader.c - Guest program to read from ivshmem PCI device
 * 
 * This program reads data from the ivshmem PCI BAR2 and measures performance
 * with detailed timing breakdown of all overheads
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>

#include "common.h"
#include "performance_counters.h"

//#define PCI_RESOURCE_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2"
#define PCI_RESOURCE_PATH "/dev/dax0.0"
#define SHMEM_PATH "/dev/shm/ivshmem"

#ifndef AARCH64_CMO_LOWLEVEL_H
#define AARCH64_CMO_LOWLEVEL_H
#endif

#if !defined(__aarch64__)
# error "This header requires AArch64."
#endif

#include <stdint.h>
#include <stddef.h>

#define SHM_ALIGNMENT 2*1024*1024

static inline void flush_data_cache(char* start, char* end_exclusive) {

    uint32_t ctr;
    __asm__ volatile("mrs %x0, ctr_el0" : "=r"(ctr));

    // Extract log2(line size) fields (DminLine[19:16], IminLine[3:0]); size = 4 << field
    uint64_t dline = 4u << ((ctr >> 16) & 0xF);

    start -= (uint64_t)start & (dline - 1);

    if (start == end_exclusive)
        end_exclusive++;

    // Clean D-cache to Point of Unification for each affected D-line
    for (char *p = start; p < (char *)end_exclusive; p += dline)
	__asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");

    __asm__ volatile("dsb ish" ::: "memory");
}

static inline void invalidate_data_cache(char* start, char* end_exclusive) {

    uint32_t ctr;
    __asm__ volatile("mrs %x0, ctr_el0" : "=r"(ctr));

    // Extract log2(line size) fields (DminLine[19:16], IminLine[3:0]); size = 4 << field
    uint64_t dline = 4u << ((ctr >> 16) & 0xF);

    start -= (uint64_t)start & (dline - 1);

    if (start == end_exclusive)
        end_exclusive++;

    // Clean D-cache to Point of Unification for each affected D-line
    for (char *p = start; p < (char *)end_exclusive; p += dline)
	__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");

    __asm__ volatile("dsb ish" ::: "memory");
}
// Print hash comparison for debugging
static void print_hash_data(const uint8_t *expected)
{
    printf("  Expected: ");
    for (int i = 0; i < 32; i++) printf("%02x", expected[i]);
    printf("\n");
}

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Debug logging helper
static void debug_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("DEBUG: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

// Guest state management
static void set_guest_state(volatile struct shared_data *shm, guest_state_t new_state)
{
    guest_state_t old_state = (guest_state_t)shm->guest_state;
    if (old_state != new_state) {
        printf("GUEST STATE: %s -> %s\n", guest_state_name(old_state), guest_state_name(new_state));
        shm->guest_state = (uint32_t)new_state;
	invalidate_data_cache((char *)&shm->guest_state,(char *)&shm->guest_state+sizeof(uint32_t));
        __sync_synchronize();
    }
}

static guest_state_t get_guest_state(volatile struct shared_data *shm)
{
    return (guest_state_t)shm->guest_state;
}

static host_state_t get_host_state(volatile struct shared_data *shm)
{
    return (host_state_t)shm->host_state;
}

// Verify data integrity using SHA256
static bool verify_data_integrity(const uint8_t *data, uint32_t size, const uint8_t *expected_hash, uint8_t *calculated_hash)
{
    EVP_MD_CTX *mdctx;
    unsigned int hashlen;

    if((mdctx = EVP_MD_CTX_new()) == NULL) 
        debug_log("Function EVP_MD_CTX_new return error");

    if(1 != EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL))
        debug_log("Function EVP_DigestInit_ex return error");

    if(1 != EVP_DigestUpdate(mdctx, data, size))
        debug_log("Function EVP_DigestUpdate return error");

    if(1 != EVP_DigestFinal_ex(mdctx, calculated_hash, &hashlen))
        debug_log("Function EVP_DigestFinal_ex return error");

    EVP_MD_CTX_free(mdctx);
    
    return memcmp(calculated_hash, expected_hash, 32) == 0;
}


// Cache flush function
static void flush_cache_range(void *addr, size_t len) {
    #if defined(__x86_64__) || defined(__i386__)
    char *ptr = (char *)addr;
    for (size_t i = 0; i < len; i += 64) {
        __builtin_ia32_clflush(ptr + i);
    }
    __sync_synchronize();
    #else
    __sync_synchronize();
    #endif
}

// Force read of buffer
static void force_buffer_read(const uint8_t *data, uint32_t size)
{
    volatile uint8_t dummy = 0;
    for (size_t i = 0; i < size; i += 64) {
        dummy ^= data[i];
    }
    (void)dummy;
}

// Print usage
static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -l, --latency [COUNT]     Expect latency test (default: 100 messages)\n");
    printf("  -b, --bandwidth [COUNT]   Expect bandwidth test (default: 10 iterations)\n");
    printf("  -c, --count COUNT         Number of messages/iterations to expect\n");
    printf("  -h, --help               Show this help\n");
    printf("\n");
}

// Print hash comparison for debugging
static void print_hash_comparison(const uint8_t *expected, const uint8_t *calculated)
{
    printf("  Expected: ");
    for (int i = 0; i < 32; i++) printf("%02x", expected[i]);
    printf("\n  Got:      ");
    for (int i = 0; i < 32; i++) printf("%02x", calculated[i]);
    printf("\n");
}

void monitor_latency(volatile struct shared_data *shm, bool expect_latency, bool expect_bandwidth, int expected_count)
{
    printf("Guest Reader - Monitoring for messages from host...\n");
    printf("Expected: %s%s%s (count: %d)\n", 
           expect_latency ? "latency " : "",
           (expect_latency && expect_bandwidth) ? "+ " : "",
           expect_bandwidth ? "bandwidth" : "",
           expected_count);
    printf("Will measure: memcpy from shared memory to local buffer (actual transmission)\n");
    printf("Plus SHA256 verification time (testing only, not real overhead)\n\n");
    fflush(stdout);
    
    int message_count = 0;
    uint8_t calculated_hash[32];
    
    // STATE: GUEST_STATE_UNINITIALIZED -> GUEST_STATE_WAITING_HOST_INIT
    set_guest_state(shm, GUEST_STATE_WAITING_HOST_INIT);
    
    printf("GUEST: Checking for host initialization...\n");
    
    host_state_t host_state = get_host_state(shm);
    if (host_state != HOST_STATE_UNINITIALIZED && host_state != HOST_STATE_READY) {
        printf("GUEST: Detected host in state %s, waiting...\n", host_state_name(host_state));
    }
    
    printf("GUEST: Waiting for host initialization handshake...\n");
    
    int init_timeout = 0;
    bool host_ready = false;
    
    while (!host_ready && init_timeout < 5000) {
        if (shm->magic == MAGIC && get_host_state(shm) == HOST_STATE_READY) {
            host_ready = true;
            break;
        }
        
        if (shm->magic == 0 && get_host_state(shm) == HOST_STATE_INITIALIZING) {
            printf("GUEST: Host initialization in progress...\n");
        }
        
        usleep(10000);
        init_timeout++;
    }
    
    if (!host_ready) {
        printf("GUEST: TIMEOUT - Host not ready after 50 seconds\n");
        printf("GUEST: Current state - Magic: 0x%08X, Host state: %s\n", 
               shm->magic, host_state_name(get_host_state(shm)));
        exit(1);
    }
    
    printf("GUEST: ✓ Host initialization complete - ready for messages.\n\n");
    
    // Allocate local buffer for memcpy (reuse for all messages)
    // Max size for 4K frame
    size_t max_buffer_size = 3840 * 2160 * 3;
    uint8_t *local_buffer = malloc(max_buffer_size);
    if (!local_buffer) {
        printf("GUEST: ERROR - Failed to allocate local buffer\n");
        exit(1);
    }
    
    // Initialize performance counters
    struct perf_counters perf_counters;
    bool perf_available = perf_counters_init(&perf_counters);
    if (perf_available) {
        printf("GUEST: ✓ Hardware performance counters initialized\n\n");
    } else {
        printf("GUEST: ⚠ Hardware performance counters not available\n");
        printf("  Cache miss analysis will be limited\n\n");
    }
    
    // STATE: GUEST_STATE_WAITING_HOST_INIT -> GUEST_STATE_READY
    set_guest_state(shm, GUEST_STATE_READY);
    
    while (message_count < expected_count) {
        if (shm->test_complete == 1) {
            printf("Test completion signal received. Exiting...\n");
            break;
        }
        
        // Wait for host to start sending (HOST_STATE_SENDING)
        while (get_host_state(shm) != HOST_STATE_SENDING && shm->test_complete == 0) {
            usleep(10);
        }
        
        if (shm->test_complete == 1) {
            printf("Test completion signal received during wait. Exiting...\n");
            break;
        }
        
        // Start total processing timer (on GUEST clock)
        uint64_t processing_start = get_time_ns();
        
        // STATE: GUEST_STATE_READY -> GUEST_STATE_PROCESSING
        set_guest_state(shm, GUEST_STATE_PROCESSING);
        
        // Read message metadata
        uint32_t sequence = shm->sequence;
        uint32_t data_size = shm->data_size;
        
        uint8_t expected_hash[32];
        memcpy(expected_hash, (const void *)shm->data_sha256, 32);
        
        message_count++;
        
        printf("\n=== Message %d Received ===\n", message_count);
        printf("Sequence: %u, Data size: %u bytes (%.2f MB)\n", 
               sequence, data_size, data_size / (1024.0 * 1024.0));
        fflush(stdout);
        
        bool success = true;
        uint32_t error_code = 0;
        uint8_t *measurement_buffer = NULL;
        
        // Get data pointer from shared memory
        uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
        
        // Pre-allocate measurement buffer (reuse for consistent measurements)
        measurement_buffer = malloc(data_size);
        if (!measurement_buffer) {
            printf("Error: Failed to allocate measurement buffer\n");
            success = false;
            error_code = 2;
            goto cleanup_and_continue;
        }
        
        // Warm up the measurement buffer to avoid allocation overhead in measurements
        memset(measurement_buffer, 0, data_size);
        
        struct perf_results guest_perf_results = {0};
        
        // Start performance counters for all measurements
        if (perf_available) {
            perf_counters_start(&perf_counters);
        }
        
        // WARM-UP: Initial access to handle page faults and system overhead
        // This is not measured but prepares the system for accurate measurements
        volatile uint64_t dummy_warmup = 0;
        for (size_t i = 0; i < data_size; i += 64) {
            dummy_warmup ^= data_ptr[i];
        }
        __sync_synchronize();
        
        // PHASE A: PURE READ (HOT CACHE) - Read shared memory without writing
        // After warm-up, data should be in CPU cache
        uint64_t hot_read_start = get_time_ns();
        
        volatile uint64_t dummy_hot = 0;
        for (size_t i = 0; i < data_size; i += 64) {
            dummy_hot ^= data_ptr[i];
        }
        __sync_synchronize(); // Ensure reads complete
        
        uint64_t hot_read_end = get_time_ns();
        uint64_t hot_cache_duration = hot_read_end - hot_read_start;
        
        // PHASE B: PURE READ (COLD CACHE) - Read shared memory after cache flush
        // Flush cache lines for the shared memory to force memory access
        //flush_cache_range(data_ptr, data_size);
        
        uint64_t cold_read_start = get_time_ns();
        
        volatile uint64_t dummy_cold = 0;
        for (size_t i = 0; i < data_size; i += 64) {
            dummy_cold ^= data_ptr[i];
        }
        __sync_synchronize(); // Ensure reads complete
        
        uint64_t cold_read_end = get_time_ns();
        uint64_t cold_cache_duration = cold_read_end - cold_read_start;
        
        // PHASE C: READ+WRITE (COLD CACHE) - memcpy after cache flush to measure write overhead
        // Flush cache again to ensure we're measuring from cold state
        //flush_cache_range(data_ptr, data_size);
        
        uint64_t memcpy_start = get_time_ns();
        
        if (memchr(data_ptr, 255, data_size) != NULL) {
	   printf("Found 255 on the buffer, it should not have one \n");
	   exit(1);
	}
	invalidate_data_cache((char *)data_ptr,(char *)data_ptr + data_size);
        __sync_synchronize(); // Ensure memcpy completes
        
        uint64_t memcpy_end = get_time_ns();
        uint64_t second_pass_duration = memcpy_end - memcpy_start;
        
        // Stop performance counters after all memory operations
        if (perf_available) {
            perf_counters_stop(&perf_counters, &guest_perf_results, data_size * 4); // warmup + 2 reads + 1 memcpy
        }
        
        // Copy final data to local buffer for verification (using the memcpy result)
        //memcpy(local_buffer, measurement_buffer, data_size);
        
        // PHASE D: SHA256 INTEGRITY CHECK - SHA256 with data in local cache
        uint64_t verify_start = get_time_ns();

        bool hash_match = verify_data_integrity(data_ptr, data_size, expected_hash, calculated_hash);
        
        uint64_t verify_end = get_time_ns();
        uint64_t cached_verify_duration = verify_end - verify_start;
        
        // Calculate legacy timing for backward compatibility
        uint64_t memcpy_duration = second_pass_duration; // Use memcpy (read+write) measurement
        uint64_t verify_duration = cached_verify_duration;
        
        // Calculate total processing duration
        uint64_t processing_end = get_time_ns();
        uint64_t total_duration = processing_end - processing_start;
        
        // WRITE DURATIONS and PERFORMANCE METRICS to shared memory for host to read
        // Legacy fields for backward compatibility
        shm->timing.guest_copy_duration = memcpy_duration;
        shm->timing.guest_verify_duration = verify_duration;
        shm->timing.guest_total_duration = total_duration;
        
        // NEW: Detailed cache behavior measurements
        shm->timing.guest_hot_cache_duration = hot_cache_duration;
        shm->timing.guest_cold_cache_duration = cold_cache_duration;
        shm->timing.guest_second_pass_duration = second_pass_duration;
        shm->timing.guest_cached_verify_duration = cached_verify_duration;
        
        // Write performance metrics (convert floating point to fixed-point for shared memory)
        shm->timing.guest_perf.l1_cache_misses = guest_perf_results.l1_cache_misses;
        shm->timing.guest_perf.l1_cache_references = guest_perf_results.l1_cache_references;
        shm->timing.guest_perf.llc_misses = guest_perf_results.llc_misses;
        shm->timing.guest_perf.llc_references = guest_perf_results.llc_references;
        shm->timing.guest_perf.memory_loads = guest_perf_results.memory_loads;
        shm->timing.guest_perf.memory_stores = guest_perf_results.memory_stores;
        shm->timing.guest_perf.tlb_misses = guest_perf_results.tlb_misses;
        shm->timing.guest_perf.cpu_cycles = guest_perf_results.cpu_cycles;
        shm->timing.guest_perf.instructions = guest_perf_results.instructions;
        shm->timing.guest_perf.context_switches = guest_perf_results.context_switches;
        
        // Convert rates to fixed-point integers (multiply by 10000)
        shm->timing.guest_perf.l1_cache_miss_rate_x10000 = (uint32_t)(guest_perf_results.l1_cache_miss_rate * 10000.0);
        shm->timing.guest_perf.llc_cache_miss_rate_x10000 = (uint32_t)(guest_perf_results.llc_cache_miss_rate * 10000.0);
        shm->timing.guest_perf.instructions_per_cycle_x10000 = (uint32_t)(guest_perf_results.instructions_per_cycle * 10000.0);
        shm->timing.guest_perf.cycles_per_byte_x10000 = (uint32_t)(guest_perf_results.cycles_per_byte * 10000.0);
        shm->timing.guest_perf.tlb_miss_rate_x10000 = (uint32_t)(guest_perf_results.tlb_miss_rate * 10000.0);
        
        invalidate_data_cache((char *)&shm->timing,(char *)&shm->timing+sizeof(struct timing_data));
        __sync_synchronize();
        
        // Display results with performance metrics
        printf("Guest Timing (measured on guest clock) - Isolated Read/Write Analysis:\n");
        printf("  Phase A (Pure Read Hot):   %lu ns (%.2f µs) [%6.0f MB/s] - Read shared memory (hot cache)\n", 
               hot_cache_duration, hot_cache_duration / 1000.0, 
               (data_size / (1024.0 * 1024.0)) / (hot_cache_duration / 1e9));
        printf("  Phase B (Pure Read Cold):  %lu ns (%.2f µs) [%6.0f MB/s] - Read shared memory (cold cache)\n", 
               cold_cache_duration, cold_cache_duration / 1000.0, 
               (data_size / (1024.0 * 1024.0)) / (cold_cache_duration / 1e9));
        printf("  Phase C (Read+Write):      %lu ns (%.2f µs) [%6.0f MB/s] - memcpy (read+write)\n", 
               second_pass_duration, second_pass_duration / 1000.0, 
               (data_size / (1024.0 * 1024.0)) / (second_pass_duration / 1e9));
        printf("  Phase D (SHA256 Verify):   %lu ns (%.2f µs) [testing only] - Integrity check\n", 
               cached_verify_duration, cached_verify_duration / 1000.0);
        
        if (perf_available) {
            printf("    Performance (2 reads + 1 memcpy):  L1 cache %.1f%% miss, LLC cache %.1f%% miss, TLB %.3f%% miss\n",
                   guest_perf_results.l1_cache_miss_rate * 100.0,
                   guest_perf_results.llc_cache_miss_rate * 100.0,
                   guest_perf_results.tlb_miss_rate * 100.0);
            printf("    CPU:          %.2f IPC, %.1f cycles/byte, %lu context switches\n",
                   guest_perf_results.instructions_per_cycle,
                   guest_perf_results.cycles_per_byte,
                   guest_perf_results.context_switches);
        }
        
        printf("\nAnalysis:\n");
        printf("  Write overhead (C-B):  %+ld ns (%+.2f µs) [%+.1f%%]\n",
               (int64_t)(second_pass_duration - cold_cache_duration),
               (second_pass_duration - cold_cache_duration) / 1000.0,
               ((double)(second_pass_duration - cold_cache_duration) / cold_cache_duration) * 100.0);
        printf("  Cache effect (B-A):    %+ld ns (%+.2f µs) [%+.1f%%]\n",
               (int64_t)(cold_cache_duration - hot_cache_duration),
               (cold_cache_duration - hot_cache_duration) / 1000.0,
               ((double)(cold_cache_duration - hot_cache_duration) / hot_cache_duration) * 100.0);
        printf("  Legacy memcpy:         %lu ns (%.2f µs) [%6.0f MB/s] (Phase C)\n", 
               memcpy_duration, memcpy_duration / 1000.0, 
               (data_size / (1024.0 * 1024.0)) / (memcpy_duration / 1e9));
        printf("  Total:                 %lu ns (%.2f µs)\n", 
               total_duration, total_duration / 1000.0);
        
        if (hash_match) {
            printf("✓ Data integrity verified: SHA256 match\n");
        } else {
            printf("✗ Data integrity check FAILED: SHA256 mismatch\n");
            print_hash_comparison(expected_hash, calculated_hash);
	    printf("Begin buffer shared\n");
	    print_hash_data(data_ptr);
	    printf("End buffer shared\n");
	    print_hash_data(data_ptr+(data_size-32));
            success = false;
            error_code = 1;
        }
        
        printf("  Processing complete\n\n");
        
cleanup_and_continue:
        // Cleanup measurement buffer
        if (measurement_buffer) {
            free(measurement_buffer);
            measurement_buffer = NULL;
        }
        
        if (!success) {
            shm->error_code = error_code;
	    invalidate_data_cache((char *)&shm->error_code,(char *)&shm->error_code+sizeof(uint32_t));
            __sync_synchronize();
        }
        
        // STATE: GUEST_STATE_PROCESSING -> GUEST_STATE_ACKNOWLEDGED
        set_guest_state(shm, GUEST_STATE_ACKNOWLEDGED);
        
        // Wait for host to finish with this message
        while (get_host_state(shm) != HOST_STATE_READY && shm->test_complete == 0) {
            usleep(10);
        }
        
        // STATE: GUEST_STATE_ACKNOWLEDGED -> GUEST_STATE_READY
        set_guest_state(shm, GUEST_STATE_READY);
    }
    
    printf("Guest monitoring loop ended after %d messages\n", message_count);
    
    // Cleanup
    free(local_buffer);
    
    if (perf_available) {
        perf_counters_cleanup(&perf_counters);
    }
}

int main(int argc, char *argv[])
{
    printf("Guest Reader - ivshmem Performance Test with Timing Analysis\n");
    printf("============================================================\n\n");
    
    // Parse command line arguments
    bool expect_latency = false;
    bool expect_bandwidth = false;
    int latency_count = 1000;
    int bandwidth_count = 10;
    int custom_count = -1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            expect_latency = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                latency_count = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            expect_bandwidth = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                bandwidth_count = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            if (i + 1 < argc) {
                custom_count = atoi(argv[++i]);
            } else {
                fprintf(stderr, "Error: -c requires a count argument\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (!expect_latency && !expect_bandwidth) {
        expect_latency = true;
        expect_bandwidth = true;
    }
    
    int expected_count;
    if (custom_count > 0) {
        expected_count = custom_count;
    } else if (expect_latency && expect_bandwidth) {
        expected_count = latency_count + bandwidth_count;
    } else if (expect_latency) {
        expected_count = latency_count;
    } else {
        expected_count = bandwidth_count;
    }
    
    printf("Configuration:\n");
    printf("  Expect latency: %s (%d messages)\n", expect_latency ? "yes" : "no", latency_count);
    printf("  Expect bandwidth: %s (%d iterations)\n", expect_bandwidth ? "yes" : "no", bandwidth_count);
    printf("  Total expected messages: %d\n\n", expected_count);
    fflush(stdout);
    
    // Check device
    int fd;
    struct stat st;
    const char *device_path = PCI_RESOURCE_PATH;
    
    if (access(PCI_RESOURCE_PATH, F_OK) != 0) {
        printf("INFO: PCI device not found, trying shared memory for host testing...\n");
        device_path = SHMEM_PATH;
        
        if (access(SHMEM_PATH, F_OK) != 0) {
            printf("ERROR: Neither PCI device nor shared memory found\n");
            printf("Make sure you're running this inside the VM or have shared memory set up.\n");
            return 1;
        }
    }
    
    // Open resource
    fd = open(device_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open device resource");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        return 1;
    }
    
    // Get resource size
    //if (fstat(fd, &st) < 0) {
     //   perror("fstat");
      //  close(fd);
       // return 1;
    //}
    
    //printf("Resource: %s\n", device_path);
    //printf("Resource size: %ld bytes (%ld MB)\n", 
    //       st.st_size, st.st_size / (1024 * 1024));
    //fflush(stdout);
    
    // Map memory
    //void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, 
    void *ptr = mmap(NULL, 64*1024*1024, PROT_READ | PROT_WRITE, 
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        close(fd);
        return 1;
    }
    
    volatile struct shared_data *shm = (volatile struct shared_data *)ptr;
    
    // Initialize guest state
    set_guest_state(shm, GUEST_STATE_UNINITIALIZED);

    if (shm->magic_client != MAGIC_CLIENT) {
	printf("client magic not set\n");
	shm->magic_client = MAGIC_CLIENT;
    }
    
    printf("Mapped at address: %p\n", ptr);
    printf("Ready to receive data from host.\n\n");
    fflush(stdout);
    
    printf("Initial values:\n");
    printf("  Magic: 0x%08X\n", shm->magic);
    printf("  Magic_client: 0x%08X\n", shm->magic_client);
    printf("  Sequence: %u\n", shm->sequence);
    printf("  Data size: %u\n", shm->data_size);
    printf("  Error code: %u\n", shm->error_code);
    printf("  Test complete: %u\n", shm->test_complete);
    printf("  Host state: %u (%s)\n", shm->host_state, host_state_name((host_state_t)shm->host_state));
    printf("  Guest state: %u (%s)\n", shm->guest_state, guest_state_name((guest_state_t)shm->guest_state));
    printf("\n");
    fflush(stdout);
    
    // Start monitoring
    monitor_latency(shm, expect_latency, expect_bandwidth, expected_count);
    
    // Cleanup
    munmap(ptr, st.st_size);
    close(fd);
    
    return 0;
}

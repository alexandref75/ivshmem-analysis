/*
 * host_writer.c - Host program to write to shared memory
 * 
 * This program writes data to /dev/shm/ivshmem and measures performance
 * with detailed timing breakdown of all overheads
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#include "common.h"
#include "performance_counters.h"

//#define SHMEM_PATH "/dev/shm/ivshmem"
#define SHMEM_PATH "/dev/hugepages/ivshmem"
#define SHMEM_SIZE (64 * 1024 * 1024)  // 64MB
#define FRAME_SIZE (3840 * 2160 * 4)    // 4K RGBA frame (33MB)
#define SECOND_NS 1000000000ULL


#ifndef AARCH64_CMO_LOWLEVEL_H
#define AARCH64_CMO_LOWLEVEL_H
#endif

#if !defined(__aarch64__)
# error "This header requires AArch64."
#endif

#include <stdint.h>
#include <stddef.h>

#define SHM_ALIGNMENT 2*1024*1024
//#define SHM_ALIGNMENT 0

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
    return (uint64_t)ts.tv_sec * SECOND_NS + ts.tv_nsec;
}

// Debug logging function
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

// Host state management (host only modifies host_state)
static void set_host_state(volatile struct shared_data *shm, host_state_t new_state)
{
    host_state_t old_state = (host_state_t)shm->host_state;
    if (old_state != new_state) {
        printf("HOST STATE: %s -> %s\n", host_state_name(old_state), host_state_name(new_state));
        shm->host_state = (uint32_t)new_state;
	invalidate_data_cache((char*)&shm->host_state,(char*)&shm->host_state+sizeof(uint32_t));
        __sync_synchronize();
    }
}

static host_state_t get_host_state(volatile struct shared_data *shm)
{
    return (host_state_t)shm->host_state;
}

static guest_state_t get_guest_state(volatile struct shared_data *shm)
{
    //invalidate_data_cache(&shm->guest_state,&shm->guest_state + sizeof(guest_state_t));
    return (guest_state_t)shm->guest_state;
}

// CSV result logging helper
typedef struct {
    FILE *file;
    const char *filename;
} csv_logger_t;

static csv_logger_t* csv_create(const char *filename, const char *header)
{
    csv_logger_t *logger = malloc(sizeof(csv_logger_t));
    if (!logger) return NULL;
    
    logger->file = fopen(filename, "w");
    logger->filename = filename;
    
    if (logger->file && header) {
        fprintf(logger->file, "%s\n", header);
    }
    
    return logger;
}

static void csv_write_bandwidth_result(csv_logger_t *logger, int iteration, const char *frame_name,
                                     int width, int height, int bpp, size_t size_bytes,
                                     uint64_t write_ns, uint64_t roundtrip_ns, 
                                     uint64_t guest_read_ns, uint64_t guest_verify_ns,
                                     bool success)
{
    if (logger && logger->file) {
        double size_mb = size_bytes / (1024.0 * 1024.0);
        double write_bw = success && write_ns > 0 ? (size_mb / (write_ns / 1e9)) : 0.0;
        double read_bw = success && guest_read_ns > 0 ? (size_mb / (guest_read_ns / 1e9)) : 0.0;
        uint64_t total_ns = write_ns + roundtrip_ns;
        double total_bw = success && total_ns > 0 ? (size_mb / (total_ns / 1e9)) : 0.0;
        
        fprintf(logger->file, "%d,%s,%d,%d,%d,%zu,%.2f,%lu,%.2f,%.2f,%lu,%.2f,%lu,%.2f,%.2f,%lu,%.2f,%lu,%.2f,%.2f,%d\n",
                iteration, frame_name, width, height, bpp, size_bytes, size_mb,
                write_ns, write_ns / 1000000.0, write_bw,
                roundtrip_ns, roundtrip_ns / 1000000.0,
                guest_read_ns, guest_read_ns / 1000000.0, read_bw,
                guest_verify_ns, guest_verify_ns / 1000000.0,
                total_ns, total_ns / 1000000.0, total_bw,
                success ? 1 : 0);
    }
}

static void csv_close(csv_logger_t *logger)
{
    if (logger) {
        if (logger->file) {
            fclose(logger->file);
            printf("\n  ✓ Data exported to %s\n", logger->filename);
        }
        free(logger);
    }
}

// Calculate SHA256 hash of buffer
static void calculate_sha256(const uint8_t *data, size_t len, uint8_t *hash)
{
    EVP_MD_CTX *mdctx;
    unsigned int size;

    if((mdctx = EVP_MD_CTX_new()) == NULL) 
        debug_log("Function EVP_MD_CTX_new return error");

    if(1 != EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL))
        debug_log("Function EVP_DigestInit_ex return error");

    if(1 != EVP_DigestUpdate(mdctx, data, len))
        debug_log("Function EVP_DigestUpdate return error");

    if(1 != EVP_DigestFinal_ex(mdctx, hash, &size))
        debug_log("Function EVP_DigestFinal_ex return error");

    EVP_MD_CTX_free(mdctx);
}

// Generate random frame buffer (width x height x 24bpp)
static void generate_random_frame(uint8_t *buffer, int width, int height) {
    size_t frame_size = width * height * 3; // 24bpp = 3 bytes per pixel
    
    // Generate cryptographically random data to avoid cache-friendly patterns
    if (RAND_bytes(buffer, frame_size) != 1) {
        // Fallback to pseudo-random if OpenSSL fails
        static int iteration_counter = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned int seed = (unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ (++iteration_counter));
        srand(seed);
        
        for (size_t i = 0; i < frame_size; i++) {
            buffer[i] = rand() & 0xFF;
	    if (buffer[i]==255) {
		buffer[i]=0;
            }
        }
    }
}

// Wait for guest state change with timeout
static bool wait_for_guest_state(volatile struct shared_data *shm, guest_state_t expected_state, 
                                  uint64_t timeout_ns, const char *description)
{
    uint64_t start_time = get_time_ns();
    
    while (get_guest_state(shm) != expected_state) {
        if (get_time_ns() - start_time > timeout_ns) {
            debug_log("TIMEOUT waiting for guest state %s (current: %s)", 
                     guest_state_name(expected_state), 
                     guest_state_name(get_guest_state(shm)));
            return false;
        }
        usleep(10); // 10 microsecond polling interval
    }
    
    return true;
}

void test_latency(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Latency Test - Measuring Actual Transmission Overhead ===\n");
    printf("Measuring %d messages with 4K frame data...\n", iterations);
    printf("Host: memcpy to shared memory | Guest: memcpy from shared memory\n");
    printf("(Data generation and SHA256 done outside measurement)\n\n");
    
    // Create CSV loggers - separate files for timing and performance metrics
    csv_logger_t *csv = csv_create("latency_results.csv", 
        "iteration,host_memcpy_ns,host_memcpy_us,roundtrip_ns,roundtrip_us,guest_memcpy_ns,guest_memcpy_us,guest_verify_ns,guest_verify_us,guest_hot_cache_ns,guest_hot_cache_us,guest_cold_cache_ns,guest_cold_cache_us,guest_second_pass_ns,guest_second_pass_us,guest_cached_verify_ns,guest_cached_verify_us,notification_est_ns,notification_est_us,total_ns,total_us,success");
    
    csv_logger_t *perf_csv = csv_create("latency_performance.csv",
        "iteration,host_l1_cache_misses,host_l1_cache_references,host_l1_miss_rate,host_llc_misses,host_llc_references,host_llc_miss_rate,host_tlb_misses,host_cpu_cycles,host_instructions,host_ipc,host_cycles_per_byte,host_context_switches,guest_l1_cache_misses,guest_l1_cache_references,guest_l1_miss_rate,guest_llc_misses,guest_llc_references,guest_llc_miss_rate,guest_tlb_misses,guest_cpu_cycles,guest_instructions,guest_ipc,guest_cycles_per_byte,guest_context_switches");
    
    // Calculate available buffer size and use 4K frame for latency test
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Use 4K frame (3840x2160x3 = 24.8MB) for latency test
    int width = 3840, height = 2160, bpp = 3;
    //int width = 1440, height = 960, bpp = 3;
    size_t frame_size = width * height * bpp;
    
    if (frame_size > max_data_size) {
        printf("ERROR: 4K frame too large (%zu bytes > %zu bytes)\n", frame_size, max_data_size);
        csv_close(csv);
        return;
    }
    
    printf("Using 4K frame: %dx%d, %.2f MB per message\n", 
           width, height, frame_size / (1024.0 * 1024.0));
    
    // PRE-GENERATE test data (do this ONCE, outside measurements)
    printf("Pre-generating test frame data...\n");
    uint8_t *test_frame = malloc(frame_size);
    if (!test_frame) {
        printf("ERROR: Failed to allocate test frame buffer\n");
        csv_close(csv);
        return;
    }
    
    generate_random_frame(test_frame, width, height);

    char *char_ptr = (char *)test_frame;
    size_t cur_frame_size  = frame_size;
    while (((char_ptr = (char*)(memchr(char_ptr,255,cur_frame_size))) != NULL) && (cur_frame_size > 0)) {
        *char_ptr = 0;
	cur_frame_size = frame_size-((char *)test_frame - char_ptr);
    }

    if (memchr(test_frame,255,frame_size) != NULL) {
	printf("ERROR: 255 found on frame\n");
	exit(1);
     }
    // Pre-calculate SHA256 of test data
    uint8_t expected_hash[32];
    calculate_sha256(test_frame, frame_size, expected_hash);
    print_hash_data(expected_hash);
    printf("Frame begining:\n");
    print_hash_data(test_frame);
    printf("Frame end:\n");
    print_hash_data(test_frame+(frame_size-32));
    
    printf("Test data ready. Starting measurements...\n\n");
    
    // Initialize performance counters
    struct perf_counters perf_counters;
    bool perf_available = perf_counters_init(&perf_counters);
    if (perf_available) {
        printf("✓ Hardware performance counters initialized\n");
    } else {
        printf("⚠ Hardware performance counters not available (running without sudo or unsupported)\n");
        printf("  Cache miss analysis will be limited\n");
    }
    printf("\n");
    
    // Accumulators for statistics
    uint64_t total_memcpy = 0, total_roundtrip = 0, total_guest_copy = 0, total_verify = 0, total_notification = 0, total_total = 0;
    uint64_t min_memcpy = UINT64_MAX, max_memcpy = 0;
    uint64_t min_roundtrip = UINT64_MAX, max_roundtrip = 0;
    uint64_t min_guest_copy = UINT64_MAX, max_guest_copy = 0;
    uint64_t min_verify = UINT64_MAX, max_verify = 0;
    uint64_t min_notification = UINT64_MAX, max_notification = 0;
    int successful = 0;
    
    for (int i = 0; i < iterations; i++) {
        // Clear timing structure
        memset((void *)&shm->timing, 0, sizeof(struct timing_data));
        
        // Reset error code
        shm->error_code = 0;
	invalidate_data_cache((char *)&shm->error_code,(char *)&shm->error_code+sizeof(uint32_t));
        __sync_synchronize();
        
        uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
        
        // Prepare message headers BEFORE timing
        shm->sequence = i;
        shm->data_size = frame_size;
        memcpy((void*)shm->data_sha256, expected_hash, 32);
	invalidate_data_cache((char *)&shm->data_sha256,(char *)&shm->data_sha256+32);
        __sync_synchronize();
        
        // MEASUREMENT 1: Host memcpy time + performance counters - THIS IS THE ACTUAL WRITE OVERHEAD
        struct perf_results host_perf_results = {0};
        
        // Start performance counters
        if (perf_available) {
            perf_counters_start(&perf_counters);
        }
        
        uint64_t memcpy_start = get_time_ns();
        
        memcpy((void*)data_ptr, test_frame, frame_size);
	invalidate_data_cache((char *)data_ptr,(char *)data_ptr+frame_size);
        __sync_synchronize(); // Ensure write completes before timing ends
        
        uint64_t memcpy_end = get_time_ns();
        
        // Stop performance counters and collect results
        if (perf_available) {
            perf_counters_stop(&perf_counters, &host_perf_results, frame_size);
        }
        
        // MEASUREMENT 2: Round-trip time (from state change to guest done)
        uint64_t roundtrip_start = get_time_ns();
        
        // STATE: HOST_STATE_READY -> HOST_STATE_SENDING
        set_host_state(shm, HOST_STATE_SENDING);
        
        // Wait for guest to start processing
        //if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 30*SECOND_NS, "guest processing")) {
        //    printf("  [%d] TIMEOUT (guest didn't start processing)\n", i);
        //    if (csv && csv->file) {
        //        fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
        //    }
        //    if (perf_csv && perf_csv->file) {
        //        fprintf(perf_csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
        //    }
        //    continue;
       // }
        
        // Wait for guest to finish processing
        if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 30*SECOND_NS, "guest acknowledged")) {
            printf("  [%d] TIMEOUT (guest didn't finish processing)\n", i);
            if (csv && csv->file) {
                fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            if (perf_csv && perf_csv->file) {
                fprintf(perf_csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            continue;
        }
        
        uint64_t roundtrip_end = get_time_ns();
        
        // Check for errors
        if (shm->error_code != 0) {
            printf("  [%d] ERROR: %u\n", i, shm->error_code);
            if (csv && csv->file) {
                fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            if (perf_csv && perf_csv->file) {
                fprintf(perf_csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            continue;
        }
        
        // Calculate times
        uint64_t memcpy_time = memcpy_end - memcpy_start;
        uint64_t roundtrip_time = roundtrip_end - roundtrip_start;
        
        // Read guest-measured durations (legacy fields for backward compatibility)
        uint64_t guest_copy_time = shm->timing.guest_copy_duration;
        uint64_t guest_verify_time = shm->timing.guest_verify_duration;
        uint64_t guest_total_time = shm->timing.guest_total_duration;
        
        // Read new detailed cache behavior measurements
        uint64_t guest_hot_cache_time = shm->timing.guest_hot_cache_duration;
        uint64_t guest_cold_cache_time = shm->timing.guest_cold_cache_duration;
        uint64_t guest_second_pass_time = shm->timing.guest_second_pass_duration;
        uint64_t guest_cached_verify_time = shm->timing.guest_cached_verify_duration;
        
        // Estimate notification overhead
        uint64_t notification_est = (roundtrip_time > guest_total_time) ? 
                                    (roundtrip_time - guest_total_time) : 0;
        
        uint64_t total_time = memcpy_time + roundtrip_time;
        
        // Update statistics
        total_memcpy += memcpy_time;
        total_roundtrip += roundtrip_time;
        total_guest_copy += guest_copy_time;
        total_verify += guest_verify_time;
        total_notification += notification_est;
        total_total += total_time;
        
        if (memcpy_time < min_memcpy) min_memcpy = memcpy_time;
        if (memcpy_time > max_memcpy) max_memcpy = memcpy_time;
        if (roundtrip_time < min_roundtrip) min_roundtrip = roundtrip_time;
        if (roundtrip_time > max_roundtrip) max_roundtrip = roundtrip_time;
        if (guest_copy_time < min_guest_copy) min_guest_copy = guest_copy_time;
        if (guest_copy_time > max_guest_copy) max_guest_copy = guest_copy_time;
        if (guest_verify_time < min_verify) min_verify = guest_verify_time;
        if (guest_verify_time > max_verify) max_verify = guest_verify_time;
        if (notification_est < min_notification) min_notification = notification_est;
        if (notification_est > max_notification) max_notification = notification_est;
        
        successful++;
        
        // Extract guest performance metrics
        double guest_l1_miss_rate = shm->timing.guest_perf.l1_cache_miss_rate_x10000 / 10000.0;
        double guest_llc_miss_rate = shm->timing.guest_perf.llc_cache_miss_rate_x10000 / 10000.0;
        double guest_ipc = shm->timing.guest_perf.instructions_per_cycle_x10000 / 10000.0;
        double guest_cycles_per_byte = shm->timing.guest_perf.cycles_per_byte_x10000 / 10000.0;
        
        // Write timing data to main CSV (clean and readable)
        if (csv && csv->file) {
            fprintf(csv->file, "%d,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%d\n",
                    i, 
                    memcpy_time, memcpy_time / 1000.0,
                    roundtrip_time, roundtrip_time / 1000.0,
                    guest_copy_time, guest_copy_time / 1000.0,
                    guest_verify_time, guest_verify_time / 1000.0,
                    guest_hot_cache_time, guest_hot_cache_time / 1000.0,
                    guest_cold_cache_time, guest_cold_cache_time / 1000.0,
                    guest_second_pass_time, guest_second_pass_time / 1000.0,
                    guest_cached_verify_time, guest_cached_verify_time / 1000.0,
                    notification_est, notification_est / 1000.0,
                    total_time, total_time / 1000.0,
                    1);
        }
        
        // Write performance metrics to separate CSV
        if (perf_csv && perf_csv->file) {
            fprintf(perf_csv->file, "%d,%lu,%lu,%.4f,%lu,%lu,%.4f,%lu,%lu,%lu,%.2f,%.2f,%lu,%lu,%lu,%.4f,%lu,%lu,%.4f,%lu,%lu,%lu,%.2f,%.2f,%lu\n",
                    i,
                    // Host performance metrics
                    host_perf_results.l1_cache_misses, host_perf_results.l1_cache_references, host_perf_results.l1_cache_miss_rate,
                    host_perf_results.llc_misses, host_perf_results.llc_references, host_perf_results.llc_cache_miss_rate,
                    host_perf_results.tlb_misses, host_perf_results.cpu_cycles, host_perf_results.instructions,
                    host_perf_results.instructions_per_cycle, host_perf_results.cycles_per_byte, host_perf_results.context_switches,
                    // Guest performance metrics
                    shm->timing.guest_perf.l1_cache_misses, shm->timing.guest_perf.l1_cache_references, guest_l1_miss_rate,
                    shm->timing.guest_perf.llc_misses, shm->timing.guest_perf.llc_references, guest_llc_miss_rate,
                    shm->timing.guest_perf.tlb_misses, shm->timing.guest_perf.cpu_cycles, shm->timing.guest_perf.instructions,
                    guest_ipc, guest_cycles_per_byte, shm->timing.guest_perf.context_switches);
        }
        
        if (successful % 100 == 0 || iterations <= 10) {
            printf("  [%d] Host: %.2f µs | Guest Phases: Hot=%.2f µs, Cold=%.2f µs, 2nd=%.2f µs, Verify=%.2f µs | Total: %.2f µs\n", 
                   i, memcpy_time / 1000.0, 
                   guest_hot_cache_time / 1000.0, guest_cold_cache_time / 1000.0, 
                   guest_second_pass_time / 1000.0, guest_cached_verify_time / 1000.0,
                   total_time / 1000.0);
        }
        
        // STATE: HOST_STATE_SENDING -> HOST_STATE_READY
        set_host_state(shm, HOST_STATE_READY);
        
        // Wait for guest to be ready for next message
        if (!wait_for_guest_state(shm, GUEST_STATE_READY, 30*SECOND_NS, "guest ready for next")) {
            printf("  [%d] WARNING: Guest didn't return to ready state\n", i);
        }
    }
    
    if (successful > 0) {
        printf("\n=== Latency Test Results ===\n");
        printf("Successful: %d/%d\n", successful, iterations);
        printf("Frame size: %.2f MB (4K frame)\n\n", frame_size / (1024.0 * 1024.0));
        
        printf("TRANSMISSION OVERHEAD BREAKDOWN (Average):\n");
        printf("  Host memcpy:          %7lu ns (%7.2f µs) [%6.1f%%] %.0f MB/s\n", 
               total_memcpy / successful, (total_memcpy / successful) / 1000.0,
               100.0 * total_memcpy / total_total,
               (frame_size / (1024.0 * 1024.0)) / ((total_memcpy / successful) / 1e9));
        printf("  Notification (est):   %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_notification / successful, (total_notification / successful) / 1000.0,
               100.0 * total_notification / total_total);
        printf("  Guest memcpy:         %7lu ns (%7.2f µs) [%6.1f%%] %.0f MB/s\n", 
               total_guest_copy / successful, (total_guest_copy / successful) / 1000.0,
               100.0 * total_guest_copy / total_total,
               (frame_size / (1024.0 * 1024.0)) / ((total_guest_copy / successful) / 1e9));
        printf("  Verify (testing):     %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_verify / successful, (total_verify / successful) / 1000.0,
               100.0 * total_verify / total_total);
        printf("  ─────────────────────────────────────────────────────────\n");
        printf("  Total end-to-end:     %7lu ns (%7.2f µs) [100.0%%]\n\n", 
               total_total / successful, (total_total / successful) / 1000.0);
        
        printf("MIN/MAX:\n");
        printf("  Host memcpy:  %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_memcpy, max_memcpy, min_memcpy / 1000.0, max_memcpy / 1000.0);
        printf("  Round-trip:   %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_roundtrip, max_roundtrip, min_roundtrip / 1000.0, max_roundtrip / 1000.0);
        printf("  Notification: %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_notification, max_notification, min_notification / 1000.0, max_notification / 1000.0);
        printf("  Guest memcpy: %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_guest_copy, max_guest_copy, min_guest_copy / 1000.0, max_guest_copy / 1000.0);
        printf("  Verify:       %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_verify, max_verify, min_verify / 1000.0, max_verify / 1000.0);
        
        printf("\nNote: Notification time is estimated as (round-trip - guest_total)\n");
        printf("      Includes polling delay and state machine overhead\n");
        printf("      SHA256 verification is for testing only, not part of real transmission\n");
    } else {
        printf("\nNo successful measurements. Is the guest program running?\n");
    }
    
    // Cleanup
    free(test_frame);
    csv_close(csv);
    csv_close(perf_csv);
    
    if (perf_available) {
        perf_counters_cleanup(&perf_counters);
    }
}

void test_bandwidth(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Bandwidth Test - Measuring Actual Memory Copy Bandwidth ===\n");
    printf("Host: memcpy to shared memory | Guest: memcpy from shared memory\n");
    printf("(Data generation and SHA256 done outside measurement)\n\n");
    
    // Initialize performance counters for bandwidth test
    struct perf_counters perf_counters;
    bool perf_available = perf_counters_init(&perf_counters);
    if (perf_available) {
        printf("✓ Hardware performance counters initialized for bandwidth test\n");
    } else {
        printf("⚠ Hardware performance counters not available for bandwidth test\n");
    }
    printf("\n");
    
    // Calculate available buffer size
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Test different frame sizes
    struct {
        int width, height, bpp;
        const char *name;
    } test_frames[] = {
        {1920, 1080, 3, "1080p"},
        {2560, 1440, 3, "1440p"},
        {3840, 2160, 3, "4K"},
        {0, 0, 0, NULL}
    };
    
    // Create CSV loggers - separate files for timing and performance metrics
    csv_logger_t *csv = csv_create("bandwidth_results.csv", 
        "iteration,frame_type,width,height,bpp,size_bytes,size_mb,host_memcpy_ns,host_memcpy_ms,host_memcpy_mbps,roundtrip_ns,roundtrip_ms,guest_memcpy_ns,guest_memcpy_ms,guest_memcpy_mbps,guest_verify_ns,guest_verify_ms,total_ns,total_ms,total_mbps,success");
    
    csv_logger_t *perf_csv = csv_create("bandwidth_performance.csv",
        "iteration,frame_type,host_l1_cache_misses,host_l1_cache_references,host_l1_miss_rate,host_llc_misses,host_llc_references,host_llc_miss_rate,host_tlb_misses,host_cpu_cycles,host_instructions,host_ipc,host_cycles_per_byte,host_context_switches,guest_l1_cache_misses,guest_l1_cache_references,guest_l1_miss_rate,guest_llc_misses,guest_llc_references,guest_llc_miss_rate,guest_tlb_misses,guest_cpu_cycles,guest_instructions,guest_ipc,guest_cycles_per_byte,guest_context_switches");
    
    for (int frame_idx = 0; test_frames[frame_idx].name != NULL; frame_idx++) {
        int width = test_frames[frame_idx].width;
        int height = test_frames[frame_idx].height;
        int bpp = test_frames[frame_idx].bpp;
        size_t frame_size = width * height * bpp;
        
        if (frame_size > max_data_size) {
            printf("Skipping %s (%dx%d): frame too large\n", 
                   test_frames[frame_idx].name, width, height);
            continue;
        }
        
        printf("\n--- Testing %s (%dx%d, %.2f MB) ---\n", 
               test_frames[frame_idx].name, width, height, frame_size / (1024.0 * 1024.0));
        
        // PRE-GENERATE test data for this frame size
        printf("Pre-generating test frame...\n");
        uint8_t *test_frame = malloc(frame_size);
        if (!test_frame) {
            printf("ERROR: Failed to allocate test frame\n");
            continue;
        }
        
        generate_random_frame(test_frame, width, height);

	if (memchr(test_frame,255,frame_size) != NULL) {
            printf("ERROR: 255 found on frame\n");
	    exit(1);
	 }
	//char *char_ptr = (char *)test_frame;
	//while ((char_ptr = (char*)(memchr(char_ptr,255,frame_size-((char *)test_frame-char_ptr)))) != NULL) {
	//    *char_ptr = 0;
//	}
        
        uint8_t expected_hash[32];
        calculate_sha256(test_frame, frame_size, expected_hash);
	print_hash_data(expected_hash);

        double total_host_bw = 0.0, total_guest_bw = 0.0, total_overall_bw = 0.0;
        int successful = 0;
        
        for (int iter = 0; iter < iterations; iter++) {
            if (iter > 0) usleep(10000);
            
            // Clear timing
            memset((void *)&shm->timing, 0, sizeof(struct timing_data));
            shm->error_code = 0;
	    invalidate_data_cache((char *)&shm->timing,(char *)&shm->timing+sizeof(struct timing_data));
            __sync_synchronize();
            
            uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
            
            // Prepare headers BEFORE timing
            shm->sequence = 0xFFFF + iter;
            shm->data_size = frame_size;
            memcpy((void *)shm->data_sha256, expected_hash, 32);
	    invalidate_data_cache((char *)&shm->data_sha256,(char *)&shm->data_sha256+32);
            __sync_synchronize();
            
            // MEASURE: Host memcpy bandwidth + performance counters
            struct perf_results host_perf_results = {0};
            
            // Start performance counters
            if (perf_available) {
                perf_counters_start(&perf_counters);
            }
            
            uint64_t memcpy_start = get_time_ns();
            memcpy((void*)data_ptr, test_frame, frame_size);
	    invalidate_data_cache((char *)data_ptr,(char *)data_ptr+frame_size);
            __sync_synchronize();
            uint64_t memcpy_end = get_time_ns();
            
            // Stop performance counters and collect results
            if (perf_available) {
                perf_counters_stop(&perf_counters, &host_perf_results, frame_size);
            }
            
            // MEASURE: Round-trip time
            uint64_t roundtrip_start = get_time_ns();
            set_host_state(shm, HOST_STATE_SENDING);
            
            if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 20*SECOND_NS, "guest processing")) {
                printf("  [%d] TIMEOUT\n", iter + 1);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                if (perf_csv && perf_csv->file) {
                    fprintf(perf_csv->file, "%d,%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name);
                }
                continue;
            }
            
            if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 20*SECOND_NS, "guest acknowledged")) {
                printf("  [%d] TIMEOUT (processing)\n", iter + 1);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                if (perf_csv && perf_csv->file) {
                    fprintf(perf_csv->file, "%d,%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name);
                }
                continue;
            }
            
            uint64_t roundtrip_end = get_time_ns();
            
            if (shm->error_code != 0) {
                printf("  [%d] FAILED (error: %u)\n", iter + 1, shm->error_code);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                if (perf_csv && perf_csv->file) {
                    fprintf(perf_csv->file, "%d,%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name);
                }
                continue;
            }
            
            // Calculate times and bandwidths
            uint64_t host_memcpy_time = memcpy_end - memcpy_start;
            uint64_t roundtrip_time = roundtrip_end - roundtrip_start;
            uint64_t guest_memcpy_time = shm->timing.guest_copy_duration;
            uint64_t guest_verify_time = shm->timing.guest_verify_duration;
            
            // Read new detailed cache behavior measurements
            uint64_t guest_hot_cache_time = shm->timing.guest_hot_cache_duration;
            uint64_t guest_cold_cache_time = shm->timing.guest_cold_cache_duration;
            uint64_t guest_second_pass_time = shm->timing.guest_second_pass_duration;
            uint64_t guest_cached_verify_time = shm->timing.guest_cached_verify_duration;
            uint64_t total_time = host_memcpy_time + roundtrip_time;
            
            double size_mb = frame_size / (1024.0 * 1024.0);
            double host_bw = size_mb / (host_memcpy_time / 1e9);
            double guest_bw = size_mb / (guest_memcpy_time / 1e9);
            double total_bw = size_mb / (total_time / 1e9);
            
            total_host_bw += host_bw;
            total_guest_bw += guest_bw;
            total_overall_bw += total_bw;
            successful++;
            
            printf("  [%d] Host: %.0f MB/s | Guest: %.0f MB/s | Verify: %.1f ms | Total: %.0f MB/s\n",
                   iter + 1, host_bw, guest_bw, guest_verify_time / 1000000.0, total_bw);
            
            // Extract guest performance metrics
            double guest_l1_miss_rate = shm->timing.guest_perf.l1_cache_miss_rate_x10000 / 10000.0;
            double guest_llc_miss_rate = shm->timing.guest_perf.llc_cache_miss_rate_x10000 / 10000.0;
            double guest_ipc = shm->timing.guest_perf.instructions_per_cycle_x10000 / 10000.0;
            double guest_cycles_per_byte = shm->timing.guest_perf.cycles_per_byte_x10000 / 10000.0;
            
            // Write to main bandwidth CSV
            csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                     width, height, 24, frame_size, 
                                     host_memcpy_time, roundtrip_time, 
                                     guest_memcpy_time, guest_verify_time, true);
            
            // Write to bandwidth performance CSV
            if (perf_csv && perf_csv->file) {
                fprintf(perf_csv->file, "%d,%s,%lu,%lu,%.4f,%lu,%lu,%.4f,%lu,%lu,%lu,%.2f,%.2f,%lu,%lu,%lu,%.4f,%lu,%lu,%.4f,%lu,%lu,%lu,%.2f,%.2f,%lu\n",
                        iter + 1, test_frames[frame_idx].name,
                        // Host performance metrics
                        host_perf_results.l1_cache_misses, host_perf_results.l1_cache_references, host_perf_results.l1_cache_miss_rate,
                        host_perf_results.llc_misses, host_perf_results.llc_references, host_perf_results.llc_cache_miss_rate,
                        host_perf_results.tlb_misses, host_perf_results.cpu_cycles, host_perf_results.instructions,
                        host_perf_results.instructions_per_cycle, host_perf_results.cycles_per_byte, host_perf_results.context_switches,
                        // Guest performance metrics
                        shm->timing.guest_perf.l1_cache_misses, shm->timing.guest_perf.l1_cache_references, guest_l1_miss_rate,
                        shm->timing.guest_perf.llc_misses, shm->timing.guest_perf.llc_references, guest_llc_miss_rate,
                        shm->timing.guest_perf.tlb_misses, shm->timing.guest_perf.cpu_cycles, shm->timing.guest_perf.instructions,
                        guest_ipc, guest_cycles_per_byte, shm->timing.guest_perf.context_switches);
            }
            
            set_host_state(shm, HOST_STATE_READY);
            
            if (!wait_for_guest_state(shm, GUEST_STATE_READY, SECOND_NS, "guest ready")) {
                printf("  WARNING: Guest didn't return to ready\n");
            }
            
            usleep(100000);
        }
        
        if (successful > 0) {
            printf("\n  %s Results (%d/%d successful):\n", test_frames[frame_idx].name, successful, iterations);
            printf("    Avg Host memcpy BW:   %.0f MB/s (%.2f GB/s)\n", 
                   total_host_bw / successful, (total_host_bw / successful) / 1024.0);
            printf("    Avg Guest memcpy BW:  %.0f MB/s (%.2f GB/s)\n", 
                   total_guest_bw / successful, (total_guest_bw / successful) / 1024.0);
            printf("    Avg Overall BW:       %.0f MB/s (%.2f GB/s)\n", 
                   total_overall_bw / successful, (total_overall_bw / successful) / 1024.0);
        }
        
        free(test_frame);
    }
    
    csv_close(csv);
    csv_close(perf_csv);
    
    if (perf_available) {
        perf_counters_cleanup(&perf_counters);
    }
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -l, --latency [COUNT]     Run latency test (default: 100 messages)\n");
    printf("  -b, --bandwidth [COUNT]   Run bandwidth test (default: 10 iterations)\n");
    printf("  -c, --count COUNT         Number of messages/iterations\n");
    printf("  -h, --help               Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -l 1                  Send single latency message\n", prog_name);
    printf("  %s -l 100                Send 100 latency messages\n", prog_name);
    printf("  %s -b 5                  Run 5 bandwidth iterations\n", prog_name);
    printf("  %s -l -b                 Run both tests with defaults\n", prog_name);
}

void init_shared_memory(volatile struct shared_data *shm) {
    printf("HOST: Starting initialization...\n");
    
    guest_state_t guest_state = get_guest_state(shm);
    if (guest_state != GUEST_STATE_UNINITIALIZED) {
        printf("HOST: Detected guest started first (state: %s), clearing...\n", 
               guest_state_name(guest_state));
    }
    
    shm->magic = 0;
    set_host_state(shm, HOST_STATE_INITIALIZING);
    
    shm->sequence = 0;
    shm->data_size = 0;
    shm->error_code = 0;
    shm->test_complete = 0;
    memset((void*)shm->data_sha256, 0, 32);
    memset((void*)&shm->timing, 0, sizeof(struct timing_data));
    invalidate_data_cache((char *)&shm,(char *)&shm+sizeof(shm));
    __sync_synchronize();
    
    shm->magic = MAGIC;
    set_host_state(shm, HOST_STATE_READY);
    
    printf("HOST: Initialization complete - waiting for guest...\n");
    
    if (!wait_for_guest_state(shm, GUEST_STATE_READY, 100*SECOND_NS, "guest ready")) {
        printf("HOST: WARNING - Guest not ready within 100 seconds\n");
        printf("HOST: Current guest state: %s\n", guest_state_name(get_guest_state(shm)));
        printf("HOST: Proceeding anyway...\n");
    } else {
        printf("HOST: ✓ Guest ready - synchronization complete\n");
    }
}

int main(int argc, char *argv[])
{
    bool run_latency = false;
    bool run_bandwidth = false;
    int latency_count = 100;
    int bandwidth_count = 10;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            run_latency = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                latency_count = atoi(argv[++i]);
                if (latency_count <= 0) latency_count = 1;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            run_bandwidth = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                bandwidth_count = atoi(argv[++i]);
                if (bandwidth_count <= 0) bandwidth_count = 1;
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            if (i + 1 < argc) {
                int count = atoi(argv[++i]);
                if (count > 0) {
                    latency_count = count;
                    bandwidth_count = count;
                }
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (!run_latency && !run_bandwidth) {
        run_latency = true;
        run_bandwidth = true;
    }
    
    printf("Host Writer - ivshmem Performance Test with Overhead Analysis\n");
    printf("=============================================================\n\n");
    printf("Opening file %s\n",SHMEM_PATH);
    
    int fd = open(SHMEM_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open shared memory");
        printf("Make sure the VM setup script has been run.\n");
        return 1;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("Shared memory: %s (%ld bytes)\n", SHMEM_PATH, st.st_size);
    
    void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    volatile struct shared_data *shm = (volatile struct shared_data *)((char *)ptr + SHM_ALIGNMENT);
    
    printf("Mapped at address: %p\n", ptr);
    printf("Data buffer size: %zu bytes\n", 
           st.st_size - offsetof(struct shared_data, buffer));
    
    printf("\nInitializing shared memory protocol...\n");
    init_shared_memory(shm);
    
    printf("\nMake sure the guest program is running!\n");
    printf("Press Enter to start tests...\n");
    getchar();
    
    if (run_latency) {
        test_latency(shm, latency_count);
    }
    
    if (run_bandwidth) {
        if (run_latency) {
            printf("\nPress Enter to run bandwidth test...\n");
            getchar();
        }
        test_bandwidth(shm, bandwidth_count);
    }
    
    set_host_state(shm, HOST_STATE_COMPLETED);
    shm->test_complete = 1;
    
    munmap(ptr, st.st_size);
    close(fd);
    
    printf("\nTests completed.\n");
    return 0;
}

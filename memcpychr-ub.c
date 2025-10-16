#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define SIZE_64MB (64 * 1024 * 1024 * 16) // 1Gb
#define ITERATIONS 10
#define CACHE_BUSTER_SIZE (128 * 1024 * 1024)  // 128MB to flush caches

// Platform-specific cycle counter reading
// #if defined(__x86_64__) || defined(__i386__)
//     // x86/x86_64: Use RDTSC instruction
//     static inline uint64_t read_cycles(void) {
//         uint32_t lo, hi;
//         __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
//         return ((uint64_t)hi << 32) | lo;
//     }
//     #define HAS_CYCLE_COUNTER 1
//     #define ARCH_NAME "x86_64"

// #elif defined(__aarch64__) || defined(__arm__)
//     // ARM64: Use PMCCNTR_EL0 (requires kernel config)
    // #ifdef __aarch64__
    //     static inline uint64_t read_cycles(void) {
    //         uint64_t val;
    //         __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(val));
    //         return val;
    //     }
    //     #define HAS_CYCLE_COUNTER 1
    //     #define ARCH_NAME "ARM64"
    // #else
    //     // ARM32: Use PMCCNTR
    //     static inline uint64_t read_cycles(void) {
    //         uint32_t val;
    //         __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(val));
    //         return val;
    //     }
    //     #define HAS_CYCLE_COUNTER 1
    //     #define ARCH_NAME "ARM32"
    // #endif

// #else
    // Fallback: no cycle counter available
    #define HAS_CYCLE_COUNTER 0
    #define ARCH_NAME "Unknown"
    static inline uint64_t read_cycles(void) {
        // Monotonic clock fallback
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)(ts.tv_sec * 1e9 + ts.tv_nsec); // nanoseconds
    }
// #endif

// Fallback timing using clock
double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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

// Invalidate caches by using ARM CMO operations on a large buffer
static inline void flush_caches(unsigned char *cache_buster, size_t size) {
  invalidate_data_cache((char*)cache_buster, (char*)(cache_buster + size));
}

int main() {
    unsigned char *src_buffer;
    unsigned char *dst_buffer;
    unsigned char *cache_buster;
    unsigned char *read_buffer;
    unsigned char *result;
    uint64_t start_cycles, end_cycles;
    double start_time, end_time;
    uint64_t memcpy_cycles = 0, memchr_cycles = 0, fread_cycles = 0;
    double memcpy_time = 0.0, memchr_time = 0.0, fread_time = 0.0;
    
    printf("Memory Benchmark: memcpy vs memchr vs fread (64MB)\n");
    printf("===================================================\n");
    printf("Architecture: %s\n", ARCH_NAME);
    printf("PMU Cycle Counter: %s\n\n", HAS_CYCLE_COUNTER ? "Available" : "Not Available (using clock)");
    
    #if HAS_CYCLE_COUNTER && (defined(__aarch64__) || defined(__arm__))
    printf("NOTE: On ARM/ARM64, if you get incorrect results, you may need to enable\n");
    printf("      user-space access to PMU counters:\n");
    printf("      - Linux: echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid\n");
    printf("      - Or run this program with sudo\n\n");
    #endif
    
    printf("Allocating buffers...\n");
    
    // Allocate source buffer (64MB + 1 for terminating 0)
    src_buffer = (unsigned char *)malloc(SIZE_64MB + 1);
    if (!src_buffer) {
        fprintf(stderr, "Failed to allocate source buffer\n");
        return 1;
    }
    
    // Allocate destination buffer for memcpy
    dst_buffer = (unsigned char *)malloc(SIZE_64MB);
    if (!dst_buffer) {
        fprintf(stderr, "Failed to allocate destination buffer\n");
        free(src_buffer);
        return 1;
    }
    
    // Allocate read buffer for fread test
    read_buffer = (unsigned char *)malloc(SIZE_64MB);
    if (!read_buffer) {
        fprintf(stderr, "Failed to allocate read buffer\n");
        free(src_buffer);
        free(dst_buffer);
        return 1;
    }
    
    // Allocate cache buster buffer (128MB to exceed typical L3 cache)
    cache_buster = (unsigned char *)malloc(CACHE_BUSTER_SIZE);
    if (!cache_buster) {
        fprintf(stderr, "Failed to allocate cache buster buffer\n");
        free(src_buffer);
        free(dst_buffer);
        free(read_buffer);
        return 1;
    }
    
    // Initialize cache buster with random data
    printf("Initializing cache invalidation buffer (128MB)...\n");
    for (size_t i = 0; i < CACHE_BUSTER_SIZE; i += 4096) {
        cache_buster[i] = (unsigned char)rand();
    }
    
    printf("Generating 64MB of random non-zero data...\n");
    srand((unsigned int)time(NULL));
    
    // Fill buffer with non-zero random data
    for (size_t i = 0; i < SIZE_64MB; i++) {
        // Generate random byte, ensure it's not 0
        do {
            src_buffer[i] = (unsigned char)(rand() % 256);
        } while (src_buffer[i] == 0);
    }
    
    // Terminate with 0 so memchr has to scan all 64MB
    src_buffer[SIZE_64MB] = 0;
    printf("Buffer prepared (64MB non-zero data + 1 zero terminator)\n");
    printf("Cache invalidation: Using 128MB buffer to flush L1/L2/L3 caches between tests\n");
    
    printf("\nRunning benchmarks with %d iterations each...\n", ITERATIONS);
    printf("===================================================\n\n");
    
    // Benchmark memcpy
    printf("Testing memcpy (copying 64MB)...\n");
    for (int i = 0; i < ITERATIONS; i++) {
        // Invalidate caches before each test
        flush_caches(cache_buster, CACHE_BUSTER_SIZE);
        
        start_time = get_time_sec();
        start_cycles = read_cycles();
        memcpy(dst_buffer, src_buffer, SIZE_64MB);
        end_cycles = read_cycles();
        end_time = get_time_sec();
        printf("  Buffer content %d: ", dst_buffer[0]);

        double iteration_time = end_time - start_time;
        uint64_t iteration_cycles = end_cycles - start_cycles;
        memcpy_time += iteration_time;
        memcpy_cycles += iteration_cycles;
        
        #if HAS_CYCLE_COUNTER
        printf("  Iteration %2d: %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
               i + 1, iteration_cycles, iteration_time,
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time,
               (double)iteration_cycles / SIZE_64MB);
        #else
        printf("  Iteration %2d: %.6f seconds (%.2f MB/s)\n", 
               i + 1, iteration_time, 
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time);
        #endif
    }
    memcpy_time /= ITERATIONS;
    memcpy_cycles /= ITERATIONS;
    
    printf("\nTesting memchr (searching for 0 in 64MB+1)...\n");
    for (int i = 0; i < ITERATIONS; i++) {
        // Invalidate caches before each test
        flush_caches(cache_buster, CACHE_BUSTER_SIZE);
        
        start_time = get_time_sec();
        start_cycles = read_cycles();
        result = (unsigned char *)memchr(src_buffer, 0, SIZE_64MB + 1);
        end_cycles = read_cycles();
        end_time = get_time_sec();
        
        double iteration_time = end_time - start_time;
        uint64_t iteration_cycles = end_cycles - start_cycles;
        memchr_time += iteration_time;
        memchr_cycles += iteration_cycles;
        
        #if HAS_CYCLE_COUNTER
        printf("  Iteration %2d: %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
               i + 1, iteration_cycles, iteration_time,
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time,
               (double)iteration_cycles / SIZE_64MB);
        #else
        printf("  Iteration %2d: %.6f seconds (%.2f MB/s)\n", 
               i + 1, iteration_time,
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time);
        #endif
        
        // Verify result
        if (result != &src_buffer[SIZE_64MB]) {
            fprintf(stderr, "ERROR: memchr found wrong position!\n");
            fprintf(stderr, "Expected: %p, Got: %p\n", 
                    (void*)&src_buffer[SIZE_64MB], (void*)result);
        }
    }
    memchr_time /= ITERATIONS;
    memchr_cycles /= ITERATIONS;
    
    printf("\nTesting fread (stream read of 64MB)...\n");
    for (int i = 0; i < ITERATIONS; i++) {
        // Invalidate caches before each test
        flush_caches(cache_buster, CACHE_BUSTER_SIZE);
        
        // Create a memory stream from the buffer
        FILE *memstream = fmemopen(src_buffer, SIZE_64MB, "rb");
        if (!memstream) {
            fprintf(stderr, "Failed to create memory stream\n");
            continue;
        }
        
        start_time = get_time_sec();
        start_cycles = read_cycles();
        size_t bytes_read = fread(read_buffer, 1, SIZE_64MB, memstream);
        end_cycles = read_cycles();
        end_time = get_time_sec();
        
        double iteration_time = end_time - start_time;
        uint64_t iteration_cycles = end_cycles - start_cycles;
        fread_time += iteration_time;
        fread_cycles += iteration_cycles;
        
        #if HAS_CYCLE_COUNTER
        printf("  Iteration %2d: %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
               i + 1, iteration_cycles, iteration_time,
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time,
               (double)iteration_cycles / SIZE_64MB);
        #else
        printf("  Iteration %2d: %.6f seconds (%.2f MB/s)\n", 
               i + 1, iteration_time,
               SIZE_64MB / (1024.0 * 1024.0) / iteration_time);
        #endif
        
        fclose(memstream);
        
        // Verify that we read all bytes
        if (bytes_read != SIZE_64MB) {
            fprintf(stderr, "ERROR: fread read %zu bytes, expected %d\n", 
                    bytes_read, SIZE_64MB);
        }
    }
    fread_time /= ITERATIONS;
    fread_cycles /= ITERATIONS;
    
    // Print summary results
    printf("\n==========================================\n");
    printf("SUMMARY RESULTS (averages over %d iterations)\n", ITERATIONS);
    printf("==========================================\n");
    
    #if HAS_CYCLE_COUNTER
    // Estimate CPU frequency from one of the measurements
    double estimated_ghz = (double)memcpy_cycles / memcpy_time / 1e9;
    printf("Estimated CPU frequency: %.2f GHz\n", estimated_ghz);
    printf("------------------------------------------\n");
    printf("memcpy: %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
           memcpy_cycles, memcpy_time,
           SIZE_64MB / (1024.0 * 1024.0) / memcpy_time,
           (double)memcpy_cycles / SIZE_64MB);
    printf("memchr: %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
           memchr_cycles, memchr_time,
           SIZE_64MB / (1024.0 * 1024.0) / memchr_time,
           (double)memchr_cycles / SIZE_64MB);
    printf("fread:  %12lu cycles | %.6f sec | %.2f MB/s | %.2f cycles/byte\n", 
           fread_cycles, fread_time,
           SIZE_64MB / (1024.0 * 1024.0) / fread_time,
           (double)fread_cycles / SIZE_64MB);
    #else
    printf("memcpy: %.6f seconds | %.2f MB/s\n", 
           memcpy_time, SIZE_64MB / (1024.0 * 1024.0) / memcpy_time);
    printf("memchr: %.6f seconds | %.2f MB/s\n", 
           memchr_time, SIZE_64MB / (1024.0 * 1024.0) / memchr_time);
    printf("fread:  %.6f seconds | %.2f MB/s\n", 
           fread_time, SIZE_64MB / (1024.0 * 1024.0) / fread_time);
    #endif
    printf("------------------------------------------\n");
    
    // Find the fastest based on cycles if available, otherwise time
    #if HAS_CYCLE_COUNTER
    uint64_t fastest_cycles = memcpy_cycles;
    const char *fastest_name = "memcpy";
    
    if (memchr_cycles < fastest_cycles) {
        fastest_cycles = memchr_cycles;
        fastest_name = "memchr";
    }
    if (fread_cycles < fastest_cycles) {
        fastest_cycles = fread_cycles;
        fastest_name = "fread";
    }
    
    printf("Fastest: %s\n", fastest_name);
    printf("------------------------------------------\n");
    printf("Relative performance (based on cycles):\n");
    printf("  memcpy: %.2fx (%.1f%% %s)\n", 
           (double)memcpy_cycles / fastest_cycles,
           ((double)memcpy_cycles / fastest_cycles - 1) * 100,
           memcpy_cycles == fastest_cycles ? "baseline" : "slower");
    printf("  memchr: %.2fx (%.1f%% %s)\n", 
           (double)memchr_cycles / fastest_cycles,
           ((double)memchr_cycles / fastest_cycles - 1) * 100,
           memchr_cycles == fastest_cycles ? "baseline" : "slower");
    printf("  fread:  %.2fx (%.1f%% %s)\n", 
           (double)fread_cycles / fastest_cycles,
           ((double)fread_cycles / fastest_cycles - 1) * 100,
           fread_cycles == fastest_cycles ? "baseline" : "slower");
    #else
    double fastest_time = memcpy_time;
    const char *fastest_name = "memcpy";
    
    if (memchr_time < fastest_time) {
        fastest_time = memchr_time;
        fastest_name = "memchr";
    }
    if (fread_time < fastest_time) {
        fastest_time = fread_time;
        fastest_name = "fread";
    }
    
    printf("Fastest: %s\n", fastest_name);
    printf("------------------------------------------\n");
    printf("Relative performance (vs fastest):\n");
    printf("  memcpy: %.2fx (%.1f%% %s)\n", 
           memcpy_time / fastest_time,
           (memcpy_time / fastest_time - 1) * 100,
           memcpy_time == fastest_time ? "baseline" : "slower");
    printf("  memchr: %.2fx (%.1f%% %s)\n", 
           memchr_time / fastest_time,
           (memchr_time / fastest_time - 1) * 100,
           memchr_time == fastest_time ? "baseline" : "slower");
    printf("  fread:  %.2fx (%.1f%% %s)\n", 
           fread_time / fastest_time,
           (fread_time / fastest_time - 1) * 100,
           fread_time == fastest_time ? "baseline" : "slower");
    #endif
    printf("==========================================\n");
    
    // Cleanup
    free(src_buffer);
    free(dst_buffer);
    free(read_buffer);
    free(cache_buster);
    
    return 0;
}
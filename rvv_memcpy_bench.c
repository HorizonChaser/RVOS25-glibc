#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <stdint.h>
#include <stddef.h>

/**
 * 向量化复制内存, 与 memcpy 接口与行为一致
 */
void* memcpy_rvv_internal(void* dst, const void* src, size_t len) {
    void* original_dst = dst;
    uint8_t* dst_u8 = (uint8_t*)dst;
    const uint8_t* src_u8 = (const uint8_t*)src;
    size_t vlenb;

    // 获取硬件支持的单个向量寄存器的长度, 单位字节
    asm volatile("csrr %0, vlenb" : "=r"(vlenb));

    // 如果长度小于16字节，直接使用字节拷贝, 类似原始版本的 memcpy
    if (len < 16) {
        for (size_t i = 0; i < len; ++i)
            dst_u8[i] = src_u8[i];
        return original_dst;
    }

    // 确保两个指针都对齐到 8 字节
    size_t head_len = 0;
    if (((uintptr_t)dst_u8 % sizeof(unsigned long)) != 0) {
        head_len = sizeof(unsigned long) - ((uintptr_t)dst_u8 % sizeof(unsigned long));
        if (head_len > len)
            head_len = len;

        // 处理头部未对齐的字节
        for (size_t i = 0; i < head_len; ++i)
            dst_u8[i] = src_u8[i];
        dst_u8 += head_len;
        src_u8 += head_len;
        len -= head_len;
    }

    // 向量化, 启动
    if (len >= sizeof(unsigned long) && (((uintptr_t)src_u8 % sizeof(unsigned long)) == 0)) {
        unsigned long* dst_ul = (unsigned long*)dst_u8;
        const unsigned long* src_ul = (const unsigned long*)src_u8;
        size_t num_ul = len / sizeof(unsigned long);
        size_t current_vl_ul;
        if (num_ul > 0) {
            // 64-bit
            if (sizeof(unsigned long) == 8) {
                asm volatile(
                    "1:\n" //循环开始
                    "vsetvli %[vl], %[len_in_elements], e64, m8, ta, ma\n" //设置vl
                    "beqz %[vl], 2f\n" //如果 vl == 0, 跳到 2, 也就是结束
                    "vle64.v v0, (%[src_ptr])\n" // 从 src 读入数据到 v0
                    "vse64.v v0, (%[dst_ptr])\n" // 从 v0 写入数据到 dst
                    // 计算处理的字节数
                    "slli %[processed_bytes], %[vl], 3\n"
                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
                    "j 1b\n"
                    "2:\n"
                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
                    : [processed_bytes] "r"(0)
                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            } else {
                asm volatile(
                    "1:\n"
                    "vsetvli %[vl], %[len_in_elements], e32, m8, ta, ma\n"
                    "beqz %[vl], 2f\n"
                    "vle32.v v0, (%[src_ptr])\n"
                    "vse32.v v0, (%[dst_ptr])\n"
                    "slli %[processed_bytes], %[vl], 2\n"
                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
                    "j 1b\n"
                    "2:\n"
                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
                    : [processed_bytes] "r"(0)
                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            }
            size_t bytes_processed_ul = (len / sizeof(unsigned long) - num_ul) * sizeof(unsigned long);
            dst_u8 = (uint8_t*)dst_ul;
            src_u8 = (const uint8_t*)src_ul;
            len -= bytes_processed_ul;
        }
    }
    // 如果还有剩余的字节，使用字节向量化拷贝
    if (len > 0) {
        size_t current_vl_u8;
        uint8_t* dst_u8_asm = dst_u8;
        const uint8_t* src_u8_asm = src_u8;
        size_t len_asm = len;
        asm volatile(
            "1:\n"
            "vsetvli %[vl], %[rem_len], e8, m8, ta, ma\n"
            "beqz %[vl], 2f\n"
            "vle8.v v0, (%[src_ptr])\n"
            "vse8.v v0, (%[dst_ptr])\n"
            "add %[src_ptr], %[src_ptr], %[vl]\n"
            "add %[dst_ptr], %[dst_ptr], %[vl]\n"
            "sub %[rem_len], %[rem_len], %[vl]\n"
            "j 1b\n"
            "2:\n"
            : [vl] "=&r"(current_vl_u8), [rem_len] "+&r"(len_asm),
            [src_ptr] "+&r"(src_u8_asm), [dst_ptr] "+&r"(dst_u8_asm)
            :
            : "t0", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
    }
    return original_dst;
}

/**
 * 使用长度为单字节的向量指令复制内存
 */
void* memcpy_rvv_bytes_only(void* dst, const void* src, size_t len) {
    void* original_dst = dst;
    uint8_t* dst_u8 = (uint8_t*)dst;
    const uint8_t* src_u8 = (const uint8_t*)src;
    size_t current_vl;
    size_t n = len;

    if (n == 0) return original_dst;

    asm volatile (
        "vsetvli %[vl], %[rem_len], e8, m8, ta, ma\n"

        "1:\n"
        "beqz %[vl], 2f\n"
        "vle8.v v0, (%[src_ptr])\n"
        "vse8.v v0, (%[dst_ptr])\n"
        "add %[src_ptr], %[src_ptr], %[vl]\n"
        "add %[dst_ptr], %[dst_ptr], %[vl]\n"
        "sub %[rem_len], %[rem_len], %[vl]\n"
        "j 1b\n"
        "2:\n"
        : [vl] "=&r" (current_vl),
        [rem_len] "+&r" (n),
        [src_ptr] "+&r" (src_u8),
        [dst_ptr] "+&r" (dst_u8)
        :
        : "t0", "memory",
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"
        );
    return original_dst;
}

/**
 * 获取 CPU 周期计数的辅助函数, 因为怀疑 qemu 不适合使用直接的时间测量
 */
static inline unsigned long long get_cpu_cycles() {
    unsigned long long cycles;

// 对于 RV32，rdcycle 只读取 cycle 的低32位，需要配合 rdcycleh
// 对于 RV64，rdcycle 直接读取完整的64位 cycle 计数器
#if __riscv_xlen == 64
    asm volatile ("rdcycle %0" : "=r" (cycles));
#elif __riscv_xlen == 32
    unsigned int cycles_lo, cycles_hi0, cycles_hi1;
    // Loop to ensure correct reading of 64-bit counter on 32-bit arch
    // This is a workaround for the fact that rdcycleh may not be atomic
    do {
        asm volatile ("rdcycleh %0" : "=r" (cycles_hi0));
        asm volatile ("rdcycle  %0" : "=r" (cycles_lo));
        asm volatile ("rdcycleh %0" : "=r" (cycles_hi1));
    } while (cycles_hi0 != cycles_hi1);
    cycles = ((unsigned long long)cycles_hi0 << 32) | cycles_lo;
#else
#error "Unknown XLEN for rdcycle"
#endif
    return cycles;
}

// 测试参数
#define MAX_BUFFER_SIZE (16 * 1024 * 1024)
#define NUM_REPEATS 100
#define NUM_OUTER_LOOPS 5

const size_t test_sizes[] = {
    1, 16, 31, 32, 63, 64, 127, 128, 255, 256,
    511, 512, 1023, 1024, 2048, 4095, 4096,
    8192, 16384, 32768, 65536,
    128 * 1024, 256 * 1024, 512 * 1024,
    1 * 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024,
};
const int num_test_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

uint8_t* src_buffer;
uint8_t* dst_buffer_std;
uint8_t* dst_buffer_rvv;

/**
 * 对齐指针到指定的字节边界
 * @param ptr 指针
 * @param alignment 对齐字节数
 * @return 对齐后的指针
 */
void* align_ptr(void* ptr, size_t alignment) {
    return (void*)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));
}

int main() {
    size_t alignment = 64;
    size_t alloc_size = MAX_BUFFER_SIZE + 2 * alignment;

    uint8_t* raw_src_buffer = (uint8_t*)malloc(alloc_size);
    uint8_t* raw_dst_buffer_std = (uint8_t*)malloc(alloc_size);
    uint8_t* raw_dst_buffer_rvv = (uint8_t*)malloc(alloc_size);

    if (!raw_src_buffer || !raw_dst_buffer_std || !raw_dst_buffer_rvv) {
        perror("Failed to allocate buffers");
        return 1;
    }

    src_buffer = align_ptr(raw_src_buffer, alignment);
    dst_buffer_std = align_ptr(raw_dst_buffer_std, alignment);
    dst_buffer_rvv = align_ptr(raw_dst_buffer_rvv, alignment);

    // 填充数据
    for (size_t i = 0; i < MAX_BUFFER_SIZE; ++i) {
        src_buffer[i] = (uint8_t)(i % 251);
    }

    printf("Size (bytes)\tmemcpy_std (cycles)\tmemcpy_rvv (cycles)\tSpeedup (std/rvv)\tCorrect_std\tCorrect_rvv\n");
    printf("------------\t-------------------\t-------------------\t-----------------\t-----------\t-----------\n");

    for (int i = 0; i < num_test_sizes; ++i) {
        size_t current_size = test_sizes[i];
        if (current_size == 0) continue;
        if (current_size > MAX_BUFFER_SIZE) continue;

        long long cycles_std_min = -1, cycles_rvv_min = -1;
        int correct_std = 1, correct_rvv = 1;

        int current_repeats = NUM_REPEATS;
        if (current_size > 256 * 1024) {
            current_repeats = (current_size > 1024 * 1024) ? 10 : 30;
        }


        for (int outer_loop = 0; outer_loop < NUM_OUTER_LOOPS; ++outer_loop) {
            // 常规版本的 memcpy
            // 先清空目标缓冲区
            memset(dst_buffer_std, 0xA5, MAX_BUFFER_SIZE);
            unsigned long long start_cycles = get_cpu_cycles();
            for (int k = 0; k < current_repeats; ++k) {
                memcpy((void*)dst_buffer_std, (const void*)src_buffer, current_size);
            }
            unsigned long long end_cycles = get_cpu_cycles();
            long long elapsed_std = (end_cycles - start_cycles) / current_repeats;
            if (cycles_std_min == -1 || elapsed_std < cycles_std_min) {
                cycles_std_min = elapsed_std;
            }
            // 检查 memcpy 的正确性
            if (memcmp(dst_buffer_std, src_buffer, current_size) != 0) {
                correct_std = 0;
            }

            // RVV 版本的 memcpy
            memset(dst_buffer_rvv, 0x5A, MAX_BUFFER_SIZE);
            start_cycles = get_cpu_cycles();
            for (int k = 0; k < current_repeats; ++k) {
                memcpy_rvv_internal((void*)dst_buffer_rvv, (const void*)src_buffer, current_size);
            }
            end_cycles = get_cpu_cycles();
            long long elapsed_rvv = (end_cycles - start_cycles) / current_repeats;
            if (cycles_rvv_min == -1 || elapsed_rvv < cycles_rvv_min) {
                cycles_rvv_min = elapsed_rvv;
            }
            // 检查 RVV memcpy 的正确性
            if (memcmp(dst_buffer_rvv, src_buffer, current_size) != 0) {
                correct_rvv = 0;
            }
        }
        double speedup = (cycles_rvv_min > 0 && cycles_std_min > 0) ? (double)cycles_std_min / cycles_rvv_min : 0.0;
        printf("%-12zu\t%-19lld\t%-19lld\t%-17.2f\t%-11s\t%-11s\n",
            current_size, cycles_std_min, cycles_rvv_min, speedup,
            correct_std ? "OK" : "FAIL", correct_rvv ? "OK" : "FAIL");

        if (!correct_std || !correct_rvv) {
            fprintf(stderr, "ERROR: Correctness check failed for size %zu. Aborting.\n", current_size);
            break;
        }
    }

    // 释放内存, 虽然在程序结束时会自动释放但是还是手动释放一下吧
    free(raw_src_buffer);
    free(raw_dst_buffer_std);
    free(raw_dst_buffer_rvv);

    return 0;
}

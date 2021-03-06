
#include <immintrin.h> // AVX2
#include "../gemm_config.h"
#include "../gemm_driver.h"
#include "sgemm_pack.h"
#include <iostream>
#include <assert.h>

// https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html#AssemblerTemplate
// symbol name should append '%='(assembler template) to avoid duplicate label in inline asm
// such inline this function. support on gcc/clang

/***************************************************************************
 * packing for A
 *
 * we always look for the packing result of A as follow,
 * no matter input layout of A:
 *          (below layout is row major and dense, aka continuous in memory)
 * 
 *     MR
 *   +----+
 *   |    |
 *   |    | KC
 *   +----+
 *   |    |
 *   |    | KC
 *   +----+
 *   |    |
 *
 * if row major:
 *  for each MR*KC of A, prefer addressable by 1 TLB entry (1 PAGE_SIZE and alignment)
 * if col major:
 *  no need TLB align
 */

/*
*  sgemm_pack_n_a_n()
*
*        KC
*  +-------------+     ^
*  |             | MR  |
*  +-------------+     |
*  |             |
*  +-------------+     MC
*  |             |
*  +-------------+     |
*                      |
*                      v
*/
static void sgemm_pack_n_a_n_generic(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
    int k_itr = kc/8;
    int k_rem = kc%8;
    int k;
    int mr = ctx->mr;
    //int mc = ctx->mc;
    int m;
    //int page_size = ctx->page_size;

    //assert((mr*kc*sizeof(float) <= page_size) &&
    //    "mr*kc block must small than page_size, other not support");

    const float * s_ptr = src;
    float * d_ptr = dest;
    float v0, v1, v2, v3, v4, v5, v6, v7;
    for(m=0;m<mc;m+=mr){
        int mr_size = MIN(mc-m, mr);
        int mm;
        float * dcol = d_ptr;
        for(mm=0; mm<mr_size; mm+=1){
            
            float * dd = dcol;
            const float * ss = s_ptr;
            for(k=0;k<k_itr;k++){
                v0 = *ss;  ss++;
                v1 = *ss;  ss++;
                v2 = *ss;  ss++;
                v3 = *ss;  ss++;
                v4 = *ss;  ss++;
                v5 = *ss;  ss++;
                v6 = *ss;  ss++;
                v7 = *ss;  ss++;

                v0 *= alpha;
                v1 *= alpha;
                v2 *= alpha;
                v3 *= alpha;
                v4 *= alpha;
                v5 *= alpha;
                v6 *= alpha;
                v7 *= alpha;

                *dd = v0;  dd += mr_size;
                *dd = v1;  dd += mr_size;
                *dd = v2;  dd += mr_size;
                *dd = v3;  dd += mr_size;
                *dd = v4;  dd += mr_size;
                *dd = v5;  dd += mr_size;
                *dd = v6;  dd += mr_size;
                *dd = v7;  dd += mr_size;
            }
            for(k=0;k<k_rem;k++){
                v0 = *ss;  ss++;
                *dd = v0;  dd += mr_size;
            }
            s_ptr += ld;
            dcol ++;
        }
        //d_ptr += page_size/sizeof(float);
        d_ptr += mr*kc;
    }
}
#define PACK_A_MULTIPLE_ALPHA
static void sgemm_pack_n_a_n_mr16(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
    unsigned long long k_itr = kc/16;
    unsigned long long k_rem = kc%16;
    unsigned long long m_itr = mc/6;
    unsigned long long m_rem = mc%6;
    unsigned long long ld_ = ld;
#ifdef PACK_A_MULTIPLE_ALPHA
    float * alpha_addr = &alpha;
#endif

    asm volatile(
#ifdef PACK_A_MULTIPLE_ALPHA
    "movq               %7,             %%r9        \n" // alpha
    "vbroadcastss       (%%r9),         %%ymm15     \n"
    "vbroadcastss       (%%r9),         %%xmm14     \n"
#endif
    "movq               %0,             %%rax       \n" // src
    "movq               %1,             %%rbx       \n" // dest
    "movq               %2,             %%rcx       \n" // ld
    "movq               %3,             %%rsi       \n" // k_itr
    "movq               %4,             %%rdi       \n" // k_rem
    "movq               %5,             %%r8        \n" // m_itr
    "movq               %6,             %%r9        \n" // m_rem

    "shlq               $2,             %%rcx       \n" // ld*=4

    "testq              %%r8,           %%r8        \n"
    "je                 .LOOP_M_ITR_DONE%=          \n"

    ".LOOP_M_ITR%=:                                 \n"
    "testq          %%rsi,              %%rsi       \n" // test k_itr
    "je             .LOOP_K_ITR_DONE%=              \n"

    "movq           %%rsi,              %%rdx       \n" // restore k_itr
    "movq           %%rax,              %%r14       \n" // restore src

    "leaq           (%%rcx, %%rcx, 2),  %%r10       \n" // 3*rcx
    "leaq           (%%rcx, %%rcx, 4),  %%r11       \n" // 5*rcx

    ".LOOP_K_ITR%=:                                 \n"
    "prefetchnta    64(%%r14)                       \n"
    "prefetchnta    64(%%r14,%%rcx,1)               \n"
    "prefetchnta    64(%%r14,%%rcx,2)               \n"
    "prefetchnta    64(%%r14,%%r10,1)               \n"
    "prefetchnta    64(%%r14,%%rcx,4)               \n"
    "prefetchnta    64(%%r14,%%r11,1)               \n"

    "vmovups        (%%r14),  %%ymm0                \n" // row 0
    "vmovups        (%%r14,%%rcx,1),    %%ymm1      \n" // row 1
    "vmovups        (%%r14,%%rcx,2),    %%ymm2      \n" // row 2
    "vmovups        (%%r14,%%r10,1),    %%ymm3      \n" // row 3
    "vmovups        (%%r14,%%rcx,4),    %%ymm4      \n" // row 4
    "vmovups        (%%r14,%%r11,1),    %%ymm5      \n" // row 5
#ifdef PACK_A_MULTIPLE_ALPHA
    "vmulps         %%ymm15,  %%ymm0,   %%ymm0      \n" // multiple alpha
    "vmulps         %%ymm15,  %%ymm1,   %%ymm1      \n"
    "vmulps         %%ymm15,  %%ymm2,   %%ymm2      \n"
    "vmulps         %%ymm15,  %%ymm3,   %%ymm3      \n"
    "vmulps         %%ymm15,  %%ymm4,   %%ymm4      \n"
    "vmulps         %%ymm15,  %%ymm5,   %%ymm5      \n"
#endif
                                                    // lsb                         msb
                                                    // origin:
                                                    // X00 X01 X02 X03 X04 X05 X06 X07
                                                    // X10 X11 X12 X13 X14 X15 X16 X17
                                                    // X20 X21 X22 X23 X24 X25 X26 X27
                                                    // X30 X31 X32 X33 X34 X35 X36 X37
                                                    // X40 X41 X42 X43 X44 X45 X46 X47
                                                    // X50 X51 X52 X53 X54 X55 X56 X57
                                                    //
                                                    // final:
                                                    // X00 X10 X20 X30 X40 X50  
                                                    // X01 X11 X21 X31 X41 X51  
                                                    // X02 X12 X22 X32 X42 X52  
                                                    // X03 X13 X23 X33 X43 X53  
                                                    // X04 X14 X24 X34 X44 X54  
                                                    // X05 X15 X25 X35 X45 X55  
                                                    // X06 X16 X26 X36 X46 X56  
                                                    // X07 X17 X27 X37 X47 X57
                                                    //
                                                    // reorg:
                                                    // X00 X10 X20 X30 X40 X50 X01 X11
                                                    // X21 X31 X41 X51 X02 X12 X22 X32
                                                    // X42 X52 X03 X13 X23 X33 X43 X53
                                                    // X04 X14 X24 X34 X44 X54 X05 X15
                                                    // X25 X35 X45 X55 X06 X16 X26 X36
                                                    // X46 X56 X07 X17 X27 X37 X47 X57
    "vunpcklps      %%ymm1, %%ymm0, %%ymm6          \n"
    "vunpckhps      %%ymm1, %%ymm0, %%ymm7          \n"
    "vunpcklps      %%ymm3, %%ymm2, %%ymm8          \n"
    "vunpckhps      %%ymm3, %%ymm2, %%ymm9          \n"
    "vunpcklps      %%ymm5, %%ymm4, %%ymm10         \n"
    "vunpckhps      %%ymm5, %%ymm4, %%ymm11         \n"
                                                    // X00 X10 X01 X11 X04 X14 X05 X15
                                                    // X02 X12 X03 X13 X06 X16 X07 X17
                                                    // X20 X30 X21 X31 X24 X34 X25 X35
                                                    // X22 X32 X23 X33 X26 X36 X27 X37
                                                    // X40 X50 X41 X51 X44 X54 X45 X55
                                                    // X42 X52 X43 X53 X46 X56 X47 X57

    "vshufps        $0x44, %%ymm8,  %%ymm6,  %%ymm0 \n"
    "vshufps        $0xee, %%ymm10, %%ymm8,  %%ymm1 \n"
    "vshufps        $0xe4, %%ymm7,  %%ymm11, %%ymm2 \n"
    "vshufps        $0xe4, %%ymm6,  %%ymm10, %%ymm3 \n"
    "vshufps        $0x44, %%ymm9,  %%ymm7,  %%ymm4 \n"
    "vshufps        $0xee, %%ymm11, %%ymm9,  %%ymm5 \n"
                                                    // X00 X10 X20 X30 X04 X14 X24 X34
                                                    // X21 X31 X41 X51 X25 X35 X45 X55
                                                    // X42 X52 X03 X13 X46 X56 X07 X17
                                                    // X40 X50 X01 X11 X44 X54 X05 X15
                                                    // X02 X12 X22 X32 X06 X16 X26 X36
                                                    // X23 X33 X43 X53 X27 X37 X47 X57

    "vperm2f128     $0x20, %%ymm3, %%ymm0, %%ymm6   \n"
    "vperm2f128     $0x20, %%ymm4, %%ymm1, %%ymm7   \n"
    "vperm2f128     $0x20, %%ymm5, %%ymm2, %%ymm8   \n"
    "vperm2f128     $0x31, %%ymm3, %%ymm0, %%ymm9   \n"
    "vperm2f128     $0x31, %%ymm4, %%ymm1, %%ymm10  \n"
    "vperm2f128     $0x31, %%ymm5, %%ymm2, %%ymm11  \n"
                                                    // X00 X10 X20 X30 X40 X50 X01 X11
                                                    // X21 X31 X41 X51 X02 X12 X22 X32
                                                    // X42 X52 X03 X13 X23 X33 X43 X53
                                                    // X04 X14 X24 X34 X44 X54 X05 X15
                                                    // X25 X35 X45 X55 X06 X16 X26 X36
                                                    // X46 X56 X07 X17 X27 X37 X47 X57

    "vmovups        %%ymm6,         32*0(%%rbx)     \n"
    "vmovups        %%ymm7,         32*1(%%rbx)     \n"
    "vmovups        %%ymm8,         32*2(%%rbx)     \n"
    "vmovups        %%ymm9,         32*3(%%rbx)     \n"
    "vmovups        %%ymm10,        32*4(%%rbx)     \n"
    "vmovups        %%ymm11,        32*5(%%rbx)     \n"

    "addq           $8*4,           %%r14           \n" // src
    "addq           $6*8*4,         %%rbx           \n" // dest

    "vmovups        (%%r14),  %%ymm0                \n" // row 0
    "vmovups        (%%r14,%%rcx,1),    %%ymm1      \n" // row 1
    "vmovups        (%%r14,%%rcx,2),    %%ymm2      \n" // row 2
    "vmovups        (%%r14,%%r10,1),    %%ymm3      \n" // row 3
    "vmovups        (%%r14,%%rcx,4),    %%ymm4      \n" // row 4
    "vmovups        (%%r14,%%r11,1),    %%ymm5      \n" // row 5
#ifdef PACK_A_MULTIPLE_ALPHA
    "vmulps         %%ymm15,  %%ymm0,   %%ymm0      \n"  // multiple alpha
    "vmulps         %%ymm15,  %%ymm1,   %%ymm1      \n"
    "vmulps         %%ymm15,  %%ymm2,   %%ymm2      \n"
    "vmulps         %%ymm15,  %%ymm3,   %%ymm3      \n"
    "vmulps         %%ymm15,  %%ymm4,   %%ymm4      \n"
    "vmulps         %%ymm15,  %%ymm5,   %%ymm5      \n"
#endif
                                                    // lsb                         msb
                                                    // origin:
                                                    // X00 X01 X02 X03 X04 X05 X06 X07
                                                    // X10 X11 X12 X13 X14 X15 X16 X17
                                                    // X20 X21 X22 X23 X24 X25 X26 X27
                                                    // X30 X31 X32 X33 X34 X35 X36 X37
                                                    // X40 X41 X42 X43 X44 X45 X46 X47
                                                    // X50 X51 X52 X53 X54 X55 X56 X57
                                                    //
                                                    // final:
                                                    // X00 X10 X20 X30 X40 X50  
                                                    // X01 X11 X21 X31 X41 X51  
                                                    // X02 X12 X22 X32 X42 X52  
                                                    // X03 X13 X23 X33 X43 X53  
                                                    // X04 X14 X24 X34 X44 X54  
                                                    // X05 X15 X25 X35 X45 X55  
                                                    // X06 X16 X26 X36 X46 X56  
                                                    // X07 X17 X27 X37 X47 X57
                                                    //
                                                    // reorg:
                                                    // X00 X10 X20 X30 X40 X50 X01 X11
                                                    // X21 X31 X41 X51 X02 X12 X22 X32
                                                    // X42 X52 X03 X13 X23 X33 X43 X53
                                                    // X04 X14 X24 X34 X44 X54 X05 X15
                                                    // X25 X35 X45 X55 X06 X16 X26 X36
                                                    // X46 X56 X07 X17 X27 X37 X47 X57
    "vunpcklps      %%ymm1, %%ymm0, %%ymm6          \n"
    "vunpckhps      %%ymm1, %%ymm0, %%ymm7          \n"
    "vunpcklps      %%ymm3, %%ymm2, %%ymm8          \n"
    "vunpckhps      %%ymm3, %%ymm2, %%ymm9          \n"
    "vunpcklps      %%ymm5, %%ymm4, %%ymm10         \n"
    "vunpckhps      %%ymm5, %%ymm4, %%ymm11         \n"
                                                    // X00 X10 X01 X11 X04 X14 X05 X15
                                                    // X02 X12 X03 X13 X06 X16 X07 X17
                                                    // X20 X30 X21 X31 X24 X34 X25 X35
                                                    // X22 X32 X23 X33 X26 X36 X27 X37
                                                    // X40 X50 X41 X51 X44 X54 X45 X55
                                                    // X42 X52 X43 X53 X46 X56 X47 X57

    "vshufps        $0x44, %%ymm8,  %%ymm6,  %%ymm0 \n"
    "vshufps        $0xee, %%ymm10, %%ymm8,  %%ymm1 \n"
    "vshufps        $0xe4, %%ymm7,  %%ymm11, %%ymm2 \n"
    "vshufps        $0xe4, %%ymm6,  %%ymm10, %%ymm3 \n"
    "vshufps        $0x44, %%ymm9,  %%ymm7,  %%ymm4 \n"
    "vshufps        $0xee, %%ymm11, %%ymm9,  %%ymm5 \n"
                                                    // X00 X10 X20 X30 X04 X14 X24 X34
                                                    // X21 X31 X41 X51 X25 X35 X45 X55
                                                    // X42 X52 X03 X13 X46 X56 X07 X17
                                                    // X40 X50 X01 X11 X44 X54 X05 X15
                                                    // X02 X12 X22 X32 X06 X16 X26 X36
                                                    // X23 X33 X43 X53 X27 X37 X47 X57

    "vperm2f128     $0x20, %%ymm3, %%ymm0, %%ymm6   \n"
    "vperm2f128     $0x20, %%ymm4, %%ymm1, %%ymm7   \n"
    "vperm2f128     $0x20, %%ymm5, %%ymm2, %%ymm8   \n"
    "vperm2f128     $0x31, %%ymm3, %%ymm0, %%ymm9   \n"
    "vperm2f128     $0x31, %%ymm4, %%ymm1, %%ymm10  \n"
    "vperm2f128     $0x31, %%ymm5, %%ymm2, %%ymm11  \n"
                                                    // X00 X10 X20 X30 X40 X50 X01 X11
                                                    // X21 X31 X41 X51 X02 X12 X22 X32
                                                    // X42 X52 X03 X13 X23 X33 X43 X53
                                                    // X04 X14 X24 X34 X44 X54 X05 X15
                                                    // X25 X35 X45 X55 X06 X16 X26 X36
                                                    // X46 X56 X07 X17 X27 X37 X47 X57

    "vmovups        %%ymm6,         32*0(%%rbx)     \n"
    "vmovups        %%ymm7,         32*1(%%rbx)     \n"
    "vmovups        %%ymm8,         32*2(%%rbx)     \n"
    "vmovups        %%ymm9,         32*3(%%rbx)     \n"
    "vmovups        %%ymm10,        32*4(%%rbx)     \n"
    "vmovups        %%ymm11,        32*5(%%rbx)     \n"

    "addq           $8*4,           %%r14           \n" // src
    "addq           $6*8*4,         %%rbx           \n" // dest


    "decq           %%rdx                           \n"

    "jne            .LOOP_K_ITR%=                   \n"

    ".LOOP_K_ITR_DONE%=:                            \n"

    "testq          %%rdi,              %%rdi       \n" // test k_rem
    "je             .LOOP_K_REM_DONE%=              \n"

    "movq           %%rdi,              %%rdx       \n" // restore k_rem

    ".LOOP_K_REM%=:                                 \n"
    "vmovss         (%%r14),                %%xmm0  \n"
    "vmovss         (%%r14, %%rcx, 1),      %%xmm1  \n"
    "vmovss         (%%r14, %%rcx, 2),      %%xmm2  \n"
    "vmovss         (%%r14, %%r10, 1),      %%xmm3  \n"
    "vmovss         (%%r14, %%rcx, 4),      %%xmm4  \n"
    "vmovss         (%%r14, %%r11, 1),      %%xmm5  \n"
#ifdef PACK_A_MULTIPLE_ALPHA
    "vmulss         %%xmm14,  %%xmm0,   %%xmm0      \n"
    "vmulss         %%xmm14,  %%xmm1,   %%xmm1      \n"
    "vmulss         %%xmm14,  %%xmm2,   %%xmm2      \n"
    "vmulss         %%xmm14,  %%xmm3,   %%xmm3      \n"
    "vmulss         %%xmm14,  %%xmm4,   %%xmm4      \n"
    "vmulss         %%xmm14,  %%xmm5,   %%xmm5      \n"
#endif
    "vmovss         %%xmm0,             (%%rbx)     \n"
    "vmovss         %%xmm1,          4*1(%%rbx)     \n"
    "vmovss         %%xmm2,          4*2(%%rbx)     \n"
    "vmovss         %%xmm3,          4*3(%%rbx)     \n"
    "vmovss         %%xmm4,          4*4(%%rbx)     \n"
    "vmovss         %%xmm5,          4*5(%%rbx)     \n"

    "addq           $4,             %%r14           \n" // src
    "addq           $6*4,           %%rbx           \n" // dest
    "decq           %%rdx                           \n"
    "jne            .LOOP_K_REM%=                   \n"

    ".LOOP_K_REM_DONE%=:                            \n"

    "addq           %%rcx,              %%r11       \n" // 6x
    "leaq           (%%rax, %%r11, 1),  %%rax       \n" // src += 6xld
    "decq           %%r8                            \n"
    "jne            .LOOP_M_ITR%=                   \n"

    ".LOOP_M_ITR_DONE%=:                            \n"

    "testq          %%r9,               %%r9        \n" // test m_rem
    "je             .LOOP_M_REM_DONE%=              \n"
    "movq           %%r9,               %%r15       \n" // restore r_rem
    "shlq           $2,                 %%r9        \n" // *4

    ".LOOP_M_REM%=:                                 \n"
    "testq          %%rsi,              %%rsi       \n" // test k_itr
    "je             .LOOP_K_ITR_IN_M_DONE%=         \n"

    "movq           %%rsi,              %%rdx       \n" // restore k_itr

    "movq           %%rax,              %%r14       \n" // restore src
    "movq           %%rbx,              %%r8        \n" // restore dest
    
    "leaq           (%%r9,%%r9,2),      %%r10       \n" // 3x
    "leaq           (%%r9,%%r9,4),      %%r11       \n" // 5x
    "movq           %%r11,              %%r12       \n"
    "addq           %%r9,               %%r12       \n" // 6x
    "movq           %%r12,              %%r13       \n"
    "addq           %%r9,               %%r13       \n" // 7x

    ".LOOP_K_ITR_IN_M%=:                            \n"
    "vmovss            (%%r14),         %%xmm0      \n"
    "vmovss         4*1(%%r14),         %%xmm1      \n"
    "vmovss         4*2(%%r14),         %%xmm2      \n"
    "vmovss         4*3(%%r14),         %%xmm3      \n"
    "vmovss         4*4(%%r14),         %%xmm4      \n"
    "vmovss         4*5(%%r14),         %%xmm5      \n"
    "vmovss         4*6(%%r14),         %%xmm6      \n"
    "vmovss         4*7(%%r14),         %%xmm7      \n"
#ifdef PACK_A_MULTIPLE_ALPHA
    "vmulss         %%xmm14,  %%xmm0,   %%xmm0      \n"
    "vmulss         %%xmm14,  %%xmm1,   %%xmm1      \n"
    "vmulss         %%xmm14,  %%xmm2,   %%xmm2      \n"
    "vmulss         %%xmm14,  %%xmm3,   %%xmm3      \n"
    "vmulss         %%xmm14,  %%xmm4,   %%xmm4      \n"
    "vmulss         %%xmm14,  %%xmm5,   %%xmm5      \n"
    "vmulss         %%xmm14,  %%xmm6,   %%xmm6      \n"
    "vmulss         %%xmm14,  %%xmm7,   %%xmm7      \n"
#endif
    "vmovss         %%xmm0,             (%%r8)      \n"
    "vmovss         %%xmm1,   (%%r8, %%r9,  1)      \n"
    "vmovss         %%xmm2,   (%%r8, %%r9,  2)      \n"
    "vmovss         %%xmm3,   (%%r8, %%r10, 1)      \n"
    "vmovss         %%xmm4,   (%%r8, %%r9,  4)      \n"
    "vmovss         %%xmm5,   (%%r8, %%r11, 1)      \n"
    "vmovss         %%xmm6,   (%%r8, %%r12, 1)      \n"
    "vmovss         %%xmm7,   (%%r8, %%r13, 1)      \n"

    "addq           $8*4,               %%r14       \n"
    "leaq           (%%r8,%%r9,8),      %%r8        \n"
    "decq           %%rdx                           \n"
    "jne            .LOOP_K_ITR_IN_M%=              \n"

    ".LOOP_K_ITR_IN_M_DONE%=:                       \n"
    "testq          %%rdi,              %%rdi       \n"
    "je             .LOOP_K_REM_IN_M_DONE%=         \n"
    "movq           %%rdi,              %%rdx       \n"

    ".LOOP_K_REM_IN_M%=:                            \n"
    "vmovss         (%%r14),            %%xmm0      \n"
#ifdef PACK_A_MULTIPLE_ALPHA
    "vmulss         %%xmm14,  %%xmm0,   %%xmm0      \n"
#endif
    "vmovss         %%xmm0,             (%%r8)      \n"
    "addq           $4,                 %%r14       \n"
    "leaq           (%%r8,%%r9,1),      %%r8        \n"
    "decq           %%rdx                           \n"
    "jne            .LOOP_K_REM_IN_M%=              \n"

    ".LOOP_K_REM_IN_M_DONE%=:                       \n"

    "addq           $4,                 %%rbx       \n" // dest
    "leaq           (%%rax,%%rcx,1),    %%rax       \n" // src
    "decq           %%r15                           \n"
    "jne            .LOOP_M_REM%=                   \n"

    ".LOOP_M_REM_DONE%=:                            \n"
    ""
    ""
    : // output
    : // input
        "m"(src),       // 0
        "m"(dest),      // 1
        "m"(ld_),       // 2
        "m"(k_itr),     // 3
        "m"(k_rem),     // 4
        "m"(m_itr),     // 5
        "m"(m_rem)      // 6
#ifdef PACK_A_MULTIPLE_ALPHA
        ,"m"(alpha_addr) // 7
#endif
    : // clobber
        "rax","rbx","rcx","rdx","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15",
        "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm14",
        "ymm0","ymm1","ymm2","ymm3","ymm4","ymm5","ymm6",
        "ymm7","ymm8","ymm9","ymm10","ymm11","ymm12","ymm13",
        "ymm14","ymm15"
    );
}
static void sgemm_pack_n_a_n(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
    if(ctx->mr == 6){
        return sgemm_pack_n_a_n_mr16(mc, nc, kc, alpha, src, ld, dest, ctx);
    }
    return sgemm_pack_n_a_n_generic(mc, nc, kc, alpha, src, ld, dest, ctx);
}
static void sgemm_pack_n_a_t(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}

static void sgemm_pack_t_a_n(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}

static void sgemm_pack_t_a_t(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}

/***************************************************************************
 * packing for B
 *
 * we always look for the packing result of B as follow,
 * no matter input layout of B:
 *          (below layout is row major and dense, aka continuous in memory)
 * 
 *     NR
 *   +----+
 *   |    |
 *   |    | KC
 *   +----+
 *   |    |
 *   |    | KC
 *   +----+
 *   |    |
 * 
 * if row major:
 *  no need TLB align
 * if col major:
 *  for each NR*KC of B, prefer addressable by 1 TLB entry (1 PAGE_SIZE and alignment)
 */

/*
* sgemm_pack_n_b_n(), B no page alignment
*
*   <--   NC   -->
*     NR
*   +---+---+---+
*   |   |   |
* KC|   |   |
*   +---+---+---+
*/
static void sgemm_pack_n_b_n_generic(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{ 
    //int k_itr = kc/8;
    //int k_rem = kc%8;
    int k;
    int nr = ctx->nr;
    //int nc = ctx->nc;
    int n;
    //int page_size = ctx->page_size;

    //assert((nr*kc*sizeof(float) <= page_size) &&
    //    "nr*kc B block must small than page_size, other not support");
    const float * s_ptr = src;
    float * d_ptr = dest;
    float v;
    for(n=0; n<nc; n+=nr){
        int nr_size = MIN(nc-n, nr);
        int nn;
        const float * sline = s_ptr;
        float * dline = d_ptr;
        
        for(k=0;k<kc;k++){
            const float * ss = sline;
            float * dd = dline;
            for(nn=0; nn<nr_size; nn++){
                v = *ss;  ss++;
                *dd = v;  dd++;
            }
            sline += ld;
            dline += nr_size;
        }

        s_ptr += nr_size;
        //d_ptr += page_size/sizeof(float);
        d_ptr += nr*kc;

    }
}
//#define PACK_B_MULTIPLE_ALPHA
static void sgemm_pack_n_b_n_nr16(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
    assert(ctx->nr==16 && "mx16 kernel pack B");
    unsigned long long n_itr = nc/16;    // NR
    unsigned long long n_rem = nc%16;

    unsigned long long k_itr = kc/8;    // copy every 8 row
    unsigned long long k_rem = kc%8;
    unsigned long long ld_  = ld;
#ifdef PACK_B_MULTIPLE_ALPHA
    float * alpha_addr = &alpha;
#endif

    asm volatile(
    "movq               %0,             %%rax       \n" // src
    "movq               %1,             %%rbx       \n" // dest
    "movq               %2,             %%rcx       \n" // ld
    "movq               %3,             %%rsi       \n" // k_itr
    "movq               %4,             %%rdi       \n" // k_rem
    "movq               %5,             %%r8        \n" // n_itr
    "movq               %6,             %%r9        \n" // n_rem
#ifdef PACK_B_MULTIPLE_ALPHA
    "movq               %7,             %%r10       \n" // alpha_addr
    "vbroadcastss       (%%r10),        %%ymm15     \n" // alpha
#endif
    // n_itr
    "testq              %%r8,           %%r8        \n"
    "je                 .B16_LOOP_N_ITR_DONE%=      \n"

    "shlq               $2,             %%rcx       \n"

    ".B16_LOOP_N_ITR%=:                             \n"

    "testq              %%rsi,          %%rsi       \n"
    "je                 .B16_LOOP_K_ITR_DONE%=      \n"

    "leaq               (%%rcx, %%rcx, 2),  %%r11   \n" // 3x ld

    "movq               %%rsi,          %%rdx       \n" // restore k_itr
    "movq               %%rax,          %%r14       \n" // restore src
    //"movq               %%rbx,          %%r15       \n" // restore dest
    ".B16_LOOP_K_ITR%=:                             \n"
    "vmovups            (%%r14),            %%ymm0  \n"
    "vmovups          32(%%r14),            %%ymm1  \n"
    "vmovups            (%%r14, %%rcx),     %%ymm2  \n"
    "vmovups          32(%%r14, %%rcx),     %%ymm3  \n"
    "vmovups            (%%r14, %%rcx, 2),  %%ymm4  \n"
    "vmovups          32(%%r14, %%rcx, 2),  %%ymm5  \n"
    "vmovups            (%%r14, %%r11, 1),  %%ymm6  \n"
    "vmovups          32(%%r14, %%r11, 1),  %%ymm7  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulps             %%ymm15, %%ymm0,    %%ymm0  \n"
    "vmulps             %%ymm15, %%ymm1,    %%ymm1  \n"
    "vmulps             %%ymm15, %%ymm2,    %%ymm2  \n"
    "vmulps             %%ymm15, %%ymm3,    %%ymm3  \n"
    "vmulps             %%ymm15, %%ymm4,    %%ymm4  \n"
    "vmulps             %%ymm15, %%ymm5,    %%ymm5  \n"
    "vmulps             %%ymm15, %%ymm6,    %%ymm6  \n"
    "vmulps             %%ymm15, %%ymm7,    %%ymm7  \n"
#endif
    "vmovups            %%ymm0,             (%%rbx) \n"
    "vmovups            %%ymm1,         32*1(%%rbx) \n"
    "vmovups            %%ymm2,         32*2(%%rbx) \n"
    "vmovups            %%ymm3,         32*3(%%rbx) \n"
    "vmovups            %%ymm4,         32*4(%%rbx) \n"
    "vmovups            %%ymm5,         32*5(%%rbx) \n"
    "vmovups            %%ymm6,         32*6(%%rbx) \n"
    "vmovups            %%ymm7,         32*7(%%rbx) \n"

    "leaq               (%%r14,%%rcx,4),    %%r14   \n"
    "addq               $32*8,              %%rbx   \n"

    "vmovups            (%%r14),            %%ymm0  \n"
    "vmovups          32(%%r14),            %%ymm1  \n"
    "vmovups            (%%r14, %%rcx),     %%ymm2  \n"
    "vmovups          32(%%r14, %%rcx),     %%ymm3  \n"
    "vmovups            (%%r14, %%rcx, 2),  %%ymm4  \n"
    "vmovups          32(%%r14, %%rcx, 2),  %%ymm5  \n"
    "vmovups            (%%r14, %%r11, 1),  %%ymm6  \n"
    "vmovups          32(%%r14, %%r11, 1),  %%ymm7  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulps             %%ymm15, %%ymm0,    %%ymm0  \n"
    "vmulps             %%ymm15, %%ymm1,    %%ymm1  \n"
    "vmulps             %%ymm15, %%ymm2,    %%ymm2  \n"
    "vmulps             %%ymm15, %%ymm3,    %%ymm3  \n"
    "vmulps             %%ymm15, %%ymm4,    %%ymm4  \n"
    "vmulps             %%ymm15, %%ymm5,    %%ymm5  \n"
    "vmulps             %%ymm15, %%ymm6,    %%ymm6  \n"
    "vmulps             %%ymm15, %%ymm7,    %%ymm7  \n"
#endif
    "vmovups            %%ymm0,             (%%rbx) \n"
    "vmovups            %%ymm1,         32*1(%%rbx) \n"
    "vmovups            %%ymm2,         32*2(%%rbx) \n"
    "vmovups            %%ymm3,         32*3(%%rbx) \n"
    "vmovups            %%ymm4,         32*4(%%rbx) \n"
    "vmovups            %%ymm5,         32*5(%%rbx) \n"
    "vmovups            %%ymm6,         32*6(%%rbx) \n"
    "vmovups            %%ymm7,         32*7(%%rbx) \n"

    "leaq               (%%r14,%%rcx,4),    %%r14   \n"
    "addq               $32*8,              %%rbx   \n"

    "decq               %%rdx                       \n"
    "jne                .B16_LOOP_K_ITR%=           \n"
    ".B16_LOOP_K_ITR_DONE%=:                        \n"

    "testq              %%rdi,          %%rdi       \n"
    "je                 .B16_LOOP_K_REM_DONE%=      \n"
    "movq               %%rdi,          %%rdx       \n" // restore k_itr

    ".B16_LOOP_K_REM%=:                             \n"
    "vmovups            (%%r14),            %%ymm0  \n"
    "vmovups          32(%%r14),            %%ymm1  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulps             %%ymm15, %%ymm0,    %%ymm0  \n"
    "vmulps             %%ymm15, %%ymm1,    %%ymm1  \n"
#endif
    "vmovups            %%ymm0,             (%%rbx) \n"
    "vmovups            %%ymm1,           32(%%rbx) \n"

    "leaq               (%%r14, %%rcx),     %%r14   \n"
    "addq               $32*2,              %%rbx   \n"

    "decq               %%rdx                       \n"
    "jne                .B16_LOOP_K_REM%=           \n"
    ".B16_LOOP_K_REM_DONE%=:                        \n"
    
    "addq               $16*4,              %%rax   \n" // inc src, dst is updated inside loop
    "decq               %%r8                        \n"
    "jne                .B16_LOOP_N_ITR%=           \n"
    ".B16_LOOP_N_ITR_DONE%=:                        \n"

    // n_rem
    "testq              %%r9,           %%r9        \n" // n_rem
    "je                 .B16_LOOP_N_REM_DONE%=      \n"

#ifdef PACK_B_MULTIPLE_ALPHA
    "vmovss             (%%r10),        %%xmm7      \n" // load alpha
#endif
    "movq               %%r9,           %%r8        \n"
    "shlq               $2,             %%r8        \n" // *4
    "leaq               (%%r8, %%r8, 2),    %%r10   \n" // 3x
    "leaq               (%%rcx, %%rcx, 2),  %%r11   \n" // 3x ld
    
    ".B16_LOOP_N_REM%=:                             \n"

    "testq              %%rsi,          %%rsi       \n"
    "je           .B16_LOOP_K_ITR_IN_N_REM_DONE%=   \n"
    "movq               %%rsi,          %%rdx       \n" // k_itr

    "movq               %%rax,          %%r14       \n" // restore src
    "movq               %%rbx,          %%r15       \n" // restore dest

    ".B16_LOOP_K_ITR_IN_N_REM%=:                    \n"
    "vmovss             (%%r14),            %%xmm0  \n"
    "vmovss             (%%r14,%%rcx),      %%xmm1  \n"
    "vmovss             (%%r14,%%rcx,2),    %%xmm2  \n"
    "vmovss             (%%r14,%%r11),      %%xmm3  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulss             %%xmm7, %%xmm0,     %%xmm0  \n"
    "vmulss             %%xmm7, %%xmm1,     %%xmm1  \n"
    "vmulss             %%xmm7, %%xmm2,     %%xmm2  \n"
    "vmulss             %%xmm7, %%xmm3,     %%xmm3  \n"
#endif
    "vmovss             %%xmm0,             (%%r15) \n"
    "vmovss             %%xmm1,    (%%r15, %%r8, 1) \n"
    "vmovss             %%xmm2,    (%%r15, %%r8, 2) \n"
    "vmovss             %%xmm3,    (%%r15,%%r10, 1) \n"

    "leaq               (%%r14, %%rcx, 4),  %%r14   \n"
    "leaq               (%%r15, %%r8,  4),  %%r15   \n"

    "vmovss             (%%r14),            %%xmm0  \n"
    "vmovss             (%%r14,%%rcx),      %%xmm1  \n"
    "vmovss             (%%r14,%%rcx,2),    %%xmm2  \n"
    "vmovss             (%%r14,%%r11),      %%xmm3  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulss             %%xmm7, %%xmm0,     %%xmm0  \n"
    "vmulss             %%xmm7, %%xmm1,     %%xmm1  \n"
    "vmulss             %%xmm7, %%xmm2,     %%xmm2  \n"
    "vmulss             %%xmm7, %%xmm3,     %%xmm3  \n"
#endif
    "vmovss             %%xmm0,             (%%r15) \n"
    "vmovss             %%xmm1,    (%%r15, %%r8, 1) \n"
    "vmovss             %%xmm2,    (%%r15, %%r8, 2) \n"
    "vmovss             %%xmm3,    (%%r15,%%r10, 1) \n"

    "leaq               (%%r14, %%rcx, 4),  %%r14   \n"
    "leaq               (%%r15, %%r8,  4),  %%r15   \n"

    "decq               %%rdx                       \n"
    "jne                .B16_LOOP_K_ITR_IN_N_REM%=  \n"
    ".B16_LOOP_K_ITR_IN_N_REM_DONE%=:               \n"

    "testq              %%rdi,          %%rdi       \n"
    "je              .B16_LOOP_K_REM_IN_N_REM_DONE%=\n"
    "movq               %%rdi,          %%rdx       \n"
    ".B16_LOOP_K_REM_IN_N_REM%=:                    \n"
    "vmovss             (%%r14),            %%xmm0  \n"
#ifdef PACK_B_MULTIPLE_ALPHA
    "vmulss             %%xmm7, %%xmm0,     %%xmm0  \n"
#endif
    "vmovss             %%xmm0,             (%%r15) \n"

    "leaq               (%%r14, %%rcx, 1),  %%r14   \n"
    "leaq               (%%r15, %%r8,  1),  %%r15   \n"

    "decq               %%rdx                       \n"
    "jne                .B16_LOOP_K_REM_IN_N_REM%=  \n"
    ".B16_LOOP_K_REM_IN_N_REM_DONE%=:               \n"

    "addq               $4,                 %%rax   \n"
    "addq               $4,                 %%rbx   \n"

    "decq               %%r9                        \n"
    "jne                .B16_LOOP_N_REM%=           \n"
    ".B16_LOOP_N_REM_DONE%=:                        \n"

    : // output
    : // input
        "m"(src),       // 0
        "m"(dest),      // 1
        "m"(ld_),       // 2
        "m"(k_itr),     // 3
        "m"(k_rem),     // 4
        "m"(n_itr),     // 5
        "m"(n_rem)      // 6
#ifdef PACK_B_MULTIPLE_ALPHA
        , "m"(alpha_addr) // 7
#endif
    : // clobber list
        "rax","rbx","rcx","rdx","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15",
        "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5", "xmm6", "xmm7",
        "ymm0","ymm1","ymm2","ymm3","ymm4","ymm5","ymm6",
        "ymm7","ymm8","ymm9","ymm10","ymm11","ymm12","ymm13",
        "ymm14","ymm15"
    );
}
static void sgemm_pack_n_b_n(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
    if(ctx->nr == 16){
        return sgemm_pack_n_b_n_nr16(mc,nc,kc,alpha,src,ld,dest,ctx);
    }
    return sgemm_pack_n_b_n_generic(mc,nc,kc,alpha,src,ld,dest,ctx);
}
static void sgemm_pack_n_b_t(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}

static void sgemm_pack_t_b_n(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}


static void sgemm_pack_t_b_t(int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{

}


/***************************************************************************
 * 
 * 
 * 
*/

// https://software.intel.com/en-us/mkl-developer-reference-c-cblas-gemm-alloc
#if 0
extern "C"
float * sgemm_alloc(identifier_t ident, int m, int n, int k, const gemm_context_t * ctx)
{
    if(ident == IDENT_A_MATRIX)
        return sgemm_alloc_a(m,n,k,ctx);
    else
        return sgemm_alloc_b(m,n,k,ctx);
}

extern "C"
void sgemm_free(float * buf){
    __aligned_free((void*)buf);
}
#endif

//https://software.intel.com/en-us/mkl-developer-reference-c-cblas-gemm-pack
extern "C"
void sgemm_pack(layout_t layout, trans_t trans, identifier_t ident,
    int mc, int nc, int kc,
    float alpha, const float * src,
    int ld, float * dest, const gemm_context_t * ctx)
{
#if 0
    if(layout == LAYOUT_ROW_MAJOR){
        if(trans == TRANS_NO_TRANS || trans == TRANS_CONJ_NO_TRANS){
            if(ident == IDENT_A_MATRIX)
                sgemm_pack_nn_a(m,n,k,alpha,src,ld,dest);
            else
                sgemm_pack_nn_b(m,n,k,alpha,src,ld,dest);
        }
    }else{

    }
#endif
    if(ident == IDENT_A_MATRIX){
        if(layout == LAYOUT_ROW_MAJOR){
            if(trans == TRANS_NO_TRANS || trans == TRANS_CONJ_NO_TRANS){
                sgemm_pack_n_a_n(mc,nc,kc,alpha,src,ld,dest,ctx);
            }else{
                sgemm_pack_n_a_t(mc,nc,kc,alpha,src,ld,dest,ctx);
            }
        }else{
            if(trans == TRANS_NO_TRANS || trans == TRANS_CONJ_NO_TRANS){
                sgemm_pack_t_a_t(mc,nc,kc,alpha,src,ld,dest,ctx);
            }else{
                sgemm_pack_t_a_n(mc,nc,kc,alpha,src,ld,dest,ctx);
            }
        }
    }else{
        if(layout == LAYOUT_ROW_MAJOR){
            if(trans == TRANS_NO_TRANS || trans == TRANS_CONJ_NO_TRANS){
                sgemm_pack_n_b_n(mc,nc,kc,alpha,src,ld,dest,ctx);
            }else{
                sgemm_pack_n_b_t(mc,nc,kc,alpha,src,ld,dest,ctx);
            }
        }else{
            if(trans == TRANS_NO_TRANS || trans == TRANS_CONJ_NO_TRANS){
                sgemm_pack_t_b_t(mc,nc,kc,alpha,src,ld,dest,ctx);
            }else{
                sgemm_pack_t_b_n(mc,nc,kc,alpha,src,ld,dest,ctx);
            }
        }
    }
}

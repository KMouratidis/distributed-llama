#include "nn-quants.hpp"
#include <cassert>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <cstdio>

#if defined(CONVERT_F16_TO_F32_LOOKUP)
float f16ToF32Lookup[65536];
#endif

void initQuants() {
#if defined(CONVERT_F16_TO_F32_LOOKUP)
    for (NnSize i = 0; i < 65536; i++)
        f16ToF32Lookup[i] = convertF16toF32Impl((NnFp16)i);
#endif
}

float convertF16toF32Impl(const NnFp16 value) {
    union Fl32 {
        uint32_t u;
        float f;
    };
    const Fl32 magic = { (254U - 15U) << 23 };
    const Fl32 infNan = { (127U + 16U) << 23 };
    Fl32 result;
    result.u = (value & 0x7FFFU) << 13;
    result.f *= magic.f;
    if (result.f >= infNan.f)
        result.u |= 255U << 23;
    result.u |= (value & 0x8000U) << 16;
    return result.f;
}

NnFp16 convertF32ToF16Impl(const float x) {
    int i = *(int *)&x;
    int s = (i >> 16) & 0x00008000;
    int e = ((i >> 23) & 0x000000ff) - (127 - 15);
    int m = i & 0x007fffff;
    if (e <= 0) {
        if (e < -10) {
            return s;
        }
        m = m | 0x00800000;
        int t = 14 - e;
        int a = (1 << (t - 1)) - 1;
        int b = (m >> t) & 1;
        m = (m + a + b) >> t;
        return s | m;
    }
    if (e == 0xff - (127 - 15)) {
        if (m == 0) {
            return s | 0x7c00;
        }
        m >>= 13;
        return s | 0x7c00 | m | (m == 0);
    }
    m = m + 0x00000fff + ((m >> 13) & 1);
    if (m & 0x00800000) {
        m =  0;
        e += 1;
    }
    assert(e <= 30);
    return s | (e << 10) | (m >> 13);
}

void quantizeF32toQ80(const float *input, NnBlockQ80 *output, const NnSize n, const NnSize nThreads, const NnSize threadIndex) {
    assert(n % Q80_BLOCK_SIZE == 0);
    const NnSize nBlocks = n / Q80_BLOCK_SIZE;
    SPLIT_THREADS(start, end, nBlocks, nThreads, threadIndex);

#if defined(__ARM_NEON)
    for (NnSize i = start; i < end; i++) {
        const float *x = &input[i * Q80_BLOCK_SIZE];
        NnBlockQ80 *y = &output[i];

        float32x4_t amaxVec = vdupq_n_f32(0.0f);
        for (NnSize j = 0; j < Q80_BLOCK_SIZE; j += 4) {
            const float32x4_t vec = vld1q_f32(&x[j]);
            const float32x4_t abs_vec = vabsq_f32(vec);
            amaxVec = vmaxq_f32(amaxVec, abs_vec);
        }

        float amax = vmaxvq_f32(amaxVec);

        const float d = amax / 127.0f;
        const float id = d != 0.0f ? 1.0f / d : 0.0f;
        
        y->d = CONVERT_F32_TO_F16(d);

        const float32x4_t vid_vec = vdupq_n_f32(id);

        for (NnSize j = 0; j < Q80_BLOCK_SIZE; j += 4) {
            float32x4_t vec = vld1q_f32(&x[j]);
            vec = vmulq_f32(vec, vid_vec);

            const uint32x4_t sign_mask = vcgeq_f32(vec, vdupq_n_f32(0.0f));
            const float32x4_t half = vbslq_f32(sign_mask, vdupq_n_f32(0.5f), vdupq_n_f32(-0.5f));
            vec = vaddq_f32(vec, half);
            
            const int32x4_t vec_i32 = vcvtq_s32_f32(vec);
            const int16x4_t vec_i16 = vqmovn_s32(vec_i32);
            const int8x8_t vec_i8 = vqmovn_s16(vcombine_s16(vec_i16, vec_i16));

            vst1_lane_s32((int32_t *)(y->qs + j), vreinterpret_s32_s8(vec_i8), 0);
        }
    }
#else
    for (NnSize i = start; i < end; i++) {
        const float *x = &input[i * Q80_BLOCK_SIZE];
        NnBlockQ80 *y = &output[i];

        float amax = 0.0f;
        for (NnSize j = 0; j < Q80_BLOCK_SIZE; j++) {
            const float v = fabsf(x[j]);
            amax = amax > v ? amax : v;
        }

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f / d : 0.0f;
        y->d = CONVERT_F32_TO_F16(d);
        for (NnSize j = 0; j < Q80_BLOCK_SIZE; ++j) {
            y->qs[j] = roundf(x[j] * id);
        }
    }
#endif
}

void dequantizeQ80toF32(const NnBlockQ80 *input, float* output, const NnSize k, const NnSize nThreads, const NnSize threadIndex) {
    assert(k % Q80_BLOCK_SIZE == 0);
    const int nBlocks = k / Q80_BLOCK_SIZE;
    const int blocksPerThread = nBlocks / nThreads;
    const int sk = blocksPerThread * Q80_BLOCK_SIZE;
    const int currentThreadBlocks = blocksPerThread + (threadIndex == nThreads - 1 ? nBlocks % nThreads : 0);

    const NnBlockQ80 *x = &input[blocksPerThread * threadIndex];
    float* y = &output[sk * threadIndex];

    for (int i = 0; i < currentThreadBlocks; i++) {
        const float d = CONVERT_F16_TO_F32(x[i].d);
        for (int j = 0; j < Q80_BLOCK_SIZE; j++) {
            y[i * Q80_BLOCK_SIZE + j] = x[i].qs[j] * d;
        }
    }
}

void quantizeF32toQ40(const float *x, NnBlockQ40 *output, const NnSize n, const NnSize nThreads, const NnSize threadIndex) {
    assert(n % Q40_BLOCK_SIZE == 0);
    const NnSize nBlocks = n / Q40_BLOCK_SIZE;
    const NnSize halfSize = Q40_BLOCK_SIZE / 2;
    SPLIT_THREADS(start, end, nBlocks, nThreads, threadIndex);

    for (NnSize i = start; i < end; i++) {
        float amax = 0.0f;
        float max = 0.0f;
        for (NnSize j = 0; j < Q40_BLOCK_SIZE; j++) {
            float v = x[i * Q40_BLOCK_SIZE + j];
            if (amax < fabsf(v)) {
                amax = fabsf(v);
                max = v;
            }
        }

        const float d = max / -8.0f;
        const float id = d ? 1.0f / d : 0.0f;

        NnBlockQ40 *o = &output[i];
        o->d = CONVERT_F32_TO_F16(d);
        for (NnSize j = 0; j < halfSize; j++) {
            const float x0 = x[i * Q40_BLOCK_SIZE + j] * id;
            const float x1 = x[i * Q40_BLOCK_SIZE + halfSize + j] * id;
    
            uint8_t xi0 = (int8_t)(x0 + 8.5f);
            uint8_t xi1 = (int8_t)(x1 + 8.5f);
            if (xi0 > 15) xi0 = 15;
            if (xi1 > 15) xi1 = 15;

            o->qs[j] = xi0 | (xi1 << 4);
        }
    }
}

void dequantizeQ40toF32(const NnBlockQ40 *x, float *output, const NnSize n, const NnSize nThreads, const NnSize threadIndex) {
    assert(n % Q40_BLOCK_SIZE == 0);
    const NnSize nBlocks = n / Q40_BLOCK_SIZE;
    SPLIT_THREADS(start, end, nBlocks, nThreads, threadIndex);

    for (NnSize i = start; i < end; i++) {
        const NnBlockQ40 *b = &x[i];
        const float d = CONVERT_F16_TO_F32(b->d);

        for (int j = 0; j < Q40_BLOCK_SIZE / 2; ++j) {
            const int x0 = (b->qs[j] & 0x0F) - 8;
            const int x1 = (b->qs[j] >> 4) - 8;

            output[i * Q40_BLOCK_SIZE + j] = x0 * d;
            output[i * Q40_BLOCK_SIZE + j + Q40_BLOCK_SIZE / 2] = x1 * d;
        }
    }
}

const char *floatTypeToString(NnFloatType type) {
    if (type == F_UNK) return "F_UNK";
    if (type == F_32) return "F_32";
    if (type == F_16) return "F_16";
    if (type == F_Q40) return "F_Q40";
    if (type == F_Q80) return "F_Q80";
    throw std::invalid_argument("Unknown float type");
}

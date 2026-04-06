#pragma once
// AttoLang Runtime - type aliases, containers, math, and stubs
// Generated programs #include this header.

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <list>
#include <queue>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <array>
#include <mutex>
#include <random>

// Scalar type aliases
using u8  = uint8_t;
using s8  = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;

// Math constants
constexpr f64 atto_pi  = 3.14159265358979323846;
constexpr f64 atto_e   = 2.71828182845904523536;
constexpr f64 atto_tau = 6.28318530717958647692;

// Audio output accumulator — accumulates per-sample output during on_audio_tick
inline f32 _atto_mix_accum = 0.0f;

inline void output_mix(f32 value) {
    _atto_mix_accum += value;
}

inline f32 atto_consume_mix() {
    f32 v = _atto_mix_accum;
    _atto_mix_accum = 0.0f;
    return v;
}

// Random number generation
inline std::mt19937& atto_rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}

inline f32 atto_rand_float(f32 min, f32 max) {
    std::uniform_real_distribution<f32> dist(min, max);
    return dist(atto_rng());
}

inline s32 atto_rand_int(s32 min, s32 max) {
    std::uniform_int_distribution<s32> dist(min, max);
    return dist(atto_rng());
}

// Standard event handler declarations — implemented by generated code
extern void on_start(std::vector<std::string> args, std::vector<std::string> envs);

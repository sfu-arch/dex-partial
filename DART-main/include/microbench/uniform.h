#pragma once

#ifndef UNIFORM_HPP
#define UNIFORM_HPP

#include <cstdint>

class UniformRandom {
public:
    UniformRandom() : seed_(0) {}
    explicit UniformRandom(uint64_t seed) : seed_(seed) {}

    uint64_t get_current_seed() const { return seed_; }
    void set_current_seed(uint64_t seed) { seed_ = seed; }

    uint64_t next_uint64() {
        return (static_cast<uint64_t>(next_uint32()) << 32) | next_uint32();
    }

    uint32_t next_uint32() {
        seed_ = seed_ * 0xD04C3175 + 0x53DA9022;
        return (seed_ >> 32) ^ (seed_ & 0xFFFFFFFF);
    }

private:
    uint64_t seed_;

    uint32_t get_c(uint32_t A) const {
        const uint64_t kCSeed = 0x734b00c6d7d3bbdaULL;
        return kCSeed % (A + 1);
    }
};

#endif

#ifndef _HLL_H_
#define _HLL_H_
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cinttypes>
#include <cassert>

constexpr double make_alpha(size_t m) {
    switch(m) {
        case 16: return .673;
        case 32: return .697;
        case 64: return .709;
        default: return 0.7213 / (1 + 1.079/m);
    }
}

template<size_t np>
class hll_t {
// Attributes
public:
    static const size_t m_ = 1 << np;
    static constexpr double alpha_ = make_alpha(m_);
    static constexpr double relative_error_ = 1.03896 / std::sqrt(m_);
    static const uint64_t bitmask_ = (~((uint64_t)0)) >> np;
// Members
    uint8_t *core_;
private:
    double sum_;
    int is_calculated_;
// Functions
public:
    void sum() {
        sum_ = 0;
        int noninf(0);
        for(unsigned i(0); i < m_; ++i) if(core_[i]) sum_ += 1. / (1 << core_[i]);
        is_calculated_ = 1;
    }
    double report() {
        if(!is_calculated_) sum();
        const double ret = alpha_ * m_ * m_ / sum_;
        // Correct for small values
        if(ret < m_ * 2.5) {
            int t(0);
            for(int i(0); i < m_; ++i) t += (core_[i] == 0);
            if(t) return m_ * std::log((double)(m_) / t);
        }
        return ret;
        // We don't correct for too large just yet, but we should soon.
    }
    double est_err() {
        return relative_error_ * report();
    }
    void add(uint64_t hashval) {
        const uint32_t index = hashval >> (64 - np);
        assert(index < m_);
        const int lzt(__builtin_clzll(hashval << np) + 1);
        //assert(lzt < (64 - np) + 1);
        assert(!hashval || lzt == __builtin_clzll(hashval << np) + 1);
        if(core_[index] < lzt) core_[index] = lzt;
    }
    hll_t(): core_((uint8_t *)calloc(m_, sizeof(uint8_t))), sum_(0.), is_calculated_(0) {}
    ~hll_t() {free(core_);}
    hll_t<np> const &operator+=(const hll_t<np> &other) {
        for(unsigned i(0); i < m_; ++i) if(core_[i] < other.core_[i]) core_[i] = other.core_[i];
        return *this;
    }
    // Note: We store values as (64 - value) and take the maximum when updating,
    // which allows us to minimize our trailing zeroes without sacrificing calloc.
};

typedef hll_t<22> HyperLogLog;

#endif // #ifndef _HLL_H_
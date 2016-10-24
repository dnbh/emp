#ifndef _ENCODER_H_
#define _ENCODER_H_
#include <thread>
#include <future>

#include <zlib.h>
#include <unistd.h>

#include "htslib/kstring.h"

#include "spacer.h"
#include "hll.h"
#include "hash.h"
#include "kseq_declare.h"


namespace kpg {

template<int (*is_lt)(uint64_t, uint64_t, void *)>
class Encoder {
    const char *s_;
    size_t l_;
    const Spacer &sp_;
    unsigned pos_;
    void *data_;
public:
    Encoder(char *s, size_t l, const Spacer &sp, void *data=nullptr):
      s_(s),
      l_(l),
      sp_(sp),
      data_(data),
      pos_(0)
    {
    }
    void assign(char *s, size_t l) {
        s_ = s; l_ = l; pos_ = 0;
    }
    void assign(kstring_t *ks) {assign(ks->s, ks->l);}
    void assign(kseq_t *ks) {assign(ks->seq.s, ks->seq.l);}
    // Algorithmic inefficiencies
    // 1. Not skipping over previousy discovered ambiguous bases
    // 2. Recalculating kmers for positions shared between neighboring windows.
    uint64_t window(unsigned start) {
        // Encode kmer here.
        if(start > l_ - sp_.w_ + 1) {
            fprintf(stderr, "FAIL window %i, start %i, length %i, diff %i.\n", sp_.w_, start, l_, l_ - start - sp_.w_);
            assert(sp_.w_ + start <= l_);
        }
        uint64_t best_kmer(BF);
        for(unsigned wpos(start), end(sp_.w_ + start - sp_.c_ + 1); wpos != end; ++wpos) {
            uint64_t new_kmer(cstr_lut[s_[wpos]]);
            unsigned j(0);
            for(auto s: sp_.s_) {
                j += s + 1;
                assert(j < sp_.c_);
                new_kmer <<= 2;
                new_kmer |= cstr_lut[s_[wpos + j]];
            }
            new_kmer = canonical_representation(new_kmer, sp_.k_);
            new_kmer ^= XOR_MASK;
            if(is_lt(new_kmer, best_kmer, data_)) best_kmer = new_kmer;
        }
        return best_kmer;
    }
    int has_next_window() {return pos_ < l_ - sp_.w_ - 1;}
    uint64_t next_kmer() {
        return window(pos_++);
    }
};


// C++ std lib doesn't actually give you a way to check on the status directly
// without joining the thread. This is a hacky workaroud c/o
// http://stackoverflow.com/questions/10890242/get-the-status-of-a-stdfuture
template<typename R>
static inline bool is_ready(std::future<R> const& f) {
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

template<int (*is_lt)(uint64_t, uint64_t, void *), size_t np=22>
hll_t<np> count_lmers(const std::string &path, const Spacer &space, unsigned k, uint16_t w,
                      void *data=nullptr) {
    Encoder<is_lt> enc(nullptr, 0, space, data);
    gzFile fp(gzopen(path.data(), "rb"));
    fprintf(stderr, "Opening from path %s.\n", path.data());
    kseq_t *ks(kseq_init(fp));
    hll_t<np> ret;
    size_t n(0);
    while(kseq_read(ks) >= 0) {
        enc.assign(ks);
        while(enc.has_next_window()) {++n; ret.add(u64hash(enc.next_kmer()));}
    }
    kseq_destroy(ks);
    gzclose(fp);
    return ret;
}

template<int (*is_lt)(uint64_t, uint64_t, void *), size_t np>
size_t estimate_cardinality(const std::vector<std::string> paths,
                            unsigned k, uint16_t w, spvec_t *spaces=nullptr,
                            void *data=nullptr, int num_threads=-1) {
    // Default to using all available threads.
    if(num_threads < 0) num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    const Spacer space(k, w, spaces);
    size_t submitted(0), completed(0), todo(paths.size());
    std::vector<std::future<hll_t<np>>> futures;
    std::vector<hll_t<np>> hlls;
    // Submit the first set of jobs
    for(int i(0); i < num_threads && i < (ssize_t)todo; ++i) {
        futures.emplace_back(std::async(
          std::launch::async, count_lmers<is_lt, np>, paths[i], space, k, w, data));
        ++submitted;
    }
    // Daemon -- check the status of currently running jobs, submit new ones when available.
    while(submitted < todo) {
        for(auto &f: futures) {
            if(is_ready(f)) {
                hlls.push_back(f.get());
                f = std::async(
                  std::launch::async, count_lmers<is_lt, np>, paths[submitted++],
                  space, k, w, data);
                ++completed;
            }
        }
    }
    // Get values from the rest of these threads.
    for(auto &f: futures) if(f.valid()) hlls.push_back(f.get());
    // Combine them all for a final count
    for(auto i(hlls.begin() + 1), end = hlls.end(); i != hlls.end(); ++i) hlls[0] += *i;
    hlls[0].sum();
    return (size_t)hlls[0].report();
}

} //namespace kpg
#endif // _ENCODER_H_
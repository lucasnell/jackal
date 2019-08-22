

#include "mutator_subs.h" // SubMutator and debugging preprocessor directives



#include <RcppArmadillo.h>
#include <pcg/pcg_random.hpp> // pcg prng
#include <vector>  // vector class
#include <string>  // string class


#include "var_classes.h"  // Var* classes
#include "pcg.h"  // pcg seeding
#include "alias_sampler.h"  // alias method of sampling
#include "util.h"  // str_stop





void SubMutator::new_chrom(VarChrom& var_chrom_, pcg64& eng) {

    var_chrom = &var_chrom_;

    // (Gammas go from 0 to (n-1), invariants are n.)
    const uint8 n = Q.size();

    const uint64 N = var_chrom_.size();
    const uint64 N0 = rate_inds.size();

    if (invariant <= 0) {

        if (N0 < N) {

            for (uint64 i = 0; i < N0; i++) {
                rate_inds[i] = static_cast<uint8>(runif_01(eng) * n);
            }
            for (uint64 i = N0; i < N; i++) {
                rate_inds.push_back(static_cast<uint8>(runif_01(eng) * n));
            }

        } else {

            if (N0 > N) rate_inds.resize(N);
            for (uint64 i = 0; i < N; i++) {
                rate_inds[i] = static_cast<uint8>(runif_01(eng) * n);
            }

        }

    } else {

        if (N0 < N) {

            for (uint64 i = 0; i < N0; i++) {
                if (runif_01(eng) > invariant) {
                    rate_inds[i] = static_cast<uint8>(runif_01(eng) * n);
                } else rate_inds[i] = n;
            }
            for (uint64 i = N0; i < N; i++) {
                if (runif_01(eng) > invariant) {
                    rate_inds.push_back(static_cast<uint8>(runif_01(eng) * n));
                } else rate_inds.push_back(n);
            }

        } else {

            if (N0 > N) rate_inds.resize(N);
            for (uint64 i = 0; i < N; i++) {
                if (runif_01(eng) > invariant) {
                    rate_inds[i] = static_cast<uint8>(runif_01(eng) * n);
                } else rate_inds[i] = n;
            }

        }
    }

    return;
}




inline void SubMutator::new_branch(const double& b_len) {

    // UNREST model
    if (U.size() == 0) {
        for (uint32 i = 0; i < Q.size(); i++) {
            // Adjust P(t) matrix
            Pt_calc(Q[i], 30, b_len, Pt[i]);
            // Now adjust the alias samplers:
            std::vector<AliasSampler>& samp(samplers[i]);
            for (uint32 j = 0; j < 4; j++) {
                samp[j] = AliasSampler(Pt[i].row(j));
            }
        }
    } else {
#ifdef __JACKALOPE_DEBUG
        if (U.size() != Q.size()) stop("SubMutator::new_branch-> U.size() != Q.size()");
        if (Ui.size() != Q.size()) stop("SubMutator::new_branch-> Ui.size() != Q.size()");
        if (L.size() != Q.size()) stop("SubMutator::new_branch-> L.size() != Q.size()");
#endif
        // All other models
        for (uint32 i = 0; i < Q.size(); i++) {
            // Adjust P(t) matrix
            Pt_calc(U[i], Ui[i], L[i], b_len, Pt[i]);
            // Now adjust the alias samplers:
            std::vector<AliasSampler>& samp(samplers[i]);
            for (uint32 j = 0; j < 4; j++) {
                samp[j] = AliasSampler(Pt[i].row(j));
            }
        }
    }

    return;

}


inline void SubMutator::subs_before_muts(uint64& pos,
                                         const uint64& end,
                                         const uint8& max_gamma,
                                         const std::string& bases,
                                         pcg64& eng) {

    for (; pos < end; pos++) {

        uint8& rate_i(rate_inds[pos]);
        if (rate_i > max_gamma) continue; // this is an invariant region

        uint8 c_i = char_map[var_chrom->ref_chrom->nucleos[pos]];
        if (c_i > 3) continue; // only changing T, C, A, or G
        AliasSampler& samp(samplers[rate_i][c_i]);
        uint8 nt_i = samp.sample(eng);
        if (nt_i != c_i) var_chrom->add_substitution(bases[nt_i], pos);

    }

    return;

}

inline void SubMutator::subs_after_muts(uint64& pos,
                                        const uint64& end1,
                                        const uint64& end2,
                                        const uint64& mut_i,
                                        const uint8& max_gamma,
                                        const std::string& bases,
                                        pcg64& eng) {

    uint64 end = std::min(end1, end2);

    while (pos < end) {

        uint8& rate_i(rate_inds[pos]);
        if (rate_i > max_gamma) {
            pos++;
            continue; // this is an invariant region
        }

        uint8 c_i = char_map[var_chrom->get_char_(pos, mut_i)];
        if (c_i > 3) {
            pos++;
            continue; // only changing T, C, A, or G
        }
        AliasSampler& samp(samplers[rate_i][c_i]);
        uint8 nt_i = samp.sample(eng);
        if (nt_i != c_i) var_chrom->add_substitution(bases[nt_i], pos);

        ++pos;
    }

    return;

}





//' Add substitutions for a whole chromosome or just part of one.
//'
//' Here, `end` is NOT inclusive, so can be == var_chrom->size()
//'
//' @noRd
//'
void SubMutator::add_subs(const double& b_len,
                          const uint64& begin,
                          const uint64& end,
                          pcg64& eng) {

#ifdef __JACKALOPE_DEBUG
    if (b_len < 0) stop("b_len < 0 in add_subs");
    if (begin > var_chrom->size()) stop("begin > var_chrom->size() in add_subs");
    if (end > var_chrom->size()) stop("end > var_chrom->size() in add_subs");
#endif

    if (b_len == 0) return;

    new_branch(b_len);

    uint8 max_gamma = Q.size() - 1; // any rate_inds above this means an invariant region
    std::string bases = "TCAG";

    // To make code less clunky:
    std::deque<Mutation>& mutations(var_chrom->mutations);

    /*
     If there are no mutations or if `end-1` is before the first mutation,
     then we don't need to use the `mutations` field at all.
     */
    if (mutations.empty() || ((end-1) < mutations.front().new_pos)) {

        uint64 pos = begin;
        subs_before_muts(pos, end, max_gamma, bases, eng);

        return;

    }


    // Index to the first Mutation object not past `start` position:
    uint64 mut_i = var_chrom->get_mut_(start);
    // Current position
    pos = start;

    /*
     If `start` is before the first mutation (resulting in `mut_i == mutations.size()`),
     we must process any nucleotides before the first mutation.
     */
    if (mut_i == mutations.size()) {

        mut_i = 0;
        subs_before_muts(pos, mutations[mut_i].new_pos, max_gamma, bases, eng);

    }


    /*
     Now, for each subsequent mutation except the last, process all nucleotides
     at or after its position but before the next one.
     */
    uint64 next_mut_i = mut_i + 1;
    while (pos <= end && next_mut_i < mutations.size()) {

        subs_after_muts(pos, end, mutations[next_mut_i].new_pos,
                          mut_i, max_gamma, bases, eng);

        ++mut_i;
        ++next_mut_i;
    }

    // Now taking care of nucleotides after the last Mutation
    subs_after_muts(pos, end, var_chrom->chrom_size,
                      mut_i, max_gamma, bases, eng);


    return;

}


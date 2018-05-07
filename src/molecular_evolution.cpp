
#include <RcppArmadillo.h>
#include <cmath>  // pow, log, exp
#include <pcg/pcg_random.hpp> // pcg prng
#include <vector>  // vector class
#include <string>  // string class



#include "gemino_types.h"
#include "molecular_evolution.h"
#include "sequence_classes.h"  // Var* and Ref* classes
#include "pcg.h"  // pcg seeding
#include "table_sampler.h"  // table method of sampling
#include "weighted_reservoir.h"  // weighted reservoir sampling
#include "mevo_gammas.h"  // SequenceGammas class
#include "mevo_rate_matrices.h"  // rate matrix functions

using namespace Rcpp;





//' Fill in vectors of mutation probabilities and lengths.
//'
//' These vectors should be initialized already, but there's no need to resize them.
//'
//'
//' @param Q A matrix of substitution rates for each nucleotide.
//' @param xi Overall rate of indels.
//' @param psi Proportion of insertions to deletions.
//' @param pis Vector of nucleotide equilibrium frequencies for
//'     "T", "C", "A", and "G", respectively.
//' @param rel_insertion_rates Relative insertion rates.
//' @param rel_deletion_rates Relative deletion rates.
//'
//' @noRd
//'
void fill_mut_prob_length_vectors(
        std::vector<std::vector<double>>& probs,
        std::vector<sint>& mut_lengths,
        const arma::mat& Q,
        const double& xi,
        const double& psi,
        const std::vector<double>& pis,
        arma::vec rel_insertion_rates,
        arma::vec rel_deletion_rates) {

    uint n_muts = 4 + rel_insertion_rates.n_elem + rel_deletion_rates.n_elem;

    if (n_muts == 4 && xi > 0) {
        stop("If indel rate > 0, vectors of the relative rates of insertions and "
                 "deletions cannot both be of length 0.");
    }

    // 1 vector of probabilities for each nucleotide: T, C, A, then G
    probs.resize(4);

    // make relative rates sum to 1:
    rel_insertion_rates /= arma::accu(rel_insertion_rates);
    rel_deletion_rates /= arma::accu(rel_deletion_rates);
    // Now make them sum to the overall insertion/deletion rate:
    double xi_i = xi / (1 + 1/psi);  // overall insertion rate
    double xi_d = xi / (1 + psi);    // overall deletion rate
    rel_insertion_rates *= xi_i;
    rel_deletion_rates *= xi_d;


    /*
     (1) Combine substitution, insertion, and deletion rates into a single vector
     (2) Create TableSampler for each nucleotide
     (3) Fill the `rates` field with mutation rates for each nucleotide
     */
    for (uint i = 0; i < 4; i++) {

        std::vector<double>& qc(probs[i]);

        qc = arma::conv_to<std::vector<double>>::from(Q.col(i));
        // Get the overall mutation rate for this nucleotide
        double qi = -1 * qc[i];
        // Add insertions, then deletions
        qc.reserve(n_muts);
        for (uint j = 0; j < rel_insertion_rates.n_elem; j++) {
            qc.push_back(rel_insertion_rates(j));
        }
        for (uint j = 0; j < rel_deletion_rates.n_elem; j++) {
            qc.push_back(rel_deletion_rates(j));
        }
        // Divide all by qi to make them probabilities
        for (uint j = 0; j < n_muts; j++) qc[j] /= qi;

        // Change the diagonal back to the mutation rate, which will be used later.
        qc[i] = qi;
    }

    // Now filling in mut_lengths field of MutationTypeSampler
    mut_lengths = std::vector<sint>(n_muts, 0);
    for (uint i = 0; i < rel_insertion_rates.n_elem; i++) {
        mut_lengths[i + 4] = static_cast<sint>(i+1);
    }
    for (uint i = 0; i < rel_deletion_rates.n_elem; i++) {
        sint ds = static_cast<sint>(i + 1);
        ds *= -1;
        mut_lengths[i + 4 + rel_insertion_rates.n_elem] = ds;
    }

    return;
}



MutationSampler make_mutation_sampler(VarSequence& vs,
                                      const std::vector<std::vector<double>>& probs,
                                      const std::vector<sint>& mut_lengths,
                                      const std::vector<double>& pis,
                                      const arma::mat& gamma_mat) {

    MutationTypeSampler mts(probs, mut_lengths);
    TableStringSampler<std::string> tss(mevo::bases, pis);

    SequenceGammas gammas(gamma_mat);
    MutationRates mr(vs, pis, gammas);
    LocationSampler ls(mr);

    MutationSampler ms(vs, ls, mts, tss);

    return ms;
}


ChunkMutationSampler make_mutation_sampler(VarSequence& vs,
                                           const std::vector<std::vector<double>>& probs,
                                           const std::vector<sint>& mut_lengths,
                                           const std::vector<double>& pis,
                                           const arma::mat& gamma_mat,
                                           const uint& chunk_size) {

    MutationTypeSampler mts(probs, mut_lengths);
    TableStringSampler<std::string> tss(mevo::bases, pis);

    SequenceGammas gammas(gamma_mat);
    MutationRates mr(vs, pis, gammas);
    ChunkLocationSampler ls(mr, chunk_size);

    ChunkMutationSampler ms(vs, ls, mts, tss);

    return ms;
}







//[[Rcpp::export]]
void test_sampling(const SEXP& vs_sexp, const uint& N,
                   const double& pi_t, const double& pi_c,
                   const double& pi_a, const double& pi_g,
                   const double& alpha_1, const double& alpha_2,
                   const double& beta,
                   const double& xi, const double& psi,
                   const arma::vec& rel_insertion_rates,
                   const arma::vec& rel_deletion_rates,
                   const uint& gamma_size,
                   const double& gamma_alpha,
                   const uint& chunk_size) {

    XPtr<VarSet> vs_xptr(vs_sexp);
    VarSet& vs_(*vs_xptr);
    VarSequence& vs(vs_[0][0]);

    arma::mat Q = TN93_rate_matrix(pi_t, pi_c, pi_a, pi_g, alpha_1, alpha_2, beta, xi);

    std::vector<double> pis = {pi_t, pi_c, pi_a, pi_g};

    std::vector<std::vector<double>> probs;
    std::vector<sint> mut_lengths;

    fill_mut_prob_length_vectors(probs, mut_lengths, Q, xi, psi, pis,
                                 rel_insertion_rates, rel_deletion_rates);

    pcg32 eng = seeded_pcg();

    arma::mat gamma_mat = make_gamma_mat(vs.size(), gamma_size, gamma_alpha, eng);

    ChunkMutationSampler ms = make_mutation_sampler(vs, probs, mut_lengths, pis,
                                                    gamma_mat, chunk_size);

    for (uint i = 0; i < N; i++) {
        Rcpp::checkUserInterrupt();
        ms.mutate(eng);
    }

    return;
}




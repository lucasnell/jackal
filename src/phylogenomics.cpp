
/*
 ********************************************************

 Methods for evolving chromosomes along phylogenies / gene trees

 ********************************************************
 */



#include <RcppArmadillo.h>
#include <vector>  // vector class
#include <string>  // string class
#include <algorithm>  // lower_bound, sort
#include <deque>  // deque
#include <progress.hpp>  // for the progress bar
#ifdef _OPENMP
#include <omp.h>  // omp
#endif

#include "jackalope_types.h"  // integer types
#include "var_classes.h"  // Var* classes
#include "mutator.h"  // samplers
#include "pcg.h" // pcg sampler types
#include "phylogenomics.h"
#include "util.h"  // thread_check


using namespace Rcpp;







/*
 Process one phylogenetic tree for a single chromosome with no recombination.

 Note that this function should be changed if any of these VarChroms differ from
 each other (within the range specified if recombination = true).
 They can already have mutations, but to start out, they must all be the same.
 */
int PhyloOneChrom::one_tree(PhyloTree& tree,
                             pcg64& eng,
                             Progress& prog_bar) {

    // Reset tree of samplers and VarChrom objects representing nodes and tips:
    reset(tree);

    /*
     Check for a user interrupt. Using a Progress object allows the user to interrupt
     the process during multithreaded operations.
     */
    if (prog_bar.is_aborted() || prog_bar.check_abort()) return -1;

    // Exponential distribution to do the time-jumps along the branch lengths:
    std::exponential_distribution<double> distr(1.0);

    /*
     Now iterate through the phylogeny:
     */
    for (uint64 i = 0; i < tree.n_edges; i++) {

        // Checking for abort every edge:
        if (prog_bar.is_aborted() || prog_bar.check_abort()) return -1;

        // Indices for nodes/tips that the branch length in `branch_lens` refers to
        uint64 b1 = tree.edges(i,0);
        uint64 b2 = tree.edges(i,1);

        /*
         Update `samplers`, `chrom_rates`, and `distr` for this edge:
         */
        update(distr, b1, b2);
        MutationSampler& m_samp(samplers[b2]);

        /*
         Now do exponential jumps and mutate until you exceed the branch length.
         */
        double& rate(chrom_rates[b2]);
        double amt_time = tree.branch_lens[i];
        double time_jumped = distr(eng);
        double rate_change = 0;
        if (recombination) {
            uint64& end_(tree.ends[b2]);
            end_ = tree.ends[b1];
            const uint64 start_ = tree.start;
            uint64 n_jumps = 0;
            while (time_jumped <= amt_time && end_ >= start_) {
                /*
                 Add mutation here, outputting how much the overall chromosome rate should
                 change:
                 (`end_` is automatically adjusted for indels)
                 */
                rate_change = m_samp.mutate(eng, start_, end_);
                /*
                 Adjust the overall chromosome rate, then update the exponential
                 distribution:
                 */
                rate += rate_change;
                distr.param(std::exponential_distribution<double>::param_type(rate));
                // Jump again:
                time_jumped += distr(eng);
                // Check for a user interrupt every 128 (2^7) jumps:
                if (n_jumps == 128) {
                    if (prog_bar.is_aborted() || prog_bar.check_abort()) return -1;
                    n_jumps = 0;
                }
                n_jumps++;
            }
        } else {
            uint64 n_jumps = 0;
            // Same thing but without recombination
            while (time_jumped <= amt_time && tmp_chroms[b2].size() > 0) {
                rate_change = m_samp.mutate(eng);
                rate += rate_change;
                distr.param(std::exponential_distribution<double>::param_type(rate));
                time_jumped += distr(eng);
                // Check for a user interrupt every 128 (2^7) jumps:
                if (n_jumps == 128) {
                    if (prog_bar.is_aborted() || prog_bar.check_abort()) return -1;
                    n_jumps = 0;
                }
                n_jumps++;
            }
        }

        /*
         To free up some memory, clear info from VarChrom object at `b1` if it's no
         longer needed.
         */
        clear_branches(b1, i, tree);

    }

    /*
     Update final `VarChrom` objects:
     */
    update_var_chrom(tree);

    // Update progress bar:
    if (recombination) {
        prog_bar.increment(tree.end - tree.start + 1);
    } else prog_bar.increment(var_chrom_ptrs[0]->ref_chrom->size());

    return 0;

}


void PhyloOneChrom::update_var_chrom(const PhyloTree& tree) {

    std::vector<uint64> spp_order = match_(ordered_tip_labels,
                                           tree.tip_labels);

    if (recombination) {
        for (uint64 i = 0; i < tree.n_tips; i++) {
            uint64 j = spp_order[i];
            (*var_chrom_ptrs[i]) += tmp_chroms[j];
        }
    } else {
        for (uint64 i = 0; i < tree.n_tips; i++) {
            uint64 j = spp_order[i];
            (*var_chrom_ptrs[i]).replace(tmp_chroms[j]);
        }
    }
    return;
}




/*
 Evolve all chromosomes along trees.
*/
XPtr<VarSet> PhyloInfo::evolve_chroms(
        SEXP& ref_genome_ptr,
        SEXP& sampler_base_ptr,
        uint64 n_threads,
        const bool& show_progress) {

    XPtr<RefGenome> ref_genome(ref_genome_ptr);
    XPtr<MutationSampler> sampler_base(sampler_base_ptr);

    // Extract tip labels from the first tree:
    std::vector<std::string> var_names = phylo_one_chroms[0].trees[0].tip_labels;

    XPtr<VarSet> var_set(new VarSet(*ref_genome, var_names), true);

    uint64 n_chroms = ref_genome->size();
    uint64 total_chrom = ref_genome->total_size;

    Progress prog_bar(total_chrom, show_progress);
    std::vector<int> status_codes(n_threads, 0);


    if (n_chroms != phylo_one_chroms.size()) {
        std::string err_msg = "\n# tips in phylo. info must be of same length as ";
        err_msg += "# chromosomes in reference genome";
        throw(Rcpp::exception(err_msg.c_str(), false));
    }

    // Generate seeds for random number generators (1 RNG per thread)
    const std::vector<std::vector<uint64>> seeds = mt_seeds(n_threads);

#ifdef _OPENMP
#pragma omp parallel default(shared) num_threads(n_threads) if (n_threads > 1)
{
#endif

    std::vector<uint64> active_seeds;

    // Write the active seed per thread or just write one of the seeds.
#ifdef _OPENMP
    uint64 active_thread = omp_get_thread_num();
#else
    uint64 active_thread = 0;
#endif
    int& status_code(status_codes[active_thread]);
    active_seeds = seeds[active_thread];

    pcg64 eng = seeded_pcg(active_seeds);

    // Parallelize the Loop
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
    for (uint64 i = 0; i < n_chroms; i++) {

        if (status_code != 0) continue;

        PhyloOneChrom& chrom_phylo(phylo_one_chroms[i]);

        // Set values for variant info and sampler:
        chrom_phylo.set_samp_var_info(*var_set, *sampler_base, i, eng);

        // Evolve the chromosome using the chrom_phylo object:
        status_code = chrom_phylo.evolve(eng, prog_bar);

    }

#ifdef _OPENMP
}
#endif

    for (const int& status_code : status_codes) {
        if (status_code == -1) {
            std::string warn_msg = "\nThe user interrupted phylogenetic evolution. ";
            warn_msg += "Note that changes occur in place, so your variants have ";
            warn_msg += "already been partially added.";
            Rcpp::warning(warn_msg.c_str());
            break;
        }
    }

    return var_set;

}







//' Create XPtr to nested vector of PhyloTree objects from phylogeny information.
//'
//' @noRd
//'
//[[Rcpp::export]]
SEXP phylo_info_to_trees(const List& genome_phylo_info) {

    uint64 n_chroms = genome_phylo_info.size();

    if (n_chroms == 0) {
        throw(Rcpp::exception("\nEmpty list provided for phylogenetic information.",
                              false));
    }

    XPtr<PhyloInfo> all_chroms_xptr(new PhyloInfo(genome_phylo_info));

    return all_chroms_xptr;
}





//' Evolve all chromosomes in a reference genome.
//'
//' @noRd
//'
//[[Rcpp::export]]
SEXP evolve_chroms(
        SEXP& ref_genome_ptr,
        SEXP& sampler_base_ptr,
        SEXP& phylo_info_ptr,
        uint64 n_threads,
        const bool& show_progress) {

    XPtr<PhyloInfo> phylo_info(phylo_info_ptr);

    // Check that # threads isn't too high and change to 1 if not using OpenMP:
    thread_check(n_threads);

    XPtr<VarSet> var_set = phylo_info->evolve_chroms(
        ref_genome_ptr, sampler_base_ptr,
        n_threads, show_progress);

    return var_set;
}








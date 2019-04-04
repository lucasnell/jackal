


# library(jackalope)
# library(testthat)

context("Testing VCF file input/output")

options(stringsAsFactors = FALSE)

dir <- tempdir()

seqs <- rep("TCAGTCAGTC", 2)
ref <- ref_genome$new(jackalope:::make_ref_genome(seqs))
vars <- variants$new(jackalope:::make_var_set(ref$genome, 4), ref$genome)

# First sequence combines deletions and substitutions
{
    seq <- 1

    vars$add_sub(1, seq, 6, "T")
    vars$add_sub(2, seq, 6, "A")
    vars$add_sub(3, seq, 6, "A")
    vars$add_sub(3, seq, 7, "T")
    vars$add_sub(4, seq, 6, "G")
    vars$add_sub(4, seq, 8, "T")

    vars$add_del(1, seq, 7, 1)
    vars$add_del(2, seq, 7, 2)
    vars$add_del(4, seq, 9, 1)

    vars$add_del(1, seq, 1, 3)
    vars$add_del(2, seq, 2, 3)
    vars$add_del(3, seq, 1, 3)
    vars$add_del(4, seq, 3, 2)

}
# Second sequence combines insertions, deletions, and substitutions
{
    seq <- 2

    vars$add_del(1, seq, 9, 1)

    vars$add_ins(1, seq, 8, "A")

    vars$add_sub(1, seq, 6, "A")
    vars$add_sub(2, seq, 6, "A")
    vars$add_sub(3, seq, 6, "T")
    vars$add_del(4, seq, 6, 1)

    vars$add_ins(1, seq, 5, "TT")
    vars$add_ins(2, seq, 5, "TT")
    vars$add_ins(3, seq, 5, "T")
    vars$add_ins(4, seq, 5, "C")

    vars$add_sub(4, seq, 3, "T")

    vars$add_ins(2, seq, 2, "AG")
    vars$add_del(3, seq, 2, 2)
    vars$add_ins(4, seq, 2, "AG")

    vars$add_del(1, seq, 1, 1)

}


# From these mutations, I know what the ref and alt strings should be, as well as the
# genotypes for each variant:
vcf_info <-
    rbind(data.frame(seq = ref$names()[1],
                     pos = c(1, 6),
                     ref = c("TCAG", "CAGT"),
                     alt = c("G,T,TC", "TGT,AT,ATGT,GAT"),
                     gt1 = c(1, 1),
                     gt2 = c(2, 2),
                     gt3 = c(1, 3),
                     gt4 = c(3, 4)),
          data.frame(seq = ref$names()[2],
                     pos = c(1, 5, 8),
                     ref = c("TCA", "TC", "GT"),
                     alt = c("CA,TCAGA,T,TCAGT", "TTTA,TTT", "GA"),
                     gt1 = c(1, 1, 1),
                     gt2 = c(2, 1, 0),
                     gt3 = c(3, 2, 0),
                     gt4 = c(4, 0, 0)))



# ===============================================================`
# ===============================================================`

#               WRITING -----

# ===============================================================`
# ===============================================================`


# ------------------------*
# Haploid version -----
# (i.e., each variant is a separate sample)
# ------------------------*


vcf <- capture.output({
    jackalope:::write_vcf_cpp(out_prefix = "",
                           compress = FALSE,
                           var_set_ptr = vars$genomes,
                           sample_matrix = cbind(1:vars$n_vars()),
                           testing = TRUE)
})


test_that("VCF file header is accurate for haploid samples", {

    header <-
        c("##fileformat=VCFv4.3",
          sprintf("##fileDate=%s", format(Sys.Date(), "%Y%m%d")),
          "##source=jackalope",
          sprintf("##contig=<ID=%s,length=%i>", ref$names()[1], ref$sizes()[1]),
          sprintf("##contig=<ID=%s,length=%i>", ref$names()[2], ref$sizes()[2]),
          "##phasing=full",
          paste("##INFO=<ID=NS,Number=1,Type=Integer,Description=\"Number of",
                "Samples With Data\">"),
          "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">",
          "##FORMAT=<ID=GQ,Number=1,Type=Integer,Description=\"GenotypeQuality\">",
          paste0("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t",
                 paste(vars$var_names(), collapse = "\t")))

    expect_identical(vcf[1:length(header)], header)

})


# Remove header lines
vcf <- vcf[!grepl("^#", vcf)]

test_that("VCF file data lines are accurate for haploid samples", {

    # String to insert VCF info into:
    test_str <- paste0(c("%s\t%i\t.\t%s\t%s\t441453\tPASS\tNS=4\tGT:GQ",
                         rep("%i:441453", 4)), collapse = "\t")

    data_lines <- lapply(1:2,
                         function(i) {
                             sprintf(
                                 rep(test_str, sum(vcf_info$seq == ref$names()[i])),
                                 ref$names()[i],
                                 vcf_info$pos[vcf_info$seq == ref$names()[i]],
                                 vcf_info$ref[vcf_info$seq == ref$names()[i]],
                                 vcf_info$alt[vcf_info$seq == ref$names()[i]],
                                 vcf_info$gt1[vcf_info$seq == ref$names()[i]],
                                 vcf_info$gt2[vcf_info$seq == ref$names()[i]],
                                 vcf_info$gt3[vcf_info$seq == ref$names()[i]],
                                 vcf_info$gt4[vcf_info$seq == ref$names()[i]])
                         })

    expect_identical(data_lines[[1]], vcf[grepl(paste0("^", ref$names()[1]), vcf)])
    expect_identical(data_lines[[2]], vcf[grepl(paste0("^", ref$names()[2]), vcf)])

})




# ------------------------*
# Diploid version -----
# (i.e., 2 variants represent one sample)
# ------------------------*



sample_mat <- t(combn(vars$n_vars(), 2))

vcf <- capture.output({
    jackalope:::write_vcf_cpp(out_prefix = "",
                           compress = FALSE,
                           var_set_ptr = vars$genomes,
                           sample_matrix = sample_mat,
                           testing = TRUE)
})


test_that("VCF file header is accurate for diploid samples", {

    sample_names <- apply(sample_mat, 1, function(x) paste(vars$var_names()[x],
                                                           collapse = "__"))

    header <-
        c("##fileformat=VCFv4.3",
          sprintf("##fileDate=%s", format(Sys.Date(), "%Y%m%d")),
          "##source=jackalope",
          sprintf("##contig=<ID=%s,length=%i>", ref$names()[1], ref$sizes()[1]),
          sprintf("##contig=<ID=%s,length=%i>", ref$names()[2], ref$sizes()[2]),
          "##phasing=full",
          paste("##INFO=<ID=NS,Number=1,Type=Integer,Description=\"Number of",
                "Samples With Data\">"),
          "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">",
          "##FORMAT=<ID=GQ,Number=1,Type=Integer,Description=\"GenotypeQuality\">",
          paste0("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t",
                 paste(sample_names, collapse = "\t")))

    expect_identical(vcf[1:length(header)], header)

})



test_that("VCF file data lines are accurate for diploid samples", {

    # String to insert VCF info into:
    test_str <- paste0(c("%s\t%i\t.\t%s\t%s\t441453\tPASS\tNS=%i\tGT:GQ",
                         rep("%s:441453", nrow(sample_mat))), collapse = "\t")

    data_lines <- lapply(1:2,
                         function(i) {
                             vcf_info_ <- vcf_info[vcf_info$seq == ref$names()[i],]
                             n_lines <- nrow(vcf_info_)
                             gt_mat <- vcf_info_[, paste0("gt", 1:4)]
                             gt_mat <- as.matrix(gt_mat)
                             gt_strings <- lapply(
                                 1:nrow(sample_mat),
                                 function(j) {
                                     sapply(1:n_lines,
                                           function(k) paste(gt_mat[k,][sample_mat[j,]],
                                                             collapse = "|"))
                                 })
                             arg_list <- c(list(rep(test_str, n_lines),
                                                rep(ref$names()[i], n_lines),
                                                vcf_info_$pos,
                                                vcf_info_$ref,
                                                vcf_info_$alt,
                                                rep(nrow(sample_mat), n_lines)),
                                           gt_strings)

                             do.call(sprintf, arg_list)
                         })

    expect_identical(data_lines[[1]], vcf[grepl(paste0("^", ref$names()[1]), vcf)])
    expect_identical(data_lines[[2]], vcf[grepl(paste0("^", ref$names()[2]), vcf)])

})







# ===============================================================`
# ===============================================================`

#           READING -----

# ===============================================================`
# ===============================================================`







test_that("reading haploid variant info from VCF produces proper output", {

    write_vcf(vars, out_prefix = sprintf("%s/%s", dir, "test"))

    skip_if_not_installed("vcfR")

    vars2 <- create_variants(ref, method = "vcf",
                             method_info = sprintf("%s/%s.vcf", dir, "test"))

    expect_identical(vars$n_vars(), vars2$n_vars())

    for (i in 1:vars$n_vars()) {
        expect_identical(sapply(1:ref$n_seqs(), function(j) vars$sequence(i, j)),
                         sapply(1:ref$n_seqs(), function(j) vars2$sequence(i, j)))
    }


})


test_that("reading diploid variant info from VCF produces proper output", {

    skip_if_not_installed("vcfR")

    sample_mat <- matrix(1:4, 2, 2, byrow = TRUE)

    write_vcf(vars, out_prefix = sprintf("%s/%s", dir, "test"),
              sample_matrix = sample_mat)

    vars2 <- create_variants(ref, method = "vcf",
                             method_info = sprintf("%s/%s.vcf", dir, "test"))

    expect_identical(vars$n_vars(), vars2$n_vars())

    for (i in 1:vars$n_vars()) {
        expect_identical(sapply(1:ref$n_seqs(), function(j) vars$sequence(i, j)),
                         sapply(1:ref$n_seqs(), function(j) vars2$sequence(i, j)))
    }

})
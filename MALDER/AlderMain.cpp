/*
  todo:
  - seg fault in Nick's getstring when blank
  - seg fault when raw_outname is blank
  - print message instead of mindis -1.0 when not set
  - print message when no populations have curve (don't crash in DGESVD as in Italians)
  - print warnings about being careful with interpretation of curves?
    (1-ref can be caused by bottleneck, 2-ref doesn't necessarily imply closeness to true anc)
  - improve code documentation, particularly for tricky algorithmic parts
 */

/*
  1-ref:
  - decide how far out to start fitting (mindis):
    - if user has specified mindis param, use that (and print warning)
    - else determine extent of correlated LD; use max(0.5cM, second significance failure)
        if > thresh (2cM), refuse to proceed (but can override via mindis param)
  - compute 1-ref weighted LD; output the data; fit the curve (at start bin +/- a few?)
  2-ref:
  - decide how far out to start fitting (mindis):
    - if user has specified mindis param, use that (and print warning)
    - else if using external weights and not specified, default to 0.5cM (and print warning)
    - else determine extent of correlated LD; use max(0.5cM, second significance failure)
        if > thresh (2cM), refuse to proceed (but can override via mindis param)
  - compute 2-ref weighted LD; output the data; fit the curve (at start bin +/- a few)
  - compute 1-ref weighted LD with each ref (but don't output these curves); fit the curves
  - run tests:
    - (a) are the individual curves real?  check min z-score of amp, decay has p-value < 0.05
    - (b) are the curves consistent?  check diffs in decay <= 25% of 2-ref decay
  3+-ref:
  - iterate through ref pops
    - if correlated LD extends past 2cM, ref fails
    - if 1-ref weighted LD has no curve, ref fails
  - compute multiple hypothesis correction
  - run 2-ref weighted LD for pairs of non-failed refs
    - fit each pair starting from max of correlated LD and run test
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <utility>
#include <set>

#include <omp.h>

#include "nicklib.h"
#include "mcmcpars.h"

#include "Timer.hpp"
#include "ExpFitALD.hpp"
#include "MiscUtils.hpp"
#include "AlderParams.hpp"
#include "Alder.hpp"
#include "ProcessInput.hpp"

using namespace std;
using namespace ALD;

const char VERSION[] = "1.0";

// required to link mcio.o
int numchrom = 22 ;
char *trashdir = "/var/tmp" ;
int verbose = NO ;
int qtmode = NO ;


void print_header(void) {
  printnl() ;
  cout << "        |                          " << endl;
  cout << "        |      ALDER,   v" << string(VERSION) << endl;
  cout << "     \\..|./                        " << endl;
  cout << "    \\ \\  /       Admixture         " << endl;
  cout << "     \\ |/ /      Linkage           " << endl;
  cout << "      \\| /       Disequilibrium for" << endl;
  cout << "       |/        Evolutionary      " << endl;
  cout << "       |         Relationships     " << endl;
  cout << "       |                           " << endl;
  cout << endl;
  cout << "  +--------------------------------------------------------------------------+" << endl;
  cout << "  |  ALDER computes weighted LD decay curves, performs curve-fitting to      |" << endl;
  cout << "  |  infer admixture dates, and uses the results to test for admixture.      |" << endl;
  cout << "  |  For full details about options and parameters, please see the README    |" << endl;
  cout << "  |  file included with this software.                                       |" << endl;
  cout << "  +--------------------------------------------------------------------------+" << endl;
  printnl() ;
}

vector <double> subtract_freqs(const vector < vector <double> > &ref_freqs, int r1, int r2) {
  // get them from ref_freqs; in this case invalid snps already removed
  vector <double> weights(ref_freqs[r1].size());
  for (int s = 0; s < (int) ref_freqs[r1].size(); s++)
    weights[s] = ref_freqs[r1][s] - ref_freqs[r2][s];
  return weights;
}

int main(int argc, char *argv[]) {

  Timer timer;
  print_header();

  // ----------------------------------- read commands ------------------------------------ //

  AlderParams pars;
  pars.readcommands(argc, argv, VERSION);
  omp_set_num_threads(pars.num_threads);
  verbose = pars.verbose;

  // ----------------------------------- process input ------------------------------------ //

  cout << "                        *** Processing data ***" << endl << endl;

  Indiv **indivmarkers;
  SNP **snpmarkers ;
  int num_mixed_indivs; string mixed_pop_name;
  vector <int> num_ref_indivs; vector <string> ref_pop_names;
  vector <int> indiv_pop_inds =
    ProcessInput::process_indivs(pars.indivname, &indivmarkers, pars.admixlist, pars.admixpop,
				 pars.refpops, pars.poplistname, num_mixed_indivs, mixed_pop_name,
				 num_ref_indivs, ref_pop_names);
  if (pars.mincount > num_mixed_indivs) fatalx("mincount must be <= num mixed indivs\n");

  int orig_numsnps;
  vector < pair <int, double> > snp_locs =
    ProcessInput::process_snps(pars.snpname, pars.badsnpname, pars.fast_snp_read, &snpmarkers,
			       pars.checkmap, orig_numsnps, pars.chrom_set, pars.nochrom_set);

  char *mixed_geno = new char[snp_locs.size() * num_mixed_indivs];
  vector <char *> ref_genos(num_ref_indivs.size());
  for (int r = 0; r < (int) num_ref_indivs.size(); r++)
    ref_genos[r] = new char[snp_locs.size() * num_ref_indivs[r]];

  vector < vector <double> > ref_freqs =
    ProcessInput::process_geno(pars.genotypename, indiv_pop_inds, mixed_geno, ref_genos,
			       snpmarkers, orig_numsnps);
  
  // ----------------------- determine number of refs; set weights ------------------------ //

  vector <double> weights;
  int num_alder_refs = -1; // num_alder_refs either 1 or 2 (for external weights, 2)
  int num_ref_freqs = ref_freqs.size(); // num_ref_freqs is the number of ref pops with geno data
  vector <int> ref_inds;
  if (pars.weightname != NULL) { // load external weights
    weights = ProcessInput::process_weights(pars.weightname, snpmarkers, orig_numsnps);
    num_alder_refs = 2;
  }
  else {
    if (num_ref_freqs == 0) { // error
      fatalx("no data from ref populations\n");
    }
    else if (num_ref_freqs == 1) { // single ref
      if (pars.mincount < 4)
	fatalx("mincount must be >= 4 to compute single-reference LD (polyache)\n") ;
      weights = ref_freqs[0];
      num_alder_refs = 1;
      ref_inds.push_back(0);
    }
    else if (num_ref_freqs == 2) { // two refs
      weights = subtract_freqs(ref_freqs, 0, 1);
      num_alder_refs = 2;
      ref_inds.push_back(0); ref_inds.push_back(1);
    }
    else { // multiple refs
      if (pars.raw_outname != NULL) {
	cout << "WARNING: raw output is not written when testing with >= 3 ref pops" << endl;
	ofstream fout(pars.raw_outname);
	fout << "raw output is not written when testing with >= 3 ref pops" << endl;
	fout << "(to obtain raw data, perform individual 1-ref or 2-ref runs)" << endl;
	fout.close();
      }
    }
    cout << "number of reference populations: " << num_ref_freqs << endl;
  }

  Alder alder(mixed_geno, num_mixed_indivs, mixed_pop_name, ref_genos, num_ref_indivs,
	     ref_pop_names, snp_locs, timer);
  if (alder.get_num_chroms_used() < 2 && pars.print_raw_jackknife)
    cout << "WARNING: jackknife = YES, but need data from >= 2 chroms to jackknife" << endl;

  cout << endl << "Form of ALDER to run: ";
  if (num_ref_freqs <= 2) {
    if (num_alder_refs == 1)
      cout << "1-reference weighted LD" << endl;
    else
      cout << "2-reference weighted LD" << endl;
  }
  else
    cout << "3+ references (multiple admixture tests)" << endl;

  cout << endl << "==> Time to process data: " << timer.update_time() << endl << endl;

  // --------------------------- find extent of LD correlation ---------------------------- //

  printhline();
  vector <double> fit_starts = alder.find_ld_corr_stops(pars.binsize, pars.approx_ld_corr,
							pars.mindis);

  // --------------- perform weighted LD computation: 1-ref and 2-ref cases --------------- //

  if (num_ref_freqs <= 2) { // includes the external-weights case of num_ref_freqs == 0

    double fit_start_dis = *max_element(fit_starts.begin(), fit_starts.end());

    // ------------ compute weighted LD curve (1-ref or 2-ref as appropriate) ------------- //

    printhline();
    vector <ExpFitALD> fits_all_starts; int fit_test_ind = 0;
    vector <AlderResults> results_jackknife =
      alder.run(num_alder_refs, ref_inds, weights, pars.maxdis, pars.binsize, pars.mincount,
		pars.use_naive_algo, fit_start_dis, fits_all_starts, fit_test_ind);

    output_curve_data(results_jackknife.back());
    plot_ascii_curve(results_jackknife.back(), fit_start_dis);
    if (pars.raw_outname != NULL)
      write_raw_output(pars.raw_outname, pars.print_raw_jackknife, results_jackknife);

    for (int f = 0; f < (int) fits_all_starts.size(); f++)
      fits_all_starts[f].print_fit(pars.print_jackknife_fits);

    cout << "==> Time to run fits: " << timer.update_time() << endl << endl;

    // ----------------- 2-ref case: run test for admixture if possible ------------------- //

    if (num_alder_refs == 2) {
      // check if test for admixture can be run
      if (alder.get_num_chroms_used() < 2) // need to be able to jackknife
	cout << "finished: cannot test for admixture (need >= 2 chroms to jackknife)" << endl;
      else if (ref_inds.empty()) // can't run test if weights provided directly in external file
	cout << "finished: cannot test for admixture (need reference genotypes)" << endl;
      else {
	printhline();
	cout << "                    *** Running test for admixture ***" << endl << endl;

	// ---------------- compute and fit 1-ref curve with each ref --------------------- //

	vector <ExpFitALD> fits_all_starts_refs[2];
	int fit_test_ind_refs[2];
	for (int r = 0; r < 2; r++) {
	  printhline();
	  alder.run(1, vector <int> (1, r), ref_freqs[r], pars.maxdis, pars.binsize, pars.mincount,
		    pars.use_naive_algo, fit_starts[r]/*fit_start_dis*/, fits_all_starts_refs[r],
		    fit_test_ind_refs[r]); // fit starting from each LD corr cutoff

	  for (int f = 0; f < (int) fits_all_starts_refs[r].size(); f++)
	    fits_all_starts_refs[r][f].print_fit(pars.print_jackknife_fits);
	  cout << "==> Time to run fits: " << timer.update_time() << endl << endl;
	}

	// --------------------------- test for admixture --------------------------------- //

	printhline();
	cout << "               *** Comparing curves to test for admixture ***" << endl << endl;
    
	int r1 = 0, r2 = 1;
	for (int f = 0; f < (int) fits_all_starts.size(); f++) {
	  fits_all_starts[f].print_fit_header();
	  fits_all_starts_refs[r1][f].print_fit_diff(fits_all_starts[f], "decay", 2,
						     "1-ref " + ref_pop_names[r1], "2-ref");
	  fits_all_starts_refs[r2][f].print_fit_diff(fits_all_starts[f], "decay", 2,
						     "1-ref " + ref_pop_names[r2], "2-ref");
	  fits_all_starts_refs[r2][f].print_fit_diff(fits_all_starts_refs[r1][f], "decay", 2,
						     "1-ref " + ref_pop_names[r2],
						     "1-ref " + ref_pop_names[r1]);
	  cout << endl;
	}

	ExpFitALD::run_admixture_test(fits_all_starts[fit_test_ind],
				      fits_all_starts_refs[r1][fit_test_ind_refs[r1]],
				      fits_all_starts_refs[r2][fit_test_ind_refs[r2]],
				      mixed_pop_name, ref_pop_names[r1], ref_pop_names[r2], true,
				      1.0); // no multiple-hypothesis correction
	ExpFitALD::print_data_header(); // header line for grepping data
      }
    }
    else { // 1-ref case: compute mixture fraction bounds
      vector <double> f2_jacks = alder.compute_one_ref_f2_jacks(0);
      pair <double, double> alpha_mean_std
	= fits_all_starts[fit_test_ind].mix_frac_bound(f2_jacks);
      printf("Mixture fraction %% lower bound (assuming admixture): %.1f +/- %.1f\n",
	     100*alpha_mean_std.first, 100*alpha_mean_std.second);
    }
  }

  // ------------------- perform weighted LD computation: >=3-ref case -------------------- //

  else { // multiple-ref case

    if (alder.get_num_chroms_used() < 2)
      fatalx("cannot test for admixture: need >= 2 chroms to jackknife\n");

    // ----------------- find which refs have a significant 1-ref curve ------------------- //

    vector <bool> has_oneref_curve(num_ref_freqs, true);

    printhline();
    cout << "                     *** Running 1-ref pre-tests ***" << endl << endl;

    vector <ExpFitALD> fits_all_starts_refs[num_ref_freqs];
    int fit_test_ind_refs[num_ref_freqs];
    for (int r = 0; r < num_ref_freqs; r++) {
      if (fit_starts[r] == INFINITY) {
	has_oneref_curve[r] = false;
	continue;
      }
      printhline();
      alder.run(1, vector <int> (1, r), ref_freqs[r], pars.maxdis, pars.binsize, pars.mincount,
		pars.use_naive_algo, fit_starts[r], fits_all_starts_refs[r], fit_test_ind_refs[r]);

      for (int f = 0; f < (int) fits_all_starts_refs[r].size(); f++)
	fits_all_starts_refs[r][f].print_fit(pars.print_jackknife_fits);
      cout << "==> Time to run fits: " << timer.update_time() << endl << endl;
      
      cout << "Pre-test: Does " << mixed_pop_name << " have a 1-ref weighted LD curve with "
	   << ref_pop_names[r] << "?" << endl;
      has_oneref_curve[r] =
	fits_all_starts_refs[r][fit_test_ind_refs[r]].test_and_print_oneref_curve();
    }
    
    printhline();
    cout << "                 *** Summary of 1-ref pre-test results ***" << endl << endl;
    cout << "Pre-test: Does " << mixed_pop_name << " have a 1-ref weighted LD curve with..."
	 << endl;
    for (int r = 0; r < num_ref_freqs; r++) {
      printf("%20s: %3s ", ref_pop_names[r].c_str(), has_oneref_curve[r] ? "YES" : "NO");
      if (fit_starts[r] == INFINITY)
	printf("(cannot pre-test: long-range LD)\n");
      else
	printf("(z = %.2f)\n", min(fits_all_starts_refs[r][fit_test_ind_refs[r]].zscore("decay"),
				   fits_all_starts_refs[r][fit_test_ind_refs[r]].zscore("amp_exp")));
    }
    cout << endl;

    double mult_hyp_corr = alder.compute_mult_hyp_corr(vector <bool> (num_ref_freqs, true));

    printhline();
    ExpFitALD::print_data_header(); // header line for grepping data

    // ------------ run test on all pairs of refs with significant 1-ref curves ----------- //

    for (int r1 = 0; r1 < num_ref_freqs; r1++) {
      if (!has_oneref_curve[r1]) continue;
      for (int r2 = r1+1; r2 < num_ref_freqs; r2++) {
    	  if (!has_oneref_curve[r2]) continue;

    	  printhline();
    	  double fit_start_dis = max(fit_starts[r1], fit_starts[r2]);
    	  vector <ExpFitALD> fits_all_starts; int fit_test_ind = 0;
    	  weights = subtract_freqs(ref_freqs, r1, r2);
    	  ref_inds.resize(2); ref_inds[0] = r1; ref_inds[1] = r2;
    	  vector <AlderResults> results_jackknife =
    			  alder.run(2, ref_inds, weights, pars.maxdis, pars.binsize, pars.mincount,
							pars.use_naive_algo, fit_start_dis, fits_all_starts, fit_test_ind);
    	  plot_ascii_curve(results_jackknife.back(), fit_start_dis);

    	  for (int f = 0; f < (int) fits_all_starts.size(); f++)
			fits_all_starts[f].print_fit(pars.print_jackknife_fits);

    	  cout << "==> Time to run fits: " << timer.update_time() << endl << endl;

    	  ExpFitALD::run_admixture_test(fits_all_starts[fit_test_ind],
				      fits_all_starts_refs[r1][fit_test_ind_refs[r1]],
				      fits_all_starts_refs[r2][fit_test_ind_refs[r2]],
				      mixed_pop_name, ref_pop_names[r1], ref_pop_names[r2],
				      false, mult_hyp_corr);
      }
    }
  }
  delete[] mixed_geno;
  for (int r = 0; r < (int) num_ref_indivs.size(); r++) delete[] ref_genos[r];
}

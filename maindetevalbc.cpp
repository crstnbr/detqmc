#if defined (MAX_DEBUG) && ! defined(DUMA_NO_DUMA)
#include "dumapp.h"
#endif

// Evaluate time series generated by detqmc*.  Average over different
// boundary conditions pbc, apbc-x, apbc-y, apbc-xy.  Pass 4
// directories containing timeseries files as command line arguments.

#include <iostream>
#include <algorithm>            // all_of
#include <iterator>
#include <memory>
#include <map>
#include <cmath>
#include <vector>
#include <string>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#include "boost/program_options.hpp"
#include "boost/filesystem.hpp"
#pragma GCC diagnostic pop
#include "git-revision.h"
#include "tools.h"                      //glob
#include "dataseriesloader.h"
#include "datamapwriter.h"
#include "metadata.h"
#include "exceptions.h"
#include "statistics.h"

template<typename KeyType, typename ValType>
bool all_map_values_are_equal(const std::map<KeyType, ValType>& map) {
    bool are_all_equal = false;
    if (!map.empty()) {
        ValType val = map.begin()->second;
        are_all_equal = std::all_of(std::next(map.begin()), map.end(), 
                                    [val](typename std::map<KeyType, ValType>::const_reference t) -> bool
                                    { return t.second == val; });
    } else {
        are_all_equal = true;
    }
    return are_all_equal;
}


int main(int argc, char **argv) {
    uint32_t discard = 0;
    uint32_t read = 0;
    uint32_t subsample = 1;
    uint32_t jkBlocks = 1;
    bool notau = true;
    bool noexp = false;
    std::vector< std::string > inputDirectories;
    std::string outputDirectory;

    //parse command line options
    namespace po = boost::program_options;
    po::options_description evalOptions("Time series evaluation options");
    evalOptions.add_options()
        ("help", "print help on allowed options and exit")
        ("version,v", "print version information (git hash, build date) and exit")
        ("discard,d", po::value<uint32_t>(&discard)->default_value(0),
         "number of initial time series entries to discard (additional thermalization)")
        ("read,r", po::value<uint32_t>(&read)->default_value(0),
         "maximum number of time series entries to read (after discarded initial samples, before subsampling).  Default value of 0: read all entries")
        ("subsample,s", po::value<uint32_t>(&subsample)->default_value(1),
         "take only every s'th sample into account")
        ("jkblocks,j", po::value<uint32_t>(&jkBlocks)->default_value(1),
         "number of jackknife blocks to use")
        ("notau", po::bool_switch(&notau)->default_value(true),
         "ignored")
        ("noexp", po::bool_switch(&noexp)->default_value(false),
         "switch of estimation of expectation values and errorbars")
        ("outputDirectory", po::value<std::string>(&outputDirectory)->default_value("."))
        ("inputDirectories", po::value< std::vector<std::string> >(),
         "4 directories containing timeseries [positional arguments]")
        ;
    
    po::positional_options_description positionalArguments; // specify which options are positional
    positionalArguments.add("inputDirectories", 4);
    
    po::variables_map vm;
    //po::store(po::parse_command_line(argc, argv, evalOptions), vm);
    po::store(po::command_line_parser(argc, argv)
              .options(evalOptions)
              .positional(positionalArguments).run(), vm);
    po::notify(vm);

    bool earlyExit = false;
    if (vm.count("help")) {
        std::cout << "Evaluate time series generated by detqmc. \n"
                  << "Average over different boundary conditions pbc, apbc-x, apbc-y, apbc-xy. \n"
                  << "Pass 4 directories containing timeseries files as command line arguments. \n"
                  << "Will write results to file eval-results.values in given output directory.\n\n"
                  << evalOptions << std::endl;
        earlyExit = true;
    }
    if (vm.count("version")) {
        std::cout << "Build info:\n"
                  << metadataToString(collectVersionInfo())
                  << std::endl;
        earlyExit = true;
    }
    if (earlyExit) {
        return 0;
    }

    uint32_t dircount = 0;
    if (vm.count("inputDirectories")) {
        inputDirectories = vm["inputDirectories"].as<std::vector< std::string > >();
        dircount = (uint32_t)inputDirectories.size();
    }
    if (dircount != 4) {
        throw_GeneralError("Number of passed input directories " + numToString(dircount) + " is not 4.");
    }

    namespace fs = boost::filesystem;
    fs::path outputDirectory_path(outputDirectory);
    std::vector<fs::path> inputDirectories_path;
    for (const std::string& dir : inputDirectories) {
        inputDirectories_path.push_back(fs::path(dir));
    }

    //Store averages / nonlinear estimates, jackknife errors
    //key: observable name
    typedef std::map<std::string, double> ObsValMap;
    // one estimate / error per boundary conditions; "pbc", "apbc-x", "apbc-y", "apbc-xy" as keys.
    // for the different b.c. timeseries may be of different lengths -- but we use the same number of
    // jackknife blocks in any case
    typedef std::map< std::string, std::map<std::string, double> > ObsBcValMap;
    ObsBcValMap obs_bc_estimates, obs_bc_errors;
    // quantities averaged over b.c.s:
    ObsValMap avg_estimates, avg_errors;
    //jackknife-block wise estimates:
    typedef std::map<std::string, std::vector<double>> ObsVecMap;
    typedef std::map<std::string, std::map<std::string, std::vector<double> > > ObsBcVecMap;    
    ObsBcVecMap obs_bc_jkBlockEstimates;
    ObsVecMap avg_jkBlockEstimates;


    std::map<std::string, uint32_t> bc_evalSamples;

    std::map<std::string, uint32_t> bc_L, bc_N, bc_m;
    std::map<std::string, double> bc_dtau;

    // simulation meta data for each bc
    std::map<std::string, MetadataMap> bc_meta;

    // process one directory of time series after the other
    for (fs::path in_path : inputDirectories_path) {
        std::string info_dat_fname = (in_path / fs::path("info.dat")).string();

        //take simulation metadata from subdirectory file info.dat, remove some unnecessary parts.
        //this also tells us the boundary conditions.
        MetadataMap this_meta = readOnlyMetadata(info_dat_fname);
        std::string this_bc = this_meta["bc"];
        if (bc_evalSamples.count(this_bc)) {
            throw_GeneralError("Boundary condition " + this_bc + " appears more than one time");
        }
        std::string keys[] = {"buildDate", "buildHost", "buildTime",
                              "cppflags", "cxxflags", "gitBranch", "gitRevisionHash",
                              "sweepsDone", "sweepsDoneThermalization", "totalWallTimeSecs"};
        for ( std::string key : keys) {
            if (this_meta.count(key)) {
                this_meta.erase(key);
            }
        }
        bc_meta[this_bc] = this_meta;
        
        uint32_t guessedLength = static_cast<uint32_t>(fromString<double>(this_meta.at("sweeps")) /
                                                       fromString<double>(this_meta.at("measureInterval")));

        //metadata necessary for the computation of the susceptibility
        // spatial system size, and number of imaginary time slices
        uint32_t L = fromString<uint32_t>(this_meta.at("L"));
        uint32_t N = L*L;
        uint32_t m = fromString<uint32_t>(this_meta.at("m"));
        double dtau = fromString<double>(this_meta.at("dtau"));
        bc_L[this_bc] = L;
        bc_N[this_bc] = N;
        bc_m[this_bc] = m;
        bc_dtau[this_bc] = dtau;

        //process time series files
        std::vector<std::string> filenames = glob((in_path / fs::path("*.series")).string());
        for (std::string fn : filenames) {
            std::cout << "Processing " << fn << ", ";
            DoubleSeriesLoader reader;
            reader.readFromFile(fn, subsample, discard, read, guessedLength);
            if (reader.getColumns() != 1) {
                throw_GeneralError("File " + fn + " does not have exactly 1 column");
            }

            std::shared_ptr<std::vector<double>> data = reader.getData();
            std::string obsName;
            reader.getMeta("observable", obsName);
            std::cout << "observable: " << obsName << "..." << std::flush;

            if (not noexp) {
                obs_bc_estimates[obsName][this_bc] = average(*data);
                obs_bc_jkBlockEstimates[obsName][this_bc] = jackknifeBlockEstimates(*data, jkBlocks);

                // compute Binder cumulant and susceptibility (connected,
                // i.e. with the disconnected part substracted), 
                // the suscseptibility *without* the subtracted part:
                //   normMeanPhiSquared
                if (obsName == "normMeanPhi") {
                    using std::pow;
                    obs_bc_estimates["normMeanPhiSquared"][this_bc] = average<double>(
                        [](double v) { return pow(v, 2); },
                        *data );
                    obs_bc_jkBlockEstimates["normMeanPhiSquared"][this_bc] = jackknifeBlockEstimates<double>(
                        [](double v) { return pow(v, 2); },
                        *data, jkBlocks );
                
                    obs_bc_estimates["normMeanPhiFourth"][this_bc] = average<double>(
                        [](double v) { return pow(v, 4); },
                        *data );
                    obs_bc_jkBlockEstimates["normMeanPhiFourth"][this_bc] = jackknifeBlockEstimates<double>(
                        [](double v) { return pow(v, 4); },
                        *data, jkBlocks );
                
                    // nonlinear combinations of averages (connected susceptibility, Binder cumulant, ...)
                    // will be computed only after averaging over boundary conditions
                    
                    // bc_estimates[this_bc]["phiBinder"] =
                    //     1.0 - (3.0*bc_estimates[this_bc]["normMeanPhiFourth"]) /
                    //     (5.0*pow(bc_estimates[this_bc]["normMeanPhiSquared"], 2));
                    // bc_jkBlockEstimates[this_bc]["phiBinder"] = std::vector<double>(jkBlocks, 0);
                    // for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                    //     bc_jkBlockEstimates[this_bc]["phiBinder"][jb] =
                    //         1.0 - (3.0*jkBlockEstimates[this_bc]["normMeanPhiFourth"][jb]) /
                    //         (5.0*pow(jkBlockEstimates[this_bc]["normMeanPhiSquared"][jb], 2));
                    // }

                    // estimates[this_bc]["phiSusceptibility"] = (dtau * m * N) * (
                    //     estimates[this_bc]["normMeanPhiSquared"] -
                    //     pow(estimates[this_bc]["normMeanPhi"], 2)
                    //     );
                    // jkBlockEstimates[this_bc]["phiSusceptibility"] = std::vector<double>(jkBlocks, 0);
                    // for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                    //     jkBlockEstimates[this_bc]["phiSusceptibility"][jb] = (dtau * m * N) * (
                    //         jkBlockEstimates[this_bc]["normMeanPhiSquared"][jb] -
                    //         pow(jkBlockEstimates[this_bc]["normMeanPhi"][jb], 2)
                    //         );
                    // }
                
                }
            }

            //jackknifed computation of averaged quantities...
            // -- should time series be truncated? -> not necessarily.

            bc_evalSamples[this_bc] = static_cast<uint32_t>(data->size());

            reader.deleteData();

            std::cout << std::endl;
        }
    }

    std::string needed_bcs[] = {"pbc", "apbc-x", "apbc-y", "apbc-xy"};
    
    // verify that each bc is present, meta data is present and matches
    for (const std::string& bc : needed_bcs) {
        if (bc_L.count(bc) == 0) {
            throw_GeneralError("No data present for boundary condition: " + bc);
        }        
    }
    if (not all_map_values_are_equal(bc_L)) throw_GeneralError("mismatch for parameter L");
    if (not all_map_values_are_equal(bc_N)) throw_GeneralError("mismatch for parameter N");    
    if (not all_map_values_are_equal(bc_m)) throw_GeneralError("mismatch for parameter m");
    if (not all_map_values_are_equal(bc_dtau)) throw_GeneralError("mismatch for parameter dtau");

    uint32_t N = bc_N["pbc"];
    uint32_t m = bc_m["pbc"];
    double dtau = bc_dtau["pbc"];
        
    // reduce bc_meta to a common metadata map
    MetadataMap common_meta = getCommonMetadata( getCommonMetadata(bc_meta["pbc"], bc_meta["apbc-x"]),
                                                 getCommonMetadata(bc_meta["apbc-y"], bc_meta["apbc-xy"]) );
    common_meta["bc"] = std::string("averaged");
    
    // first deal with the simple average observables:
    
    // calculate averages over boundary conditions
    for (const auto& obs_BcVecMap_pair : obs_bc_jkBlockEstimates) {
        const std::string& obs = obs_BcVecMap_pair.first;
        const std::map<std::string, std::vector<double> >& bc_jkBlockEstimates = obs_BcVecMap_pair.second;

        avg_jkBlockEstimates[obs] = std::vector<double>(jkBlocks, 0);
        
        for (const auto& bc_blockVec_pair : bc_jkBlockEstimates) {
            // const std::string& bc = bc_blockVec_pair.first;
            std::vector<double> jkBlockEstimates = bc_blockVec_pair.second;
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                avg_jkBlockEstimates[obs][jb] += jkBlockEstimates[jb];
            }
        }

        for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
            avg_jkBlockEstimates[obs][jb] /= 4.0;
        }
    }

    for (const auto& obs_BcValMap_pair : obs_bc_estimates) {
        const std::string& obs = obs_BcValMap_pair.first;
        const std::map<std::string, double>& bc_estimates = obs_BcValMap_pair.second;

        avg_estimates[obs] = 0.0;
        
        for (const auto& bc_val_pair : bc_estimates) {
            // const std::string& bc = bc_val_pair.first;
            double estimate = bc_val_pair.second;
            avg_estimates[obs] += estimate;
        }

        avg_estimates[obs] /= 4.0;
    }

    // now deal with non-linear combinations of averages: connected
    // susceptibility, Binder cumulant
    avg_estimates["phiBinder"] =
        1.0 - (3.0*avg_estimates["normMeanPhiFourth"]) /
        (5.0*pow(avg_estimates["normMeanPhiSquared"], 2));
    avg_jkBlockEstimates["phiBinder"] = std::vector<double>(jkBlocks, 0);
    for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
        avg_jkBlockEstimates["phiBinder"][jb] =
            1.0 - (3.0*avg_jkBlockEstimates["normMeanPhiFourth"][jb]) /
            (5.0*pow(avg_jkBlockEstimates["normMeanPhiSquared"][jb], 2));
    }

    avg_estimates["phiBinderRatio"] =
        avg_estimates["normMeanPhiFourth"] /
        pow(avg_estimates["normMeanPhiSquared"], 2);
    avg_jkBlockEstimates["phiBinderRatio"] = std::vector<double>(jkBlocks, 0);
    for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
        avg_jkBlockEstimates["phiBinderRatio"][jb] =
            avg_jkBlockEstimates["normMeanPhiFourth"][jb] /
            pow(avg_jkBlockEstimates["normMeanPhiSquared"][jb], 2);
    }

    avg_estimates["phiSusceptibilityDisconnected"] = (dtau * m * N) * 
        avg_estimates["normMeanPhiSquared"];
    avg_jkBlockEstimates["phiSusceptibilityDisconnected"] = std::vector<double>(jkBlocks, 0);
    for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
        avg_jkBlockEstimates["phiSusceptibilityDisconnected"][jb] = (dtau * m * N) * 
            avg_jkBlockEstimates["normMeanPhiSquared"][jb];
    }

    avg_estimates["phiSusceptibility"] = (dtau * m * N) * (
        avg_estimates["normMeanPhiSquared"] -
        pow(avg_estimates["normMeanPhi"], 2)
        );
    avg_jkBlockEstimates["phiSusceptibility"] = std::vector<double>(jkBlocks, 0);
    for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
        avg_jkBlockEstimates["phiSusceptibility"][jb] = (dtau * m * N) * (
            avg_jkBlockEstimates["normMeanPhiSquared"][jb] -
            pow(avg_jkBlockEstimates["normMeanPhi"][jb], 2)
            );
    }    

    // calculate error bars for all quantities from jackknife block estimates
    if (not noexp and jkBlocks > 1) {
        for (auto const& nameBlockPair : avg_jkBlockEstimates) {
            const std::string obsName = nameBlockPair.first;
            const std::vector<double> blockEstimates = nameBlockPair.second;
            avg_errors[obsName] = jackknife(blockEstimates, avg_estimates[obsName]);
        }
    }

    if (not noexp) {
        StringDoubleMapWriter resultsWriter;
        resultsWriter.addMetadataMap(common_meta);
        resultsWriter.addMeta("eval-jackknife-blocks", jkBlocks);
        resultsWriter.addMeta("eval-discard", discard);
        resultsWriter.addMeta("eval-read", read);
        resultsWriter.addMeta("eval-subsample", subsample);
        for (const auto& bc_samples : bc_evalSamples) {
            const std::string& bc = bc_samples.first;
            const uint32_t& evalSamples = bc_samples.second;
            resultsWriter.addMeta("eval-samples_" + bc, evalSamples);
        }
        if (jkBlocks > 1) {
            resultsWriter.addHeaderText("Averages and jackknife error bars computed from time series for boundary conditions pbc, apbc-x, apbc-y, apbc-xy");
            resultsWriter.setData(std::make_shared<ObsValMap>(avg_estimates));
            resultsWriter.setErrors(std::make_shared<ObsValMap>(avg_errors));
        } else {
            resultsWriter.addHeaderText("Averages computed from time series for boundary conditions pbc, apbc-x, apbc-y, apbc-xy");
            resultsWriter.setData(std::make_shared<ObsValMap>(avg_estimates));
        }
        resultsWriter.writeToFile((outputDirectory_path / fs::path("eval-results.values")).string());
    }

    std::cout << "Done!" << std::endl;

    return 0;
}

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  See the enclosed file LICENSE for a copy or if
 * that was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2017 Max H. Gerlach
 * 
 * */

#if defined (MAX_DEBUG) && ! defined(DUMA_NO_DUMA)
#include "dumapp.h"
#endif
/*
 * deteval.cpp
 *
 *  Created on: Apr 29, 2013
 *      Author: gerlach
 */

// Evaluate time series generated by detqmc.
// Call in directory containing timeseries files.

#include <iostream>
#include <algorithm>
#include <iterator>
#include <functional>
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


namespace {
    // what to do    
    uint32_t discard = 0;
    uint32_t read_maximally = 0;
    uint32_t subsample_interval = 1;
    uint32_t jkBlocks = 1;
    bool notau = false;
    bool noexp = false;
    bool reweight = false;
    num original_r = 0;
    num reweight_to_this_r = 0;
    std::vector<std::string> noncollect_observables; // do not process these observables
    

    //Store averages / nonlinear estimates, jackknife errors,
    //integrated autocorrelation times here
    //key: observable name
    typedef std::map<std::string, double> ObsValMap;
    ObsValMap estimates, errors, tauints;
    //jackknife-block wise estimates
    typedef std::map<std::string, std::vector<double>> ObsVecMap;
    ObsVecMap jkBlockEstimates;

    uint32_t evalSamples = 0;
    uint32_t guessedLength = 0;
    //metadata necessary for the computation of the susceptibility
    // spatial system size, and number of imaginary time slices
    uint32_t L;
    uint32_t N;
    uint32_t m;
    double dtau;

    // for reweighting the other timeseries: need to store a function
    // of the timeseries of associatedEnergy
    std::shared_ptr<std::vector<num>> reweightingFactors;
};


MetadataMap readAndCleanMetadata(const std::string& filename) {
    //take simulation metadata from file info.dat, remove some unnecessary parts
    MetadataMap meta = readOnlyMetadata(filename);
    std::string keys[] = {"buildDate", "buildHost", "buildTime",
                          "cppflags", "cxxflags", "gitBranch", "gitRevisionHash",
                          "sweepsDone", "sweepsDoneThermalization", "totalWallTimeSecs"};
    for ( std::string key : keys) {
        if (meta.count(key)) {
            meta.erase(key);
        }
    }
    return meta;
}


void prepareReweightingFactors() {
    // we need the associatedEnergy time series to be able to do reweighting
    DoubleSeriesLoader associatedEnergyReader;
    associatedEnergyReader.readFromFile("associatedEnergy.series", subsample_interval,
                                        discard, read_maximally, guessedLength);
    std::shared_ptr<std::vector<num>> associatedEnergyData = associatedEnergyReader.getData();
    reweightingFactors.reset(new std::vector<num>());
    reweightingFactors->reserve(associatedEnergyData->size());
    for (num e : *associatedEnergyData) {
        // the data read in has been normalized by system size -- correct this effect:
        e *= (dtau * m * N);
        reweightingFactors->push_back( std::exp(-(reweight_to_this_r - original_r) * e) );
    }
}


void processTimeseries(const std::string& filename) {
    std::cout << "Processing " << filename << ", ";
    DoubleSeriesLoader reader;
    reader.readFromFile(filename, subsample_interval, discard, read_maximally, guessedLength);
    
    int reader_cols = reader.getColumns();

    if (reader_cols == 0) {
        // time series is empty
        std::cout << "Time series " + filename + " is empty, skip" << std::endl;
        return;
    }
    else if (reader_cols != 1) {
        throw_GeneralError("File " + filename + " does not have exactly 1 column, but " + numToString(reader_cols));
    }

    std::shared_ptr<std::vector<double>> data = reader.getData();
    std::string obsName;
    reader.getMeta("observable", obsName);
    std::cout << "observable: " << obsName << "...";
    
    if (std::any_of(std::begin(noncollect_observables),
                    std::end(noncollect_observables),
                    [&](const std::string& nc_obs) { return nc_obs == obsName; })) {
        std::cout << " skip" << std::endl;
        return;
    }

    if (reweight) {
        std::cout << " [reweighting from r=" << original_r
                  << " to r=" << reweight_to_this_r << "] ...";
    }
    
    std::cout << std::flush;

    if (not noexp) {
        using std::pow;

        auto average_func_maybe_reweight = [&]( const std::function<double(double)>& func ) -> double {
            if (reweight) {
                return average<double>( func, *data, *reweightingFactors );
            } else {
                return average<double>( func, *data );
            }
        };
        auto average_maybe_reweight = [&]( )  -> double {
            if (reweight) {
                return average<double>( *data, *reweightingFactors );
            } else {
                return average<double>( *data );
            }
        };
        auto jackknifeBlockEstimates_func_maybe_reweight = [&]( const std::function<double(double)>& func )  -> std::vector<double> {
            if (reweight) {
                return jackknifeBlockEstimates<double>( func, *data, *reweightingFactors, jkBlocks );
            } else {
                return jackknifeBlockEstimates<double>( func, *data, jkBlocks );
            }
        };
        auto jackknifeBlockEstimates_maybe_reweight = [&]( )  -> std::vector<double> {
            if (reweight) {
                return jackknifeBlockEstimates<double>( *data, *reweightingFactors, jkBlocks );
            } else {
                return jackknifeBlockEstimates<double>( *data, jkBlocks );
            }
        };
        
        estimates[obsName] = average_maybe_reweight();
        jkBlockEstimates[obsName] = jackknifeBlockEstimates_maybe_reweight();

        // compute Binder cumulant and susceptibility (<.^2> - <.>^2);
        // part susceptibility: <.^2>
        if (obsName == "normMeanPhi") {
            estimates["normMeanPhiSquared"] = average_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
            jkBlockEstimates["normMeanPhiSquared"] = jackknifeBlockEstimates_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
                
            estimates["normMeanPhiFourth"] = average_func_maybe_reweight(
                [](double v) { return pow(v, 4); } );
            jkBlockEstimates["normMeanPhiFourth"] = jackknifeBlockEstimates_func_maybe_reweight(
                [](double v) { return pow(v, 4); } );
                
            estimates["phiBinder"] = 1.0 - (3.0*estimates["normMeanPhiFourth"]) /
                (5.0*pow(estimates["normMeanPhiSquared"], 2));
            jkBlockEstimates["phiBinder"] = std::vector<double>(jkBlocks, 0);
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                jkBlockEstimates["phiBinder"][jb] =
                    1.0 - (3.0*jkBlockEstimates["normMeanPhiFourth"][jb]) /
                    (5.0*pow(jkBlockEstimates["normMeanPhiSquared"][jb], 2));
            }
            estimates["phiBinderRatio"] = estimates["normMeanPhiFourth"] /
                pow(estimates["normMeanPhiSquared"], 2);
            jkBlockEstimates["phiBinderRatio"] = std::vector<double>(jkBlocks, 0);
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                jkBlockEstimates["phiBinderRatio"][jb] =
                    jkBlockEstimates["normMeanPhiFourth"][jb] /
                    pow(jkBlockEstimates["normMeanPhiSquared"][jb], 2);
            }

            estimates["phiSusceptibilityPart"] = (dtau * m * N) * 
                estimates["normMeanPhiSquared"];
            jkBlockEstimates["phiSusceptibilityPart"] = std::vector<double>(jkBlocks, 0);
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                jkBlockEstimates["phiSusceptibilityPart"][jb] = (dtau * m * N) *
                    jkBlockEstimates["normMeanPhiSquared"][jb];
            }

            estimates["phiSusceptibility"] = (dtau * m * N) * (
                estimates["normMeanPhiSquared"] -
                pow(estimates["normMeanPhi"], 2)
                );
            jkBlockEstimates["phiSusceptibility"] = std::vector<double>(jkBlocks, 0);
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                jkBlockEstimates["phiSusceptibility"][jb] = (dtau * m * N) * (
                    jkBlockEstimates["normMeanPhiSquared"][jb] -
                    pow(jkBlockEstimates["normMeanPhi"][jb], 2)
                    );
            }
                
        }

        // experimental: compute a Binder parameter for the energy
        if (obsName == "associatedEnergy") {
            auto eSquared = average_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
            auto jbe_eSquared = jackknifeBlockEstimates_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
            auto eForth = average_func_maybe_reweight(
                [](double v) { return pow(v, 4); } );
            auto jbe_eForth = jackknifeBlockEstimates_func_maybe_reweight(
                [](double v) { return pow(v, 4); } );
            estimates["energyBinder"] = 1.0 - (3.0*eForth) /
                (5.0*pow(eSquared, 2));
            jkBlockEstimates["energyBinder"] = std::vector<double>(jkBlocks, 0);
            for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
                jkBlockEstimates["energyBinder"][jb] =
                    1.0 - (3.0*jbe_eForth[jb]) /
                    (5.0*pow(jbe_eSquared[jb], 2));
            }
        }

        // also compute bosonic spin stiffness, if the data is present
        //   rhoS = (beta / L**2) * ( <Gc> + <Gs>**2 + - <Gs**2> )
        // ==> need to compute <Gs**2>
        
        if (obsName == "phiRhoS_Gs") {
            estimates["phiRhoS_Gs_squared"] = average_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
            jkBlockEstimates["phiRhoS_Gs_squared"] = jackknifeBlockEstimates_func_maybe_reweight(
                [](double v) { return pow(v, 2); } );
        }
    }

//      std::copy(std::begin(jkBlockEstimates[obsName]), std::end(jkBlockEstimates[obsName]), std::ostream_iterator<double>(std::cout, " "));
//      std::cout << std::endl;
//      std::cout << average(jkBlockEstimates[obsName]);

    if (not notau) {
        tauints[obsName] = tauint(*data);
    }

    evalSamples = static_cast<uint32_t>(data->size());

    std::cout << std::endl;
}


void evaluateCombinedQuantities() {
    using std::pow;
    // also compute bosonic spin stiffness, if the data is present
    //   rhoS = (1. / (L**2 beta)) * ( <Gc> + <Gs>**2 - <Gs**2> )
    
    if (estimates.count("phiRhoS_Gs") and estimates.count("phiRhoS_Gc")) {
        assert(estimates.count("phiRhoS_Gs_squared"));
        estimates["phiRhoS"] = (1.0 / ((dtau * m) * N)) *
            (  pow(estimates["phiRhoS_Gs"], 2)
             + estimates["phiRhoS_Gc"]
             - estimates["phiRhoS_Gs_squared"]);
        jkBlockEstimates["phiRhoS"] = std::vector<double>(jkBlocks, 0);
        for (uint32_t jb = 0; jb < jkBlocks; ++jb) {
            jkBlockEstimates["phiRhoS"][jb] = (1.0 / ((dtau * m) * N)) * 
                (  pow(jkBlockEstimates["phiRhoS_Gs"][jb], 2)
                 + jkBlockEstimates["phiRhoS_Gc"][jb]
                 - jkBlockEstimates["phiRhoS_Gs_squared"][jb]);
        }
    }    
}


void jackknifeEvaluation() {
    for (auto const& nameBlockPair : jkBlockEstimates) {
        const std::string obsName = nameBlockPair.first;
        const std::vector<double> blockEstimates = nameBlockPair.second;
        errors[obsName] = jackknife(blockEstimates, estimates[obsName]);
    }
}

std::string get_results_filename() {
    std::string filename_insert = (reweight ? "-reweighted-r" + numToString(reweight_to_this_r) : "");
    return "eval-results" + filename_insert + ".values";
}

void removeOldResultsFile() {
    std::string fname = get_results_filename();
    if(boost::filesystem::exists(fname)) {
        boost::filesystem::remove(fname);
    }
}

void writeoutResults(MetadataMap meta) {
    if (estimates.empty()) {
        // nothing to write out, create no file
        return;
    }
    StringDoubleMapWriter resultsWriter;
    if (reweight) {
        meta["r"] = numToString(reweight_to_this_r);
        meta["original-r"] = numToString(original_r);
    }
    resultsWriter.addMetadataMap(meta);
    resultsWriter.addMeta("eval-jackknife-blocks", jkBlocks);
    resultsWriter.addMeta("eval-discard", discard);
    resultsWriter.addMeta("eval-read", read_maximally);
    resultsWriter.addMeta("eval-subsample", subsample_interval);
    resultsWriter.addMeta("eval-samples", evalSamples);
    if (reweight) {
        resultsWriter.addMeta("eval-reweighted-to-r", reweight_to_this_r);
        resultsWriter.addMeta("eval-original-r", original_r);
        resultsWriter.addHeaderText("Time series were reweighted");
    }
    if (jkBlocks > 1) {
        resultsWriter.addHeaderText("Averages and jackknife error bars computed from time series");
        resultsWriter.setData(std::make_shared<ObsValMap>(estimates));
        resultsWriter.setErrors(std::make_shared<ObsValMap>(errors));
    } else {
        resultsWriter.addHeaderText("Averages computed from time series");
        resultsWriter.setData(std::make_shared<ObsValMap>(estimates));
    }
    resultsWriter.writeToFile(get_results_filename());
}

std::string get_tauint_filename() {
    return "eval-tauint.values";
}

void removeOldTauintFile() {
    std::string fname = get_tauint_filename();
    if(boost::filesystem::exists(fname)) {
        boost::filesystem::remove(fname);
    }
}

void writeoutTauints(MetadataMap meta) {
    if (tauints.empty()) {
        // nothing to write out, create no file
        return;
    }
    StringDoubleMapWriter tauintWriter;
    tauintWriter.addMetadataMap(meta);
    tauintWriter.addMeta("eval-discard", discard);
    tauintWriter.addMeta("eval-read", read_maximally);
    tauintWriter.addMeta("eval-subsample", subsample_interval);
    tauintWriter.addMeta("eval-samples", evalSamples);
    tauintWriter.addHeaderText("Tauint estimates computed from time series");
    tauintWriter.setData(std::make_shared<ObsValMap>(tauints));
    tauintWriter.writeToFile(get_tauint_filename());
}



int main(int argc, char **argv) {
    //parse command line options
    namespace po = boost::program_options;
    po::options_description evalOptions("Time series evaluation options");
    evalOptions.add_options()
        ("help", "print help on allowed options and exit")
        ("version,v", "print version information (git hash, build date) and exit")
        ("discard,d", po::value<uint32_t>(&discard)->default_value(0),
         "number of initial time series entries to discard (additional thermalization)")
        ("read,r", po::value<uint32_t>(&read_maximally)->default_value(0),
         "maximum number of time series entries to read (after discarded initial samples, before subsampling).  Default value of 0: read all entries")
        ("subsample,s", po::value<uint32_t>(&subsample_interval)->default_value(1),
         "take only every s'th sample into account")
        ("jkblocks,j", po::value<uint32_t>(&jkBlocks)->default_value(1),
         "number of jackknife blocks to use")
        ("notau", po::bool_switch(&notau)->default_value(false),
         "switch off estimation of integrated autocorrelation times")
        ("noexp", po::bool_switch(&noexp)->default_value(false),
         "switch off estimation of expectation values and errorbars")
        ("reweight", po::value<double>(&reweight_to_this_r), "reweight timeseries to a new value of parameter r (SDW-model) "
            "[will not affect tauint]")
        ("noncollect,n", po::value<std::vector<std::string>>(&noncollect_observables)->multitoken(),
         "do not process these observables")
        ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, evalOptions), vm);
    po::notify(vm);
    bool earlyExit = false;
    if (vm.count("help")) {
        std::cout << "Evaluate time series generated by detqmc.  Call in directory containing timeseries files.\n"
                  << "Will write results to files eval-results.values and eval-tauint.values\n\n"
                  << evalOptions << std::endl;
        earlyExit = true;
    }
    if (vm.count("version")) {
        std::cout << "Build info:\n"
                  << metadataToString(collectVersionInfo())
                  << std::endl;
        earlyExit = true;
    }
    if (vm.count("reweight")) {
        reweight = true;
        // reweight_to_this_r has been set to the argument
    }

    if (earlyExit) {
        return 0;
    }

    MetadataMap meta = readAndCleanMetadata("info.dat");

    guessedLength = static_cast<uint32_t>(fromString<double>(meta.at("sweeps")) /
                                          fromString<double>(meta.at("measureInterval")));

    L = fromString<uint32_t>(meta.at("L"));
    N = L*L;
    m = fromString<uint32_t>(meta.at("m"));
    dtau = fromString<double>(meta.at("dtau"));
    original_r = fromString<double>(meta.at("r"));

    if (reweight) {
        prepareReweightingFactors();
    }

    //process time series files
    std::vector<std::string> filenames = glob("*.series");
    for (std::string fn : filenames) {
        processTimeseries(fn);
    }

    //maybe compute bosonic spin stiffness
    if (not noexp) {
        evaluateCombinedQuantities();
    }

    //calculate error bars from jackknife block estimates
    if (not noexp and jkBlocks > 1) {
        jackknifeEvaluation();
    }

    if (not noexp) {
        removeOldResultsFile();
        writeoutResults(meta);
    }

    if (not notau) {
        removeOldTauintFile();
        writeoutTauints(meta);
    }

    std::cout << "Done!" << std::endl;

    return 0;
}

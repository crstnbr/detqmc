/*
 * observablehandler.h
 *
 *  Created on: Dec 13, 2012
 *      Author: gerlach
 */

#ifndef OBSERVABLEHANDLER_H_
#define OBSERVABLEHANDLER_H_


// manage measurements of an observable
// calculate expectation values and jackknife error bars
// optionally store time series

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <armadillo>
#include "parameters.h"
#include "observable.h"
#include "metadata.h"
#include "dataserieswritersucc.h"
#include "datamapwriter.h"
#include "statistics.h"

class SerializeContentsKey;

#include "boost/serialization/vector.hpp"
#include "boost/serialization/export.hpp"
#include "boost_serialize_uniqueptr.h"

template <typename ObsType>
class ObservableHandlerCommon {
public:
	ObservableHandlerCommon(const Observable<ObsType>& observable,
			const MCParams& simulationParameters,
			const MetadataMap& metadataToStoreModel,
			const MetadataMap& metadataToStoreMC,
			ObsType zeroValue = ObsType())
		: obs(observable),
		  name(obs.name),
		  zero(zeroValue),			//ObsType() may not be a valid choice!
		  mcparams(simulationParameters),
		  metaModel(metadataToStoreModel), metaMC(metadataToStoreMC),
		  jkBlockCount(mcparams.jkBlocks),
		  jkBlockSizeSweeps(mcparams.sweeps / jkBlockCount),
		  lastSweepLogged(0), countValues(0),
		  jkBlockValues(jkBlockCount, zero),
		  total(zero) {
	}

	virtual ~ObservableHandlerCommon() { }

	// Log a newly measured observable value via the the reference contained in this->obs,
	// pass the number of the current sweep.
	// Measurements do not need to be stored at every sweep, but the number of skipped
	// sweeps must be constant.
	void insertValue(unsigned curSweep) {
		ObsType const value = obs.valRef;
		unsigned curJkBlock = curSweep / jkBlockSizeSweeps;
		for (unsigned jb = 0; jb < jkBlockCount; ++jb) {
			if (jb != curJkBlock) {
				jkBlockValues[jb] += value;
			}
		}
		total += value;
		++countValues;
		lastSweepLogged = curSweep;
	}

	//return [mean value, error] at end of simulation
	//if jkBlockCount <= 1, only estimate an error (using variance()) if the whole
	//timeseries is in memory
	//
	//return [mean value, 0] if this is called earlier
	std::tuple<ObsType,ObsType> evaluateJackknife() const {
		ObsType mean = total / countValues;
		ObsType error = zero;
		if (mcparams.sweeps - lastSweepLogged <= mcparams.measureInterval) {
			//after the first sweep lastSweepLogged==1 and so on --> here the simulation is finished.
			//we can only calculate an error estimate if we have multiple jackknife blocks
			if (jkBlockCount > 1) {
				unsigned jkBlockSizeSamples = countValues / jkBlockCount;
				unsigned jkTotalSamples = countValues - jkBlockSizeSamples;
				std::vector<ObsType> jkBlockAverages = jkBlockValues;	//copy
				for (unsigned jb = 0; jb < jkBlockCount; ++jb) {
					jkBlockAverages[jb] /= jkTotalSamples;
				}
				error = jackknife(jkBlockAverages, mean, zero);    //TODO: make this work with vector observables
			}
		}
		return std::make_tuple(mean, error);
	}
protected:
	Observable<ObsType> obs;
	const std::string& name;	//reference to name in obs
	ObsType zero;				//an instance of ObsType that works like the number zero
								//for addition -- this is not totally trivial for vector
								//valued observables

	MCParams mcparams;
	MetadataMap metaModel, metaMC;
	unsigned jkBlockCount;
	unsigned jkBlockSizeSweeps;

	unsigned lastSweepLogged;
	unsigned countValues;

	std::vector<ObsType> jkBlockValues;			// running counts of jackknife block values
	ObsType total;								// running accumulation regardless of jackknife block

//private:
//    friend class boost::serialization::access;
//	template<class Archive>
//    void serialize(Archive &ar, const unsigned int version) {
//		(void)version;
////		ar & obs;
////		ar & name;
////		ar & zero;
////		ar & mcparams;
////		ar & metaModel & metaMC;
////		ar & jkBlockCount
////		ar & jkBlockSizeSweeps;
//		//the variables above should not need to be serialized
//		ar & lastSweepLogged;
//		ar & countValues;
//		ar & total;
//	}

public:
    // only functions that can pass the key to this function have access
    // -- in this way access is granted only to DetQMC::serializeContents
    template<class Archive>
    void serializeContents(SerializeContentsKey const &, Archive &ar) {
		ar & lastSweepLogged;
		ar & countValues;
		ar & total;
    }
};



//specialized ObservableHandler that uses num as a value type
// -- can store time series, can be output into a common file "results.values"
//    for all scalar observables
class ScalarObservableHandler : public ObservableHandlerCommon<num> {
public:
	ScalarObservableHandler(const ScalarObservable& observable,
			const MCParams& simulationParameters,
			const MetadataMap& metadataToStoreModel,
			const MetadataMap& metadataToStoreMC)
		: ObservableHandlerCommon(observable, simulationParameters,
				metadataToStoreModel, metadataToStoreMC),
		timeseriesBuffer(),			//empty by default
		storage()					//initialize to something like a nullptr
	{
		if (mcparams.timeseries) {
			std::string filename = observable.name + ".series";
			storage = std::unique_ptr<DoubleVectorWriterSuccessive>(
					new DoubleVectorWriterSuccessive(filename));
			storage->addHeaderText("Timeseries for observable " + observable.name);
			storage->addMetadataMap(metaModel);
			storage->addMetadataMap(metaMC);
			storage->addMeta("observable", observable.name);
			storage->writeHeader();
		}
	}

	//in addition to base class functionality supports adding to the timeseries buffer
	void insertValue(unsigned curSweep) {
		num value = obs.valRef.get();
		if (mcparams.timeseries) {
			timeseriesBuffer.push_back(value);
		}
		ObservableHandlerCommon<num>::insertValue(curSweep);
	}

	//If we don't have multiple jackknife blocks and the whole timeseries is stored
	//in memory, this can also give a naive variance estimate for the error
	std::tuple<num, num> evaluateJackknife() const {
		num mean;
		num error;
		std::tie(mean, error) = ObservableHandlerCommon<num>::evaluateJackknife();
		if (jkBlockCount <= 1 and timeseriesBuffer.size() == countValues) {
			error = variance(timeseriesBuffer, mean);
		}
		return std::make_tuple(mean, error);
	}

	//update timeseries file, discard batch of
	//data written to file from memory
	void outputTimeseries() {
		//TODO: reserve reasonable amount of memory for data to be added afterwards
		//TODO: float precision
		if (mcparams.timeseries) {
			storage->writeData(timeseriesBuffer);	//append last batch of measurements
			timeseriesBuffer.resize(0);				//no need to keep it in memory anymore
		}
	}

	friend void ::outputResults(
			const std::vector<std::unique_ptr<ScalarObservableHandler>>& obsHandlers);
protected:
	std::vector<num> timeseriesBuffer;		// time series entries added since last call to writeData()
	std::unique_ptr<DoubleVectorWriterSuccessive> storage;

public:
	// only functions that can pass the key to this function have access
    // -- in this way access is granted only to DetQMC::serializeContents
    template<class Archive>
    void serializeContents(SerializeContentsKey const &sck, Archive &ar) {
    	ObservableHandlerCommon<num>::serializeContents(sck, ar);
    	ar & timeseriesBuffer;
    	//*storage should not need to be serialized.  It will always write to the end
		//of the timeseries file it finds at construction.
    }
//
//private:
//	friend class boost::serialization::access;
//	template<class Archive>
//    void serialize(Archive& ar, const unsigned int version) {
//		(void)version;
//		ar & boost::serialization::base_object<ObservableHandlerCommon<num>>(*this);
//		ar & timeseriesBuffer;
//		//*storage should not need to be serialized.  It will always write to the end
//		//of the timeseries file it finds at construction.
//	}
};

//BOOST_CLASS_EXPORT_GUID(ObservableHandlerCommon<num>, "ObservableHandlerCommon<num>")
//BOOST_CLASS_EXPORT_GUID(ScalarObservableHandler, "ScalarObservableHandler")



//Vector valued observables.  We use Armadillo vectors as they support arithmetics.
//A fixed vector size must be specified at initialization. This indexes the vector from 0 to
//the vector size.
class VectorObservableHandler : public ObservableHandlerCommon<arma::Col<num>> {
public:
	VectorObservableHandler(const VectorObservable& observable,
			const MCParams& simulationParameters,
			const MetadataMap& metadataToStoreModel,
			const MetadataMap& metadataToStoreMC)
		: ObservableHandlerCommon<arma::Col<num>>(observable,
				simulationParameters, metadataToStoreModel, metadataToStoreMC,
				arma::zeros<arma::Col<num>>(observable.vectorSize)),
		  vsize(observable.vectorSize), indexes(vsize), indexName("site")
	{
		for (unsigned counter = 0; counter < vsize; ++counter) {
			indexes[counter] = counter;
		}
	}
	unsigned getVectorSize() {
		return vsize;
	}
	friend void ::outputResults(
			const std::vector<std::unique_ptr<VectorObservableHandler>>& obsHandlers);
protected:
	unsigned vsize;
	arma::Col<num> indexes;
	std::string indexName;
//
//private:
//	friend class boost::serialization::access;
//	template<class Archive>
//    void serialize(Archive& ar, const unsigned int version) {
//		(void)version;
//		ar & boost::serialization::base_object<ObservableHandlerCommon<arma::Col<num>>>(*this);
//		//the other variables should not need to be serialized
//	}
};

//BOOST_CLASS_EXPORT_GUID(ObservableHandlerCommon<arma::Col<num>>, "ObservableHandlerCommon<arma::Col<num>>")
//BOOST_CLASS_EXPORT_GUID(VectorObservableHandler, "VectorObservableHandler")

//Vector indexed by arbitrary key
class KeyValueObservableHandler : public VectorObservableHandler {
public:
	KeyValueObservableHandler(const KeyValueObservable& observable,
			const MCParams& simulationParameters,
			const MetadataMap& metadataToStoreModel,
			const MetadataMap& metadataToStoreMC) :
				VectorObservableHandler(observable, simulationParameters,
						metadataToStoreModel, metadataToStoreMC) {
		//this code is convenient but sets the vector indexes twice upon construction
		indexes = observable.keys;
		indexName = observable.keyName;
	}

//private:
//	friend class boost::serialization::access;
//	template<class Archive>
//    void serialize(Archive& ar, const unsigned int version) {
//		(void)version;
//		ar & boost::serialization::base_object<VectorObservableHandler>(*this);
//		//the other variables should not need to be serialized
//	}
};

//BOOST_CLASS_EXPORT_GUID(KeyValueObservableHandler, "KeyValueObservableHandler")



//Write expectation values and error bars for all observables to a file
//take metadata to store from the first entry in obsHandlers
void outputResults(const std::vector<std::unique_ptr<ScalarObservableHandler>>& obsHandlers);

//write the results for each vector observable into a seperate file
void outputResults(const std::vector<std::unique_ptr<VectorObservableHandler>>& obsHandlers);


#endif /* OBSERVABLEHANDLER_H_ */

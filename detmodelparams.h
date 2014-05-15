#ifndef DETMODELPARAMS_H
#define DETMODELPARAMS_H

#include <string>
#include <set>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#include "boost/serialization/string.hpp"
#include "boost/serialization/set.hpp"
#pragma GCC diagnostic pop            

// Collect various structs defining various parameters.

// The set specified included in each struct contains string representations
// of all parameters actually specified.  This allows throwing an exception
// at the appropriate point in the program if a parameter is missing.


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
typedef double num;     //possibility to switch to single precision if ever desired
#pragma GCC diagnostic pop


// Template for struct representing model specific parameters
// -- needs to have a proper specialization for each model considered, which actually
//    implements the functions and provides data members
// for a class derived of DetModelGC this should at least be beta, m, s, dtau
template<class Model>
struct ModelParams {
    void check() { }
    MetadataMap prepareMetadataMap() { return MetadataMap(); }
private:
    friend class boost::serialization::access;

    template<class Archive>
    void serialize(Archive& ar, const uint32_t version) {
        (void)version; (void)ar;
    }    
};


// specializations in separate header files: DetHubbardParams, DetSDWParams



#endif /* DETMODELPARAMS_H */


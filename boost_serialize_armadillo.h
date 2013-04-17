/*
 * boost_serialize_armadillo.h
 *
 *  Created on: Apr 17, 2013
 *      Author: gerlach
 */

#ifndef BOOST_SERIALIZE_ARMADILLO_H_
#define BOOST_SERIALIZE_ARMADILLO_H_

//code to serialize Armadillo matrices / cubes with Boost

#include <armadillo>
#include <sstream>

namespace boost { namespace serialization {


template<class Archive, class T>
inline void save(Archive& ar,
				 const arma::Mat<T>& mat,
				 const unsigned int /*version*/) {
	std::ostringstream outStream;
	mat.save(outStream, arma::arma_binary);
	ar << outStream.str();
}

template<class Archive, class T>
inline void load(Archive& ar,
				 arma::Mat<T>& mat,
				 const unsigned int /*version*/) {
	std::string inString;
	ar >> inString;
	std::istringstream inStream(inString);
	mat.load(inStream, arma::arma_binary);
}

template<class Archive, class T>
inline void serialize(Archive& ar,
					  arma::Mat<T>& mat,
					  const unsigned int file_version) {
    boost::serialization::split_free(ar, mat, file_version);
}

template<class Archive, class T>
inline void serialize(Archive& ar,
					  arma::Cube<T>& cube,
					  const unsigned int file_version) {
    boost::serialization::split_free(ar, cube, file_version);
}

template<class Archive, class T>
inline void save(Archive& ar,
				 const arma::Cube<T>& cube,
				 const unsigned int /*version*/) {
	std::ostringstream outStream;
	cube.save(outStream, arma::arma_binary);
	ar << outStream.str();
}

template<class Archive, class T>
inline void load(Archive& ar,
				 arma::Cube<T>& cube,
				 const unsigned int /*version*/) {
	std::string inString;
	ar >> inString;
	std::istringstream inStream(inString);
	cube.load(inStream, arma::arma_binary);
}




} } // namespace boost::serialization



#endif /* BOOST_SERIALIZE_ARMADILLO_H_ */

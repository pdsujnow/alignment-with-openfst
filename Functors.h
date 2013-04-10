#ifndef _ALIGNMENT_WITH_OPENFST_FUNCTORS_H_
#define _ALIGNMENT_WITH_OPENFST_FUNCTORS_H_

#include <set>

struct AggregateFastSparseVectors2 {
  FastSparseVector<double> operator()(FastSparseVector<double> &v1, const FastSparseVector<double> &v2) {
    cerr << "AggregateFastSparseVector(){...";
    FastSparseVector<double> vTotal(v2);
    for(FastSparseVector<double>::iterator v1Iter = v1.begin(); v1Iter != v1.end(); ++v1Iter) {
      vTotal[v1Iter->first] += v1Iter->second;
    }
    cerr << "}" << endl;
    return vTotal;
  }
};

struct AggregateSets2 {
  std::set<std::string> operator()(const std::set<std::string> &v1, const std::set<std::string> &v2) {
    std::set<std::string> vTotal(v2);
    for(std::set<std::string>::const_iterator v1Iter = v1.begin(); v1Iter != v1.end(); ++v1Iter) {
      vTotal.insert(*v1Iter);
    }
    return vTotal; 
  }
};

struct AggregateVectors2 {
  std::vector<double> operator()(std::vector<double> &v1, const std::vector<double> &v2) {
    assert(v1.size() == v2.size());
    std::vector<double> vTotal(v1.size());
    for(unsigned i = 0; i < v1.size(); i++) {
      vTotal[i] = v1[i] + v2[i];
    }
    return vTotal;
  }  
};

#endif
#include "FstUtils.h"

using namespace fst;

LogPairWeight FstUtils::EncodePairInfinity() {
  return EncodePair(numeric_limits<float>::infinity(), numeric_limits<float>::infinity());
}

LogTripleWeight FstUtils::EncodeTripleInfinity() {
  return EncodeTriple(numeric_limits<float>::infinity(), numeric_limits<float>::infinity(), numeric_limits<float>::infinity());
}

LogQuadWeight FstUtils::EncodeQuadInfinity() {
  return EncodeQuad(numeric_limits<float>::infinity(), 
		    numeric_limits<float>::infinity(), 
		    numeric_limits<float>::infinity(), 
		    numeric_limits<float>::infinity());
}

LogPairWeight FstUtils::EncodePair(float val1, float val2) {
  LogWeight v1, v2;
  v1 = val1;
  v2 = val2;
  return ProductWeight<LogWeight, LogWeight>(v1, v2);
}

LogTripleWeight FstUtils::EncodeTriple(float val1, float val2, float val3) {
  LogWeight v3;
  v3 = val3;
  return LogTripleWeight(EncodePair(val1, val2), v3);
}

LogQuadWeight FstUtils::EncodeQuad(float val1, float val2, float val3, float val4) {
  LogWeight v4;
  v4 = val4;
  return LogQuadWeight(EncodeTriple(val1, val2, val3), v4);
}

string FstUtils::PrintPair(const ProductWeight<LogWeight, LogWeight>& w) {
  stringstream ss;
  ss << "(" << w.Value1().Value() << "," << w.Value2().Value() << ")";
  return ss.str();
}

string FstUtils::PrintTriple(const LogTripleWeight& w) {
  stringstream ss;
  ss << "(" << w.Value1().Value1().Value() 
     << "," << w.Value1().Value2().Value() 
     << "," << w.Value2().Value() 
     << ")";
  return ss.str();
}

string FstUtils::PrintQuad(const LogQuadWeight& w) {
  stringstream ss;
  ss << "(" << w.Value1().Value1().Value1().Value() 
     << "," << w.Value1().Value1().Value2().Value()
     << "," << w.Value1().Value2().Value()
     << "," << w.Value2().Value()
     << ")";
  return ss.str();
}

void FstUtils::DecodePair(const LogPairWeight& w, float& v1, float& v2) {
  v1 = w.Value1().Value();
  v2 = w.Value2().Value();
}

void FstUtils::DecodeTriple(const LogTripleWeight& w, float& v1, float& v2, float& v3) {
  DecodePair(w.Value1(), v1, v2);
  v3 = w.Value2().Value();
}

void FstUtils::DecodeQuad(const LogQuadWeight& w, float& v1, float& v2, float& v3, float& v4) {
  DecodeTriple(w.Value1(), v1, v2, v3);
  v4 = w.Value2().Value();
}

string FstUtils::PrintFstSummary(VectorFst<LogArc>& fst) {
  stringstream ss;
  ss << "states:" << endl;
  for(StateIterator< VectorFst<LogArc> > siter(fst); !siter.Done(); siter.Next()) {
    const LogArc::StateId &stateId = siter.Value();
    string final = fst.Final(stateId) != 0? " FINAL": "";
    string initial = fst.Start() == stateId? " START" : "";
    ss << "state:" << stateId << initial << final << endl;
    ss << "arcs:" << endl;
    for(ArcIterator< VectorFst<LogArc> > aiter(fst, stateId); !aiter.Done(); aiter.Next()) {
      const LogArc &arc = aiter.Value();
      ss << arc.ilabel << ":" << arc.olabel << " " <<  stateId << "-->" << arc.nextstate << " " << arc.weight << endl;
    } 
    ss << endl;
  }
  return ss.str();
}

string FstUtils::PrintFstSummary(VectorFst<LogPairArc>& fst) {
  stringstream ss;
  ss << "=======" << endl;
  ss << "states:" << endl;
  ss << "=======" << endl << endl;
  for(StateIterator< VectorFst<LogPairArc> > siter(fst); !siter.Done(); siter.Next()) {
    const LogPairArc::StateId &stateId = siter.Value();
    string initial = fst.Start() == stateId? " START " : "";
    ss << "state:" << stateId << initial << " FinalScore=" <<  PrintPair(fst.Final(stateId)) << endl;
    ss << "arcs:" << endl;
    for(ArcIterator< VectorFst<LogPairArc> > aiter(fst, stateId); !aiter.Done(); aiter.Next()) {
      const LogPairArc &arc = aiter.Value();
      ss << arc.ilabel << ":" << arc.olabel << " " <<  stateId;
      ss << "-->" << arc.nextstate << " " << PrintPair(arc.weight) << endl;
    } 
    ss << endl;
  }
  return ss.str();
}

string FstUtils::PrintFstSummary(VectorFst<LogTripleArc>& fst) {
  stringstream ss;
  ss << "=======" << endl;
  ss << "states:" << endl;
  ss << "=======" << endl << endl;
  for(StateIterator< VectorFst<LogTripleArc> > siter(fst); !siter.Done(); siter.Next()) {
    const LogTripleArc::StateId &stateId = siter.Value();
    string initial = fst.Start() == stateId? " START " : "";
    ss << "state:" << stateId << initial << " FinalScore=" <<  PrintTriple(fst.Final(stateId)) << endl;
    ss << "arcs:" << endl;
    for(ArcIterator< VectorFst<LogTripleArc> > aiter(fst, stateId); !aiter.Done(); aiter.Next()) {
      const LogTripleArc &arc = aiter.Value();
      ss << arc.ilabel << ":" << arc.olabel << " " <<  stateId;
      ss << "-->" << arc.nextstate << " " << PrintTriple(arc.weight) << endl;
    } 
    ss << endl;
  }
  return ss.str();
}

string FstUtils::PrintFstSummary(VectorFst<LogQuadArc>& fst) {
  stringstream ss;
  ss << "=======" << endl;
  ss << "states:" << endl;
  ss << "=======" << endl << endl;
  for(StateIterator< VectorFst<LogQuadArc> > siter(fst); !siter.Done(); siter.Next()) {
    const LogQuadArc::StateId &stateId = siter.Value();
    string initial = fst.Start() == stateId? " START " : "";
    ss << "state:" << stateId << initial << " FinalScore=" <<  PrintQuad(fst.Final(stateId)) << endl;
    ss << "arcs:" << endl;
    for(ArcIterator< VectorFst<LogQuadArc> > aiter(fst, stateId); !aiter.Done(); aiter.Next()) {
      const LogQuadArc &arc = aiter.Value();
      ss << arc.ilabel << ":" << arc.olabel << " " <<  stateId;
      ss << "-->" << arc.nextstate << " " << PrintQuad(arc.weight) << endl;
    } 
    ss << endl;
  }
  return ss.str();
}

// assumption: the fst has one or more final state
// output: all states which used to be final in the original fst, are not final now, but they go to a new final state with epsion input/output labels and the transition's weight = the original stopping weight of the corresponding state.
void FstUtils::MakeOneFinalState(fst::VectorFst<LogQuadArc>& fst) {
  int finalStateId = fst.AddState();

  for(StateIterator< VectorFst<LogQuadArc> > siter(fst); !siter.Done(); siter.Next()) {
    LogQuadWeight stoppingWeight = fst.Final(siter.Value());
    if(stoppingWeight != LogQuadWeight::Zero()) {
      fst.SetFinal(siter.Value(), LogQuadWeight::Zero());
      fst.AddArc(siter.Value(), LogQuadArc(FstUtils::EPSILON, FstUtils::EPSILON, stoppingWeight, finalStateId));
    }
  }

  fst.SetFinal(finalStateId, LogQuadWeight::One());
}

// return the id of a final states in this fst. if no final state found, returns -1.
int FstUtils::FindFinalState(const fst::VectorFst<LogQuadArc>& fst) {
  for(StateIterator< VectorFst<LogQuadArc> > siter(fst); !siter.Done(); siter.Next()) {
    const LogQuadArc::StateId &stateId = siter.Value();
    if(fst.Final(stateId) != LogQuadWeight::Zero()) {
      return (int) stateId;
    }
  }
  return -1;
}

// make sure that these two fsts have the same structure, including state ids, input/output labels, and nextstates, but not the weights.
bool FstUtils::AreShadowFsts(const fst::VectorFst<LogQuadArc>& fst1, const fst::VectorFst<LogArc>& fst2) {
  // verify number of states
  if(fst1.NumStates() != fst2.NumStates()) {
    cerr << "different state count" << endl;
    return false;
  }

  StateIterator< VectorFst<LogQuadArc> > siter1(fst1);
  StateIterator< VectorFst<LogArc> > siter2(fst2);
  while(!siter1.Done() || !siter2.Done()) {
    // verify state ids
    int from1 = siter1.Value(), from2 = siter2.Value();
    if(from1 != from2) {
      cerr << "different state ids" << endl;
      return false;
    }
    
    ArcIterator< VectorFst<LogQuadArc> > aiter1(fst1, from1);
    ArcIterator< VectorFst<LogArc> > aiter2(fst2, from2);
    while(!aiter1.Done() || !aiter2.Done()) {
      // verify number of arcs leaving this state
      if(aiter1.Done() || aiter2.Done()) {
	cerr << "different number of arcs leaving " << from1 << endl;
	return false;
      }

      // verify the arc input/output labels
      if(aiter1.Value().ilabel != aiter2.Value().ilabel) {
	cerr << "different input label" << endl;
	return false;
      } else if(aiter1.Value().olabel != aiter2.Value().olabel) {
	cerr << "different output label" << endl;
	return false;
      }
     
      // verify the arc next state
      if(aiter1.Value().nextstate != aiter2.Value().nextstate) {
	cerr << "different next states" << endl;
	return false;
      }
      
      // advance the iterators
      aiter1.Next();
      aiter2.Next();
    }

    // advance hte iterators
    siter1.Next();
    siter2.Next();
  }

  return true;
}

// assumption: 
// - acyclic fst
// - the last element in a LogQuadWeight represent the logprob of using this arc, not conditioned on anything.
void FstUtils::SampleFst(const fst::VectorFst<LogQuadArc>& fst, fst::VectorFst<LogQuadArc>& sampledFst, int sampleSize) {
  assert(sampledFst.NumStates() == 0 && fst.NumStates() > 0);
  
  // for debugging only
  //cerr << "sampling" << endl;
  
  // create start and final states of sampledFst
  int sampledFstStartState = sampledFst.AddState();
  sampledFst.SetStart(sampledFstStartState);
  int sampledFstFinalState = sampledFst.AddState();

  // create a LogArc shadow fst of 'fst' which can be later used to compute potentials (LogQuadArc is too complex to compute potentials with)
  fst::VectorFst<fst::LogArc> probFst;
  fst::ArcMap(fst, &probFst, LogQuadToLogMapper());
  assert(AreShadowFsts(fst, probFst));

  // compute the potential of each state in fst towards the final state (i.e. unnormalized p(state->final))
  std::vector<fst::LogWeight> betas;
  fst::ShortestDistance(probFst, &betas, true);

  // set the stopping weight of the sampledFst's final state to the inverse of beta[0] = \sum_{path \in fst} weight(path), 
  // effectively making each complete path in the sampledFst has a proper probability according to the path distribution defined
  // as weights on the fst
  sampledFst.SetFinal(sampledFstFinalState, EncodeQuad(0.0, 0.0, 0.0, 0.0 - betas[probFst.Start()].Value()));

  // now we have all the necessary ingredients to start sampling. lets go!
  int samplesCounter = 0;
  srand(time(0));
  assert(sampleSize > 0);
  while(samplesCounter++ < sampleSize) {
    int currentFstState = fst.Start();
    int currentSampledFstState = sampledFstStartState;
    
    // for debugging only
    //cerr << endl << "sample #" << samplesCounter << endl;

    // keep adding arcs until we hit a final fst state
    while(fst.Final(currentFstState) == LogQuadWeight::Zero()) {

      // for debugging only
      //cerr << "currentFstState = " << currentFstState << endl;

      // enumerate the arcs leaving the current fst state, and calculate their respective scores
      // which define the likelihood of taking that arc now. The arc score is 
      // Times(arc's unconditional prob, toState's beta potential)
      std::vector<double> arcScores;
      double totalScores = 0;
      for(ArcIterator< VectorFst<LogQuadArc> > aiter(fst, currentFstState); !aiter.Done(); aiter.Next()) {
	float dummy, arcProb;
	DecodeQuad(aiter.Value().weight, dummy, dummy, dummy, arcProb);
	double score = nExp(Times(arcProb, betas[aiter.Value().nextstate]).Value());
	arcScores.push_back(score);
	totalScores += score;
      }

      // generate a random number between 0 and totalScores
      double randomScore = ((double) rand() / (RAND_MAX)) * totalScores;
      
      // choose one of the available arcs based on the generated number
      std::vector<double>::const_iterator scoreIterator = arcScores.begin();
      LogQuadArc chosenArc;
      bool chosenArcIsSet = false;
      for(ArcIterator< VectorFst<LogQuadArc> > aiter(fst, currentFstState); 
	  !aiter.Done() && scoreIterator != arcScores.end(); 
	  aiter.Next(), scoreIterator++) {
	if(randomScore <= *scoreIterator) {
	  chosenArc = aiter.Value();
	  chosenArcIsSet = true;
	  break;
	} else {
	  randomScore -= *scoreIterator;
	}
      }
      assert(chosenArcIsSet);

      // for debugging only
      //cerr << "chosen arc->" << chosenArc.nextstate << " with stopping weight " << PrintQuad(fst.Final(chosenArc.nextstate)) << " " << chosenArc.ilabel << ":" << chosenArc.olabel << " / " << PrintQuad(chosenArc.weight) << endl;

      // move currentFstState
      currentFstState = chosenArc.nextstate;
      
      // are we done with this sample?
      if(fst.Final(chosenArc.nextstate) == LogQuadWeight::Zero()) {
	// not yet
	chosenArc.nextstate = sampledFst.AddState();
      } else {
	// done, yay!
	chosenArc.nextstate = sampledFstFinalState;
      }
      
      // add the chosen arc to sampledFst
      sampledFst.AddArc(currentSampledFstState, chosenArc);

      // move currentSampledFstState
      currentSampledFstState = chosenArc.nextstate;      
    }
  }
}

#include "LatentCrfModel.h"

using namespace std;
using namespace OptAlgorithm;

// singlenton instance definition and trivial initialization
LatentCrfModel* LatentCrfModel::instance = 0;
int LatentCrfModel::START_OF_SENTENCE_Y_VALUE = -100;
unsigned LatentCrfModel::NULL_POSITION = -100;

LatentCrfModel& LatentCrfModel::GetInstance() {
  if(!instance) {
    assert(false);
  }
  return *instance;
}

LatentCrfModel::~LatentCrfModel() {
  delete &lambda->srcTypes;
  delete lambda;
}

// initialize model weights to zeros
LatentCrfModel::LatentCrfModel(const string &textFilename, 
			       const string &outputPrefix, 
			       LearningInfo &learningInfo, 
			       unsigned FIRST_LABEL_ID,
			       LatentCrfModel::Task task) : 
  gaussianSampler(0.0, 10.0),
  UnsupervisedSequenceTaggingModel(textFilename) {

  countOfConstrainedLambdaParameters = 0;

  AddEnglishClosedVocab();
  
  if(learningInfo.mpiWorld->rank() == 0) {
    vocabEncoder.PersistVocab(outputPrefix + string(".vocab"));
  }

  // all processes will now read from the .vocab file master is writing. so, lets wait for the master before we continue.
  bool syncAllProcesses;
  mpi::broadcast<bool>(*learningInfo.mpiWorld, syncAllProcesses, 0);

  // read the .vocab file master just wrote
  VocabDecoder *vocabDecoder = new VocabDecoder(outputPrefix + string(".vocab"));
  lambda = new LogLinearParams(*vocabDecoder);
  
  // set member variables
  this->textFilename = textFilename;
  this->outputPrefix = outputPrefix;
  this->learningInfo = learningInfo;
  this->lambda->SetLearningInfo(learningInfo);
  
  // hand-crafted weights for constrained features
  REWARD_FOR_CONSTRAINED_FEATURES = 10.0;
  PENALTY_FOR_CONSTRAINED_FEATURES = -10.0;

  // by default, we are operating in the training (not testing) mode
  testingMode = false;

  // what task is this core being used for? pos tagging? word alignment?
  this->task = task;
}

void LatentCrfModel::AddEnglishClosedVocab() {
  string closedVocab[] = {"a", "an", "the", 
			  "some", "one", "many", "few", "much",
			  "from", "to", "at", "by", "in", "on", "for", "as",
			  ".", ",", ";", "!", "?",
			  "is", "are", "be", "am", "was", "were",  
			  "has", "have", "had",
			  "i", "you", "he", "she", "they", "we", "it",
			  "myself", "himself", "themselves", "herself", "yourself",
			  "this", "that", "which",
			  "and", "or", "but", "not",
			  "what", "how", "why", "when",
			  "can", "could", "will", "would", "shall", "should", "must"};
  vector<string> closedVocabVector(closedVocab, closedVocab + sizeof(closedVocab) / sizeof(closedVocab[0]) );
  for(unsigned i = 0; i < closedVocabVector.size(); i++) {
    vocabEncoder.AddToClosedVocab(closedVocabVector[i]);
    // add the capital initial version as well
    if(closedVocabVector[i][0] >= 'a' && closedVocabVector[i][0] <= 'z') {
      closedVocabVector[i][0] += ('A' - 'a');
       vocabEncoder.AddToClosedVocab(closedVocabVector[i]);
    }
  }
}

// compute the partition function Z_\lambda(x)
// assumptions:
// - fst and betas are populated using BuildLambdaFst()
double LatentCrfModel::ComputeNLogZ_lambda(const fst::VectorFst<FstUtils::LogArc> &fst, const vector<FstUtils::LogWeight> &betas) {
  return betas[fst.Start()].Value();
}

// builds an FST to compute Z(x) = \sum_y \prod_i \exp \lambda h(y_i, y_{i-1}, x, i), but doesn't not compute the potentials
void LatentCrfModel::BuildLambdaFst(unsigned sentId, fst::VectorFst<FstUtils::LogArc> &fst) {

  PrepareExample(sentId);

  const vector<int> &x = GetObservableSequence(sentId);
  // arcs represent a particular choice of y_i at time step i
  // arc weights are -\lambda h(y_i, y_{i-1}, x, i)
  assert(fst.NumStates() == 0);
  int startState = fst.AddState();
  fst.SetStart(startState);

  // map values of y_{i-1} and y_i to fst states
   map<int, int> yIM1ToState, yIToState;
  assert(yIM1ToState.size() == 0);
  assert(yIToState.size() == 0);
  yIM1ToState[LatentCrfModel::START_OF_SENTENCE_Y_VALUE] = startState;

  // for each timestep
  for(int i = 0; i < x.size(); i++){

    // timestep i hasn't reached any states yet
    yIToState.clear();
    // from each state reached in the previous timestep
    for(map<int, int>::const_iterator prevStateIter = yIM1ToState.begin();
	prevStateIter != yIM1ToState.end();
	prevStateIter++) {

      int fromState = prevStateIter->second;
      int yIM1 = prevStateIter->first;
      // to each possible value of y_i
      for(set<int>::const_iterator yDomainIter = yDomain.begin();
	  yDomainIter != yDomain.end();
	  yDomainIter++) {

	int yI = *yDomainIter;
	
	// skip special classes
	if(yI == LatentCrfModel::START_OF_SENTENCE_Y_VALUE || yI == LatentCrfModel::END_OF_SENTENCE_Y_VALUE) {
	  continue;
	}

	// compute h(y_i, y_{i-1}, x, i)
	FastSparseVector<double> h;
	FireFeatures(yI, yIM1, sentId, i, enabledFeatureTypes, h);
	// compute the weight of this transition:
	// \lambda h(y_i, y_{i-1}, x, i), and multiply by -1 to be consistent with the -log probability representation
	double nLambdaH = -1.0 * lambda->DotProduct(h);
	// determine whether to add a new state or reuse an existing state which also represent label y_i and timestep i
	int toState;
	if(yIToState.count(yI) == 0) {
	  toState = fst.AddState();
	  yIToState[yI] = toState;
	  // is it a final state?
	  if(i == x.size() - 1) {
	    fst.SetFinal(toState, FstUtils::LogWeight::One());
	  }
	} else {
	  toState = yIToState[yI];
	}
	// now add the arc
	fst.AddArc(fromState, FstUtils::LogArc(yIM1, yI, nLambdaH, toState));
      } 
   }
    // now, that all states reached in step i have already been created, yIM1ToState has become irrelevant
    yIM1ToState = yIToState;
  }  
}

// builds an FST to compute Z(x) = \sum_y \prod_i \exp \lambda h(y_i, y_{i-1}, x, i), and computes the potentials
void LatentCrfModel::BuildLambdaFst(unsigned sentId, fst::VectorFst<FstUtils::LogArc> &fst, vector<FstUtils::LogWeight> &alphas, vector<FstUtils::LogWeight> &betas) {
  clock_t timestamp = clock();

  const vector<int> &x = GetObservableSequence(sentId);

  // first, build the fst
  BuildLambdaFst(sentId, fst);

  // then, compute potentials
  assert(alphas.size() == 0);
  ShortestDistance(fst, &alphas, false);
  assert(betas.size() == 0);
  ShortestDistance(fst, &betas, true);

  // debug info
  if(learningInfo.debugLevel == DebugLevel::SENTENCE) {
    cerr << " BuildLambdaFst() for this sentence took " << (float) (clock() - timestamp) / CLOCKS_PER_SEC << " sec. " << endl;
  }
}

// assumptions: 
// - fst is populated using BuildLambdaFst()
// - FXk is cleared
void LatentCrfModel::ComputeF(unsigned sentId,
			      const fst::VectorFst<FstUtils::LogArc> &fst,
			      const vector<FstUtils::LogWeight> &alphas, const vector<FstUtils::LogWeight> &betas,
			      FastSparseVector<LogVal<double> > &FXk) {
  clock_t timestamp = clock();
  
  const vector<int> &x = GetObservableSequence(sentId);

  assert(FXk.size() == 0);
  assert(fst.NumStates() > 0);
  
  // a schedule for visiting states such that we know the timestep for each arc
  set<int> iStates, iP1States;
  iStates.insert(fst.Start());

  // for each timestep
  for(int i = 0; i < x.size(); i++) {
    int xI = x[i];
    
    // from each state at timestep i
    for(set<int>::const_iterator iStatesIter = iStates.begin(); 
	iStatesIter != iStates.end(); 
	iStatesIter++) {
      int fromState = *iStatesIter;

      // for each arc leaving this state
      for(fst::ArcIterator< fst::VectorFst<FstUtils::LogArc> > aiter(fst, fromState); !aiter.Done(); aiter.Next()) {
	FstUtils::LogArc arc = aiter.Value();
	int yIM1 = arc.ilabel;
	int yI = arc.olabel;
	double arcWeight = arc.weight.Value();
	int toState = arc.nextstate;

	// compute marginal weight of passing on this arc
	double nLogMarginal = alphas[fromState].Value() + betas[toState].Value() + arcWeight;

	// for each feature that fires on this arc
	FastSparseVector<double> h;
	FireFeatures(yI, yIM1, sentId, i, enabledFeatureTypes, h);
	for(FastSparseVector<double>::iterator h_k = h.begin(); h_k != h.end(); ++h_k) {
	  // add the arc's h_k feature value weighted by the marginal weight of passing through this arc
	  if(FXk.find(h_k->first) == FXk.end()) {
	    FXk[h_k->first] = LogVal<double>(0.0);
	  }
	  FXk[h_k->first] += LogVal<double>(-1.0 * nLogMarginal, init_lnx()) * LogVal<double>(h_k->second);
	}

	// prepare the schedule for visiting states in the next timestep
	iP1States.insert(toState);
      } 
    }

    // prepare for next timestep
    iStates = iP1States;
    iP1States.clear();
  }  

  if(learningInfo.debugLevel == DebugLevel::SENTENCE) {
    cerr << "ComputeF() for this sentence took " << (float) (clock() - timestamp) / CLOCKS_PER_SEC << " sec." << endl;
  }
}			   

void LatentCrfModel::FireFeatures(int yI, int yIM1, unsigned sentId, int i, 
				  const std::vector<bool> &enabledFeatureTypes, 
				  FastSparseVector<double> &activeFeatures) { 
  if(task == Task::POS_TAGGING) {
    // fire the pos tagger features
    lambda->FireFeatures(yI, yIM1, GetObservableSequence(sentId), i, enabledFeatureTypes, activeFeatures);
  } else if(task == Task::WORD_ALIGNMENT) {
    // fire the word aligner features
    lambda->FireFeatures(yI, yIM1, GetObservableSequence(sentId), GetObservableContext(sentId), i, 
			 LatentCrfModel::START_OF_SENTENCE_Y_VALUE, NULL_POSITION, 
			 enabledFeatureTypes, activeFeatures);
    assert(GetObservableSequence(sentId).size() > 0);
  } else {
    assert(false);
  }
}

void LatentCrfModel::FireFeatures(unsigned sentId,
				  const fst::VectorFst<FstUtils::LogArc> &fst,
				  FastSparseVector<double> &h) {
  clock_t timestamp = clock();
  
  const vector<int> &x = GetObservableSequence(sentId);

  assert(fst.NumStates() > 0);
  
  // a schedule for visiting states such that we know the timestep for each arc
  set<int> iStates, iP1States;
  iStates.insert(fst.Start());

  // for each timestep
  for(int i = 0; i < x.size(); i++) {
    int xI = x[i];
    
    // from each state at timestep i
    for(set<int>::const_iterator iStatesIter = iStates.begin(); 
	iStatesIter != iStates.end(); 
	iStatesIter++) {
      int fromState = *iStatesIter;

      // for each arc leaving this state
      for(fst::ArcIterator< fst::VectorFst<FstUtils::LogArc> > aiter(fst, fromState); !aiter.Done(); aiter.Next()) {
	FstUtils::LogArc arc = aiter.Value();
	int yIM1 = arc.ilabel;
	int yI = arc.olabel;
	double arcWeight = arc.weight.Value();
	int toState = arc.nextstate;

	// for each feature that fires on this arc
	FireFeatures(yI, yIM1, sentId, i, enabledFeatureTypes, h);

	// prepare the schedule for visiting states in the next timestep
	iP1States.insert(toState);
      } 
    }

    // prepare for next timestep
    iStates = iP1States;
    iP1States.clear();
  }  

  if(learningInfo.debugLevel == DebugLevel::SENTENCE) {
    cerr << "FireFeatures() for this sentence took " << (float) (clock() - timestamp) / CLOCKS_PER_SEC << " sec." << endl;
  }
}			   

// assumptions: 
// - fst is populated using BuildThetaLambdaFst()
// - DXZk is cleared
void LatentCrfModel::ComputeD(unsigned sentId, const vector<int> &z, 
			      const fst::VectorFst<FstUtils::LogArc> &fst,
			      const vector<FstUtils::LogWeight> &alphas, const vector<FstUtils::LogWeight> &betas,
			      FastSparseVector<LogVal<double> > &DXZk) {
  clock_t timestamp = clock();

  const vector<int> &x = GetObservableSequence(sentId);
  // enforce assumptions
  assert(DXZk.size() == 0);

  // schedule for visiting states such that we know the timestep for each arc
  set<int> iStates, iP1States;
  iStates.insert(fst.Start());

  // for each timestep
  for(int i = 0; i < x.size(); i++) {
    int xI = x[i];
    int zI = z[i];
    
    // from each state at timestep i
    for(set<int>::const_iterator iStatesIter = iStates.begin(); 
	iStatesIter != iStates.end(); 
	iStatesIter++) {
      int fromState = *iStatesIter;

      // for each arc leaving this state
      for(fst::ArcIterator< fst::VectorFst<FstUtils::LogArc> > aiter(fst, fromState); !aiter.Done(); aiter.Next()) {
	FstUtils::LogArc arc = aiter.Value();
	int yIM1 = arc.ilabel;
	int yI = arc.olabel;
	double arcWeight = arc.weight.Value();
	int toState = arc.nextstate;

	// compute marginal weight of passing on this arc
	double nLogMarginal = alphas[fromState].Value() + betas[toState].Value() + arcWeight;

	// for each feature that fires on this arc
	FastSparseVector<double> h;
	FireFeatures(yI, yIM1, sentId, i, enabledFeatureTypes, h);
	for(FastSparseVector<double>::iterator h_k = h.begin(); h_k != h.end(); ++h_k) {

	  // add the arc's h_k feature value weighted by the marginal weight of passing through this arc
	  if(DXZk.find(h_k->first) == DXZk.end()) {
	    DXZk[h_k->first] = 0;
	  }
	  DXZk[h_k->first] += LogVal<double>(-nLogMarginal, init_lnx()) * LogVal<double>(h_k->second);
	}

	// prepare the schedule for visiting states in the next timestep
	iP1States.insert(toState);
      } 
    }

    // prepare for next timestep
    iStates = iP1States;
    iP1States.clear();
  }  

  if(learningInfo.debugLevel == DebugLevel::SENTENCE) {
    cerr << "ComputeD() for this sentence took " << (float) (clock() - timestamp) / CLOCKS_PER_SEC << " sec." << endl;
  }
}

// assumptions:
// - fst, betas are populated using BuildThetaLambdaFst()
double LatentCrfModel::ComputeNLogC(const fst::VectorFst<FstUtils::LogArc> &fst,
				    const vector<FstUtils::LogWeight> &betas) {
  double nLogC = betas[fst.Start()].Value();
  return nLogC;
}

// compute B(x,z) which can be indexed as: BXZ[y^*][z^*] to give B(x, z, z^*, y^*)
// assumptions: 
// - BXZ is cleared
// - fst, alphas, and betas are populated using BuildThetaLambdaFst
void LatentCrfModel::ComputeB(unsigned sentId, const vector<int> &z, 
			      const fst::VectorFst<FstUtils::LogArc> &fst, 
			      const vector<FstUtils::LogWeight> &alphas, const vector<FstUtils::LogWeight> &betas, 
			      map< int, map< int, LogVal<double> > > &BXZ) {
  // \sum_y [ \prod_i \theta_{z_i\mid y_i} e^{\lambda h(y_i, y_{i-1}, x, i)} ] \sum_i \delta_{y_i=y^*,z_i=z^*}
  assert(BXZ.size() == 0);

  const vector<int> &x = GetObservableSequence(sentId);

  // schedule for visiting states such that we know the timestep for each arc
  set<int> iStates, iP1States;
  iStates.insert(fst.Start());

  // for each timestep
  for(int i = 0; i < x.size(); i++) {
    int xI = x[i];
    int zI = z[i];
    
    // from each state at timestep i
    for(set<int>::const_iterator iStatesIter = iStates.begin(); 
	iStatesIter != iStates.end(); 
	iStatesIter++) {
      int fromState = *iStatesIter;

      // for each arc leaving this state
      for(fst::ArcIterator< fst::VectorFst<FstUtils::LogArc> > aiter(fst, fromState); !aiter.Done(); aiter.Next()) {
	FstUtils::LogArc arc = aiter.Value();
	int yIM1 = arc.ilabel;
	int yI = arc.olabel;
	double arcWeight = arc.weight.Value();
	int toState = arc.nextstate;

	// compute marginal weight of passing on this arc
	double nLogMarginal = alphas[fromState].Value() + betas[toState].Value() + arcWeight;

	// update the corresponding B value
	if(BXZ.count(yI) == 0 || BXZ[yI].count(zI) == 0) {
	  BXZ[yI][zI] = 0;
	}
	BXZ[yI][zI] += LogVal<double>(-nLogMarginal, init_lnx());

	// prepare the schedule for visiting states in the next timestep
	iP1States.insert(toState);
      } 
    }

    // prepare for next timestep
    iStates = iP1States;
    iP1States.clear();
  }
  
}

// compute B(x,z) which can be indexed as: BXZ[y^*][z^*] to give B(x, z, z^*, y^*)
// assumptions: 
// - BXZ is cleared
// - fst, alphas, and betas are populated using BuildThetaLambdaFst
void LatentCrfModel::ComputeB(unsigned sentId, const vector<int> &z, 
			      const fst::VectorFst<FstUtils::LogArc> &fst, 
			      const vector<FstUtils::LogWeight> &alphas, const vector<FstUtils::LogWeight> &betas, 
			      map< std::pair<int, int>, map< int, LogVal<double> > > &BXZ) {
  // \sum_y [ \prod_i \theta_{z_i\mid y_i} e^{\lambda h(y_i, y_{i-1}, x, i)} ] \sum_i \delta_{y_i=y^*,z_i=z^*}
  assert(BXZ.size() == 0);

  const vector<int> &x = GetObservableSequence(sentId);

  // schedule for visiting states such that we know the timestep for each arc
  set<int> iStates, iP1States;
  iStates.insert(fst.Start());

  // for each timestep
  for(int i = 0; i < x.size(); i++) {
    int xI = x[i];
    int zI = z[i];
    
    // from each state at timestep i
    for(set<int>::const_iterator iStatesIter = iStates.begin(); 
	iStatesIter != iStates.end(); 
	iStatesIter++) {
      int fromState = *iStatesIter;

      // for each arc leaving this state
      for(fst::ArcIterator< fst::VectorFst<FstUtils::LogArc> > aiter(fst, fromState); !aiter.Done(); aiter.Next()) {
	FstUtils::LogArc arc = aiter.Value();
	int yIM1 = arc.ilabel;
	int yI = arc.olabel;
	double arcWeight = arc.weight.Value();
	int toState = arc.nextstate;

	// compute marginal weight of passing on this arc
	double nLogMarginal = alphas[fromState].Value() + betas[toState].Value() + arcWeight;

	// update the corresponding B value
	std::pair<int, int> yIM1AndyI = std::pair<int, int>(yIM1, yI);
	if(BXZ.count(yIM1AndyI) == 0 || BXZ[yIM1AndyI].count(zI) == 0) {
	  BXZ[yIM1AndyI][zI] = 0;
	}
	BXZ[yIM1AndyI][zI] += LogVal<double>(-nLogMarginal, init_lnx());

	// prepare the schedule for visiting states in the next timestep
	iP1States.insert(toState);
      } 
    }
  
    // prepare for next timestep
    iStates = iP1States;
    iP1States.clear();
  }
  
  //  cerr << "}\n";
}

double LatentCrfModel::GetNLogTheta(int yim1, int yi, int zi, unsigned exampleId) {
  if(task == Task::POS_TAGGING) {
    if(learningInfo.zIDependsOnYIM1) {
      return nLogThetaGivenTwoLabels[pair<int,int>(yim1, yi)][zi];
    } else {
      return nLogThetaGivenOneLabel[yi][zi];
    }
  } else if(task == Task::WORD_ALIGNMENT) {
    assert(!learningInfo.zIDependsOnYIM1); // not implemented for the word alignment model
    vector<int> &srcSent = GetObservableContext(exampleId);
    vector<int> &tgtSent = GetObservableSequence(exampleId);
    assert(find(tgtSent.begin(), tgtSent.end(), zi) != tgtSent.end());
    assert(yi < srcSent.size());
    assert(nLogThetaGivenOneLabel.params.count( srcSent[yi] ) > 0);
    return nLogThetaGivenOneLabel[ srcSent[yi] ] [zi];
  } else {
    assert(false);
  }
}

// build an FST which path sums to 
// -log \sum_y [ \prod_i \theta_{z_i\mid y_i} e^{\lambda h(y_i, y_{i-1}, x, i)} ]
void LatentCrfModel::BuildThetaLambdaFst(unsigned sentId, const vector<int> &z, 
					 fst::VectorFst<FstUtils::LogArc> &fst, 
					 vector<FstUtils::LogWeight> &alphas, vector<FstUtils::LogWeight> &betas) {

  clock_t timestamp = clock();

  PrepareExample(sentId);

  const vector<int> &x = GetObservableSequence(sentId);

  // arcs represent a particular choice of y_i at time step i
  // arc weights are -log \theta_{z_i|y_i} - \lambda h(y_i, y_{i-1}, x, i)
  assert(fst.NumStates() == 0);
  int startState = fst.AddState();
  fst.SetStart(startState);
  
  // map values of y_{i-1} and y_i to fst states
  map<int, int> yIM1ToState, yIToState;
  assert(yIM1ToState.size() == 0);
  assert(yIToState.size() == 0);

  yIM1ToState[LatentCrfModel::START_OF_SENTENCE_Y_VALUE] = startState;

  // for each timestep
  for(int i = 0; i < x.size(); i++){

    // timestep i hasn't reached any states yet
    yIToState.clear();
    // from each state reached in the previous timestep
    for(map<int, int>::const_iterator prevStateIter = yIM1ToState.begin();
	prevStateIter != yIM1ToState.end();
	prevStateIter++) {

      int fromState = prevStateIter->second;
      int yIM1 = prevStateIter->first;
      // to each possible value of y_i
      for(set<int>::const_iterator yDomainIter = yDomain.begin();
	  yDomainIter != yDomain.end();
	  yDomainIter++) {

	int yI = *yDomainIter;

	// skip special classes
	if(yI == LatentCrfModel::START_OF_SENTENCE_Y_VALUE || yI == END_OF_SENTENCE_Y_VALUE) {
	  continue;
	}

	// compute h(y_i, y_{i-1}, x, i)
	FastSparseVector<double> h;
	FireFeatures(yI, yIM1, sentId, i, enabledFeatureTypes, h);

	// prepare -log \theta_{z_i|y_i}
	int zI = z[i];
	
	double nLogTheta_zI_y = GetNLogTheta(yIM1, yI, zI, sentId);
	assert(!std::isnan(nLogTheta_zI_y) && !std::isinf(nLogTheta_zI_y));

	// compute the weight of this transition: \lambda h(y_i, y_{i-1}, x, i), and multiply by -1 to be consistent with the -log probability representatio
	double nLambdaH = -1.0 * lambda->DotProduct(h);
	assert(!std::isnan(nLambdaH) && !std::isinf(nLambdaH));
	double weight = nLambdaH + nLogTheta_zI_y;
	assert(!std::isnan(weight) && !std::isinf(weight));

	// determine whether to add a new state or reuse an existing state which also represent label y_i and timestep i
	int toState;	
	if(yIToState.count(yI) == 0) {
	  toState = fst.AddState();
	  yIToState[yI] = toState;
	  // is it a final state?
	  if(i == x.size() - 1) {
	    fst.SetFinal(toState, FstUtils::LogWeight::One());
	  }
	} else {
	  toState = yIToState[yI];
	}
	// now add the arc
	fst.AddArc(fromState, FstUtils::LogArc(yIM1, yI, weight, toState));
      }
    }
    // now, that all states reached in step i have already been created, yIM1ToState has become irrelevant
    yIM1ToState = yIToState;
  }

  // compute forward/backward state potentials
  assert(alphas.size() == 0);
  assert(betas.size() == 0);
  ShortestDistance(fst, &alphas, false);
  ShortestDistance(fst, &betas, true);

  if(learningInfo.debugLevel == DebugLevel::SENTENCE) {
    cerr << " BuildThetaLambdaFst() for this sentence took " << (float) (clock() - timestamp) / CLOCKS_PER_SEC << " sec. " << endl;
  }
}

void LatentCrfModel::SupervisedTrain(string goldLabelsFilename) {
  // encode labels
  assert(goldLabelsFilename.size() != 0);
  VocabEncoder labelsEncoder(goldLabelsFilename, FIRST_ALLOWED_LABEL_VALUE);
  labels.clear();
  labelsEncoder.Read(goldLabelsFilename, labels);
  
  // use lbfgs to fit the lambda CRF parameters
  double *lambdasArray = lambda->GetParamWeightsArray();
  unsigned lambdasArrayLength = lambda->GetParamsCount();
  lbfgs_parameter_t lbfgsParams = SetLbfgsConfig();  
  lbfgsParams.max_iterations = 10;
  lbfgsParams.m = 50;
  lbfgsParams.max_linesearch = 20;
  double optimizedNllYGivenX = 0;
  int allSents = -1;
  
  int lbfgsStatus = lbfgs(lambdasArrayLength, lambdasArray, &optimizedNllYGivenX, 
			  LbfgsCallbackEvalYGivenXLambdaGradient, LbfgsProgressReport, &allSents, &lbfgsParams);
  if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH && learningInfo.mpiWorld->rank() == 0) {
    cerr << "master" << learningInfo.mpiWorld->rank() << ": lbfgsStatusCode = " << LbfgsUtils::LbfgsStatusIntToString(lbfgsStatus) << " = " << lbfgsStatus << endl;
  }
  if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": loglikelihood_{p(y|x)}(\\lambda) = " << -optimizedNllYGivenX << endl;
  }
  
  // optimize theta (i.e. multinomial) parameters to maximize the likeilhood of the data
  MultinomialParams::ConditionalMultinomialParam<int> thetaMle;
  MultinomialParams::MultinomialParam thetaMleMarginals;
  // for each sentence
  for(unsigned sentId = 0; sentId < examplesCount; sentId++) {
    // collect number of times each theta parameter has been used
    vector<int> &x_s = GetObservableContext(sentId);
    vector<int> &z = GetObservableSequence(sentId);
    vector<int> &y = labels[sentId];
    assert(z.size() == y.size());
    for(unsigned i = 0; i < z.size(); i++) {
      if(task == Task::POS_TAGGING) {
	thetaMle[y[i]][z[i]] += 1;
	thetaMleMarginals[y[i]] += 1;
      } else if(task == Task::WORD_ALIGNMENT) {
	thetaMle[ x_s[y[i]] ][ z[i] ] += 1;
	thetaMleMarginals[ x_s[y[i]] ] += 1;
      } else {
	assert(false);
      }
    }
  }
  // normalize thetas
  NormalizeThetaMle<int>(thetaMle, thetaMleMarginals);
  nLogThetaGivenOneLabel = thetaMle;
  // compute likelihood of \theta for z|y
  assert(!learningInfo.zIDependsOnYIM1); // supervised training does not support this configuration of the model
  double NllZGivenY = 0; 
  for(unsigned sentId = 0; sentId < examplesCount; sentId++) {
    vector<int> &z = GetObservableSequence(sentId);
    vector<int> &y = labels[sentId];
    for(unsigned i = 0; i < z.size(); i++){ 
      int DONT_CARE = -100;
      NllZGivenY += GetNLogTheta(DONT_CARE, y[i], z[i], sentId);
    }
  } 
  if(learningInfo.debugLevel == DebugLevel::MINI_BATCH && learningInfo.mpiWorld->rank() == 0) {
    cerr << "master" << learningInfo.mpiWorld->rank() << ": loglikelihood_{p(z|y)}(\\theta) = " << - NllZGivenY << endl;
    cerr << "master" << learningInfo.mpiWorld->rank() << ": loglikelihood_{p(z|x)}(\\theta, \\lambda) = " << - optimizedNllYGivenX - NllZGivenY << endl;
  }
}

void LatentCrfModel::Train() {
  testingMode = false;
  switch(learningInfo.optimizationMethod.algorithm) {
  case BLOCK_COORD_DESCENT:
  case SIMULATED_ANNEALING:
    BlockCoordinateDescent();
    break;
    /*  case EXPECTATION_MAXIMIZATION:
    ExpectationMaximization();
    break;*/
  default:
    assert(false);
    break;
  }
}

// to interface with the simulated annealing library at http://www.taygeta.com/annealing/simanneal.html
float LatentCrfModel::EvaluateNll(float *lambdasArray) {
  // singleton
  LatentCrfModel &model = LatentCrfModel::GetInstance();
  // unconstrained lambda parameters count
  unsigned lambdasCount = model.lambda->GetParamsCount() - model.countOfConstrainedLambdaParameters;
  // which sentences to work on?
  static int fromSentId = 0;
  if(fromSentId >= model.examplesCount) {
    fromSentId = 0;
  }
  double *dblLambdasArray = model.lambda->GetParamWeightsArray();
  for(unsigned i = 0; i < lambdasCount; i++) {
    dblLambdasArray[i] = (double)lambdasArray[i];
  }
  // next time, work on different sentences
  fromSentId += model.learningInfo.optimizationMethod.subOptMethod->miniBatchSize;
  // call the other function ;-)
  void *ptrFromSentId = &fromSentId;
  double dummy[lambdasCount];
  float objective = (float)LbfgsCallbackEvalZGivenXLambdaGradient(ptrFromSentId, dblLambdasArray, dummy, lambdasCount, 1.0);
  return objective;
}

// lbfgs' callback function for evaluating -logliklihood(y|x) and its d/d_\lambda
// this is needed for supervised training of the CRF
double LatentCrfModel::LbfgsCallbackEvalYGivenXLambdaGradient(void *uselessPtr,
							      const double *lambdasArray,
							      double *gradient,
							      const int lambdasCount,
							      const double step) {
  // this method needs to be reimplemented/modified according to https://github.com/ldmt-muri/alignment-with-openfst/issues/83
  assert(false);
  
  LatentCrfModel &model = LatentCrfModel::GetInstance();
  
  if(model.learningInfo.debugLevel  >= DebugLevel::REDICULOUS){
    cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": entered LbfgsCallbackEvalYGivenXLambdaGradient" << endl;
  }

  // important note: the parameters array manipulated by liblbfgs is the same one used in lambda. so, the new weights are already in effect

  double Nll = 0;
  FastSparseVector<double> nDerivative;
  unsigned from = 0, to = model.examplesCount;
  assert(model.examplesCount == model.labels.size());

  // for each training example (x, y)
  for(unsigned sentId = from; sentId < to; sentId++) {
    if(sentId % model.learningInfo.mpiWorld->size() != model.learningInfo.mpiWorld->rank()) {
      continue;
    }

    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": proessing sentId " << sentId << endl;
    }

    // Make |y| = |x|
    assert(model.GetObservableSequence(sentId).size() == model.labels[sentId].size());
    const vector<int> &x = model.GetObservableSequence(sentId);
    vector<int> &y = model.labels[sentId];

    // build the FSTs
    fst::VectorFst<FstUtils::LogArc> lambdaFst;
    vector<FstUtils::LogWeight> lambdaAlphas, lambdaBetas;
    model.BuildLambdaFst(sentId, lambdaFst, lambdaAlphas, lambdaBetas);

    // compute the Z value for this sentence
    double nLogZ = model.ComputeNLogZ_lambda(lambdaFst, lambdaBetas);
    if(std::isnan(nLogZ) || std::isinf(nLogZ)) {
      if(model.learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	cerr << "ERROR: nLogZ = " << nLogZ << ". my mistake. will halt!" << endl;
      }
      assert(false);
    } 
    
    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": nLogZ = " << nLogZ << endl;
    }

    // compute the F map fro this sentence
    FastSparseVector<LogVal<double> > FSparseVector;
    model.ComputeF(sentId, lambdaFst, lambdaAlphas, lambdaBetas, FSparseVector);
    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": F.size = " << FSparseVector.size();
    }

    // compute feature aggregate values on the gold labels of this sentence
    FastSparseVector<double> goldFeatures;
    for(unsigned i = 0; i < x.size(); i++) {
      model.FireFeatures(y[i], i==0? LatentCrfModel::START_OF_SENTENCE_Y_VALUE:y[i-1], sentId, i, model.enabledFeatureTypes, goldFeatures);
    }
    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": size of gold features = " << goldFeatures.size() << endl; 
    }

    // update the loglikelihood
    double dotProduct = model.lambda->DotProduct(goldFeatures);
    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": dotProduct of gold features with crf params = " << dotProduct << endl;
    }
    if(nLogZ == 0 ||  dotProduct == 0 || nLogZ - dotProduct == 0) {
      cerr << "something is wrong! tell me more about lambdaFst." << endl << "lambdaFst has " << lambdaFst.NumStates() << "states. " << endl;
      if(model.learningInfo.mpiWorld->rank() == 0) {
	cerr << "lambda parameters are: ";
	model.lambda->PrintParams();
      }
    } 
    Nll += - dotProduct - nLogZ;
    if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": Nll = " << Nll << endl; 
    }

    // update the gradient
    for(FastSparseVector<LogVal<double> >::iterator fIter = FSparseVector.begin(); fIter != FSparseVector.end(); ++fIter) {
      double nLogf = fIter->second.s_? fIter->second.v_ : -fIter->second.v_; // multiply the inner logF representation by -1.
      double nFOverZ = - MultinomialParams::nExp(nLogf - nLogZ);
      if(std::isnan(nFOverZ) || std::isinf(nFOverZ)) {
	if(model.learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	  cerr << "ERROR: nFOverZ = " << nFOverZ << ", nLogf = " << nLogf << ". my mistake. will halt!" << endl;
	}
        assert(false);
      }
      nDerivative[fIter->first] += - goldFeatures[fIter->first] - nFOverZ;
    }
    if(model.learningInfo.debugLevel >= DebugLevel::SENTENCE) {
      cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": nDerivative size = " << nDerivative.size() << endl;
    }

    if(model.learningInfo.debugLevel >= DebugLevel::MINI_BATCH) {
      cerr << "." << model.learningInfo.mpiWorld->rank();
    }
  }

  // write the gradient in the array 'gradient' (which is pre-allocated by the lbfgs library)
  // init gradient to zero
  for(unsigned gradientIter = 0; gradientIter < model.lambda->GetParamsCount(); gradientIter++) {
    gradient[gradientIter] = 0;
  }
  // for each active feature 
  for(FastSparseVector<double>::iterator derivativeIter = nDerivative.begin(); 
      derivativeIter != nDerivative.end(); 
      ++derivativeIter) {
    // set active feature's value in the gradient
    gradient[derivativeIter->first] = derivativeIter->second;
  }

  // accumulate Nll from all processes

  // the all_reduce way => Nll
  mpi::all_reduce<double>(*model.learningInfo.mpiWorld, Nll, Nll, std::plus<double>());

  
  if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS /*&& model.learningInfo.mpiWorld->rank() == 0*/) {
    cerr << "rank" << model.learningInfo.mpiWorld->rank() << ": Nll after all_reduce = " << Nll << endl;
  }

  // accumulate the gradient vectors from all processes
  vector<double> gradientVector(model.lambda->GetParamsCount());
  for(unsigned gradientIter = 0; gradientIter < model.lambda->GetParamsCount(); gradientIter++) {
    gradientVector[gradientIter] = gradient[gradientIter];
  }

  mpi::all_reduce<vector<double> >(*model.learningInfo.mpiWorld, gradientVector, gradientVector, AggregateVectors2());
  assert(gradientVector.size() == lambdasCount);
  for(int i = 0; i < gradientVector.size(); i++) {
    gradient[i] = gradientVector[i];
    assert(!std::isnan(gradient[i]) || !std::isinf(gradient[i]));
  }
  
  if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank #" << model.learningInfo.mpiWorld->rank() << ": exiting LbfgsCallbackEvalYGivenXLambdaGradient" << endl;
  }

  if(model.learningInfo.debugLevel >= DebugLevel::MINI_BATCH && model.learningInfo.mpiWorld->rank() == 0) {
    cerr << "master" << model.learningInfo.mpiWorld->rank() << ": eval(y|x) = " << Nll << endl;
  }
  return Nll;
}

// the callback function lbfgs calls to compute the -log likelihood(z|x) and its d/d_\lambda
// this function is not expected to be executed by any slave; only the master process with rank 0
double LatentCrfModel::LbfgsCallbackEvalZGivenXLambdaGradient(void *ptrFromSentId,
							      const double *lambdasArray,
							      double *gradient,
							      const int lambdasCount,
							      const double step) {
  
  LatentCrfModel &model = LatentCrfModel::GetInstance();
  // only the master executes the lbfgs() call and therefore only the master is expected to come here
  assert(model.learningInfo.mpiWorld->rank() == 0);

  // important note: the parameters array manipulated by liblbfgs is the same one used in lambda. so, the new weights are already in effect

  // the master tells the slaves that he needs their help to collectively compute the gradient
  bool NEED_HELP = true;
  mpi::broadcast<bool>(*model.learningInfo.mpiWorld, NEED_HELP, 0);

  // the master broadcasts its lambda parameters so that all processes are on the same page while 
  // collectively computing the gradient
  mpi::broadcast<vector<double> >( (*model.learningInfo.mpiWorld), (model.lambda->paramWeights), 0);

  // even the master needs to process its share of sentences
  vector<double> gradientPiece(model.lambda->GetParamsCount(), 0.0), reducedGradient;
  double NllPiece = model.ComputeNllZGivenXAndLambdaGradient(gradientPiece);
  double reducedNll = 0;

  // now, the master aggregates gradient pieces computed by the slaves
  mpi::reduce< vector<double> >(*model.learningInfo.mpiWorld, gradientPiece, reducedGradient, AggregateVectors2(), 0);
  mpi::reduce<double>(*model.learningInfo.mpiWorld, NllPiece, reducedNll, std::plus<double>(), 0);

  // fill in the gradient array allocated by lbfgs
  if(model.learningInfo.optimizationMethod.subOptMethod->regularizer == Regularizer::L2) {
    // this is where the L2 term is added to both the gradient and objective function
    for(unsigned i = 0; i < model.lambda->GetParamsCount(); i++) {
      gradient[i] = reducedGradient[i] + 2.0 * model.learningInfo.optimizationMethod.subOptMethod->regularizationStrength * lambdasArray[i];
      reducedNll += model.learningInfo.optimizationMethod.subOptMethod->regularizationStrength * lambdasArray[i] * lambdasArray[i];
      assert(!std::isnan(gradient[i]) || !std::isinf(gradient[i]));
    } 
  } else {
    assert(gradientPiece.size() == reducedGradient.size() && gradientPiece.size() == model.lambda->GetParamsCount());
    for(unsigned i = 0; i < model.lambda->GetParamsCount(); i++) {
      gradient[i] = reducedGradient[i];
      assert(!std::isnan(gradient[i]) || !std::isinf(gradient[i]));
    } 
  }

  if(model.learningInfo.debugLevel == DebugLevel::MINI_BATCH) {
    if(model.learningInfo.optimizationMethod.subOptMethod->regularizer == Regularizer::L2) {
      cerr << " objective = " << reducedNll << endl;
    } else if(model.learningInfo.optimizationMethod.subOptMethod->regularizer == Regularizer::L1) {
      cerr << " unregularized objective = " << reducedNll << endl;	
    } else {
      cerr << " objective = " << reducedNll << endl;
    }
  }
  
  // fill in the gradient array with respective values
  assert(model.countOfConstrainedLambdaParameters == 0); // not implemented

  return reducedNll;
}

// -loglikelihood is the return value
double LatentCrfModel::ComputeNllZGivenXAndLambdaGradient(vector<double> &derivativeWRTLambdaSparseVector) {

  // for each sentence in this mini batch, aggregate the Nll and its derivatives across sentences
  double Nll = 0;

  // mini batch is not supported
  assert(learningInfo.optimizationMethod.subOptMethod->miniBatchSize == 0 || 
	 learningInfo.optimizationMethod.subOptMethod->miniBatchSize == examplesCount);

  // for each training example
  for(int sentId = 0; sentId < examplesCount; sentId++) {
    
    // sentId is assigned to the process with rank = sentId % world.size()
    if(sentId % learningInfo.mpiWorld->size() != learningInfo.mpiWorld->rank()) {
      continue;
    }

    // build the FSTs
    fst::VectorFst<FstUtils::LogArc> thetaLambdaFst, lambdaFst;
    vector<FstUtils::LogWeight> thetaLambdaAlphas, lambdaAlphas, thetaLambdaBetas, lambdaBetas;
    BuildThetaLambdaFst(sentId, GetObservableSequence(sentId), thetaLambdaFst, thetaLambdaAlphas, thetaLambdaBetas);
    BuildLambdaFst(sentId, lambdaFst, lambdaAlphas, lambdaBetas);

    // compute the D map for this sentence
    FastSparseVector<LogVal<double> > DSparseVector;
    ComputeD(sentId, GetObservableSequence(sentId), thetaLambdaFst, thetaLambdaAlphas, thetaLambdaBetas, DSparseVector);

    // compute the C value for this sentence
    double nLogC = ComputeNLogC(thetaLambdaFst, thetaLambdaBetas);
    if(std::isnan(nLogC) || std::isinf(nLogC)) {
      if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	cerr << "ERROR: nLogC = " << nLogC << ". my mistake. will halt!" << endl;
	cerr << "thetaLambdaFst summary:" << endl;
	cerr << FstUtils::PrintFstSummary(thetaLambdaFst);
      }
      assert(false);
    } 

    // update the loglikelihood
    Nll += nLogC;

    // add D/C to the gradient
    for(FastSparseVector<LogVal<double> >::iterator dIter = DSparseVector.begin(); dIter != DSparseVector.end(); ++dIter) {
      double nLogd = dIter->second.s_? dIter->second.v_ : -dIter->second.v_; // multiply the inner logD representation by -1.
      double dOverC = MultinomialParams::nExp(nLogd - nLogC);
      if(std::isnan(dOverC) || std::isinf(dOverC)) {
	if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	  cerr << "ERROR: dOverC = " << dOverC << ", nLogd = " << nLogd << ". my mistake. will halt!" << endl;
	}
        assert(false);
      }
      derivativeWRTLambdaSparseVector[dIter->first] -= dOverC;
    }

    // compute the F map fro this sentence
    FastSparseVector<LogVal<double> > FSparseVector;
    ComputeF(sentId, lambdaFst, lambdaAlphas, lambdaBetas, FSparseVector);

    // compute the Z value for this sentence
    double nLogZ = ComputeNLogZ_lambda(lambdaFst, lambdaBetas);

    // keep an eye on bad numbers
    if(std::isnan(nLogZ) || std::isinf(nLogZ)) {
      if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	cerr << "ERROR: nLogZ = " << nLogZ << ". my mistake. will halt!" << endl;
      }
      assert(false);
    } 

    // update the log likelihood
    Nll -= nLogZ;

    // subtract F/Z from the gradient
    for(FastSparseVector<LogVal<double> >::iterator fIter = FSparseVector.begin(); fIter != FSparseVector.end(); ++fIter) {
      double nLogf = fIter->second.s_? fIter->second.v_ : -fIter->second.v_; // multiply the inner logF representation by -1.
      double fOverZ = MultinomialParams::nExp(nLogf - nLogZ);
      if(std::isnan(fOverZ) || std::isinf(fOverZ)) {
	if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	  cerr << "ERROR: fOverZ = " << nLogZ << ", nLogf = " << nLogf << ". my mistake. will halt!" << endl;
	}
	assert(false);
      }
      derivativeWRTLambdaSparseVector[fIter->first] += fOverZ;
      if(std::isnan(derivativeWRTLambdaSparseVector[fIter->first]) || 
	 std::isinf(derivativeWRTLambdaSparseVector[fIter->first])) {
	cerr << "rank #" << learningInfo.mpiWorld->rank() << ": ERROR: fOverZ = " << nLogZ << ", nLogf = " << nLogf << ". my mistake. will halt!" << endl;
	assert(false);
      }
    }

    // debug info
    if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH) {
      cerr << ".";
    }
  } // end of training examples 

  return Nll;  
}

int LatentCrfModel::LbfgsProgressReport(void *ptrFromSentId,
					const lbfgsfloatval_t *x, 
					const lbfgsfloatval_t *g,
					const lbfgsfloatval_t fx,
					const lbfgsfloatval_t xnorm,
					const lbfgsfloatval_t gnorm,
					const lbfgsfloatval_t step,
					int n,
					int k,
					int ls) {
  
  LatentCrfModel &model = LatentCrfModel::GetInstance();

  int index = *((int*)ptrFromSentId), from, to;
  if(index == -1) {
    from = 0;
    to = model.examplesCount;
  } else {
    from = index;
    to = min((int)model.examplesCount, from + model.learningInfo.optimizationMethod.subOptMethod->miniBatchSize);
  }
  
  // show progress
  if(model.learningInfo.debugLevel >= DebugLevel::MINI_BATCH /* && model.learningInfo.mpiWorld->rank() == 0*/) {
    cerr << endl << "rank" << model.learningInfo.mpiWorld->rank() << ": -report: coord-descent iteration # " << model.learningInfo.iterationsCount;
    cerr << " sents(" << from << "-" << to;
    cerr << ")\tlbfgs Iteration " << k;
    if(model.learningInfo.optimizationMethod.subOptMethod->regularizer == Regularizer::NONE) {
      cerr << ":\t";
    } else {
      cerr << ":\tregularized ";
    }
    cerr << "objective = " << fx;
  }
  if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << ",\txnorm = " << xnorm;
    cerr << ",\tgnorm = " << gnorm;
    cerr << ",\tstep = " << step;
  }
  if(model.learningInfo.debugLevel >= DebugLevel::MINI_BATCH /* && model.learningInfo.mpiWorld->rank() == 0*/) {
    cerr << endl << endl;
  }

  // update the old lambdas
  if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "updating the old lambda params (necessary for applying the moveAwayPenalty) ..." << endl;
  }
  model.lambda->UpdateOldParamWeights();
  if(model.learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "done" << endl;
  }

  return 0;
}

// add constrained features here and set their weights by hand. those weights will not be optimized.
void LatentCrfModel::AddConstrainedFeatures() {
  if(learningInfo.debugLevel >= DebugLevel::CORPUS) {
    cerr << "adding constrained lambda features..." << endl;
  }
  FastSparseVector<double> activeFeatures;
  int yI, xI;
  int yIM1_dummy, index; // we don't really care
  vector<int> x;
  string xIString;
  vector<bool> constrainedFeatureTypes(lambda->COUNT_OF_FEATURE_TYPES, false);
  for(int i = 0; i < learningInfo.constraints.size(); i++) {
    switch(learningInfo.constraints[i].type) {
      // constrains the latent variable corresponding to certain types
    case ConstraintType::yIExclusive_xIString:
      // we only want to constrain one specific feature type
      std::fill(constrainedFeatureTypes.begin(), constrainedFeatureTypes.end(), false);
      constrainedFeatureTypes[54] = true;
      // parse the constraint
      xIString.clear();
      learningInfo.constraints[i].GetFieldsOfConstraintType_yIExclusive_xIString(yI, xIString);
      xI = vocabEncoder.Encode(xIString);
      // fire positively constrained features
      x.clear();
      x.push_back(xI);
      yIM1_dummy = yI; // we don't really care
      index = 0; // we don't really care
      activeFeatures.clear();
      // start hack 
      SetTestExample(x);
      testingMode = true;
      FireFeatures(yI, yIM1_dummy, 0, index, constrainedFeatureTypes, activeFeatures);
      testingMode = false;
      // end hack
      // set appropriate weights to favor those parameters
      for(FastSparseVector<double>::iterator featureIter = activeFeatures.begin(); featureIter != activeFeatures.end(); ++featureIter) {
	lambda->UpdateParam(featureIter->first, REWARD_FOR_CONSTRAINED_FEATURES);
      }
      // negatively constrained features (i.e. since xI is constrained to get the label yI, any other label should be penalized)
      for(set<int>::const_iterator yDomainIter = yDomain.begin(); yDomainIter != yDomain.end(); yDomainIter++) {
	if(*yDomainIter == yI) {
	  continue;
	}
	// fire the negatively constrained features
	activeFeatures.clear();
	// start hack 
	SetTestExample(x);
	testingMode = true;
	FireFeatures(*yDomainIter, yIM1_dummy, 0, index, constrainedFeatureTypes, activeFeatures);
	testingMode = false;
	// end hack
	// set appropriate weights to penalize those parameters
	for(FastSparseVector<double>::iterator featureIter = activeFeatures.begin(); featureIter != activeFeatures.end(); ++featureIter) {
	  lambda->UpdateParam(featureIter->first, PENALTY_FOR_CONSTRAINED_FEATURES);
	}   
      }
      break;
    case ConstraintType::yI_xIString:
      // we only want to constrain one specific feature type
      std::fill(constrainedFeatureTypes.begin(), constrainedFeatureTypes.end(), false);
      constrainedFeatureTypes[54] = true;
      // parse the constraint
      xIString.clear();
      learningInfo.constraints[i].GetFieldsOfConstraintType_yI_xIString(yI, xIString);
      xI = vocabEncoder.Encode(xIString);
      // fire positively constrained features
      x.clear();
      x.push_back(xI);
      yIM1_dummy = yI; // we don't really care
      index = 0; // we don't really care
      activeFeatures.clear();
      // start hack
      SetTestExample(x);
      testingMode = true;
      FireFeatures(yI, yIM1_dummy, 0, index, constrainedFeatureTypes, activeFeatures);
      testingMode = false;
      // end hack
      // set appropriate weights to favor those parameters
      for(FastSparseVector<double>::iterator featureIter = activeFeatures.begin(); featureIter != activeFeatures.end(); ++featureIter) {
	lambda->UpdateParam(featureIter->first, REWARD_FOR_CONSTRAINED_FEATURES);
      }
      break;
    default:
      assert(false);
      break;
    }
  }
  // take note of the number of constrained lambda parameters. use this to limit optimization to non-constrained params
  countOfConstrainedLambdaParameters = lambda->GetParamsCount();
  if(learningInfo.debugLevel >= DebugLevel::CORPUS) {
    cerr << "done adding constrainted lambda features. Count:" << lambda->GetParamsCount() << endl;
  }
}

void LatentCrfModel::UpdateThetaMleForSent(const unsigned sentId, 
					   MultinomialParams::ConditionalMultinomialParam<int> &mleGivenOneLabel, 
					   map<int, double> &mleMarginalsGivenOneLabel,
					   MultinomialParams::ConditionalMultinomialParam< pair<int, int> > &mleGivenTwoLabels, 
					   map< pair<int, int>, double> &mleMarginalsGivenTwoLabels) {

  // in the word alignment model, yDomain depends on the example
  PrepareExample(sentId);
  
  if(learningInfo.zIDependsOnYIM1) {
    UpdateThetaMleForSent(sentId, mleGivenTwoLabels, mleMarginalsGivenTwoLabels);
  } else {
    UpdateThetaMleForSent(sentId, mleGivenOneLabel, mleMarginalsGivenOneLabel);
  }
}

void LatentCrfModel::NormalizeThetaMleAndUpdateTheta(MultinomialParams::ConditionalMultinomialParam<int> &mleGivenOneLabel, 
						     map<int, double> &mleMarginalsGivenOneLabel,
						     MultinomialParams::ConditionalMultinomialParam< std::pair<int, int> > &mleGivenTwoLabels, 
						     map< std::pair<int, int>, double> &mleMarginalsGivenTwoLabels) {
  if(learningInfo.zIDependsOnYIM1) {
    NormalizeThetaMle(mleGivenTwoLabels, mleMarginalsGivenTwoLabels);
    nLogThetaGivenTwoLabels = mleGivenTwoLabels;
  } else {
    NormalizeThetaMle(mleGivenOneLabel, mleMarginalsGivenOneLabel);
    nLogThetaGivenOneLabel = mleGivenOneLabel;
  }
}

lbfgs_parameter_t LatentCrfModel::SetLbfgsConfig() {
  // lbfgs configurations
  lbfgs_parameter_t lbfgsParams;
  lbfgs_parameter_init(&lbfgsParams);
  assert(learningInfo.optimizationMethod.subOptMethod != 0);
  lbfgsParams.max_iterations = learningInfo.optimizationMethod.subOptMethod->lbfgsParams.maxIterations;
  lbfgsParams.m = learningInfo.optimizationMethod.subOptMethod->lbfgsParams.memoryBuffer;
  cerr << "rank #" << learningInfo.mpiWorld->rank() << ": m = " << lbfgsParams.m  << endl;
  lbfgsParams.xtol = learningInfo.optimizationMethod.subOptMethod->lbfgsParams.precision;
  cerr << "rank #" << learningInfo.mpiWorld->rank() << ": xtol = " << lbfgsParams.xtol  << endl;
  lbfgsParams.max_linesearch = learningInfo.optimizationMethod.subOptMethod->lbfgsParams.maxEvalsPerIteration;
  cerr << "rank #" << learningInfo.mpiWorld->rank() << ": max_linesearch = " << lbfgsParams.max_linesearch  << endl;
  switch(learningInfo.optimizationMethod.subOptMethod->regularizer) {
  case Regularizer::L1:
    lbfgsParams.orthantwise_c = learningInfo.optimizationMethod.subOptMethod->regularizationStrength;
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": orthantwise_c = " << lbfgsParams.orthantwise_c  << endl;
    // this is the only linesearch algorithm that seems to work with orthantwise lbfgs
    lbfgsParams.linesearch = LBFGS_LINESEARCH_BACKTRACKING;
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": linesearch = " << lbfgsParams.linesearch  << endl;
    break;
  case Regularizer::L2:
    // nothing to be done now. l2 is implemented in the lbfgs callback evaluate function.
    break;
  case Regularizer::NONE:
    // do nothing
    break;
  default:
    cerr << "regularizer not supported" << endl;
    assert(false);
    break;
  }

  return lbfgsParams;
}

void LatentCrfModel::BroadcastLambdas(unsigned rankId)  {
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": before executing BroadcastLambdas()" << endl;
  }
  lambda->Broadcast(*learningInfo.mpiWorld, rankId);
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": after executing BroadcastLambdas()" << endl;
  }
}

void LatentCrfModel::BroadcastTheta(unsigned rankId) {
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": before calling BroadcastTheta()" << endl;
  }

  if(learningInfo.zIDependsOnYIM1) {
    mpi::broadcast< map< pair<int,int>, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, nLogThetaGivenTwoLabels.params, rankId);
  } else {
    mpi::broadcast< map< int, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, nLogThetaGivenOneLabel.params, rankId);
  }

  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": after calling BroadcastTheta()" << endl;
  }
}

void LatentCrfModel::ReduceMleAndMarginals(MultinomialParams::ConditionalMultinomialParam<int> mleGivenOneLabel, 
					   MultinomialParams::ConditionalMultinomialParam< pair<int, int> > mleGivenTwoLabels,
					   map<int, double> mleMarginalsGivenOneLabel,
					   map<std::pair<int, int>, double> mleMarginalsGivenTwoLabels) {
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank" << learningInfo.mpiWorld->rank() << ": before calling ReduceMleAndMarginals()" << endl;
  }
  
  if(learningInfo.zIDependsOnYIM1) {
    mpi::reduce< map< pair<int,int>, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, 
									     mleGivenTwoLabels.params, mleGivenTwoLabels.params, 
									     MultinomialParams::AccumulateConditionalMultinomials< pair<int, int> >, 0);
    mpi::reduce< map< pair<int, int>, double > >(*learningInfo.mpiWorld, 
					       mleMarginalsGivenTwoLabels, mleMarginalsGivenTwoLabels, 
					       MultinomialParams::AccumulateMultinomials< pair<int,int> >, 0);
  } else {
    mpi::reduce< map< int, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, 
								   mleGivenOneLabel.params, mleGivenOneLabel.params, 
								   MultinomialParams::AccumulateConditionalMultinomials< int >, 0);
    mpi::reduce< map< int, double > >(*learningInfo.mpiWorld, 
				      mleMarginalsGivenOneLabel, mleMarginalsGivenOneLabel, 
				      MultinomialParams::AccumulateMultinomials<int>, 0);
  }

  // debug info
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << "rank" << learningInfo.mpiWorld->rank() << ": after calling ReduceMleAndMarginals()" << endl;
  }
  
}

void LatentCrfModel::PersistTheta(string thetaParamsFilename) {
  if(learningInfo.zIDependsOnYIM1) {
    MultinomialParams::PersistParams(thetaParamsFilename, nLogThetaGivenTwoLabels, vocabEncoder);
  } else {
    MultinomialParams::PersistParams(thetaParamsFilename, nLogThetaGivenOneLabel, vocabEncoder);
  }
}

void LatentCrfModel::BlockCoordinateDescent() {  
  
  // if you're not using mini batch, set the minibatch size to data.size()
  if(learningInfo.optimizationMethod.subOptMethod->miniBatchSize <= 0) {
    learningInfo.optimizationMethod.subOptMethod->miniBatchSize = examplesCount;
  }

  // set lbfgs configurations
  lbfgs_parameter_t lbfgsParams = SetLbfgsConfig();

  // TRAINING ITERATIONS
  bool converged = false;
  do {

    // UPDATE THETAS by normalizing soft counts (i.e. the closed form MLE solution)
    // data structure to hold theta MLE estimates
    MultinomialParams::ConditionalMultinomialParam<int> mleGivenOneLabel;
    MultinomialParams::ConditionalMultinomialParam< pair<int, int> > mleGivenTwoLabels;
    map<int, double> mleMarginalsGivenOneLabel;
    map<std::pair<int, int>, double> mleMarginalsGivenTwoLabels;

    // debug info
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << "master" << learningInfo.mpiWorld->rank() << ": ====================== ITERATION " << learningInfo.iterationsCount << " =====================" << endl << endl;
      cerr << "master" << learningInfo.mpiWorld->rank() << ": ========== first, update thetas: =========" << endl << endl;
    }

    // update the mle for each sentence
    for(unsigned sentId = 0; sentId < examplesCount; sentId++) {
      // sentId is assigned to the process # (sentId % world.size())
      if(sentId % learningInfo.mpiWorld->size() != learningInfo.mpiWorld->rank()) {
	continue;
      }

      UpdateThetaMleForSent(sentId, mleGivenOneLabel, mleMarginalsGivenOneLabel, mleGivenTwoLabels, mleMarginalsGivenTwoLabels);
    }

    // accumulate mle counts from slaves
    ReduceMleAndMarginals(mleGivenOneLabel, mleGivenTwoLabels, mleMarginalsGivenOneLabel, mleMarginalsGivenTwoLabels);
    
    // normalize mle and update nLogTheta on master
    if(learningInfo.mpiWorld->rank() == 0) {
      NormalizeThetaMleAndUpdateTheta(mleGivenOneLabel, mleMarginalsGivenOneLabel, 
				      mleGivenTwoLabels, mleMarginalsGivenTwoLabels);
    }

    // update nLogTheta on slaves
    BroadcastTheta(0);

    // debug info
    if(learningInfo.iterationsCount % learningInfo.persistParamsAfterNIteration == 0 && learningInfo.mpiWorld->rank() == 0) {
      stringstream thetaParamsFilename;
      thetaParamsFilename << outputPrefix << "." << learningInfo.iterationsCount;
      thetaParamsFilename << ".theta";
      if(learningInfo.debugLevel >= DebugLevel::CORPUS) {
	cerr << "master" << learningInfo.mpiWorld->rank() << ": persisting theta parameters in iteration " \
	     << learningInfo.iterationsCount << " at " << thetaParamsFilename.str() << endl;
      }
      PersistTheta(thetaParamsFilename.str());
    }

    // update the lambdas
    // debug info
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << endl << "master" << learningInfo.mpiWorld->rank() << ": ========== second, update lambdas ==========" << endl << endl;
    }
    
    double Nll = 0;
    // note: batch == minibatch with size equals to data.size()
    for(int sentId = 0; sentId < examplesCount; sentId += learningInfo.optimizationMethod.subOptMethod->miniBatchSize) {

      // debug info
      double optimizedMiniBatchNll = 0;
      if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH && learningInfo.mpiWorld->rank() == 0) {
	int to = min(sentId+learningInfo.optimizationMethod.subOptMethod->miniBatchSize, (int)examplesCount);
	cerr << "master" << learningInfo.mpiWorld->rank() << ": optimizing lambda weights to max likelihood(z|x) for sents " \
	     << sentId << "-" << to << endl;
      }

      // use LBFGS to update lambdas
      if(learningInfo.optimizationMethod.subOptMethod->algorithm == LBFGS) {

	if(learningInfo.debugLevel >= DebugLevel::REDICULOUS && learningInfo.mpiWorld->rank() == 0) {
	  cerr << "master" << learningInfo.mpiWorld->rank() << ": we'll use LBFGS to update the lambda parameters" << endl;
	}
	
	// parallelizing the lbfgs callback function is complicated
	if(learningInfo.mpiWorld->rank() == 0) {

	  // populate lambdasArray and lambasArrayLength
	  // don't optimize all parameters. only optimize unconstrained ones
	  double* lambdasArray;
	  int lambdasArrayLength;
	  lambdasArray = lambda->GetParamWeightsArray() + countOfConstrainedLambdaParameters;
	  lambdasArrayLength = lambda->GetParamsCount() - countOfConstrainedLambdaParameters;
	  assert(countOfConstrainedLambdaParameters == 0); // not fully supported anymore. will require some changes.

	  // only the master executes lbfgs
	  int lbfgsStatus = lbfgs(lambdasArrayLength, lambdasArray, &optimizedMiniBatchNll, 
				  LbfgsCallbackEvalZGivenXLambdaGradient, LbfgsProgressReport, &sentId, &lbfgsParams);
	  bool NEED_HELP = false;
	  mpi::broadcast<bool>(*learningInfo.mpiWorld, NEED_HELP, 0);

	  // debug
	  if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH) {
	    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": lbfgsStatusCode = " \
		 << LbfgsUtils::LbfgsStatusIntToString(lbfgsStatus) << " = " << lbfgsStatus << endl;
	  }
	  
	} else {

	  // be loyal to your master
	  while(true) {

	    // does the master need help computing the gradient? this line always "receives" rather than broacasts
	    bool masterNeedsHelp = false;
	    mpi::broadcast<bool>(*learningInfo.mpiWorld, masterNeedsHelp, 0);
	    if(!masterNeedsHelp) {
	      break;
	    }

	    // receive the latest parameter weights from the master (TODO: can we assume that slaves already have the latest lambda parameter weights at this point?)
	    mpi::broadcast<vector<double> >( *learningInfo.mpiWorld, lambda->paramWeights, 0);
	    
	    // process your share of examples
	    vector<double> gradientPiece(lambda->GetParamsCount(), 0.0), dummy;
	    double nllPiece = ComputeNllZGivenXAndLambdaGradient(gradientPiece);
	    
	    // merge your gradient with other slaves
	    mpi::reduce< vector<double> >(*learningInfo.mpiWorld, gradientPiece, dummy, 
					  AggregateVectors2(), 0);

	    // aggregate the loglikelihood computation as well
	    double dummy2;
	    mpi::reduce<double>(*learningInfo.mpiWorld, nllPiece, dummy2, std::plus<double>(), 0);

	    // for debug
	    if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
	      cerr << "rank" << learningInfo.mpiWorld->rank() << ": i'm trapped in this loop, repeatedly helping master evaluate likelihood and gradient for lbfgs." << endl;
	    }
	  }
	} // end if master => run lbfgs() else help master
	
      } else if(learningInfo.optimizationMethod.subOptMethod->algorithm == SIMULATED_ANNEALING) {
	// use simulated annealing to optimize likelihood
	
	// populate lambdasArray and lambasArrayLength
	// don't optimize all parameters. only optimize unconstrained ones
	double* lambdasArray;
	int lambdasArrayLength;
	lambdasArray = lambda->GetParamWeightsArray() + countOfConstrainedLambdaParameters;
	lambdasArrayLength = lambda->GetParamsCount() - countOfConstrainedLambdaParameters;

	simulatedAnnealer.set_up(EvaluateNll, lambdasArrayLength);
	// initialize the parameters array
	float simulatedAnnealingArray[lambdasArrayLength];
	for(int i = 0; i < lambdasArrayLength; i++) {
	  simulatedAnnealingArray[i] = lambdasArray[i];
	}
	simulatedAnnealer.initial(simulatedAnnealingArray);
	// optimize
	simulatedAnnealer.anneal(10);
	// get the optimum parameters
	simulatedAnnealer.current(simulatedAnnealingArray);
	for(int i = 0; i < lambdasArrayLength; i++) {
	  lambdasArray[i] = simulatedAnnealingArray[i];
	}
      } else {
	assert(false);
      }
      
      // debug info
      if(learningInfo.debugLevel >= DebugLevel::MINI_BATCH && learningInfo.mpiWorld->rank() == 0) {
	cerr << "master" << learningInfo.mpiWorld->rank() << ": optimized Nll is " << optimizedMiniBatchNll << endl;
      }
      
      // update iteration's Nll
      if(std::isnan(optimizedMiniBatchNll) || std::isinf(optimizedMiniBatchNll)) {
	if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
	  cerr << "ERROR: optimizedMiniBatchNll = " << optimizedMiniBatchNll << ". didn't add this batch's likelihood to the total likelihood. will halt!" << endl;
	}
	assert(false);
      } else {
	Nll += optimizedMiniBatchNll;
      }

    } // for each minibatch

    // debug info
    if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      lambda->PrintParams();
    }

    // persist updated lambda params
    stringstream lambdaParamsFilename;
    if(learningInfo.iterationsCount % learningInfo.persistParamsAfterNIteration == 0 && learningInfo.mpiWorld->rank() == 0) {
      lambdaParamsFilename << outputPrefix << "." << learningInfo.iterationsCount << ".lambda";
      if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
	cerr << "persisting lambda parameters after iteration " << learningInfo.iterationsCount << " at " << lambdaParamsFilename.str() << endl;
      }
      lambda->PersistParams(lambdaParamsFilename.str());
    }

    // debug info
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << endl << "master" << learningInfo.mpiWorld->rank() << ": finished coordinate descent iteration #" << learningInfo.iterationsCount << " Nll=" << Nll << endl;
    }
    
    // update learningInfo
    mpi::broadcast<double>(*learningInfo.mpiWorld, Nll, 0);
    learningInfo.logLikelihood.push_back(Nll);
    learningInfo.iterationsCount++;

    // check convergence
    if(learningInfo.mpiWorld->rank() == 0) {
      converged = learningInfo.IsModelConverged();
    }
    
    if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "rank" << learningInfo.mpiWorld->rank() << ": coord descent converged = " << converged << endl;
    }
    
    // broadcast the convergence decision
    mpi::broadcast<bool>(*learningInfo.mpiWorld, converged, 0);    
  
  } while(!converged);

  // debug
  if(learningInfo.mpiWorld->rank() == 0) {
    lambda->PersistParams(outputPrefix + string(".final.lambda"));
    PersistTheta(outputPrefix + string(".final.theta"));
  }
}

void LatentCrfModel::Label(vector<string> &tokens, vector<int> &labels) {
  assert(labels.size() == 0);
  assert(tokens.size() > 0);
  vector<int> tokensInt;
  for(int i = 0; i < tokens.size(); i++) {
    tokensInt.push_back(vocabEncoder.Encode(tokens[i]));
  }
  Label(tokensInt, labels);
}

void LatentCrfPosTagger::SetTestExample(vector<int> &tokens) {
  testData.clear();
  testData.push_back(tokens);
}

void LatentCrfModel::Label(vector<int> &tokens, vector<int> &labels) {
  assert(labels.size() == 0); 
  assert(tokens.size() > 0);
  testingMode = true;

  // hack to reuse the code that manipulates the fst
  SetTestExample(tokens);
  unsigned sentId = 0;
  
  fst::VectorFst<FstUtils::LogArc> fst;
  vector<FstUtils::LogWeight> alphas, betas;
  BuildThetaLambdaFst(sentId, tokens, fst, alphas, betas);
  fst::VectorFst<FstUtils::StdArc> fst2, shortestPath;
  fst::ArcMap(fst, &fst2, FstUtils::LogToTropicalMapper());
  fst::ShortestPath(fst2, &shortestPath);
  std::vector<int> dummy;
  FstUtils::LinearFstToVector(shortestPath, dummy, labels);
  assert(labels.size() == tokens.size());
}

void LatentCrfModel::Label(vector<vector<int> > &tokens, vector<vector<int> > &labels) {
  assert(labels.size() == 0);
  labels.resize(tokens.size());
  for(int i = 0; i < tokens.size(); i++) {
    Label(tokens[i], labels[i]);
  }
}

void LatentCrfModel::Label(vector<vector<string> > &tokens, vector<vector<int> > &labels) {
  assert(labels.size() == 0);
  labels.resize(tokens.size());
  for(int i = 0 ; i <tokens.size(); i++) {
    Label(tokens[i], labels[i]);
  }
}

void LatentCrfModel::Label(string &inputFilename, string &outputFilename) {
  std::vector<std::vector<std::string> > tokens;
  StringUtils::ReadTokens(inputFilename, tokens);
  vector<vector<int> > labels;
  Label(tokens, labels);
  StringUtils::WriteTokens(outputFilename, labels);
}

void LatentCrfModel::Analyze(string &inputFilename, string &outputFilename) {
  // label
  std::vector<std::vector<std::string> > tokens;
  StringUtils::ReadTokens(inputFilename, tokens);
  vector<vector<int> > labels;
  Label(tokens, labels);
  // analyze
  map<int, map<string, int> > labelToTypesAndCounts;
  map<string, map<int, int> > typeToLabelsAndCounts;
  for(int sentId = 0; sentId < tokens.size(); sentId++) {
    for(int i = 0; i < tokens[sentId].size(); i++) {
      labelToTypesAndCounts[labels[sentId][i]][tokens[sentId][i]]++;
      typeToLabelsAndCounts[tokens[sentId][i]][labels[sentId][i]]++;
    }
  }
  // write the number of tokens of each labels
  std::ofstream outputFile(outputFilename.c_str(), std::ios::out);
  outputFile << "# LABEL HISTOGRAM #" << endl;
  for(map<int, map<string, int> >::const_iterator labelIter = labelToTypesAndCounts.begin(); labelIter != labelToTypesAndCounts.end(); labelIter++) {
    outputFile << "label:" << labelIter->first;
    int totalCount = 0;
    for(map<string, int>::const_iterator typeIter = labelIter->second.begin(); typeIter != labelIter->second.end(); typeIter++) {
      totalCount += typeIter->second;
    }
    outputFile << " tokenCount:" << totalCount << endl;
  }
  // write the types of each label
  outputFile << endl << "# LABEL -> TYPES:COUNTS #" << endl;
  for(map<int, map<string, int> >::const_iterator labelIter = labelToTypesAndCounts.begin(); labelIter != labelToTypesAndCounts.end(); labelIter++) {
    outputFile << "label:" << labelIter->first << endl << "\ttypes: " << endl;
    for(map<string, int>::const_iterator typeIter = labelIter->second.begin(); typeIter != labelIter->second.end(); typeIter++) {
      outputFile << "\t\t" << typeIter->first << ":" << typeIter->second << endl;
    }
  }
  // write the labels of each type
  outputFile << endl << "# TYPE -> LABELS:COUNT #" << endl;
  for(map<string, map<int, int> >::const_iterator typeIter = typeToLabelsAndCounts.begin(); typeIter != typeToLabelsAndCounts.end(); typeIter++) {
    outputFile << "type:" << typeIter->first << "\tlabels: ";
    for(map<int, int>::const_iterator labelIter = typeIter->second.begin(); labelIter != typeIter->second.end(); labelIter++) {
      outputFile << labelIter->first << ":" << labelIter->second << " ";
    }
    outputFile << endl;
  }
  outputFile.close();
}

double LatentCrfModel::ComputeVariationOfInformation(string &aLabelsFilename, string &bLabelsFilename) {
  vector<string> clusteringA, clusteringB;
  vector<vector<string> > clusteringAByLine, clusteringBByLine;
  StringUtils::ReadTokens(aLabelsFilename, clusteringAByLine);
  StringUtils::ReadTokens(bLabelsFilename, clusteringBByLine);
  assert(clusteringAByLine.size() == clusteringBByLine.size());
  for(int i = 0; i < clusteringAByLine.size(); i++) {
    assert(clusteringAByLine[i].size() == clusteringBByLine[i].size());
    for(int j = 0; j < clusteringAByLine[i].size(); j++) {
      clusteringA.push_back(clusteringAByLine[i][j]);
      clusteringB.push_back(clusteringBByLine[i][j]);			    
    }
  }
  return ClustersComparer::ComputeVariationOfInformation(clusteringA, clusteringB);
}

double LatentCrfModel::ComputeManyToOne(string &aLabelsFilename, string &bLabelsFilename) {
  vector<string> clusteringA, clusteringB;
  vector<vector<string> > clusteringAByLine, clusteringBByLine;
  StringUtils::ReadTokens(aLabelsFilename, clusteringAByLine);
  StringUtils::ReadTokens(bLabelsFilename, clusteringBByLine);
  assert(clusteringAByLine.size() == clusteringBByLine.size());
  for(int i = 0; i < clusteringAByLine.size(); i++) {
    assert(clusteringAByLine[i].size() == clusteringBByLine[i].size());
    for(int j = 0; j < clusteringAByLine[i].size(); j++) {
      clusteringA.push_back(clusteringAByLine[i][j]);
      clusteringB.push_back(clusteringBByLine[i][j]);			    
    }
  }
  return ClustersComparer::ComputeManyToOne(clusteringA, clusteringB);
}

// make sure all features which may fire on this training data have a corresponding parameter in lambda (member)
void LatentCrfModel::InitLambda() {
  if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
    cerr << "master" << learningInfo.mpiWorld->rank() << ": initializing lambdas..." << endl;
  }

  // only the master adds constrained features with hand-crafted weights depending on the feature type
  if(learningInfo.mpiWorld->rank() == 0) {
    // but first, make sure no features are fired yet. we need the constrained features to be 
    assert(lambda->GetParamsCount() == 0);
    AddConstrainedFeatures();
  }

  // then, each process discovers the features that may show up in their sentences.
  for(int sentId = 0; sentId < examplesCount; sentId++) {

    // skip sentences not assigned to this process
    if(sentId % learningInfo.mpiWorld->size() != learningInfo.mpiWorld->rank()) {
      continue;
    }
    // debug info
    if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": now processing sent# " << sentId << endl;
    }
    // build the FST
    fst::VectorFst<FstUtils::LogArc> lambdaFst;
    if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": before calling BuildLambdaFst(), |lambda| =  " << lambda->GetParamsCount() <<", |lambdaFst| =  " << lambdaFst.NumStates() << endl;
    }
    BuildLambdaFst(sentId, lambdaFst);
    if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": after calling BuildLambdaFst(), |lambda| =  " << lambda->GetParamsCount() << ", |lambdaFst| =  " << lambdaFst.NumStates() << endl;
    }
  }

  // debug info
  if(learningInfo.debugLevel >= DebugLevel::CORPUS){ 
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": done with my share of FireFeatures(sent)" << endl;
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": before reduce()" << endl;
  }

  // master collects all feature ids fired on any sentence
  set<string> localParamIds(lambda->paramIds.begin(), lambda->paramIds.end()), allParamIds;
  mpi::reduce< set<string> >(*learningInfo.mpiWorld, localParamIds, allParamIds, AggregateSets2(), 0);

  // debug info
  if(learningInfo.debugLevel >= DebugLevel::ESSENTIAL){ 
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": after reduce()" << endl;
  }
  
  // master updates its lambda object adding all those features
  if(learningInfo.mpiWorld->rank() == 0) {
    for(set<string>::const_iterator paramIdIter = allParamIds.begin(); paramIdIter != allParamIds.end(); ++paramIdIter) {
      lambda->AddParam(*paramIdIter);
    }
  }
  
  // master broadcasts the full set of features to all slaves
  BroadcastLambdas(0);
}

vector<int>& LatentCrfPosTagger::GetObservableSequence(int exampleId) {
  if(testingMode) {
    assert(exampleId < testData.size());
    return testData[exampleId];
  } else {
    assert(exampleId < data.size());
    return data[exampleId];
  }
}

// singleton
LatentCrfModel* LatentCrfPosTagger::GetInstance(const string &textFilename, 
						    const string &outputPrefix, 
						    LearningInfo &learningInfo, 
						    unsigned NUMBER_OF_LABELS, 
						    unsigned FIRST_LABEL_ID) {
  if(!instance) {
    instance = new LatentCrfPosTagger(textFilename, outputPrefix, learningInfo, NUMBER_OF_LABELS, FIRST_LABEL_ID);
  } else {
    cerr << "A LatentCrfPosTagger object has already been initialized" << endl;
  }
  return instance;
}

LatentCrfModel* LatentCrfPosTagger::GetInstance() {
  if(!instance) {
    assert(false);
  }
  return instance;
}

LatentCrfPosTagger::LatentCrfPosTagger(const string &textFilename, 
				       const string &outputPrefix, 
				       LearningInfo &learningInfo, 
				       unsigned NUMBER_OF_LABELS, 
				       unsigned FIRST_LABEL_ID) : LatentCrfModel(textFilename, 
										 outputPrefix, 
										 learningInfo, 
										 FIRST_LABEL_ID,
										 LatentCrfModel::Task::POS_TAGGING) {
  // set constants
  LatentCrfModel::START_OF_SENTENCE_Y_VALUE = FIRST_LABEL_ID - 1;
  this->FIRST_ALLOWED_LABEL_VALUE = FIRST_LABEL_ID;
  assert(START_OF_SENTENCE_Y_VALUE > 0);


  // POS tag yDomain
  unsigned latentClasses = NUMBER_OF_LABELS;
  assert(latentClasses > 1);
  this->yDomain.insert(LatentCrfModel::START_OF_SENTENCE_Y_VALUE); // the conceptual yValue of word at position -1 in a sentence
  for(unsigned i = 0; i < latentClasses; i++) {
    this->yDomain.insert(LatentCrfModel::START_OF_SENTENCE_Y_VALUE + i + 1);
  }
  // zero is reserved for FST epsilon
  assert(this->yDomain.count(0) == 0);

  // words zDomain
  for(map<int,string>::const_iterator vocabIter = vocabEncoder.intToToken.begin();
      vocabIter != vocabEncoder.intToToken.end();
      vocabIter++) {
    if(vocabIter->second == "_unk_") {
      continue;
    }
    this->zDomain.insert(vocabIter->first);
  }
  // zero is reserved for FST epsilon
  assert(this->zDomain.count(0) == 0);
  
  // read and encode data
  data.clear();
  vocabEncoder.Read(textFilename, data);
  examplesCount = data.size();
  
  // bool vectors indicating which feature types to use
  assert(enabledFeatureTypes.size() == 0);
  // features 1-50 are reserved for wordalignment
  for(int i = 0; i <= 50; i++) {
    enabledFeatureTypes.push_back(false);
  }
  // features 51-100 are reserved for latentCrf model
  for(int i = 51; i < 100; i++) {
    enabledFeatureTypes.push_back(false);
  }
  enabledFeatureTypes[51] = true;   // y_i:y_{i-1}
  //  enabledFeatureTypes[52] = true; // y_i:x_{i-2}
  enabledFeatureTypes[53] = true; // y_i:x_{i-1}
  enabledFeatureTypes[54] = true;   // y_i:x_i
  enabledFeatureTypes[55] = true; // y_i:x_{i+1}
  //enabledFeatureTypes[56] = true; // y_i:x_{i+2}
  enabledFeatureTypes[57] = true; // y_i:i
  //  enabledFeatureTypes[58] = true;
  //  enabledFeatureTypes[59] = true;
  //  enabledFeatureTypes[60] = true;
  //  enabledFeatureTypes[61] = true;
  //  enabledFeatureTypes[62] = true;
  //  enabledFeatureTypes[63] = true;
  //  enabledFeatureTypes[64] = true;
  //  enabledFeatureTypes[65] = true;
  enabledFeatureTypes[66] = true; // y_i:(|x|-i)
  enabledFeatureTypes[67] = true; // capital and i != 0
  //enabledFeatureTypes[68] = true;
  enabledFeatureTypes[69] = true; // coarse hash functions
  //enabledFeatureTypes[70] = true;
  //enabledFeatureTypes[71] = true; // y_i:x_{i-1} where x_{i-1} is closed vocab
  //enabledFeatureTypes[72] = true;
  //enabledFeatureTypes[73] = true; // y_i:x_{i+1} where x_{i+1} is closed vocab
  //enabledFeatureTypes[74] = true;
  //enabledFeatureTypes[75] = true; // y_i

  // initialize (and normalize) the log theta params to gaussians
  InitTheta();

  // make sure all slaves have the same theta values
  BroadcastTheta(0);

  // persist initial parameters
  assert(learningInfo.iterationsCount == 0);
  if(learningInfo.iterationsCount % learningInfo.persistParamsAfterNIteration == 0 && learningInfo.mpiWorld->rank() == 0) {
    stringstream thetaParamsFilename;
    thetaParamsFilename << outputPrefix << ".initial.theta";
    if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      cerr << "persisting theta parameters after iteration " << learningInfo.iterationsCount << " at " << thetaParamsFilename.str() << endl;
    }
    PersistTheta(thetaParamsFilename.str());
  }
  
  // initialize the lambda parameters
  // add all features in this data set to lambda.params
  InitLambda();
}

LatentCrfPosTagger::~LatentCrfPosTagger() {}

void LatentCrfPosTagger::InitTheta() {
  if(learningInfo.mpiWorld->rank() == 0 && learningInfo.debugLevel >= DebugLevel::ESSENTIAL) {
    cerr << "master" << learningInfo.mpiWorld->rank() << ": initializing thetas...";
  }

  // first initialize nlogthetas to unnormalized gaussians
  if(learningInfo.zIDependsOnYIM1) {
    nLogThetaGivenTwoLabels.params.clear();
    for(set<int>::const_iterator yDomainIter = yDomain.begin(); yDomainIter != yDomain.end(); yDomainIter++) {
      for(set<int>::const_iterator yDomainIter2 = yDomain.begin(); yDomainIter2 != yDomain.end(); yDomainIter2++) {
	for(set<int>::const_iterator zDomainIter = zDomain.begin(); zDomainIter != zDomain.end(); zDomainIter++) {
	  nLogThetaGivenTwoLabels.params[std::pair<int, int>(*yDomainIter, *yDomainIter2)][*zDomainIter] = fabs(gaussianSampler.Draw());
	}
      }
    }
  } else {
    nLogThetaGivenOneLabel.params.clear();
    for(set<int>::const_iterator yDomainIter = yDomain.begin(); yDomainIter != yDomain.end(); yDomainIter++) {
      for(set<int>::const_iterator zDomainIter = zDomain.begin(); zDomainIter != zDomain.end(); zDomainIter++) {
	nLogThetaGivenOneLabel.params[*yDomainIter][*zDomainIter] = abs(gaussianSampler.Draw());
      }
    }
  }

  // then normalize them
  if(learningInfo.zIDependsOnYIM1) {
    MultinomialParams::NormalizeParams(nLogThetaGivenTwoLabels);
  } else {
    MultinomialParams::NormalizeParams(nLogThetaGivenOneLabel);
  }
  if(learningInfo.mpiWorld->rank() == 0) {
    cerr << "done" << endl;
  }
}

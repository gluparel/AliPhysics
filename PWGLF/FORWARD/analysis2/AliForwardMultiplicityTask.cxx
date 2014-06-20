// 
// Calculate the multiplicity in the forward regions event-by-event 
// 
// Inputs: 
//   - AliESDEvent 
//
// Outputs: 
//   - AliAODForwardMult 
// 
// Histograms 
//   
// Corrections used 
//
#include "AliForwardMultiplicityTask.h"
#include "AliTriggerAnalysis.h"
#include "AliPhysicsSelection.h"
#include "AliLog.h"
#include "AliESDEvent.h"
#include "AliAODHandler.h"
#include "AliMultiplicity.h"
#include "AliInputEventHandler.h"
#include "AliForwardCorrectionManager.h"
#include "AliAnalysisManager.h"
#include <TH1.h>
#include <TH3D.h>
#include <TDirectory.h>
#include <TTree.h>
#include <TROOT.h>
#include <TStopwatch.h>
#include <TProfile.h>
// #define ENABLE_TIMING
#ifndef ENABLE_TIMING
# define MAKE_SW(NAME) do {} while(false)
# define START_SW(NAME) do {} while(false)
# define FILL_SW(NAME,WHICH) do {} while(false)
#else
# define MAKE_SW(NAME) TStopwatch NAME
# define START_SW(NAME) if (fDoTiming) NAME.Start(true)
# define FILL_SW(NAME,WHICH)				\
  if (fDoTiming) fHTiming->Fill(WHICH,NAME.CpuTime())
#endif

//====================================================================
AliForwardMultiplicityTask::AliForwardMultiplicityTask()
  : AliForwardMultiplicityBase(),
    fESDFMD(),
    fEventInspector(),
    fESDFixer(),
    fSharingFilter(),
    fDensityCalculator(),
    fCorrections(),
    fHistCollector(),
    fEventPlaneFinder()
{
  // 
  // Constructor
  //
  DGUARD(fDebug, 3,"Default CTOR of AliForwardMultiplicityTask");
}

//____________________________________________________________________
AliForwardMultiplicityTask::AliForwardMultiplicityTask(const char* name)
  : AliForwardMultiplicityBase(name),
    fESDFMD(),
    fEventInspector("event"),
    fESDFixer("esdFizer"),
    fSharingFilter("sharing"), 
    fDensityCalculator("density"),
    fCorrections("corrections"),
    fHistCollector("collector"),
    fEventPlaneFinder("eventplane")
{
  // 
  // Constructor 
  // 
  // Parameters:
  //    name Name of task 
  //
  DGUARD(fDebug, 3,"named CTOR of AliForwardMultiplicityTask: %s", name);
}


//____________________________________________________________________
void
AliForwardMultiplicityTask::SetDoTiming(Bool_t enable)
{
#ifndef ENABLE_TIMING
  if (enable) 
    AliWarning("Timing of task explicitly disabled in compilation");
#else 
  fDoTiming = enable;
#endif
}
      
//____________________________________________________________________
void
AliForwardMultiplicityTask::PreCorrections(const AliESDEvent* esd)
{
  if (!esd) return; 
  
  AliESDFMD* esdFMD = esd->GetFMDData();  
  if (!esdFMD) return;

  Int_t tgt = GetESDFixer().FindTargetNoiseFactor(*esdFMD, false);
  if (tgt <= 0) {
    // If the target noise factor is 0 or less, disable the noise/gain
    // correction.
    GetESDFixer().SetRecoNoiseFactor(4);
    fNeededCorrections ^= AliForwardCorrectionManager::kNoiseGain;
  }
  else 
    AliWarning("The noise corrector has been enabled!");
}
//____________________________________________________________________
Bool_t
AliForwardMultiplicityTask::PreEvent()
{
  // Clear stuff 
  fHistos.Clear();
  fESDFMD.Clear();
  fAODFMD.Clear();
  fAODEP.Clear();
  return true;
}
//____________________________________________________________________
Bool_t
AliForwardMultiplicityTask::Event(AliESDEvent& esd)
{
  // 
  // Process each event 
  // 
  // Parameters:
  //    option Not used
  //  
  MAKE_SW(total);
  MAKE_SW(individual);
  START_SW(total);
  
  DGUARD(fDebug,1,"Process the input event");

  // Inspect the event
  START_SW(individual);
  Bool_t   lowFlux   = kFALSE;
  UInt_t   triggers  = 0;
  UShort_t ivz       = 0;
  TVector3 ip;
  Double_t cent      = -1;
  UShort_t nClusters = 0;
  UInt_t   found     = fEventInspector.Process(&esd, triggers, lowFlux, 
					       ivz, ip, cent, nClusters);
  FILL_SW(individual,kTimingEventInspector);
  
  if (found & AliFMDEventInspector::kNoEvent)    return false;
  if (found & AliFMDEventInspector::kNoTriggers) return false;

  // Set trigger bits, and mark this event for storage 
  fAODFMD.SetTriggerBits(triggers);
  fAODFMD.SetSNN(fEventInspector.GetEnergy());
  fAODFMD.SetSystem(fEventInspector.GetCollisionSystem());
  fAODFMD.SetCentrality(cent);
  fAODFMD.SetNClusters(nClusters);
  MarkEventForStore();
 
  // Do not check if SPD data is there - potential bias 
  // if (found & AliFMDEventInspector::kNoSPD)      return false;
  if (found    & AliFMDEventInspector::kNoFMD)      return false;
  if (found    & AliFMDEventInspector::kNoVertex)   return false;
  // Also analyse pile-up events - we'll remove them in later steps. 
  // if (triggers & AliAODForwardMult::kPileUp)        return false;
  fAODFMD.SetIpZ(ip.Z());
  if (found & AliFMDEventInspector::kBadVertex)     return false;

  // We we do not want to use low flux specific code, we disable it here. 
  if (!fEnableLowFlux) lowFlux = false;

  // Get FMD data 
  AliESDFMD* esdFMD = esd.GetFMDData();  

  // Fix up the the ESD 
  GetESDFixer().Fix(*esdFMD, ip.Z());

  // Apply the sharing filter (or hit merging or clustering if you like)
  START_SW(individual);
  if (!fSharingFilter.Filter(*esdFMD, lowFlux, fESDFMD, ip.Z())) { 
    AliWarning("Sharing filter failed!");
    return false;
  }
  FILL_SW(individual,kTimingSharingFilter);
  
  // Calculate the inclusive charged particle density 
  START_SW(individual);
  if (!fDensityCalculator.Calculate(fESDFMD, fHistos, lowFlux, cent, ip)) { 
    // if (!fDensityCalculator.Calculate(*esdFMD, fHistos, ivz, lowFlux)) { 
    AliWarning("Density calculator failed!");
    return false;
  }
  FILL_SW(individual,kTimingDensityCalculator);

  // Check if we should do the event plane finder
  if (fEventInspector.GetCollisionSystem() == AliFMDEventInspector::kPbPb) {
    START_SW(individual);
    if (!fEventPlaneFinder.FindEventplane(&esd, fAODEP, 
					  &(fAODFMD.GetHistogram()), &fHistos))
      AliWarning("Eventplane finder failed!");
    FILL_SW(individual,kTimingEventPlaneFinder);
  }
  
  // Check how many rings have been marked for skipping 
  Int_t nSkip = 0;
  for (UShort_t d=1; d<=3; d++) { 
    for (UShort_t q=0; q<=(d/2); q++) { 
      TH2D* h = fHistos.Get(d,q == 0 ? 'I' : 'O');
      if (h && h->TestBit(AliForwardUtil::kSkipRing)) nSkip++;
    }
  }
  if (nSkip > 0) 
    // Skip the rest if we have too many outliers 
    return false;
  
  // Do the secondary and other corrections. 
  START_SW(individual);
  if (!fCorrections.Correct(fHistos, ivz)) { 
    AliWarning("Corrections failed");
    return false;
  }
  FILL_SW(individual,kTimingCorrections);

  // Collect our `super' histogram 
  START_SW(individual);
  if (!fHistCollector.Collect(fHistos, fRingSums, 
			      ivz, fAODFMD.GetHistogram(),
			      fAODFMD.GetCentrality())) {
    AliWarning("Histogram collector failed");
    return false;
  }
  FILL_SW(individual,kTimingHistCollector);

  if (fAODFMD.IsTriggerBits(AliAODForwardMult::kInel) && 
      !(triggers & AliAODForwardMult::kPileUp) && nSkip < 1) 
    // Collect rough Min. Bias result
    fHData->Add(&(fAODFMD.GetHistogram()));

  FILL_SW(total,kTimingTotal);
  
  return true;
}


//
// EOF
//

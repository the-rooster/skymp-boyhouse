#pragma once
#include "libespm/Combiner.h"
#include "libespm/espm.h"
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <vector>

class LeveledListUtils
{
public:
  struct Options
  {
    // A campaign can supply its private globals and one allocation RNG.
    std::function<float(uint32_t)> readGlobal;
    std::mt19937* random = nullptr;
  };
  struct Entry
  {
    uint32_t formId = 0;
    uint32_t count = 0;
  };

  // It seems that pcLevel=0 makes it thinking that pcLevel=maximum possible pc
  // level
  static std::vector<Entry> EvaluateList(const espm::CombineBrowser& br,
                                         const espm::LookupResult& lookupRes,
                                         uint32_t pcLevel = 0,
                                         uint8_t* chanceNoneOverride = nullptr,
                                         const Options* options = nullptr);

  static std::map<uint32_t, uint32_t> EvaluateListRecurse(
    const espm::CombineBrowser& br, const espm::LookupResult& lookupRes,
    uint32_t countMult = 1, uint32_t pcLevel = 0,
    uint8_t* chanceNoneOverride = nullptr, const Options* options = nullptr);

  static std::vector<uint32_t> EvaluateTemplateChain(
    const espm::CombineBrowser& br, const espm::LookupResult& headNpc,
    uint32_t pcLevel, const Options* options = nullptr);
};

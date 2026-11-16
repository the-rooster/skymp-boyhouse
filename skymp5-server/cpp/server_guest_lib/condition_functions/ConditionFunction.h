#pragma once
#include <cstdint>

class MpActor;
struct Condition;
struct ConditionEvaluatorContext;

class ConditionFunction
{
public:
  virtual ~ConditionFunction() = default;

  virtual const char* GetName() const = 0;

  // https://en.uesp.net/wiki/Skyrim_Mod:Function_Indices
  virtual uint16_t GetFunctionIndex() const = 0;

  virtual float Execute(MpActor& actor, uint32_t parameter1,
                        uint32_t parameter2,
                        const ConditionEvaluatorContext& context) = 0;

  // String-valued engine parameters (CIS1/CIS2) must not be coerced to IDs.
  // Existing numeric functions retain their original parsing semantics.
  virtual float Execute(MpActor& actor, const Condition& condition,
                        const ConditionEvaluatorContext& context);
};

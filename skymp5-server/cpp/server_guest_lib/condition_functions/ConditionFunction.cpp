#include "ConditionFunction.h"
#include "Condition.h"
#include <cstdlib>

namespace {
uint32_t NumericParameter(const std::string& parameter)
{
  const char* str = parameter.c_str();
  int base = 10;
  if (parameter.length() > 2 && str[0] == '0') {
    if (str[1] == 'x' || str[1] == 'X') {
      base = 16;
    } else if (str[1] == 'b' || str[1] == 'B') {
      base = 2;
      str += 2;
    }
  }
  return static_cast<uint32_t>(std::strtoul(str, nullptr, base));
}
}

float ConditionFunction::Execute(MpActor& actor, const Condition& condition,
                                 const ConditionEvaluatorContext& context)
{
  return Execute(actor, NumericParameter(condition.parameter1),
                 NumericParameter(condition.parameter2), context);
}

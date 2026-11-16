#include "WorldInstanceBinding.h"

Napi::Value WorldInstanceBinding::Get(Napi::Env env, ScampServer& server,
                                      uint32_t formId)
{
  auto& ref =
    server.GetPartOne()->worldState.GetFormAt<MpObjectReference>(formId);
  return Napi::String::New(env, ref.GetWorldInstance());
}

void WorldInstanceBinding::Set(Napi::Env env, ScampServer& server,
                               uint32_t formId, Napi::Value value)
{
  if (!value.IsString()) {
    throw std::invalid_argument("worldInstance must be a string");
  }
  auto& ref =
    server.GetPartOne()->worldState.GetFormAt<MpObjectReference>(formId);
  ref.SetWorldInstance(value.As<Napi::String>().Utf8Value());
}

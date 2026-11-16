#include "ObjectReferenceApi.h"

#include "NullPointerException.h"

namespace {
RE::TESObjectREFR* GetArgObjectReference(const Napi::Value& arg)
{
  auto formId = NapiHelper::ExtractUInt32(arg, "refrFormId");
  auto refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);

  if (!refr) {
    throw NullPointerException("refr");
  }

  return refr;
}
}

Napi::Value ObjectReferenceApi::SetCollision(const Napi::CallbackInfo& info)
{
  auto refr = GetArgObjectReference(info[0]);
  refr->SetCollision(NapiHelper::ExtractBoolean(info[1], "collision"));
  return info.Env().Undefined();
}

Napi::Value ObjectReferenceApi::GetReferenceHandle(
  const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "refrFormId");
  auto refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
  // Unlike FormID (or base ID), the engine handle includes the lifetime's age.
  // Callers must additionally invalidate identities when loading another save.
  const auto handle =
    refr && !refr->IsDeleted() ? refr->GetHandle().native_handle() : 0;
  return Napi::Number::New(info.Env(), handle);
}

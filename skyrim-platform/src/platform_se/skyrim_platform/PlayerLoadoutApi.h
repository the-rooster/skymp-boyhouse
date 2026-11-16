#pragma once

namespace PlayerLoadoutApi {
void Register(Napi::Env env, Napi::Object& exports);
// Called before publishing lifecycle events to JavaScript. A load attempt is
// not evidence of a replaced world; only success or NewGame clears
// uncertainty.
void InvalidateWorld();
void BeginLoad();
void CompleteLoad(bool success);
void ResetRuntime();
}

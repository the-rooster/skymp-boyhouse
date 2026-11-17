#pragma once

namespace ActorFactionsApi {
void Register(Napi::Env env, Napi::Object& exports);
void BeginLoad();
void CompleteLoad(bool success);
void InvalidateWorld();
void ResetRuntime();
}

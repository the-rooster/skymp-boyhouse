#include "SKSE/Interfaces.h"

namespace {
void CheckRuntimeDependencies(const REL::Version& runtime)
{
  // The experimental 1.7 port has only been inspected against this Steam PE.
  // Check before LoadLibrary: the implementation resolves IDs in static objects.
  if (runtime == REL::Version{ 1, 7, 104, 0 }) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        dos->e_lfanew > 0x100000) {
      throw std::runtime_error("Unable to identify the Skyrim executable");
    }
    const auto pe =
      reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe->FileHeader.TimeDateStamp != 0x6A8C7046 ||
        pe->OptionalHeader.SizeOfImage != 0x03929000) {
      throw std::runtime_error(
        "This experimental SkyrimPlatform build requires the inspected "
        "Steam 1.7.104 executable");
    }
  }

  const auto library = std::filesystem::path("Data/SKSE/Plugins") /
    ("versionlib-" + runtime.string("-") + ".bin");
  if (!std::filesystem::is_regular_file(library)) {
    throw std::runtime_error(
      "Missing Address Library for Skyrim " + runtime.string() + ": " +
      library.string() +
      ". Install the official Address Library matching this runtime.");
  }
}
}

typedef void (*IpcMessageCallback)(const uint8_t* data, uint32_t length,
                                   void* state);

typedef bool (*SKSEPlugin_Load_Impl)(void*);
typedef uint32_t (*SkyrimPlatform_IpcSubscribe_Impl)(const char*,
                                                     IpcMessageCallback,
                                                     void*);
typedef void (*SkyrimPlatform_IpcUnsubscribe_Impl)(uint32_t);
typedef void (*SkyrimPlatform_IpcSend_Impl)(const char*, const uint8_t*,
                                            uint32_t);

class PlatformImplInterface
{
public:
  static PlatformImplInterface& GetSingleton()
  {
    static PlatformImplInterface instance;
    return instance;
  }

  bool Load(void* skse) { return load(skse); }

  uint32_t IpcSubscribe(const char* systemName, IpcMessageCallback callback,
                        void* state)
  {
    return ipcSubscribe(systemName, callback, state);
  }

  void IpcUnsubscribe(uint32_t subscriptionId)
  {
    return ipcUnsubscribe(subscriptionId);
  }

  void IpcSend(const char* systemName, const uint8_t* data, uint32_t length)
  {
    return ipcSend(systemName, data, length);
  }

private:
  PlatformImplInterface()
  {
    AppendToPathEnv(std::filesystem::current_path() / L"Data" / L"Platform" /
                    L"Distribution" / L"RuntimeDependencies");

    HMODULE skyrimPlatformImpl = LoadLibraryA("SkyrimPlatformImpl.dll");
    if (!skyrimPlatformImpl) {
      throw std::runtime_error(
        "Unable to load SkyrimPlatformImpl.dll: Error " +
        std::to_string(GetLastError()));
    }

    load = reinterpret_cast<SKSEPlugin_Load_Impl>(
      GetProcAddress(skyrimPlatformImpl, "SKSEPlugin_Load_Impl"));
    if (!load) {
      throw std::runtime_error("Unable to find SKSEPlugin_Load_Impl: Error " +
                               std::to_string(GetLastError()));
    }

    ipcSubscribe = reinterpret_cast<SkyrimPlatform_IpcSubscribe_Impl>(
      GetProcAddress(skyrimPlatformImpl, "SkyrimPlatform_IpcSubscribe_Impl"));
    if (!ipcSubscribe) {
      throw std::runtime_error(
        "Unable to find SkyrimPlatform_IpcSubscribe_Impl: Error " +
        std::to_string(GetLastError()));
    }

    ipcSend = reinterpret_cast<SkyrimPlatform_IpcSend_Impl>(
      GetProcAddress(skyrimPlatformImpl, "SkyrimPlatform_IpcSend_Impl"));
    if (!ipcSend) {
      throw std::runtime_error(
        "Unable to find SkyrimPlatform_IpcSend_Impl: Error " +
        std::to_string(GetLastError()));
    }

    ipcUnsubscribe =
      reinterpret_cast<SkyrimPlatform_IpcUnsubscribe_Impl>(GetProcAddress(
        skyrimPlatformImpl, "SkyrimPlatform_IpcUnsubscribe_Impl"));
    if (!ipcUnsubscribe) {
      throw std::runtime_error(
        "Unable to find SkyrimPlatform_IpcUnsubscribe_Impl: Error " +
        std::to_string(GetLastError()));
    }
  }

  SKSEPlugin_Load_Impl load = nullptr;
  SkyrimPlatform_IpcSubscribe_Impl ipcSubscribe = nullptr;
  SkyrimPlatform_IpcSend_Impl ipcSend = nullptr;
  SkyrimPlatform_IpcUnsubscribe_Impl ipcUnsubscribe = nullptr;
};

extern "C" {

DLLEXPORT uint32_t SkyrimPlatform_IpcSubscribe(const char* systemName,
                                               IpcMessageCallback callback,
                                               void* state)
{
  return PlatformImplInterface::GetSingleton().IpcSubscribe(systemName,
                                                            callback, state);
}

DLLEXPORT void SkyrimPlatform_IpcUnsubscribe(uint32_t subscriptionId)
{
  return PlatformImplInterface::GetSingleton().IpcUnsubscribe(subscriptionId);
}

DLLEXPORT void SkyrimPlatform_IpcSend(const char* systemName,
                                      const uint8_t* data, uint32_t length)
{
  return PlatformImplInterface::GetSingleton().IpcSend(systemName, data,
                                                       length);
}

DLLEXPORT bool SKSEPlugin_Load(void* skse)
{
  try {
    const auto* loadInterface = static_cast<const SKSE::LoadInterface*>(skse);
    const auto runtime = loadInterface->RuntimeVersion();
    if (loadInterface->IsEditor() ||
        (runtime != REL::Version{ 1, 6, 1170, 0 } &&
         runtime != REL::Version{ 1, 7, 104, 0 })) {
      throw std::runtime_error(
        "This SkyrimPlatform build targets Skyrim 1.6.1170 and the "
        "experimental 1.7.104 port. Runtime: " + runtime.string());
    }
    CheckRuntimeDependencies(runtime);
    return PlatformImplInterface::GetSingleton().Load(skse);
  } catch (std::exception& e) {
    MessageBoxA(0, e.what(), "Fatal (SkyrimPlatform)", MB_ICONERROR);
    return false;
  }
}
}

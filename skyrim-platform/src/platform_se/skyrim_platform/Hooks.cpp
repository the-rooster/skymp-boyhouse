#include "Hooks.h"
#include "EventHandler.h"
#include <mutex>

namespace hook::internal {

uintptr_t GetAllocationBase(void* ptr)
{
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(ptr, &mbi, sizeof(mbi))) {
    return reinterpret_cast<uintptr_t>(mbi.AllocationBase);
  }
  return 0;
}

std::mutex g_mutex;
std::vector<std::tuple<std::string, std::string, RE::BSScript::IFunction*,
                       uintptr_t, uintptr_t, uintptr_t>>
  g_boundNatives;
}

/**
 * @brief This hooks into the game main cycle
 * which behaves much like "our" tick cycle
 * but with a slight artificial delay between ticks.
 * Is mostly used for testing atm.
 */
struct OnFrameUpdate
{
  static void thunk(std::int64_t unk) { func(unk); };
  static inline REL::Relocation<decltype(&thunk)> func;
};

void InstallOnFrameUpdateHook()
{
  Hooks::write_thunk_call<OnFrameUpdate>(
    Offsets::Hooks::FrameUpdate.address());
}

struct OnConsoleVPrint
{
  static void thunk(void* unk1, const char* msg)
  {
    if (msg) {
      EventHandler::SendEventConsoleMsg(msg);
    }

    func(unk1, msg);
  };
  static inline REL::Relocation<decltype(&thunk)> func;
};

void InstallOnConsoleVPrintHook()
{
  const auto site = Offsets::Hooks::VPrint.address();
  if (*reinterpret_cast<const uint8_t*>(site) != 0xE8) {
    throw std::runtime_error("Console print hook does not point to a CALL");
  }
  if (REL::Module::get().version() == REL::Version{ 1, 7, 104, 0 }) {
    // Verified on Steam PE timestamp 0x6A8C7046, image size 0x03929000.
    // VPrint ID 51110 is RVA 0x90F1E0; +0x300 calls string append at 0x1497E0.
    // Do not install if the site has moved or another plugin has patched it.
    constexpr uint8_t expected[] = { 0xE8, 0xFB, 0xA2, 0x83, 0xFF };
    if (site - REL::Module::get().base() != 0x90F4E0 ||
        std::memcmp(reinterpret_cast<const void*>(site), expected,
                    sizeof(expected)) != 0) {
      throw std::runtime_error("Unexpected Skyrim 1.7.104 console print hook");
    }
  }
  Hooks::write_thunk_call<OnConsoleVPrint>(site);
}

bool BindNativeMethod(RE::BSScript::Internal::VirtualMachine* thisArg,
                      RE::BSScript::IFunction* func);

decltype(&BindNativeMethod) _BindNativeMethod;

void HookVirtualMachineBind()
{
  spdlog::info("Hooking VirtualMachine::Bind");
  REL::Relocation<std::uintptr_t> Vtbl{
    RE::BSScript::Internal::VirtualMachine::VTABLE[0]
  };
  _BindNativeMethod = reinterpret_cast<decltype(_BindNativeMethod)>(
    Vtbl.write_vfunc(0x18, BindNativeMethod));
}

bool BindNativeMethod(RE::BSScript::Internal::VirtualMachine* thisArg,
                      RE::BSScript::IFunction* func)
{
  // Only native functions have the callback field. SKSE 2.3.x preserves
  // NativeFunctionBase (0x50), followed by the callback and long-signature flag.
  if (func && func->GetIsNative()) {
    const auto* raw = reinterpret_cast<const uint8_t*>(func);
    const auto realFunc = *reinterpret_cast<const uintptr_t*>(raw + 0x50);
    const auto moduleBase =
      hook::internal::GetAllocationBase(reinterpret_cast<void*>(realFunc));
    const auto funcOffset = realFunc - moduleBase;
    const auto runtime = REL::Module::get().version();
    const auto moduleName = fmt::format("skse64_{}_{}_{}.dll", runtime[0],
                                        runtime[1], runtime[2]);
    const auto skse = reinterpret_cast<uintptr_t>(GetModuleHandleA(moduleName.c_str()));
    const uintptr_t isLongSignature =
      skse && moduleBase == skse ? *(raw + 0x58) : 0;
    const auto* funcName = func->GetName().data();
    const auto* className = func->GetObjectTypeName().data();
    spdlog::trace("VirtualMachine::Bind {} {} funcOffset={:x}, moduleBase={:x}",
                  className, funcName, funcOffset, moduleBase);
    std::lock_guard<std::mutex> lock(hook::internal::g_mutex);
    hook::internal::g_boundNatives.push_back(
      { className, funcName, func, moduleBase, funcOffset, isLongSignature });
  }
  return _BindNativeMethod(thisArg, func);
}

std::vector<std::tuple<std::string, std::string, RE::BSScript::IFunction*,
                       uintptr_t, uintptr_t, uintptr_t>>
Hooks::GetBoundNatives()
{
  std::lock_guard<std::mutex> lock(hook::internal::g_mutex);
  return hook::internal::g_boundNatives;
}

void Hooks::Install()
{
  // InstallOnFrameUpdateHook();
  InstallOnConsoleVPrintHook();
  HookVirtualMachineBind();

  logger::info("CommonLib hooks installed.");
}

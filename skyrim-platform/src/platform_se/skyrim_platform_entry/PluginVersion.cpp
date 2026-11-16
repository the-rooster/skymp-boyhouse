#include "SKSE/Interfaces.h"

extern "C" DLLEXPORT constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
  SKSE::PluginVersionData data;
  data.PluginName("SkyrimPlatform");
  data.AuthorName("SkyMP Team and Contributors");
  data.PluginVersion(REL::Version{ Version::MAJOR, Version::MINOR,
                                   Version::PATCH, Version::BETA });
  data.UsesAddressLibrary();
  data.UsesAddressLibraryV5();
  data.UsesStructsPost629();
  // These flags describe the relocation format, not arbitrary future runtime
  // compatibility. SKSEPlugin_Load checks the runtime before loading our DLL.
  return data;
}();

#pragma once
#include "CustomPacketMessage.h"
#include "MessageSerializerFactory.h"
#include "Networking.h"

class Bot
{
public:
  Bot(std::shared_ptr<Networking::IClient> cl_, bool captureCustom = false)
    : cl(cl_)
    , serializer(captureCustom
                   ? MessageSerializerFactory::CreateMessageSerializer()
                   : nullptr)
  {
  }

  void Destroy() { cl.reset(); }

  void Send(std::string packet)
  {
    if (cl)
      cl->Send(reinterpret_cast<Networking::PacketData>(packet.data()),
               packet.size(), true);
  }

  void Tick()
  {
    if (cl)
      cl->Tick(
        [](void* state, Networking::PacketType type,
           Networking::PacketData data, size_t length, const char*) {
          auto& self = *static_cast<Bot*>(state);
          if (!self.serializer || self.captureFailed ||
              type != Networking::PacketType::Message)
            return;
          try {
            const auto decoded = self.serializer->Deserialize(data, length);
            if (!decoded || decoded->msgType != MsgType::CustomPacket)
              return;
            const auto& dump =
              static_cast<CustomPacketMessage*>(decoded->message.get())
                ->contentJsonDump;
            if (self.packets.size() >= 4096 ||
                dump.size() > 8 * 1024 * 1024 - self.bytes)
              throw std::runtime_error("Bot custom packet capture overflow");
            self.packets.push_back(dump);
            self.bytes += dump.size();
          } catch (...) {
            self.captureFailed = true;
            self.packets.clear();
            self.bytes = 0;
          }
        },
        this);
  }

  std::vector<std::string> PollCustomPackets()
  {
    if (!serializer)
      throw std::runtime_error(
        "Create this bot with custom packet capture enabled");
    Tick();
    if (captureFailed)
      throw std::runtime_error(
        "Bot custom packet capture failed or exceeded its limit");
    std::vector<std::string> result;
    result.swap(packets);
    bytes = 0;
    return result;
  }

private:
  std::shared_ptr<Networking::IClient> cl;
  std::shared_ptr<MessageSerializer> serializer;
  std::vector<std::string> packets;
  size_t bytes = 0;
  bool captureFailed = false;
};

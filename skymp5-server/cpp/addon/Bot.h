#pragma once
#include "CampaignFrameMessage.h"
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
            auto decoded = self.serializer->Deserialize(data, length);
            auto* queue = &self.unframed;
            if (decoded && decoded->msgType == MsgType::CampaignFrame) {
              const auto& frame =
                static_cast<const CampaignFrameMessage&>(*decoded->message);
              if (frame.epoch != self.expectedEpoch)
                return;
              decoded = self.serializer->Deserialize(frame.payload.data(),
                                                     frame.payload.size());
              queue = &self.framed;
            }
            if (!decoded || decoded->msgType != MsgType::CustomPacket)
              return;
            const auto& dump =
              static_cast<CustomPacketMessage*>(decoded->message.get())
                ->contentJsonDump;
            if (self.unframed.packets.size() + self.framed.packets.size() >=
                  4096 ||
                dump.size() > 8 * 1024 * 1024 - self.unframed.bytes -
                    self.framed.bytes)
              throw std::runtime_error("Bot custom packet capture overflow");
            queue->packets.push_back(dump);
            queue->bytes += dump.size();
          } catch (...) {
            self.captureFailed = true;
            self.unframed = {};
            self.framed = {};
          }
        },
        this);
  }

  std::vector<std::string> PollCustomPackets(
    const std::optional<std::string>& epoch = std::nullopt)
  {
    if (!serializer)
      throw std::runtime_error(
        "Create this bot with custom packet capture enabled");
    if (epoch && !CampaignFrameMessage::ValidEpoch(*epoch))
      throw std::runtime_error("Invalid bot capture epoch");
    const auto next = epoch.value_or("");
    if (expectedEpoch != next) {
      framed = {};
      expectedEpoch = next;
    }
    Tick();
    if (captureFailed)
      throw std::runtime_error(
        "Bot custom packet capture failed or exceeded its limit");
    std::vector<std::string> result;
    auto& queue = epoch ? framed : unframed;
    result.swap(queue.packets);
    queue.bytes = 0;
    return result;
  }

private:
  std::shared_ptr<Networking::IClient> cl;
  std::shared_ptr<MessageSerializer> serializer;
  struct Queue
  {
    std::vector<std::string> packets;
    size_t bytes = 0;
  };
  Queue unframed, framed;
  std::string expectedEpoch;
  bool captureFailed = false;
};

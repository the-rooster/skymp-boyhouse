#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <string>
#include <type_traits>
#include <vector>

// Transport context around one existing packet, including its packet ID.
// The payload keeps its binary representation; nested frames are forbidden.
struct CampaignFrameMessage : public IMessageBase
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::CampaignFrame)>{};
  static constexpr size_t kMaxPayload = 1024 * 1024;
  std::string epoch;
  std::vector<uint8_t> payload;

  static bool ValidEpoch(const std::string& value);
  void Validate() const;
  void WriteBinary(SLNet::BitStream& stream) const override;
  void ReadBinary(SLNet::BitStream& stream) override;
  void WriteJson(nlohmann::json& json) const override;
  void ReadJson(const simdjson::dom::element& json) override;
};

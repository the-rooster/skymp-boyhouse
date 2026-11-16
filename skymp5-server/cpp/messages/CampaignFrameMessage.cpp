#include "CampaignFrameMessage.h"
#include "MessageSerializerFactory.h"
#include "MinPacketId.h"
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <slikenet/BitStream.h>

namespace {
MessageSerializer& Serializer()
{
  static auto serializer = MessageSerializerFactory::CreateMessageSerializer();
  return *serializer;
}

void RequireInnerType(uint64_t type)
{
  if (type == 0 || type >= static_cast<uint8_t>(MsgType::Max) ||
      type == static_cast<uint8_t>(MsgType::CampaignFrame))
    throw std::runtime_error("Invalid or nested campaign frame payload");
}
}

bool CampaignFrameMessage::ValidEpoch(const std::string& value)
{
  return value.size() == 32 &&
    std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void CampaignFrameMessage::Validate() const
{
  if (!ValidEpoch(epoch) || payload.size() < 2 ||
      payload.size() > kMaxPayload ||
      payload[0] != static_cast<uint8_t>(Networking::MinPacketId))
    throw std::runtime_error("Invalid campaign frame");
  if (payload[1] == '{') {
    simdjson::dom::parser parser;
    auto json = parser
                  .parse(reinterpret_cast<const char*>(payload.data() + 1),
                         payload.size() - 1)
                  .value();
    RequireInnerType(json["t"].get_uint64().value());
  } else {
    RequireInnerType(payload[1]);
  }
}

void CampaignFrameMessage::WriteBinary(SLNet::BitStream& stream) const
{
  Validate();
  stream.Write(static_cast<uint8_t>(kMsgType));
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < 32; i += 2)
    stream.Write(
      static_cast<uint8_t>((nibble(epoch[i]) << 4) | nibble(epoch[i + 1])));
  stream.Write(static_cast<uint32_t>(payload.size()));
  stream.Write(reinterpret_cast<const char*>(payload.data()), payload.size());
}

void CampaignFrameMessage::ReadBinary(SLNet::BitStream& stream)
{
  uint8_t type = 0;
  std::array<uint8_t, 16> token{};
  uint32_t length = 0;
  if (!stream.Read(type) || type != static_cast<uint8_t>(kMsgType))
    throw std::runtime_error("Invalid campaign frame header");
  for (auto& byte : token)
    if (!stream.Read(byte))
      throw std::runtime_error("Truncated campaign frame epoch");
  if (!stream.Read(length) || length < 2 || length > kMaxPayload ||
      stream.GetNumberOfUnreadBits() != length * 8)
    throw std::runtime_error("Invalid campaign frame length");
  CampaignFrameMessage next;
  static constexpr char hex[] = "0123456789abcdef";
  for (auto byte : token) {
    next.epoch.push_back(hex[byte >> 4]);
    next.epoch.push_back(hex[byte & 15]);
  }
  next.payload.resize(length);
  if (!stream.Read(reinterpret_cast<char*>(next.payload.data()), length))
    throw std::runtime_error("Truncated campaign frame payload");
  next.Validate();
  epoch = std::move(next.epoch);
  payload = std::move(next.payload);
}

void CampaignFrameMessage::WriteJson(nlohmann::json& json) const
{
  Validate();
  auto decoded = Serializer().Deserialize(payload.data(), payload.size());
  if (!decoded)
    throw std::runtime_error("Unknown campaign frame payload");
  nlohmann::json inner;
  decoded->message->WriteJson(inner);
  json = { { "t", static_cast<uint8_t>(kMsgType) },
           { "epoch", epoch },
           { "message", std::move(inner) } };
}

void CampaignFrameMessage::ReadJson(const simdjson::dom::element& json)
{
  if (json["t"].get_uint64().value() != static_cast<uint8_t>(kMsgType))
    throw std::runtime_error("Invalid campaign frame type");
  CampaignFrameMessage next;
  next.epoch = std::string(json["epoch"].get_string().value());
  if (!ValidEpoch(next.epoch))
    throw std::runtime_error("Invalid campaign frame epoch");
  simdjson::dom::element inner, raw;
  const auto innerError = json["message"].get(inner);
  const auto rawError = json["raw"].get(raw);
  if ((innerError == simdjson::SUCCESS) == (rawError == simdjson::SUCCESS))
    throw std::runtime_error("Campaign frame requires exactly one payload");
  if (innerError == simdjson::SUCCESS) {
    RequireInnerType(inner["t"].get_uint64().value());
    const auto text = simdjson::minify(inner);
    if (text.size() > kMaxPayload)
      throw std::runtime_error("Campaign frame JSON payload is too large");
    SLNet::BitStream stream;
    Serializer().Serialize(text.c_str(), stream);
    next.payload.assign(stream.GetData(),
                        stream.GetData() + stream.GetNumberOfBytesUsed());
  } else {
    auto bytes = raw.get_array().value();
    if (bytes.size() > kMaxPayload)
      throw std::runtime_error("Campaign frame raw payload is too large");
    next.payload.reserve(bytes.size());
    for (auto element : bytes) {
      auto byte = element.get_uint64().value();
      if (byte > 255)
        throw std::runtime_error("Invalid campaign frame byte");
      next.payload.push_back(static_cast<uint8_t>(byte));
    }
  }
  next.Validate();
  epoch = std::move(next.epoch);
  payload = std::move(next.payload);
}

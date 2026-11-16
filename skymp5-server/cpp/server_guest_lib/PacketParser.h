#pragma once
#include "ActionListener.h"
#include "NetworkingInterface.h" // UserId, PacketData
#include <cstdint>
#include <memory>

class PacketParser
{
public:
  PacketParser();
  void TransformPacketIntoAction(Networking::UserId userId,
                                 Networking::PacketData packetData,
                                 size_t packetLength,
                                 ActionListener& actionListener);

private:
  void Dispatch(Networking::UserId userId, Networking::PacketData data,
                size_t length, ActionListener& listener,
                bool hasVerifiedEpoch);
  struct Impl;
  std::shared_ptr<Impl> pImpl;
};

#include "papyrus-vm/PexScript.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sodium.h>
#include <stdexcept>
#include <string_view>

namespace {
class ExecutionHasher
{
public:
  ExecutionHasher()
  {
    if (crypto_hash_sha256_init(&state) != 0)
      throw std::runtime_error("Cannot initialize PEX execution hash");
    Text("skymp.pex.execution.v1");
  }

  void Byte(uint8_t value) { Append(&value, 1); }

  void Number(uint64_t value, size_t width)
  {
    std::array<unsigned char, 8> bytes{};
    for (size_t i = 0; i < width; ++i) {
      bytes[width - i - 1] = static_cast<unsigned char>(value & 0xff);
      value >>= 8;
    }
    Append(bytes.data(), width);
  }

  void Count(size_t count)
  {
    // PEX declaration/instruction collections use uint16 counts on disk.
    if (count > 65535)
      throw std::runtime_error("PEX execution collection exceeds limit");
    Number(count, 4);
  }

  void Text(std::string_view value)
  {
    if (value.size() > 65535)
      throw std::runtime_error("PEX execution string exceeds limit");
    Number(value.size(), 4);
    Append(value.data(), value.size());
  }

  void Operand(const VarValue& value)
  {
    if (value.promise || value.pArray)
      throw std::runtime_error("Runtime value in PEX execution metadata");
    const auto type = value.GetType();
    Byte(type);
    // Synthetic instructions may carry declared receiver type information.
    Text(value.objectType);
    switch (type) {
      case VarValue::kType_Object:
        if (static_cast<IGameObject*>(value))
          throw std::runtime_error("Runtime object in PEX execution metadata");
        return;
      case VarValue::kType_Identifier:
      case VarValue::kType_String: {
        const auto* text = static_cast<const char*>(value);
        if (!text)
          throw std::runtime_error("Null string in PEX execution metadata");
        size_t length = 0;
        while (length <= 65535 && text[length])
          ++length;
        if (length > 65535)
          throw std::runtime_error("PEX execution string exceeds limit");
        Text(std::string_view(text, length));
        return;
      }
      case VarValue::kType_Integer:
        Number(static_cast<uint32_t>(static_cast<int32_t>(value)), 4);
        return;
      case VarValue::kType_Float: {
        // Hash the actual interpreter value, including signed zero. Narrowing
        // synthetic doubles to disk float32 could hide executable changes.
        static_assert(sizeof(double) == sizeof(uint64_t) &&
                      std::numeric_limits<double>::is_iec559);
        const auto number = static_cast<double>(value);
        if (!std::isfinite(number))
          throw std::runtime_error("Nonfinite PEX execution float");
        uint64_t bits;
        std::memcpy(&bits, &number, sizeof(bits));
        Number(bits, 8);
        return;
      }
      case VarValue::kType_Bool:
        Byte(static_cast<bool>(value) ? 1 : 0);
        return;
      default:
        throw std::runtime_error("Unsupported PEX execution operand type");
    }
  }

  void Function(const FunctionInfo& function)
  {
    Byte(function.valid ? 1 : 0);
    Text(function.returnType);
    Number(function.userFlags, 4);
    Byte(function.flags);
    for (const auto* declarations : { &function.params, &function.locals }) {
      Count(declarations->size());
      for (const auto& declaration : *declarations) {
        Text(declaration.name);
        Text(declaration.type);
      }
    }
    Count(function.code.instructions.size());
    for (const auto& instruction : function.code.instructions) {
      // Skyrim opcodes, including the two array-find instructions after the
      // FunctionCode enum's last named opcode; same arities as Reader.
      constexpr std::array<int, 36> arities{ 0, 3, 3, 3, 3, 3,  3,  3,  3,
                                             3, 2, 2, 2, 2, 2,  3,  3,  3,
                                             3, 3, 1, 2, 2, -4, -3, -4, 1,
                                             3, 3, 3, 2, 2, 3,  3,  4,  4 };
      if (instruction.op >= arities.size())
        throw std::runtime_error("Invalid PEX execution opcode");
      const auto arity = arities[instruction.op];
      size_t expected = arity < 0 ? -arity : arity;
      if (arity < 0) {
        if (instruction.args.size() < expected)
          throw std::runtime_error("Truncated PEX execution call");
        const auto& count = instruction.args[expected - 1];
        if (count.GetType() != VarValue::kType_Integer)
          throw std::runtime_error("Invalid PEX execution call count");
        const auto additional = static_cast<int32_t>(count);
        if (additional < 0 || additional > 128)
          throw std::runtime_error("PEX execution call exceeds limit");
        expected += additional;
      }
      if (instruction.args.size() != expected)
        throw std::runtime_error("Invalid PEX execution instruction arity");
      Byte(instruction.op);
      Count(instruction.args.size());
      for (const auto& argument : instruction.args)
        Operand(argument);
    }
  }

  std::string Finish()
  {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256_final(&state, digest.data()) != 0)
      throw std::runtime_error("Cannot finish PEX execution hash");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 0xf]);
    }
    return result;
  }

private:
  void Append(const void* data, size_t size)
  {
    // Match the Reader's input budget without allocating a second whole PEX.
    constexpr size_t budget = 64 * 1024 * 1024;
    if (size > budget - encodedBytes)
      throw std::runtime_error("PEX execution metadata exceeds size limit");
    encodedBytes += size;
    if (size &&
        crypto_hash_sha256_update(
          &state, static_cast<const unsigned char*>(data), size) != 0)
      throw std::runtime_error("Cannot update PEX execution hash");
  }

  crypto_hash_sha256_state state{};
  size_t encodedBytes = 0;
};
}

std::string PexScript::ExecutionHash() const
{
  ExecutionHasher hash;
  hash.Number(header.Signature, 4);
  hash.Byte(header.VerMajor);
  hash.Byte(header.VerMinor);
  hash.Number(header.GameID, 2);
  hash.Text(source);
  hash.Count(userFlagTable.size());
  for (const auto& flag : userFlagTable) {
    hash.Text(flag.name);
    hash.Byte(flag.idx);
  }
  hash.Count(objectTable.size());
  for (const auto& object : objectTable) {
    hash.Text(object.NameIndex);
    hash.Text(object.parentClassName);
    hash.Number(object.userFlags, 4);
    hash.Text(object.autoStateName);
    hash.Count(object.variables.size());
    for (const auto& variable : object.variables) {
      hash.Text(variable.name);
      hash.Text(variable.typeName);
      hash.Number(variable.userFlags, 4);
      hash.Operand(variable.value);
    }
    hash.Count(object.properties.size());
    for (const auto& property : object.properties) {
      hash.Text(property.name);
      hash.Text(property.type);
      hash.Number(property.userFlags, 4);
      hash.Byte(property.flags);
      hash.Text(property.autoVarName);
      hash.Function(property.readHandler);
      hash.Function(property.writeHandler);
    }
    hash.Count(object.states.size());
    for (const auto& state : object.states) {
      hash.Text(state.name);
      hash.Count(state.functions.size());
      for (const auto& function : state.functions) {
        hash.Text(function.name);
        hash.Function(function.function);
      }
    }
  }
  return hash.Finish();
}

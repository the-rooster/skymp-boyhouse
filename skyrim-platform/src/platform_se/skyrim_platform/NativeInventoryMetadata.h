#pragma once

// These emitters are the native inventory reader's field projection. Keeping
// the projection independent of N-API lets tests exercise its real output,
// rather than providing an already-correct mock getExtraContainerChanges.
// Production instantiates them with the actual CommonLib extra-data types.
namespace NativeInventoryMetadata {
template <class Extra, class Write>
void Enchantment(const Extra& extra, Write write)
{
  write("enchantmentId",
        extra.enchantment ? static_cast<double>(extra.enchantment->formID)
                          : 0.0);
  write("maxCharge", static_cast<double>(extra.charge));
  write("removeOnUnequip", extra.removeOnUnequip);
  write("type", "Enchantment");
}

template <class Extra, class Write>
void Soul(const Extra& extra, Write write)
{
  // GetType() identifies the extra-data record (kSoul = 156), not its content.
  write("soul", static_cast<double>(extra.GetContainedSoul()));
  write("type", "Soul");
}

template <class Extra, class Write>
void Charge(const Extra* extra, Write write)
{
  // Zero is a present, depleted charge. An absent ExtraCharge remains absent
  // (implicit full charge); the native reader calls this for existing records.
  if (!extra) {
    return;
  }
  write("charge", static_cast<double>(extra->charge));
  write("type", "Charge");
}
}

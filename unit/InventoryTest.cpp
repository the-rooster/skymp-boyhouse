#include "Inventory.h"
#include <catch2/catch_all.hpp>
#include <limits>

TEST_CASE("Adding a reward batch keeps items after a merged stack",
          "[Inventory]")
{
  Inventory inv;
  inv.AddItem(0xf, 10);
  Inventory::Entry named(0xf, 2);
  named.name = "A different stack";
  inv.AddItems(
    { { 0xf, 5 }, { 0xa, 7 }, named, { 0xf, 3 }, { 0xa, 4 }, { 0xb, 0 } });
  REQUIRE(inv.entries.size() == 3);
  REQUIRE(inv.GetItemCount(0xf) == 20);
  REQUIRE(inv.GetItemCount(0xa) == 11);
  REQUIRE_FALSE(inv.HasItem(0xb));
  REQUIRE(inv.entries[2] == named);
  inv.AddItems(inv.entries);
  REQUIRE(inv.GetItemCount(0xf) == 40);
  REQUIRE(inv.GetItemCount(0xa) == 22);
}

TEST_CASE("A reward overflow cannot partially add its earlier items",
          "[Inventory]")
{
  Inventory inv;
  inv.AddItem(0xf, std::numeric_limits<uint32_t>::max());
  const auto before = inv;
  REQUIRE_THROWS_AS(inv.AddItems({ { 0xa, 10 }, { 0xf, 1 } }),
                    std::overflow_error);
  REQUIRE(inv == before);
}

TEST_CASE(
  "RemoveItems in case of multiple entries with the same baseId/extras",
  "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 5) });

  REQUIRE(inv.entries.size() == 2);
  REQUIRE(inv.entries[0].count == 5);
  REQUIRE(inv.entries[1].count == 10);
}

TEST_CASE("Similar entries should be merged if needed to remove items",
          "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 15) });

  REQUIRE(inv.entries.size() == 1);
  REQUIRE(inv.entries[0].count == 5);
}

TEST_CASE("Remove all items from similar entries", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 20) });

  REQUIRE(inv.entries.size() == 0);
}

TEST_CASE("Remove all items from a single entry", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 10) });

  REQUIRE(inv.entries.size() == 0);
}

TEST_CASE("Not enough items to remove", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 9));

  REQUIRE_THROWS_AS(inv.RemoveItems({ Inventory::Entry(0xf, 20) }),
                    std::runtime_error);
}

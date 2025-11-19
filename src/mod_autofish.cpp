#include "ScriptMgr.h"
#include "Player.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Config.h"
#include "LootMgr.h"
#include "Item.h"
#include <sstream>
#include <vector>
#include <list>
#include <string>
#include <unordered_map>

namespace
{
    bool sEnabled = true;
    bool sServerAutoLoot = true;
    bool sAutoRecast = true;
    uint32 sTickMs = 200;
    float sScanRange = 30.0f;
    uint32 sRecastDelayMs = 500;
    uint32 sRecastSpell = 18248;
    uint32 sAutoLootDelayMs = 120;
    uint32 sRequiredItemId = 0;
    uint32 sRequiredEquipId = 0;
    std::vector<uint32> sBobberEntries;

    struct GuidHash
    {
        std::size_t operator()(ObjectGuid const &g) const noexcept { return std::hash<uint64>()(g.GetRawValue()); }
    };

    std::unordered_map<ObjectGuid, uint32, GuidHash> sRecastTimers;

    struct PendingLoot
    {
        uint32 timer;
    };
    std::unordered_map<ObjectGuid, PendingLoot, GuidHash> sLootTimers;

    std::vector<uint32> ParseEntryList(std::string const &csv)
    {
        std::vector<uint32> out;
        std::stringstream ss(csv);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            item.erase(0, item.find_first_not_of(" \t\r\n"));
            item.erase(item.find_last_not_of(" \t\r\n") + 1);
            if (!item.empty())
                out.push_back(uint32(std::stoul(item)));
        }
        return out;
    }

    bool HasEquippedItem(Player *plr, uint32 entry)
    {
        if (!entry)
            return true;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item *it = plr->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (it->GetEntry() == entry)
                    return true;
        }
        return false;
    }

    bool RequirementMet(Player *plr)
    {
        if (!plr)
            return false;
        if (sRequiredItemId && !plr->HasItemCount(sRequiredItemId, 1, false))
            return false;
        if (sRequiredEquipId && !HasEquippedItem(plr, sRequiredEquipId))
            return false;
        return true;
    }

    void ScheduleRecast(Player *plr)
    {
        if (!sAutoRecast || !plr || !plr->IsInWorld())
            return;
        sRecastTimers[plr->GetGUID()] = sRecastDelayMs;
    }

    void ScheduleAutoLoot(Player *plr)
    {
        if (!sServerAutoLoot || !plr)
            return;
        sLootTimers[plr->GetGUID()] = PendingLoot{sAutoLootDelayMs};
    }

    void TickRecast(uint32 diff)
    {
        for (auto it = sRecastTimers.begin(); it != sRecastTimers.end();)
        {
            if (it->second > diff)
            {
                it->second -= diff;
                ++it;
            }
            else
            {
                Player *plr = ObjectAccessor::FindPlayer(it->first);
                if (plr && plr->IsInWorld() && plr->IsAlive() && !plr->IsInCombat() && RequirementMet(plr))
                    plr->CastSpell(plr, sRecastSpell, true);
                it = sRecastTimers.erase(it);
            }
        }
    }

    void TickAutoLoot(uint32 diff)
    {
        for (auto it = sLootTimers.begin(); it != sLootTimers.end();)
        {
            if (it->second.timer > diff)
            {
                it->second.timer -= diff;
                ++it;
                continue;
            }

            Player *plr = ObjectAccessor::FindPlayer(it->first);
            if (plr && plr->IsInWorld() && RequirementMet(plr))
            {
                ObjectGuid lootGuid = plr->GetLootGUID();
                if (lootGuid.IsGameObject())
                {
                    if (GameObject *lootGo = ObjectAccessor::GetGameObject(*plr, lootGuid))
                    {
                        Loot *loot = &lootGo->loot;

                        for (uint8 slot = 0; slot < loot->items.size(); ++slot)
                        {
                            LootItem &li = loot->items[slot];
                            if (li.is_looted)
                                continue;

                            InventoryResult msg = EQUIP_ERR_OK;
                            plr->StoreLootItem(slot, loot, msg);
                        }

                        const QuestItemMap &questItems = loot->GetPlayerQuestItems();
                        if (!questItems.empty())
                        {
                            uint8 baseSlot = static_cast<uint8>(loot->items.size());
                            for (uint8 i = 0; i < questItems.size(); ++i)
                            {
                                uint8 slot = baseSlot + i;
                                InventoryResult msg = EQUIP_ERR_OK;
                                plr->StoreLootItem(slot, loot, msg);
                            }
                        }

                        const QuestItemMap &ffaItems = loot->GetPlayerFFAItems();
                        if (!ffaItems.empty())
                        {
                            for (uint8 i = 0; i < ffaItems.size(); ++i)
                            {
                                uint8 slot = i;
                                InventoryResult msg = EQUIP_ERR_OK;
                                plr->StoreLootItem(slot, loot, msg);
                            }
                        }

                        if (loot->gold > 0)
                        {
                            plr->ModifyMoney(loot->gold);
                            loot->gold = 0;
                        }

                        plr->SendLootRelease(lootGuid);

                        if (lootGo->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE)
                            lootGo->SetLootState(GO_JUST_DEACTIVATED);
                    }
                }
            }

            it = sLootTimers.erase(it);
        }
    }

    void TryAutoFish(Player *plr)
    {
        if (!plr || !plr->IsInWorld() || !RequirementMet(plr))
            return;

        std::list<GameObject *> nearList;
        for (auto entry : sBobberEntries)
            plr->GetGameObjectListWithEntryInGrid(nearList, entry, sScanRange);

        for (GameObject *go : nearList)
        {
            if (!go || go->GetOwnerGUID() != plr->GetGUID())
                continue;

            if (go->getLootState() == GO_READY && go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE)
            {
                go->Use(plr);
                if (sServerAutoLoot)
                    ScheduleAutoLoot(plr);
                ScheduleRecast(plr);
                return;
            }
        }
    }
}

class AutoFish_WorldScript : public WorldScript
{
public:
    AutoFish_WorldScript() : WorldScript("AutoFish_WorldScript") {}

    void OnAfterConfigLoad(bool) override
    {
        sEnabled = sConfigMgr->GetOption<bool>("AutoFish.Enabled", true);
        sServerAutoLoot = sConfigMgr->GetOption<bool>("AutoFish.ServerAutoLoot", true);
        sAutoRecast = sConfigMgr->GetOption<bool>("AutoFish.AutoRecast", true);
        sTickMs = sConfigMgr->GetOption<uint32>("AutoFish.TickMs", 200u);
        sScanRange = sConfigMgr->GetOption<float>("AutoFish.ScanRange", 30.0f);
        sRecastDelayMs = sConfigMgr->GetOption<uint32>("AutoFish.RecastDelayMs", 500u);
        sRecastSpell = sConfigMgr->GetOption<uint32>("AutoFish.RecastSpell", 18248u);
        sAutoLootDelayMs = sConfigMgr->GetOption<uint32>("AutoFish.AutoLootDelayMs", 120u);
        sRequiredItemId = sConfigMgr->GetOption<uint32>("AutoFish.RequiredItemId", 0u);
        sRequiredEquipId = sConfigMgr->GetOption<uint32>("AutoFish.RequiredEquipId", 0u);
        sBobberEntries = ParseEntryList(sConfigMgr->GetOption<std::string>("AutoFish.BobberEntries", "35591"));
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sEnabled)
            return;

        static uint32 acc = 0;
        acc += diff;
        if (acc >= sTickMs)
        {
            acc = 0;
            auto const &players = ObjectAccessor::GetPlayers();
            for (auto const &kv : players)
            {
                Player *plr = kv.second;
                if (!plr || !plr->IsInWorld() || plr->IsGameMaster())
                    continue;
                TryAutoFish(plr);
            }
        }

        TickAutoLoot(diff);
        TickRecast(diff);
    }
};

void AddSC_autofish_world()
{
    new AutoFish_WorldScript();
}

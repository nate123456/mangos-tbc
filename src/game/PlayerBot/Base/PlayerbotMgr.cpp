/*
/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Config/Config.h"
#include "WorldPacket.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "../config.h"
#include "../../Chat/Chat.h"
#include "../../Entities/GossipDef.h"
#include "../../Entities/Player.h"
#include "../../Globals/ObjectMgr.h"
#include "../../Globals/ObjectAccessor.h"
#include "../../Guilds/Guild.h"
#include "../../Loot/LootMgr.h"
#include "../../MotionGenerators/WaypointMovementGenerator.h"
#include "../../Spells/SpellMgr.h"
#include "../../Tools/Language.h"
#include "../../World/World.h"
#include <boost/algorithm/string.hpp>

class LoginQueryHolder;
class CharacterHandler;

Config botConfig;

void PlayerbotMgr::SetInitialWorldSettings()
{
    //Get playerbot configuration file
    if (!botConfig.SetSource(_PLAYERBOT_CONFIG))
        sLog.outError("Playerbot: Unable to open configuration file. Database will be unaccessible. Configuration values will use default.");
    else
        sLog.outString("Playerbot: Using configuration file %s", _PLAYERBOT_CONFIG.c_str());

    //Check playerbot config file version
    if (botConfig.GetIntDefault("ConfVersion", 0) != PLAYERBOT_CONF_VERSION)
        sLog.outError("Playerbot: Configuration file version doesn't match expected version. Some config variables may be wrong or missing.");
}

PlayerbotMgr::PlayerbotMgr(Player* const master) : m_master(master), m_masterChatHandler(master)
{
    // load config variables
    m_confMaxNumBots = botConfig.GetIntDefault("PlayerbotAI.MaxNumBots", 9);
    m_confDebugWhisper = botConfig.GetBoolDefault("PlayerbotAI.DebugWhisper", false);
    m_confFollowDistance[0] = botConfig.GetFloatDefault("PlayerbotAI.FollowDistanceMin", 0.5f);
    m_confFollowDistance[1] = botConfig.GetFloatDefault("PlayerbotAI.FollowDistanceMax", 1.0f);
    m_confCollectCombat = botConfig.GetBoolDefault("PlayerbotAI.Collect.Combat", true);
    m_confCollectQuest = botConfig.GetBoolDefault("PlayerbotAI.Collect.Quest", true);
    m_confCollectProfession = botConfig.GetBoolDefault("PlayerbotAI.Collect.Profession", true);
    m_confCollectLoot = botConfig.GetBoolDefault("PlayerbotAI.Collect.Loot", true);
    m_confCollectSkin = botConfig.GetBoolDefault("PlayerbotAI.Collect.Skin", true);
    m_confCollectObjects = botConfig.GetBoolDefault("PlayerbotAI.Collect.Objects", true);
    m_confCollectDistanceMax = botConfig.GetIntDefault("PlayerbotAI.Collect.DistanceMax", 50);
    gConfigSellLevelDiff = botConfig.GetIntDefault("PlayerbotAI.SellAll.LevelDiff", 10);
    if (m_confCollectDistanceMax > 100)
    {
        sLog.outError("Playerbot: PlayerbotAI.Collect.DistanceMax higher than allowed. Using 100");
        m_confCollectDistanceMax = 100;
    }
    m_confCollectDistance = botConfig.GetIntDefault("PlayerbotAI.Collect.Distance", 25);
    if (m_confCollectDistance > m_confCollectDistanceMax)
    {
        sLog.outError("Playerbot: PlayerbotAI.Collect.Distance higher than PlayerbotAI.Collect.DistanceMax. Using DistanceMax value");
        m_confCollectDistance = m_confCollectDistanceMax;
    }

    InitLua();
}

PlayerbotMgr::~PlayerbotMgr()
{
    LogoutAllBots(true);
}

class PlayerbotChatHandler : protected ChatHandler
{
public:
	explicit PlayerbotChatHandler(Player* bot) : ChatHandler(bot)
	{
	}

	bool Revive(Player& bot) { return HandleReviveCommand(const_cast<char*>(bot.GetName())); }
	bool Teleport(Player& bot) { return HandleNamegoCommand(const_cast<char*>(bot.GetName())); }
	void SendSysMessage(const char* str) override { SendSysMessage(str); }
	bool DropQuest(char* str) { return HandleQuestRemoveCommand(str); }
};

void PlayerbotMgr::UpdateAI(const uint32 time)
{
	if (m_playerBots.empty())
		return;

	if (!m_luaEnvironment)
		return;

	std::vector<Player*> bots;
	bots.reserve(m_playerBots.size());

	for (auto& [id, bot] : m_playerBots)
		bots.push_back(bot);

	const sol::protected_function act_func = m_luaEnvironment["act"];

	if (!act_func.valid())
	{
		if (const auto error_msg = "No 'act' function with correct parameters defined."; m_lastActErrorMsg !=
			error_msg)
		{
			m_masterChatHandler.PSendSysMessage("|cffff0000%s", error_msg);
			m_lastActErrorMsg = error_msg;
		}

		return;
	}

	if (const auto act_result = act_func(time, bots, m_lastManagerMessage, m_lastCommandPosition); !act_result.valid())
	{
		const sol::error error = act_result;

		if (const char* error_msg = error.what(); m_lastActErrorMsg != error_msg)
		{
			m_masterChatHandler.PSendSysMessage("|cffff0000%s", error_msg);
			m_lastActErrorMsg = error_msg;
		}

		return;
	}

	// messages should be cleared after AI has had a chance to process it
	for (const auto& bot : bots)
		bot->GetPlayerbotAI()->ResetLastMessage();
	
	// reset 'single-use' things
	if (!m_lastActErrorMsg.empty())
		m_lastActErrorMsg = "";
}

void PlayerbotMgr::InitLua()
{
	m_lua.open_libraries(sol::lib::base,
	                     sol::lib::package,
	                     sol::lib::coroutine,
	                     sol::lib::string,
	                     sol::lib::math,
	                     sol::lib::table);

	InitLuaMembers();
	InitLuaFunctions();

	InitLuaPlayerType();
	InitLuaUnitType();
	InitLuaCreatureType();
	InitLuaObjectType();
	InitLuaWorldObjectType();
	InitLuaGroupType();
	InitLuaMapType();
	InitLuaGameObjectType();
	InitLuaPositionType();
	InitLuaPetType();
	InitLuaAuraType();
	InitLuaItemType();

	InitializeLuaEnvironment();

	m_lua.script("print('[DEBUG] LUA has been initialized.')");
}

void PlayerbotMgr::InitializeLuaEnvironment()
{
	m_luaEnvironment = sol::environment(m_lua, sol::create, m_lua.globals());

	if (!m_lastActErrorMsg.empty())
		m_lastActErrorMsg = "";

	if (!m_lastManagerMessage.empty())
		m_lastManagerMessage = "";
}

bool PlayerbotMgr::ValidateLuaScript(const char* script)
{
	InitializeLuaEnvironment();

	if (const auto result = m_lua.safe_script(std::string(script), m_luaEnvironment); !result.valid())
	{
		const sol::error error = result;
		ChatHandler(m_master).PSendSysMessage("|cffff0000failed to load ai script:\n%s", error.what());
		return false;
	}

	return true;
}

void PlayerbotMgr::InitLuaMembers()
{
	m_lua["Master"] = GetMaster();
}

void PlayerbotMgr::InitLuaFunctions()
{
	m_lua["get_raid_icon"] = [&](const uint8 iconIndex)-> Unit*
	{
		if (iconIndex < 0 || iconIndex > 7)
			return nullptr;

		if (const auto guid = GetMaster()->GetGroup()->GetTargetFromIcon(iconIndex))
			return GetMaster()->GetMap()->GetUnit(*guid);

		return nullptr;
	};
	m_lua["spell_exists"] = [](const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		return sSpellTemplate.LookupEntry<SpellEntry>(spellId) != nullptr;
	};
	m_lua["spell_is_positive"] = [](const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		return IsPositiveSpell(spellId);
	};
	m_lua.set_function("print",
	                   sol::overload(
		                   [this](const std::string& t) { ChatHandler(m_master).PSendSysMessage("[AI] %s", t.c_str()); }
	                   ));
}

void PlayerbotMgr::InitLuaPlayerType()
{
	sol::usertype<Player> player_type = m_lua.new_usertype<Player>("player",
	                                                               sol::constructors<Player(WorldSession* session)>(),
	                                                               sol::base_classes,
	                                                               sol::bases<Unit, WorldObject, Object>());

	player_type["last_message"] = sol::property([](Player* self)
	{
		const auto ai = self->GetPlayerbotAI();

		if (!ai)
			return "";

		return ai->GetLastMessage().c_str();
	});
	player_type["inventory"] = sol::property([](const Player* self)
	{
		std::list<Item*> items;

		// list out items in main backpack
		for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
			if (Item* const p_item = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
				if (p_item)
					items.push_back(p_item);

		// list out items in other removable backpacks
		for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
			if (const Bag* const p_bag = dynamic_cast<Bag*>(self->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
				for (uint32 slot = 0; slot < p_bag->GetBagSize(); ++slot)
					if (Item* const p_item = self->GetItemByPos(bag, slot))
						if (p_item)
							items.push_back(p_item);

		return items;
	});
	player_type["trinket_1"] = sol::property([](const Player* self)
	{
		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1);
	});
	player_type["trinket_2"] = sol::property([](const Player* self)
	{
		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2);
	});
	player_type["group"] = sol::property([](Player* self)
	{
		return self->GetGroup();
	});
	player_type["destination"] = sol::property([](Player* self)
	{
		float x, y, z;

		if (self)
		{
			if (const auto ai = self->GetPlayerbotAI(); ai)
			{
				self->GetMotionMaster()->GetDestination(x, y, z);
			}
		}
		return sol::tie(x, y, z);
	});

	player_type["follow"] = [](Player* self, Unit* target, const float dist, const float angle)
	{
		if (!self)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		target->GetPosition();
		self->GetMotionMaster()->MoveFollow(target, dist, angle);
	};
	player_type["move_to_pos"] = [](Player* self, const Position* pos)
	{
		if (!pos)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->GetMotionMaster()->MovePoint(0, *pos);
	};
	player_type["interrupt"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->InterruptSpell(CURRENT_GENERIC_SPELL);
		self->InterruptSpell(CURRENT_CHANNELED_SPELL);
	};
	player_type["move_to_point"] = [](Player* self, const float x, const float y, const float z)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->GetMotionMaster()->MovePoint(0, x, y, z);
	};
	player_type["move_to_target"] = [](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		float x, y, z;
		target->GetClosePoint(x, y, z, self->GetObjectBoundingRadius());
		self->GetMotionMaster()->MovePoint(0, x, y, z);
	};
	player_type["chase"] = [](Player* self, Unit* target, const float distance, const float angle)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->GetMotionMaster()->MoveChase(target, distance, angle);
	};
	player_type["set_chase_distance"] = [](Player* self, const float distance)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->GetMotionMaster()->DistanceYourself(distance);
	};
	player_type["reset_movement"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		// not sure which one is better yet, clear seem to be used more frequently...
		// self->GetMotionMaster()->Initialize();
		self->GetMotionMaster()->Clear();
	};
	player_type["stop"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->StopMoving();
	};
	player_type["teleport_to"] = [](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		float x, y, z;
		target->GetClosePoint(x, y, z, self->GetObjectBoundingRadius());
		self->TeleportTo(target->GetMapId(), x, y, z, self->GetOrientation());
	};
	player_type["whisper"] = [&](Player* self, const Player* to, const char* text)
	{
		if (!to || !text || !text[0] || self == to)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendWhisper(text, self, to);
	};
	player_type["tell_party"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_PARTY);
	};
	player_type["tell_raid"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_RAID);
	};
	player_type["say"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_SAY);
	};
	player_type["yell"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_YELL);
	};
	player_type["set_target"] = [](Player* self, WorldObject* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetTarget(target);
	};
	player_type["clear_target"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetSelectionGuid(ObjectGuid());
	};
	player_type["clear_stealth"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
	};
	player_type["face_target"] = [](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (!self->HasInArc(target, M_PI_F / 2))
			self->SetFacingTo(self->GetAngle(target));
	};
	player_type["face_orientation"] = [](Player* self, const float orientation)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetFacingTo(orientation);
	};
	player_type["has_power_to_cast"] = [](Player* self, const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		// verify player has spell
		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);

		if (!p_spell_info)
			return false;

		// verify sufficient power to cast
		const auto tmp_spell = new Spell(self, p_spell_info, false);

		return tmp_spell->CheckPower(true) == SPELL_CAST_OK;
	};
	player_type["can_cast"] = [](const Player* self, const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		// verify player has spell
		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);

		if (!p_spell_info)
			return false;

		return self->IsSpellReady(*p_spell_info);
	};
	player_type["get_cast_time"] = [](Player* self, const uint32 spellId)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return static_cast<uint32>(-1);

		if (spellId == 0)
			return static_cast<uint32>(-1);

		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
		if (!p_spell_info)
			return static_cast<uint32>(-1);

		const auto spell = new Spell(self, p_spell_info, TRIGGERED_NONE);

		return GetSpellCastTime(p_spell_info, self, spell);
	};
	player_type["get_item_in_equip_slot"] = [](const Player* self, const uint8 slot)-> Item*
	{
		if (slot < EQUIPMENT_SLOT_START || slot > EQUIPMENT_SLOT_END)
			return nullptr;

		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
	};
	player_type["get_item_by_name"] = [](const Player* self, const std::string& name)-> Item*
	{
		// list out items in main backpack
		for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
			if (Item* const p_item = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
				if (p_item && name.find(p_item->GetProto()->Name1) != std::string::npos)
					return p_item;

		// list out items in other removable backpacks
		for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
			if (const Bag* const p_bag = dynamic_cast<Bag*>(self->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
				for (uint32 slot = 0; slot < p_bag->GetBagSize(); ++slot)
					if (Item* const p_item = self->GetItemByPos(bag, slot))
						if (p_item && name.find(p_item->GetProto()->Name1) != std::string::npos)
							return p_item;

		return nullptr;
	};
	player_type["cast"] = [&](Player* self, Unit* target, const uint32 spellId)
	{
		if (CurrentCast(self, CURRENT_GENERIC_SPELL) > 0 || CurrentCast(self, CURRENT_CHANNELED_SPELL) > 0)
			return SPELL_FAILED_NOT_READY;

		return Cast(self, target, spellId);
	};
	player_type["force_cast"] = [&](Player* self, Unit* target, const uint32 spellId)
	{
		if (!target)
			return SPELL_FAILED_BAD_TARGETS;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return SPELL_FAILED_ERROR;

		self->InterruptSpell(CURRENT_GENERIC_SPELL);
		self->InterruptSpell(CURRENT_CHANNELED_SPELL);

		return Cast(self, target, spellId);
	};
};

void PlayerbotMgr::InitLuaUnitType()
{
	sol::usertype<Unit> unit_type = m_lua.new_usertype<Unit>("unit", sol::base_classes,
	                                                         sol::bases<WorldObject, Object>());

	// doesn't seem to be useful- all NPCs are always moving, always false for IRL player
	//unit_type["is_moving"] = sol::property([](const Unit* self)
	//{
	//	return !self->IsStopped();
	//});

	unit_type["bounding_radius"] = sol::property(&Unit::GetObjectBoundingRadius);
	unit_type["pet"] = sol::property(&Unit::GetPet);
	unit_type["in_combat"] = sol::property(&Unit::IsInCombat);
	unit_type["raid_icon"] = sol::property([&](const Unit* self)
	{
		return GetMaster()->GetGroup()->GetIconFromTarget(self->GetObjectGuid());
	});
	unit_type["attackers"] = sol::property([](Unit* self)
	{
		std::vector<Unit*> attackers;

		if (!self)
			return attackers;

		HostileRefManager ref_mgr = self->getHostileRefManager();

		HostileReference* ref = ref_mgr.getFirst();
		attackers.reserve(ref_mgr.getSize());

		while (ref)
		{
			const ThreatManager* threat_mgr = ref->getSource();

			attackers.push_back(threat_mgr->getOwner());
			ref = ref->next();
		}

		return attackers;
	});
	unit_type["target"] = sol::property([](const Unit* self)-> Unit*
	{
		return self->GetTarget();
	});
	unit_type["is_alive"] = sol::property(&Unit::IsAlive);
	unit_type["crowd_controlled"] = sol::property(&Unit::IsCrowdControlled);
	unit_type["health"] = sol::property(&Unit::GetHealth);
	unit_type["max_health"] = sol::property(&Unit::GetMaxHealth);
	unit_type["power"] = sol::property([](const Unit* self)
	{
		const Powers power = self->GetPowerType();
		return self->GetPower(power);
	});
	unit_type["max_power"] = sol::property([](const Unit* self)
	{
		const Powers power = self->GetPowerType();
		return self->GetMaxPower(power);
	});
	unit_type["current_cast"] = sol::property([&](const Unit* self)
	{
		return CurrentCast(self, CURRENT_GENERIC_SPELL);
	});
	unit_type["current_auto_attack"] = sol::property([&](const Unit* self)
	{
			return CurrentCast(self, CURRENT_AUTOREPEAT_SPELL);
	});
	unit_type["current_channel"] = sol::property([&](const Unit* self)
	{
			return CurrentCast(self, CURRENT_CHANNELED_SPELL);
	});
	unit_type["is_attacked_by"] = [](const Unit* self, Unit* target)
	{
		if (!target)
			return false;

		return self->IsAttackedBy(target);
	};
	unit_type["get_threat"] = [](Unit* self, const Unit* target)
	{
		if (!target)
			return -1.0f;

		HostileReference* ref = self->getHostileRefManager().getFirst();

		while (ref)
		{
			if (ThreatManager* threat_mgr = ref->getSource(); threat_mgr->getOwner() == target && threat_mgr->getOwner()
				->GetVictim() == self)
			{
				return threat_mgr->getThreat(self);
			}
			ref = ref->next();
		}

		return 0.0f;
	};
	unit_type["reachable_with_melee"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanReachWithMeleeAttack(target);
	};
	unit_type["is_enemy"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanAttack(target);
	};
	unit_type["is_friendly"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanAssist(target);
	};
	unit_type["get_aura"] = [](const Unit* self, const uint32 auraId)-> SpellAuraHolder*
	{
		if (auraId == 0)
			return nullptr;

		return self->GetSpellAuraHolder(auraId);
	};
}

void PlayerbotMgr::InitLuaCreatureType()
{
	sol::usertype<Creature> creature_type = m_lua.new_usertype<Creature>("creature", sol::base_classes,
	                                                                     sol::bases<Unit, WorldObject, Object>());

	creature_type["is_elite"] = sol::property(&Creature::IsElite);
	creature_type["is_world_boss"] = sol::property(&Creature::IsWorldBoss);
	creature_type["can_aggro"] = sol::property(&Creature::CanAggro);
	creature_type["is_totem"] = sol::property(&Creature::IsTotem);
	creature_type["is_invisible"] = sol::property(&Creature::IsInvisible);
	creature_type["is_civilian"] = sol::property(&Creature::IsCivilian);
	creature_type["is_critter"] = sol::property(&Creature::IsCritter);
	creature_type["is_regen_hp"] = sol::property(&Creature::IsRegeneratingHealth);
	creature_type["is_regen_power"] = sol::property(&Creature::IsRegeneratingHealth);
	creature_type["can_walk"] = sol::property(&Creature::CanWalk);
	creature_type["can_swim"] = sol::property(&Creature::CanSwim);
	creature_type["can_fly"] = sol::property(&Creature::CanFly);
}

void PlayerbotMgr::InitLuaObjectType()
{
	sol::usertype<Object> object_type = m_lua.new_usertype<Object>(
		"object");

	object_type["is_player"] = sol::property(&Object::IsPlayer);
	object_type["is_creature"] = sol::property(&Object::IsCreature);
	object_type["is_game_object"] = sol::property(&Object::IsGameObject);
	object_type["is_unit"] = sol::property(&Object::IsUnit);

	object_type["as_player"] = [](Object* self)-> Player*
	{
		if (!self->IsPlayer())
			return nullptr;

		return dynamic_cast<Player*>(self);
	};
	object_type["as_creature"] = [](Object* self)-> Creature*
	{
		if (!self->IsCreature())
			return nullptr;

		return dynamic_cast<Creature*>(self);
	};
	object_type["as_game_object"] = [](Object* self)-> GameObject*
	{
		if (!self->IsGameObject())
			return nullptr;

		return dynamic_cast<GameObject*>(self);
	};
	object_type["as_unit"] = [](Object* self)-> Unit*
	{
		if (!self->IsUnit())
			return nullptr;

		return dynamic_cast<Unit*>(self);
	};
}

void PlayerbotMgr::InitLuaWorldObjectType()
{
	sol::usertype<WorldObject> world_object_type = m_lua.new_usertype<WorldObject>(
		"world_object", sol::base_classes, sol::bases<Object>());

	world_object_type["orientation"] = sol::property(&WorldObject::GetOrientation);
	world_object_type["name"] = sol::property(&WorldObject::GetName);
	world_object_type["map"] = sol::property(&WorldObject::GetMap);
	world_object_type["zone_id"] = sol::property(&WorldObject::GetZoneId);
	world_object_type["area_id"] = sol::property(&WorldObject::GetAreaId);
	world_object_type["gcd"] = sol::property(&WorldObject::GetGCD);
	world_object_type["position"] = sol::property([](const WorldObject* self)
	{
		return self->GetPosition();
	});

	world_object_type["get_angle_of_obj"] = [](const WorldObject* self, const WorldObject* obj)
	{
		return self->GetAngle(obj);
	};
	world_object_type["get_angle_of_pos"] = [](const WorldObject* self, const Position* pos)
	{
		return self->GetAngle(pos->x, pos->y);
	};
	world_object_type["get_angle_of_xy"] = [](const WorldObject* self, const float x, const float y)
	{
		return self->GetAngle(x, y);
	};
	world_object_type["get_close_point"] =
		[](const WorldObject* self, const float boundingRadius, const float distance,
		   const float angle)
		{
			float x, y, z;
			self->GetClosePoint(x, y, z, boundingRadius, distance, angle, self);
			return sol::tie(x, y, z);
		};
	world_object_type["has_in_arc"] = [](const WorldObject* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->HasInArc(target, M_PI_F / 2);
	};
	world_object_type["is_within_los"] = [](const WorldObject* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->IsWithinLOSInMap(target);
	};
	world_object_type["is_in_range"] = [](const WorldObject* self, const Unit* target, const uint32 spellId)
	{
		if (!target || spellId == 0)
			return false;

		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
		if (!p_spell_info)
		{
			return false;
		}

		const SpellRangeEntry* temp_range = GetSpellRangeStore()->LookupEntry(p_spell_info->rangeIndex);

		//Spell has invalid range store so we can't use it
		if (!temp_range)
			return false;

		if (temp_range->minRange == 0.0f && temp_range->maxRange == 0.0f)
			return false;

		//Unit is out of range of this spell
		if (!self->IsInRange(target, temp_range->minRange, temp_range->maxRange))
			return false;

		return true;
	};
}

void PlayerbotMgr::InitLuaGroupType()
{
	sol::usertype<Group> group_type = m_lua.new_usertype<Group>("group");

	group_type["is_raid"] = sol::property(&Group::IsRaidGroup);
	group_type["leader"] = sol::property([&](const Group* self)
	{
		const ObjectGuid guid = self->GetLeaderGuid();
		return GetMaster()->GetMap()->GetPlayer(guid);
	});
	group_type["members"] = sol::property([&](const Group* self)
	{
		const Group::MemberSlotList& slots = self->GetMemberSlots();

		std::vector<Player*> members;
		members.reserve(slots.size());

		Map* map = GetMaster()->GetMap();

		for (auto [guid, name, group, assistant, lastMap] : slots)
			members.push_back(map->GetPlayer(guid));

		return members;
	});
}

void PlayerbotMgr::InitLuaMapType()
{
	sol::usertype<Map> map_type = m_lua.new_usertype<Map>("map");

	map_type["players"] = sol::property(&Map::GetPlayers);
	map_type["map_name"] = sol::property(&Map::GetMapName);
	map_type["is_heroic"] = sol::property([&](const Map* self)
	{
		return self->GetDifficulty() == DUNGEON_DIFFICULTY_HEROIC;
	});
}

void PlayerbotMgr::InitLuaGameObjectType()
{
	sol::usertype<GameObject> game_object_type = m_lua.new_usertype<GameObject>(
		"game_object", sol::base_classes, sol::bases<WorldObject, Object>());

	game_object_type["in_use"] = sol::property(&GameObject::IsInUse);
	game_object_type["owner"] = sol::property(&GameObject::GetOwner);
	game_object_type["level"] = sol::property(&GameObject::GetLevel);
}

void PlayerbotMgr::InitLuaPositionType()
{
	sol::usertype<Position> position_type = m_lua.new_usertype<Position>(
		"position");

	position_type["x"] = &Position::x;
	position_type["y"] = &Position::y;
	position_type["z"] = &Position::z;
	position_type["o"] = &Position::o;

	position_type["get_distance_between"] = [](const Position* self, const Position* other)
	{
		return self->GetDistance(*other);
	};
	position_type["get_angle_to_pos"] = [](const Position* self, const Position* pos)
	{
		return self->GetAngle(pos->x, pos->y);
	};
	position_type["get_angle_to_xy"] = [](const Position* self, const float x, const float y)
	{
		return self->GetAngle(x, y);
	};
}

void PlayerbotMgr::InitLuaPetType()
{
	sol::usertype<Pet> pet_type = m_lua.new_usertype<Pet>(
		"pet", sol::base_classes, sol::bases<Creature, Unit, WorldObject, Object>());

	pet_type["pet_owner"] = sol::property(&Pet::GetSpellModOwner);
	pet_type["happiness"] = sol::property(&Pet::GetHappinessState);
	pet_type["is_feeding"] = sol::property([](const Pet* self)
	{
		return self->HasAura(1738 /*PET_FEED*/, EFFECT_INDEX_0);
	});
	pet_type["react_state"] = sol::property([](Pet* self) { return self->AI()->GetReactState(); },
	                                        [](Pet* self, const int state)
	                                        {
		                                        const auto player = self->GetSpellModOwner();

		                                        if (!player)
			                                        return;

		                                        if (player->GetPlayerbotAI())
			                                        return;

		                                        self->AI()->SetReactState(static_cast<ReactStates>(state));
	                                        });
	
	pet_type["set_autocast"] = [](Pet* self, const uint32 spellId, const bool enable)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return;

		if (player->GetPlayerbotAI())
			return;

		if (spellId == 0)
			return;

		if (self->HasSpell(spellId))
		{
			if (const auto itr = self->m_spells.find(spellId); itr != self->m_spells.end())
			{
				if (itr->second.active == ACT_ENABLED)
				{
					if (enable)
						return;

					self->ToggleAutocast(spellId, false);
					if (self->HasAura(spellId))
						self->RemoveAurasByCasterSpell(spellId, self->GetObjectGuid());
				}
				else
				{
					if (!enable)
						return;

					self->ToggleAutocast(spellId, true);
				}
			}
		}
	};
	pet_type["summon"] = [](Pet* self)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return;

		if (player->GetPlayerbotAI())
			return;

		self->AddToWorld();
	};
	pet_type["dismiss"] = [](Pet* self)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return;

		if (player->GetPlayerbotAI())
			return;

		self->RemoveFromWorld();
	};
	pet_type["attempt_feed"] = [](const Pet* self)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return false;

		if (player->GetPlayerbotAI())
			return false;

		auto attempt_feed = [&](Item* const pItem)
		{
			const ItemPrototype* const p_item_proto = pItem->GetProto();
			if (!p_item_proto)
				return false;

			if (self->HaveInDiet(p_item_proto))
			{
				// sketch- doing the job of the server, could send packet?
				uint32 used_items = 1;
				const int benefit = self->GetCurrentFoodBenefitLevel(p_item_proto->ItemLevel) * 15;
				// nutritional value of food
				player->DestroyItemCount(pItem, used_items, true); // remove item from inventory
				player->CastCustomSpell(player, 1738 /*PET_FEED*/, &benefit, nullptr, nullptr,
				                        TRIGGERED_OLD_TRIGGERED); // feed pet
				return true;
			}

			return false;
		};

		// list out items in main backpack
		for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
			if (Item* const p_item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
				if (attempt_feed(p_item))
					return true;

		// list out items in other removable backpacks
		for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
			if (const Bag* const p_bag = dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
				for (uint32 slot = 0; slot < p_bag->GetBagSize(); ++slot)
					if (Item* const p_item = player->GetItemByPos(bag, slot))
						if (attempt_feed(p_item))
							return true;

		return false;
	};
}

void PlayerbotMgr::InitLuaAuraType()
{
	sol::usertype<SpellAuraHolder> aura_type = m_lua.new_usertype<SpellAuraHolder>("aura");

	aura_type["stacks"] = sol::property(&SpellAuraHolder::GetStackAmount);
	aura_type["duration"] = sol::property(&SpellAuraHolder::GetAuraDuration);
	aura_type["max_duration"] = sol::property(&SpellAuraHolder::GetAuraMaxDuration);
	aura_type["charges"] = sol::property(&SpellAuraHolder::GetAuraCharges);
}

void PlayerbotMgr::InitLuaItemType()
{
	sol::usertype<Item> item_type = m_lua.new_usertype<Item>("item", sol::base_classes, sol::bases<Object>());

	item_type["stack_count"] = sol::property(&Item::GetCount);
	item_type["is_potion"] = sol::property(&Item::IsPotion);
	item_type["max_stack_count"] = sol::property(&Item::GetMaxStackCount);
	item_type["id"] = sol::property([](const Item* self)
	{
		return self->GetProto()->ItemId;
	});
	item_type["name"] = sol::property([](const Item* self)
	{
		return self->GetProto()->Name1;
	});
	item_type["spell_id"] = sol::property([](const Item* self)
	{
		for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
			if (self->GetProto()->Spells[i].SpellId > 0)
				return self->GetProto()->Spells[i].SpellId;
		return static_cast<uint32>(0);
	});
	item_type["ready"] = sol::property([](const Item* self)
	{
		const auto spell_id = self->GetSpell();

		if (!spell_id)
			return false;

		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spell_id);

		if (!p_spell_info)
			return false;		

		return self->GetOwner()->IsSpellReady(*p_spell_info);
	});
	item_type["use_on_self"] = [&](Item* self)
	{
		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_UNIT, self->GetOwnerGuid());
	};
	item_type["use_on_equip_slot"] = [&](Item* self, const uint8 slot)
	{
		if (slot >= EQUIPMENT_SLOT_END || slot < EQUIPMENT_SLOT_START)
			return;

		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		Item* const item = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

		if (!item)
			return;

		UseItem(owner, self, TARGET_FLAG_ITEM, item->GetObjectGuid());
	};
	item_type["use_on_item"] = [&](Item* self, const Item* target)
	{
		if (!target)
			return;

		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_ITEM, target->GetObjectGuid());
	};
	item_type["use_on_game_object"] = [&](Item* self, GameObject* obj)
	{
		if (!obj)
			return;

		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_GAMEOBJECT, obj->GetObjectGuid());
	};
}

SpellCastResult PlayerbotMgr::Cast(Player* bot, Unit* target, const uint32 spellId) const
{
	if (!target)
		return SPELL_FAILED_BAD_TARGETS;

	if (spellId == 0)
		return SPELL_NOT_FOUND;

	if (const auto ai = bot->GetPlayerbotAI(); !ai)
		return SPELL_FAILED_ERROR; // no good option here

	// verify spell exists
	const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
	if (!p_spell_info)
	{
		return SPELL_NOT_FOUND;
	}

	// verify spell is ready
	if (!bot->IsSpellReady(*p_spell_info))
		return SPELL_FAILED_NOT_READY;

	// verify caster can afford to cast
	const auto tmp_spell = new Spell(bot, p_spell_info, false);
	if (const SpellCastResult res = tmp_spell->CheckPower(true); res != SPELL_CAST_OK)
		return res;

	if (const SpellCastResult power_check_result = tmp_spell->CheckPower(true); power_check_result != SPELL_CAST_OK)
		return power_check_result;

	// set target to self if spell is positive and target is enemy
	if (IsPositiveSpell(spellId))
	{
		if (target && bot->CanAttack(target))
			return SPELL_FAILED_TARGET_ENEMY;
	}
	else
	{
		// Can't cast hostile spell on friendly unit
		if (target && bot->CanAssist(target))
			return SPELL_FAILED_TARGET_FRIENDLY;
	}

	// Check line of sight
	if (!bot->IsWithinLOSInMap(target))
		return SPELL_FAILED_LINE_OF_SIGHT;

	const SpellRangeEntry* temp_range = GetSpellRangeStore()->LookupEntry(p_spell_info->rangeIndex);

	//Spell has invalid range store so we can't use it
	if (!temp_range)
		return SPELL_FAILED_OUT_OF_RANGE;

	if (!(temp_range->minRange == 0.0f && temp_range->maxRange == 0.0f))
		//Unit is out of range of this spell
		if (!bot->IsInRange(target, temp_range->minRange, temp_range->maxRange))
			return SPELL_FAILED_OUT_OF_RANGE;

	// stop movement to prevent cancel spell casting
	if (const SpellCastTimesEntry* cast_time_entry = sSpellCastTimesStore.
		LookupEntry(p_spell_info->CastingTimeIndex); cast_time_entry && cast_time_entry->CastTime)
	{
		// only stop moving if spell is not instant
		if (cast_time_entry->CastTime > 0)
			bot->StopMoving();
	}

	if (!bot->HasInArc(target, M_PI_F / 2))
		bot->SetFacingTo(bot->GetAngle(target));

	return bot->CastSpell(target, p_spell_info, TRIGGERED_NONE);
}

uint32 PlayerbotMgr::CurrentCast(const Unit* unit, const CurrentSpellTypes type)
{
	const auto current_spell = unit->GetCurrentSpell(type);

	if (!current_spell)
		return 0;

	const auto current_spell_info = current_spell->m_spellInfo;

	if (!current_spell_info)
		return 0;

	return static_cast<int>(current_spell_info->Id);
}

void PlayerbotMgr::UseItem(Player* bot, Item* item, uint32 targetFlag, const ObjectGuid targetGuid) const
{
	if (!item)
		return;

	if (const auto ai = bot->GetPlayerbotAI(); !ai)
		return;

	const uint8 bag_index = item->GetBagSlot();
	const uint8 slot = item->GetSlot();
	constexpr uint8 cast_count = 0;
	const ObjectGuid item_guid = item->GetObjectGuid();

	if (const uint32 quest_id = item->GetProto()->StartQuest)
	{
		if (sObjectMgr.GetQuestTemplate(quest_id))
		{
			bot->GetMotionMaster()->Clear(true);
			std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_QUESTGIVER_ACCEPT_QUEST, 8 + 4 + 4));
			*packet << item_guid;
			*packet << quest_id;
			*packet << static_cast<uint32>(0);
			bot->GetSession()->QueuePacket(std::move(packet)); // queue the packet to get around race condition
			// "|cffffff00Quest taken |r" << qInfo->GetTitle();
		}
		return;
	}

	uint32 spell_id = 0;
	uint8 spell_index = 0;

	for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
	{
		if (item->GetProto()->Spells[i].SpellId > 0)
		{
			spell_id = item->GetProto()->Spells[i].SpellId;
			spell_index = i;
			break;
		}
	}

	if (item->GetProto()->Flags & ITEM_FLAG_HAS_LOOT && spell_id == 0)
	{
		// Open quest item in inventory, containing related items (e.g Gnarlpine necklace, containing Tallonkai's Jewel)
		std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_OPEN_ITEM, 2));
		*packet << item->GetBagSlot();
		*packet << item->GetSlot();
		bot->GetSession()->QueuePacket(std::move(packet)); // queue the packet to get around race condition
		return;
	}

	const auto spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spell_id);
	if (!spell_info)
	{
		return;
	}

	if (const SpellCastTimesEntry* casting_time_entry = sSpellCastTimesStore.LookupEntry(spell_info->CastingTimeIndex);
		!casting_time_entry)
	{
		return;
	}

	// stop movement to prevent cancel spell casting
	else if (casting_time_entry && casting_time_entry->CastTime)
	{
		bot->StopMoving();
	}

	if (!bot->IsSpellReady(*spell_info))
		return;

	std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_USE_ITEM, 20));
	*packet << bag_index;
	*packet << slot;
	*packet << spell_index;
	*packet << cast_count;
	*packet << item_guid;
	*packet << targetFlag;

	if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_GAMEOBJECT))
		*packet << targetGuid.WriteAsPacked();

	bot->GetSession()->QueuePacket(std::move(packet));
}

void PlayerbotMgr::TellMaster(const std::string& text, const Player* fromPlayer) const
{
	SendWhisper(text, fromPlayer, GetMaster());
}

void PlayerbotMgr::SendWhisper(const std::string& text, const Player* fromPlayer, const Player* toPlayer) const
{
	const auto packet = new WorldPacket(CMSG_MESSAGECHAT, 200);
	*packet << CHAT_MSG_WHISPER;
	*packet << static_cast<uint32>(LANG_UNIVERSAL);
	*packet << toPlayer->GetName();
	*packet << text;
	fromPlayer->GetSession()->QueuePacket(std::move(std::unique_ptr<WorldPacket>(packet)));
}

void PlayerbotMgr::SendChatMessage(const std::string& text, const Player* fromPlayer, const uint32 opCode) const
{
	const auto packet = new WorldPacket(CMSG_MESSAGECHAT, 200);
	*packet << opCode;
	*packet << static_cast<uint32>(LANG_UNIVERSAL);
	*packet << text;
	fromPlayer->GetSession()->QueuePacket(std::move(std::unique_ptr<WorldPacket>(packet)));
}

void PlayerbotMgr::HandleMasterIncomingPacket(const WorldPacket& packet)
{
    switch (packet.GetOpcode())
    {
		case CMSG_USE_ITEM:
		{
			WorldPacket p(packet);
			p.rpos(0); // reset reader
			uint8 bag_index, slot;
			uint8 spell_index;                                      // item spell index which should be used
			uint8 cast_count;                                       // next cast if exists (single or not)
			ObjectGuid item_guid;

			p >> bag_index >> slot >> spell_index >> cast_count >> item_guid;

			Item* p_item = GetMaster()->GetItemByPos(bag_index, slot);
			if (!p_item)
				return;

			if (p_item->GetObjectGuid() != item_guid)
				return;

			if (p_item->GetProto()->ItemId == 100000)
			{
				SpellCastTargets targets;

				p >> targets.ReadForCaster(GetMaster());

				m_lastCommandPosition = targets.m_destPos;
			}
		}

        case CMSG_OFFER_PETITION:
        {
            WorldPacket p(packet);
            p.rpos(0);    // reset reader
            ObjectGuid petitionGuid;
            ObjectGuid playerGuid;
            uint32 junk;

            p >> junk;                                      // this is not petition type!
            p >> petitionGuid;                              // petition guid
            p >> playerGuid;                                // player guid

            Player* player = ObjectAccessor::FindPlayer(playerGuid);
            if (!player)
                return;

            uint32 petitionLowGuid = petitionGuid.GetCounter();

            QueryResult* result = CharacterDatabase.PQuery("SELECT * FROM petition_sign WHERE playerguid = '%u' AND petitionguid = '%u'", player->GetGUIDLow(), petitionLowGuid);

            if (result)
            {
                ChatHandler(m_master).PSendSysMessage("%s has already signed the petition", player->GetName());
                delete result;
                return;
            }

            CharacterDatabase.PExecute("INSERT INTO petition_sign (ownerguid,petitionguid, playerguid, player_account) VALUES ('%u', '%u', '%u','%u')",
                                       GetMaster()->GetGUIDLow(), petitionLowGuid, player->GetGUIDLow(), GetMaster()->GetSession()->GetAccountId());

            p.Initialize(SMSG_PETITION_SIGN_RESULTS, (8 + 8 + 4));
            p << ObjectGuid(petitionGuid);
            p << ObjectGuid(playerGuid);
            p << uint32(PETITION_SIGN_OK);

            // close at signer side
            GetMaster()->GetSession()->SendPacket(p);

            return;
        }

        case CMSG_ACTIVATETAXI:
        {
            WorldPacket p(packet);
            p.rpos(0); // reset reader

            ObjectGuid guid;
            std::vector<uint32> nodes;
            nodes.resize(2);
            uint8 delay = 9;
            p >> guid >> nodes[0] >> nodes[1];

            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_ACTIVATETAXI from %d to %d", nodes[0], nodes[1]);

            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {

                delay = delay + 3;
                Player* const bot = it->second;
                if (!bot)
                    return;

                Group* group = bot->GetGroup();
                if (!group)
                    continue;

                Unit* target = ObjectAccessor::GetUnit(*bot, guid);

                bot->GetPlayerbotAI()->SetIgnoreUpdateTime(delay);

                bot->GetMotionMaster()->Clear(true);
                bot->GetMotionMaster()->MoveFollow(target, INTERACTION_DISTANCE, bot->GetOrientation());
                bot->GetPlayerbotAI()->GetTaxi(guid, nodes);
            }
            return;
        }

        case CMSG_ACTIVATETAXIEXPRESS:
        {
            WorldPacket p(packet);
            p.rpos(0); // reset reader

            ObjectGuid guid;
            uint32 node_count;
            uint8 delay = 9;

            p >> guid;
            p.read_skip<uint32>();
            p >> node_count;

            std::vector<uint32> nodes;
            for (uint32 i = 0; i < node_count; ++i)
            {
                uint32 node;
                p >> node;
                nodes.push_back(node);
            }

            if (nodes.empty())
                return;

            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_ACTIVATETAXIEXPRESS from %d to %d", nodes.front(), nodes.back());

            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {

                delay = delay + 3;
                Player* const bot = it->second;
                if (!bot)
                    return;
                Group* group = bot->GetGroup();
                if (!group)
                    continue;
                Unit* target = ObjectAccessor::GetUnit(*bot, guid);

                bot->GetPlayerbotAI()->SetIgnoreUpdateTime(delay);

                bot->GetMotionMaster()->Clear(true);
                bot->GetMotionMaster()->MoveFollow(target, INTERACTION_DISTANCE, bot->GetOrientation());
                bot->GetPlayerbotAI()->GetTaxi(guid, nodes);
            }
            return;
        }

        // if master is logging out, log out all bots
        case CMSG_LOGOUT_REQUEST:
        {
            LogoutAllBots();
            return;
        }
        
        case CMSG_INSPECT:
        // handle emotes from the master
        //case CMSG_EMOTE:
        case CMSG_TEXT_EMOTE:
        {
            WorldPacket p(packet);
            p.rpos(0); // reset reader
            uint32 emoteNum;
            p >> emoteNum;

            /* std::ostringstream out;
            out << "emote is: " << emoteNum;
            ChatHandler ch(m_master);
            ch.SendSysMessage(out.str().c_str()); */

            switch (emoteNum)
            {
                case TEXTEMOTE_BOW:
                {
                    // Buff anyone who bows before me. Useful for players not in bot's group
                    // How do I get correct target???
                    //Player* const pPlayer = GetPlayerBot(m_master->GetSelection());
                    //if (pPlayer->GetPlayerbotAI()->GetClassAI())
                    //    pPlayer->GetPlayerbotAI()->GetClassAI()->BuffPlayer(pPlayer);
                    return;
                }
                /*
                case TEXTEMOTE_BONK:
                {
                Player* const pPlayer = GetPlayerBot(m_master->GetSelection());
                if (!pPlayer || !pPlayer->GetPlayerbotAI())
                return;
                PlayerbotAI* const pBot = pPlayer->GetPlayerbotAI();

                ChatHandler ch(m_master);
                {
                std::ostringstream out;
                out << "CurrentTime: " << CurrentTime()
                << " m_ignoreAIUpdatesUntilTime: " << pBot->m_ignoreAIUpdatesUntilTime;
                ch.SendSysMessage(out.str().c_str());
                }
                {
                std::ostringstream out;
                out << "m_CurrentlyCastingSpellId: " << pBot->m_CurrentlyCastingSpellId;
                ch.SendSysMessage(out.str().c_str());
                }
                {
                std::ostringstream out;
                out << "IsBeingTeleported() " << pBot->GetPlayer()->IsBeingTeleported();
                ch.SendSysMessage(out.str().c_str());
                }
                {
                std::ostringstream out;
                bool tradeActive = (pBot->GetPlayer()->GetTrader()) ? true : false;
                out << "tradeActive: " << tradeActive;
                ch.SendSysMessage(out.str().c_str());
                }
                {
                std::ostringstream out;
                out << "IsCharmed() " << pBot->getPlayer()->isCharmed();
                ch.SendSysMessage(out.str().c_str());
                }
                return;
                }
                */

                case TEXTEMOTE_EAT:
                case TEXTEMOTE_DRINK:
                    return;

                // emote to attack selected target
                case TEXTEMOTE_POINT:
                {
                    ObjectGuid attackOnGuid = m_master->GetSelectionGuid();
                    if (!attackOnGuid)
                        return;

                    Unit* thingToAttack = ObjectAccessor::GetUnit(*m_master, attackOnGuid);
                    if (!thingToAttack) return;

                    Player* bot = 0;
                    for (PlayerBotMap::iterator itr = m_playerBots.begin(); itr != m_playerBots.end(); ++itr)
                    {
                        bot = itr->second;
                        if (bot->CanAttack(thingToAttack))
                        {
                            if (!bot->IsWithinLOSInMap(thingToAttack))
                                bot->GetPlayerbotAI()->DoTeleport(*m_master);
                            if (bot->IsWithinLOSInMap(thingToAttack))
                                bot->GetPlayerbotAI()->Attack(thingToAttack);
                        }
                    }
                    return;
                }

                // emote to stay
                case TEXTEMOTE_STAND:
                {
                    Player* const bot = GetPlayerBot(m_master->GetSelectionGuid());
                    if (bot)
                        bot->GetPlayerbotAI()->SetMovementOrder(PlayerbotAI::MOVEMENT_STAY);
                    else
                        for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
                        {
                            Player* const bot = it->second;
                            bot->GetPlayerbotAI()->SetMovementOrder(PlayerbotAI::MOVEMENT_STAY);
                        }
                    return;
                }

                // 324 is the followme emote (not defined in enum)
                // if master has bot selected then only bot follows, else all bots follow
                case 324:
                case TEXTEMOTE_WAVE:
                {
                    Player* const bot = GetPlayerBot(m_master->GetSelectionGuid());
                    if (bot)
                        bot->GetPlayerbotAI()->SetMovementOrder(PlayerbotAI::MOVEMENT_FOLLOW, m_master);
                    else
                        for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
                        {
                            Player* const bot = it->second;
                            bot->GetPlayerbotAI()->SetMovementOrder(PlayerbotAI::MOVEMENT_FOLLOW, m_master);
                        }
                    return;
                }
            }
            return;
            } /* EMOTE ends here */

        case CMSG_GAMEOBJ_USE: // Used by bots to turn in quest to GameObjects when also used by master
        {
            WorldPacket p(packet);
            p.rpos(0);     // reset reader
            ObjectGuid objGUID;
            p >> objGUID;

            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;

                // If player and bot are on different maps: then player was teleported by GameObject
                // let's return and let playerbot summon do its job by teleporting bots
                Map* masterMap = m_master->IsInWorld() ? m_master->GetMap() : nullptr;
                if (!masterMap || bot->GetMap() != masterMap || m_master->IsBeingTeleported())
                    return;

                GameObject* obj = masterMap->GetGameObject(objGUID);
                if (!obj)
                    return;

                bot->GetPlayerbotAI()->FollowAutoReset();

                if (obj->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                    bot->GetPlayerbotAI()->TurnInQuests(obj);
                // add other go types here, i.e.:
                // GAMEOBJECT_TYPE_CHEST - loot quest items of chest
            }
        }
        break;

        case CMSG_QUESTGIVER_HELLO:
        {
            WorldPacket p(packet);
            p.rpos(0);    // reset reader
            ObjectGuid npcGUID;
            p >> npcGUID;
            WorldObject* pNpc = m_master->GetMap()->GetWorldObject(npcGUID);
            if (!pNpc)
                return;

            // for all master's bots
            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                bot->GetPlayerbotAI()->FollowAutoReset();
                bot->GetPlayerbotAI()->TurnInQuests(pNpc);
            }

            return;
        }

        // if master accepts a quest, bots should also try to accept quest
        case CMSG_QUESTGIVER_ACCEPT_QUEST:
        {
            WorldPacket p(packet);
            p.rpos(0);    // reset reader
            ObjectGuid guid;
            uint32 quest;
            // uint32 unk1;
            p >> guid >> quest; // >> unk1;

            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_QUESTGIVER_ACCEPT_QUEST npc = %s, quest = %u, unk1 = %u", guid.GetString().c_str(), quest, unk1);

            Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);
            if (qInfo)
                for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
                {
                    Player* const bot = it->second;
                    bot->GetPlayerbotAI()->FollowAutoReset();
                    if (bot->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE)
                        bot->GetPlayerbotAI()->TellMaster("I already completed that quest.");
                    else if (!bot->CanTakeQuest(qInfo, false))
                    {
                        if (!bot->SatisfyQuestStatus(qInfo, false))
                            bot->GetPlayerbotAI()->TellMaster("I already have that quest.");
                        else
                            bot->GetPlayerbotAI()->TellMaster("I can't take that quest.");
                    }
                    else if (!bot->SatisfyQuestLog(false))
                        bot->GetPlayerbotAI()->TellMaster("My quest log is full.");
                    else if (!bot->CanAddQuest(qInfo, false))
                        bot->GetPlayerbotAI()->TellMaster("I can't take that quest because it requires that I take items, but my bags are full!");

                    else
                    {
                        p.rpos(0);         // reset reader
                        bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);
                        bot->GetPlayerbotAI()->TellMaster("Got the quest.");

                        // build needed items if quest contains any
                        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
                            if (qInfo->ReqItemCount[i] > 0)
                            {
                                bot->GetPlayerbotAI()->SetQuestNeedItems();
                                break;
                            }

                        // build needed creatures if quest contains any
                        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
                            if (qInfo->ReqCreatureOrGOCount[i] > 0)
                            {
                                bot->GetPlayerbotAI()->SetQuestNeedCreatures();
                                break;
                            }
                    }
                }
            return;
        }

        case CMSG_AREATRIGGER:
        {
            WorldPacket p(packet);

            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                if (!bot)
                    continue;

                if (bot->IsWithinDistInMap(GetMaster(), 50))
                {
                    p.rpos(0);         // reset reader
                    bot->GetSession()->HandleAreaTriggerOpcode(p);
                }
            }
            return;
        }

        case CMSG_QUESTGIVER_COMPLETE_QUEST:
        {
            WorldPacket p(packet);
            p.rpos(0);    // reset reader
            uint32 quest;
            ObjectGuid npcGUID;
            p >> npcGUID >> quest;

            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_QUESTGIVER_COMPLETE_QUEST npc = %s, quest = %u", npcGUID.GetString().c_str(), quest);

            WorldObject* pNpc = m_master->GetMap()->GetWorldObject(npcGUID);
            if (!pNpc)
                return;

            // for all master's bots
            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                bot->GetPlayerbotAI()->FollowAutoReset();
                bot->GetPlayerbotAI()->TurnInQuests(pNpc);
            }
            return;
        }

        case CMSG_LOOT_ROLL:
        {
            WorldPacket p(packet);  //WorldPacket packet for CMSG_LOOT_ROLL, (8+4+1)
            ObjectGuid Guid;
            uint32 itemSlot;
            uint8 rollType;

            p.rpos(0);              //reset packet pointer
            p >> Guid;              //guid of the lootable target
            p >> itemSlot;          //loot index
            p >> rollType;          //need,greed or pass on roll

            for (auto it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                if (!bot)
                    return;

                if (Group* group = bot->GetGroup(); !group)
                    return;

                // check that the bot did not already vote
                if (rollType >= ROLL_NOT_EMITED_YET)
                    return;
                
                sLootMgr.PlayerVote(bot, Guid, itemSlot, static_cast<RollVote>(0));
            }
            return;
        }
        // Handle GOSSIP activate actions, prior to GOSSIP select menu actions
        case CMSG_GOSSIP_HELLO:
        {
            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_GOSSIP_HELLO");

            WorldPacket p(packet);    //WorldPacket packet for CMSG_GOSSIP_HELLO, (8)
            ObjectGuid guid;
            p.rpos(0);                //reset packet pointer
            p >> guid;
            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                if (!bot)
                    continue;
                bot->GetPlayerbotAI()->FollowAutoReset();
                Creature* pCreature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
                if (!pCreature)
                {
                    DEBUG_LOG("[PlayerbotMgr]: HandleMasterIncomingPacket - Received  CMSG_GOSSIP_HELLO %s not found or you can't interact with him.", guid.GetString().c_str());
                    continue;
                }

                GossipMenuItemsMapBounds pMenuItemBounds = sObjectMgr.GetGossipMenuItemsMapBounds(pCreature->GetCreatureInfo()->GossipMenuId);
                for (GossipMenuItemsMap::const_iterator itr = pMenuItemBounds.first; itr != pMenuItemBounds.second; ++itr)
                {
                    uint32 npcflags = pCreature->GetUInt32Value(UNIT_NPC_FLAGS);

                    if (!(itr->second.npc_option_npcflag & npcflags))
                        continue;

                    switch (itr->second.option_id)
                    {
                        case GOSSIP_OPTION_TAXIVENDOR:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_TAXIVENDOR");
                            bot->GetSession()->SendLearnNewTaxiNode(pCreature);
                            break;
                        }
                        case GOSSIP_OPTION_QUESTGIVER:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_QUESTGIVER");
                            bot->GetPlayerbotAI()->TurnInQuests(pCreature);
                            break;
                        }
                        case GOSSIP_OPTION_VENDOR:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_VENDOR");
                            if (!botConfig.GetBoolDefault("PlayerbotAI.SellGarbage", true))
                                continue;

                            // changed the SellGarbage() function to support ch.SendSysMessaage()
                            bot->GetPlayerbotAI()->SellGarbage(*bot);
                            break;
                        }
                        case GOSSIP_OPTION_STABLEPET:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_STABLEPET");
                            break;
                        }
                        case GOSSIP_OPTION_AUCTIONEER:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_AUCTIONEER");
                            break;
                        }
                        case GOSSIP_OPTION_BANKER:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_BANKER");
                            break;
                        }
                        case GOSSIP_OPTION_INNKEEPER:
                        {
                            // bot->GetPlayerbotAI()->TellMaster("PlayerbotMgr:GOSSIP_OPTION_INNKEEPER");
                            break;
                        }
                    }
                }
            }
            return;
        }

        case CMSG_SPIRIT_HEALER_ACTIVATE:
        {
            // DEBUG_LOG ("[PlayerbotMgr]: HandleMasterIncomingPacket - Received CMSG_SPIRIT_HEALER_ACTIVATE SpiritHealer is resurrecting the Player %s",m_master->GetName());
            for (PlayerBotMap::iterator itr = m_playerBots.begin(); itr != m_playerBots.end(); ++itr)
            {
                Player* const bot = itr->second;
                Group* grp = bot->GetGroup();
                if (grp)
                    grp->RemoveMember(bot->GetObjectGuid(), 1);
            }
            return;
        }

        case CMSG_LIST_INVENTORY:
        {
            if (!botConfig.GetBoolDefault("PlayerbotAI.SellGarbage", true))
                return;

            WorldPacket p(packet);
            p.rpos(0);  // reset reader
            ObjectGuid npcGUID;
            p >> npcGUID;

            Object* const pNpc = (WorldObject*) m_master->GetObjectByTypeMask(npcGUID, TYPEMASK_CREATURE_OR_GAMEOBJECT);
            if (!pNpc)
                return;

            // for all master's bots
            for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
            {
                Player* const bot = it->second;
                if (!bot->IsInMap(static_cast<WorldObject*>(pNpc)))
                {
                    bot->GetPlayerbotAI()->TellMaster("I'm too far away to sell items!");
                    continue;
                }
                else
                {
                    // changed the SellGarbage() function to support ch.SendSysMessaage()
                    bot->GetPlayerbotAI()->FollowAutoReset();
                    bot->GetPlayerbotAI()->SellGarbage(*bot);
                }
            }
            return;
        }

        /*
        case CMSG_NAME_QUERY:
        case MSG_MOVE_START_FORWARD:
        case MSG_MOVE_STOP:
        case MSG_MOVE_SET_FACING:
        case MSG_MOVE_START_STRAFE_LEFT:
        case MSG_MOVE_START_STRAFE_RIGHT:
        case MSG_MOVE_STOP_STRAFE:
        case MSG_MOVE_START_BACKWARD:
        case MSG_MOVE_HEARTBEAT:
        case CMSG_STANDSTATECHANGE:
        case CMSG_QUERY_TIME:
        case CMSG_CREATURE_QUERY:
        case CMSG_GAMEOBJECT_QUERY:
        case MSG_MOVE_JUMP:
        case MSG_MOVE_FALL_LAND:
        return;*/

        default:
        {
            /*const char* oc = LookupOpcodeName(packet.GetOpcode());
            // ChatHandler ch(m_master);
            // ch.SendSysMessage(oc);

            std::ostringstream out;
            out << "masterin: " << oc;
            sLog.outError(out.str().c_str()); */
        }
    }
}

void PlayerbotMgr::HandleMasterOutgoingPacket(const WorldPacket& packet) const
{
	switch (packet.GetOpcode())
	{
	// If a change in speed was detected for the master
	// make sure we have the same mount status
	case SMSG_FORCE_RUN_SPEED_CHANGE:
		{
			WorldPacket p(packet);
			ObjectGuid guid;

			// Only adjust speed and mount if this is master that did so
			p >> guid.ReadAsPacked();
			if (guid != GetMaster()->GetObjectGuid())
				return;

			for (auto it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
			{
				Player* const bot = it->second;
				if (GetMaster()->IsMounted() && !bot->IsMounted())
				{
					// Player Part
					if (!GetMaster()->GetAurasByType(SPELL_AURA_MOUNTED).empty())
					{
						int32 master_speed1 = 0;
						int32 master_speed2 = 0;
						master_speed1 = GetMaster()->GetAurasByType(SPELL_AURA_MOUNTED).front()->GetSpellProto()->
						                             EffectBasePoints[1];
						master_speed2 = GetMaster()->GetAurasByType(SPELL_AURA_MOUNTED).front()->GetSpellProto()->
						                             EffectBasePoints[2];

						// Bot Part
						// Step 1: find spell in bot spellbook that matches the speed change from master
						uint32 spellMount = 0;
						for (auto& itr : bot->GetSpellMap())
						{
							uint32 spellId = itr.first;
							if (itr.second.state == PLAYERSPELL_REMOVED || itr.second.disabled || IsPassiveSpell(
								spellId))
								continue;
							const SpellEntry* pSpellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
							if (!pSpellInfo)
								continue;

							if (pSpellInfo->EffectApplyAuraName[0] == SPELL_AURA_MOUNTED)
							{
								if (pSpellInfo->EffectApplyAuraName[1] == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
								{
									if (pSpellInfo->EffectBasePoints[1] == master_speed1)
									{
										spellMount = spellId;
									}
								}
								else if (pSpellInfo->EffectApplyAuraName[2] == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
									if (pSpellInfo->EffectBasePoints[2] == master_speed2)
									{
										spellMount = spellId;
									}
							}
						}
						if (spellMount > 0 && bot->CastSpell(bot, spellMount, TRIGGERED_NONE) == SPELL_CAST_OK)
							continue;

						// Step 2: no spell found or cast failed -> search for an item in inventory (mount)
						// We start with the fastest mounts as bot will not be able to outrun its master since it is following him/her
						uint32 skillLevels[] = {375, 300, 225, 150, 75};
						for (uint32 level : skillLevels)
						{
							Item* mount = bot->GetPlayerbotAI()->FindMount(level);
							if (mount)
							{
								bot->GetPlayerbotAI()->UseItem(mount);
							}
						}
					}
				}
				// If master dismounted, do so
				else if (!GetMaster()->IsMounted() && bot->IsMounted())
				// only execute code if master is the one who dismounted
				{
					WorldPacket emptyPacket;
					bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket); //updated code
				}
			}
		}
	// maybe our bots should only start looting after the master loots?
	//case SMSG_LOOT_RELEASE_RESPONSE: {}
	case SMSG_NAME_QUERY_RESPONSE:
	case SMSG_MONSTER_MOVE:
	case SMSG_COMPRESSED_UPDATE_OBJECT:
	case SMSG_DESTROY_OBJECT:
	case SMSG_UPDATE_OBJECT:
	case SMSG_STANDSTATE_UPDATE:
	case MSG_MOVE_HEARTBEAT:
	case SMSG_QUERY_TIME_RESPONSE:
	case SMSG_CREATURE_QUERY_RESPONSE:
	case SMSG_GAMEOBJECT_QUERY_RESPONSE:
	default:
		{
		}
	}
}

void PlayerbotMgr::RemoveBots()
{
    for (auto& guid : m_botToRemove)
    {
        Player* bot = GetPlayerBot(guid);
        if (bot)
        {
            WorldSession* botWorldSessionPtr = bot->GetSession();
            m_playerBots.erase(guid);                               // deletes bot player ptr inside this WorldSession PlayerBotMap
            botWorldSessionPtr->LogoutPlayer();                     // this will delete the bot Player object and PlayerbotAI object
            delete botWorldSessionPtr;                              // finally delete the bot's WorldSession
        }
    }

    m_botToRemove.clear();
}

void PlayerbotMgr::LogoutAllBots(bool fullRemove /*= false*/)
{
    for (auto itr : m_playerBots)
        m_botToRemove.insert(itr.first);

    if (fullRemove)
        RemoveBots();
}

void PlayerbotMgr::Stay()
{
    for (PlayerBotMap::const_iterator itr = GetPlayerBotsBegin(); itr != GetPlayerBotsEnd(); ++itr)
    {
        Player* bot = itr->second;
        bot->GetMotionMaster()->Clear();
    }
}

// Playerbot mod: logs out a Playerbot.
void PlayerbotMgr::LogoutPlayerBot(ObjectGuid guid)
{
    m_botToRemove.insert(guid);
}

// Playerbot mod: Gets a player bot Player object for this WorldSession master
Player* PlayerbotMgr::GetPlayerBot(ObjectGuid playerGuid) const
{
    PlayerBotMap::const_iterator it = m_playerBots.find(playerGuid);
    return (it == m_playerBots.end()) ? 0 : it->second;
}

void PlayerbotMgr::OnBotLogin(Player* const bot)
{
    // simulate client taking control
    WorldPacket* const pCMSG_SET_ACTIVE_MOVER = new WorldPacket(CMSG_SET_ACTIVE_MOVER, 8);
    *pCMSG_SET_ACTIVE_MOVER << bot->GetObjectGuid();
    bot->GetSession()->QueuePacket(std::move(std::unique_ptr<WorldPacket>(pCMSG_SET_ACTIVE_MOVER)));

    WorldPacket* const pMSG_MOVE_FALL_LAND = new WorldPacket(MSG_MOVE_FALL_LAND, 28);
    *pMSG_MOVE_FALL_LAND << bot->GetMover()->m_movementInfo;
    bot->GetSession()->QueuePacket(std::move(std::unique_ptr<WorldPacket>(pMSG_MOVE_FALL_LAND)));

    // give the bot some AI, object is owned by the player class
    PlayerbotAI* ai = new PlayerbotAI(*this, bot, m_confDebugWhisper, m_lua);
    bot->SetPlayerbotAI(ai);

    // tell the world session that they now manage this new bot
    m_playerBots[bot->GetObjectGuid()] = bot;

    // if bot is in a group and master is not in group then
    // have bot leave their group
    if (bot->GetGroup() &&
            (m_master->GetGroup() == nullptr ||
             m_master->GetGroup()->IsMember(bot->GetObjectGuid()) == false))
        bot->RemoveFromGroup();

    // sometimes master can lose leadership, pass leadership to master check
    const ObjectGuid masterGuid = m_master->GetObjectGuid();
    if (m_master->GetGroup() &&
            !m_master->GetGroup()->IsLeader(masterGuid))
    {
        // But only do so if one of the master's bots is leader
        for (PlayerBotMap::const_iterator itr = GetPlayerBotsBegin(); itr != GetPlayerBotsEnd(); itr++)
        {
            Player* bot = itr->second;
            if (m_master->GetGroup()->IsLeader(bot->GetObjectGuid()))
            {
                m_master->GetGroup()->ChangeLeader(masterGuid);
                break;
            }
        }
    }
}

void PlayerbotMgr::RemoveAllBotsFromGroup()
{
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); m_master->GetGroup() && it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (bot->IsInGroup(m_master))
            m_master->GetGroup()->RemoveMember(bot->GetObjectGuid(), 0);
    }
}

void Creature::LoadBotMenu(Player* pPlayer)
{

    if (pPlayer->GetPlayerbotAI()) return;
    ObjectGuid guid = pPlayer->GetObjectGuid();
    uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid);
    QueryResult* result = CharacterDatabase.PQuery("SELECT guid, name FROM characters WHERE account='%d'", accountId);
    do
    {
        Field* fields = result->Fetch();
        ObjectGuid guidlo = ObjectGuid(fields[0].GetUInt64());
        std::string name = fields[1].GetString();
        std::string word = "";

        if ((guid == ObjectGuid()) || (guid == guidlo))
        {
            //not found or himself
        }
        else
        {
            // if(sConfig.GetBoolDefault("PlayerbotAI.DisableBots", false)) return;
            // create the manager if it doesn't already exist
            if (!pPlayer->GetPlayerbotMgr())
                pPlayer->SetPlayerbotMgr(new PlayerbotMgr(pPlayer));
            if (pPlayer->GetPlayerbotMgr()->GetPlayerBot(guidlo) == nullptr) // add (if not already in game)
            {
                word += "Recruit ";
                word += name;
                word += " as a Bot.";
                pPlayer->GetPlayerMenu()->GetGossipMenu().AddMenuItem((uint8) 9, word, guidlo, GOSSIP_OPTION_BOT, word, false);
            }
            else if (pPlayer->GetPlayerbotMgr()->GetPlayerBot(guidlo) != nullptr) // remove (if in game)
            {
                word += "Dismiss ";
                word += name;
                word += " from duty.";
                pPlayer->GetPlayerMenu()->GetGossipMenu().AddMenuItem((uint8) 0, word, guidlo, GOSSIP_OPTION_BOT, word, false);
            }
        }
    }
    while (result->NextRow());
    delete result;
}

void Player::skill(std::list<uint32>& m_spellsToLearn)
{
    for (SkillStatusMap::const_iterator itr = mSkillStatus.begin(); itr != mSkillStatus.end(); ++itr)
    {
        if (itr->second.uState == SKILL_DELETED)
            continue;

        uint32 pskill = itr->first;

        m_spellsToLearn.push_back(pskill);
    }
}

void Player::chompAndTrim(std::string& str)
{
    while (str.length() > 0)
    {
        char lc = str[str.length() - 1];
        if (lc == '\r' || lc == '\n' || lc == ' ' || lc == '"' || lc == '\'')
            str = str.substr(0, str.length() - 1);
        else
            break;
        while (str.length() > 0)
        {
            char lc = str[0];
            if (lc == ' ' || lc == '"' || lc == '\'')
                str = str.substr(1, str.length() - 1);
            else
                break;
        }
    }
}

bool Player::getNextQuestId(const std::string& pString, unsigned int& pStartPos, unsigned int& pId)
{
    bool result = false;
    unsigned int i;
    for (i = pStartPos; i < pString.size(); ++i)
    {
        if (pString[i] == ',')
            break;
    }
    if (i > pStartPos)
    {
        std::string idString = pString.substr(pStartPos, i - pStartPos);
        pStartPos = i + 1;
        chompAndTrim(idString);
        pId = atoi(idString.c_str());
        result = true;
    }
    return (result);
}

bool Player::requiredQuests(const char* pQuestIdString)
{
    if (pQuestIdString != nullptr)
    {
        unsigned int pos = 0;
        unsigned int id;
        std::string confString(pQuestIdString);
        chompAndTrim(confString);
        while (getNextQuestId(confString, pos, id))
        {
            QuestStatus status = GetQuestStatus(id);
            if (status == QUEST_STATUS_COMPLETE)
                return true;
        }
    }
    return false;
}

void Player::UpdateMail()
{
    // save money,items and mail to prevent cheating
    CharacterDatabase.BeginTransaction();
    this->SaveGoldToDB();
    this->SaveInventoryAndGoldToDB();
    this->_SaveMail();
    CharacterDatabase.CommitTransaction();
}

//See MainSpec enum in PlayerbotAI.h for details on class return values
uint32 Player::GetSpec()
{
    uint32 row = 0, spec = 0;
    Player* player = m_session->GetPlayer();
    uint32 classMask = player->getClassMask();

    for (unsigned int i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);

        if (!talentInfo)
            continue;

        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);

        if (!talentTabInfo)
            continue;

        if ((classMask & talentTabInfo->ClassMask) == 0)
            continue;

        uint32 curtalent_maxrank = 0;
        for (int32 k = MAX_TALENT_RANK - 1; k > -1; --k)
        {
            if (talentInfo->RankID[k] && HasSpell(talentInfo->RankID[k]))
            {
                if (row == 0 && spec == 0)
                    spec = talentInfo->TalentTab;

                if (talentInfo->Row > row)
                {
                    row = talentInfo->Row;
                    spec = talentInfo->TalentTab;
                }
            }
        }
    }

    //Return the tree with the deepest talent
    return spec;
}

bool PlayerbotMgr::LoadAIScript(const std::string& name, const std::string& url)
{
	const cpr::Response response = Get(cpr::Url{url}, cpr::VerifySsl(false));

	if (response.status_code != 200)
	{
		m_masterChatHandler.PSendSysMessage(
			"|cffff0000Could not acquire script from url %s returned %u HTTP status. Error message: %s", url.c_str(),
			static_cast<unsigned int>(response.status_code), response.error.message.c_str());
		m_masterChatHandler.SetSentErrorMessage(true);
		return false;
	}

	const std::string script = response.text;

	if (ValidateLuaScript(script.c_str()))
	{
		if (CharacterDatabase.PExecute(
			"INSERT INTO scripts (name, script, url) VALUES ('%s', '%s', '%s') ON DUPLICATE KEY UPDATE script = '%s'",
			name.c_str(), script.c_str(), url.c_str(), script.c_str()))
		{
			m_masterChatHandler.PSendSysMessage("Script '%s' downloaded, saved, and loaded successfully.",
			                                    name.c_str());
		}
		else
		{
			m_masterChatHandler.PSendSysMessage(
				"|cffff0000Script was downloaded and validated, but could not be inserted into the database.");
			m_masterChatHandler.SetSentErrorMessage(true);
			return false;
		}
	}

	return true;
}

bool PlayerbotMgr::VerifyScriptExists(const std::string& name)
{
	if (const QueryResult* count_result = CharacterDatabase.PQuery(
		"SELECT COUNT(*) FROM scripts WHERE name = '%s'", name.c_str()))
	{
		const Field* count_result_fields = count_result->Fetch();

		if (const int name_count = count_result_fields[0].GetInt32(); name_count == 0)
		{
			m_masterChatHandler.PSendSysMessage("|cffff0000No script was found by the name '%s'", name.c_str());
			m_masterChatHandler.SetSentErrorMessage(true);
			return false;
		}
	}
	else
	{
		m_masterChatHandler.PSendSysMessage("|cffff0000No script result for the name '%s'", name.c_str());
		m_masterChatHandler.SetSentErrorMessage(true);
		return false;
	}

	return true;
}

bool ChatHandler::HandlePlayerbotCommand(char* args)
{
    if (!(m_session->GetSecurity() > SEC_PLAYER))
    {
        if (botConfig.GetBoolDefault("PlayerbotAI.DisableBots", false))
        {
            PSendSysMessage("|cffff0000Playerbot system is currently disabled!");
            SetSentErrorMessage(true);
            return false;
        }
    }

    if (!m_session)
    {
        PSendSysMessage("|cffff0000You may only add bots from an active session");
        SetSentErrorMessage(true);
        return false;
    }

    if (!*args)
    {
        PSendSysMessage("|cffff0000usage: add PLAYERNAME  or  remove PLAYERNAME or ai ...");
        SetSentErrorMessage(true);
        return false;
    }

    std::string cmd_string = args;
    boost::algorithm::trim(cmd_string);

    // if args starts with ai
    if (cmd_string.rfind("ai", 0) == 0)
    {
	    // create the playerbot manager if it doesn't already exist
	    PlayerbotMgr* mgr = m_session->GetPlayer()->GetPlayerbotMgr();
	    if (!mgr)
	    {
		    mgr = new PlayerbotMgr(m_session->GetPlayer());
		    m_session->GetPlayer()->SetPlayerbotMgr(mgr);
	    }

	    std::string rem_cmd = cmd_string.substr(2);
	    boost::algorithm::trim(rem_cmd);

	    // print help if requested or missing args
	    if (rem_cmd.empty() || rem_cmd.rfind("help", 0) == 0)
	    {
		    PSendSysMessage(
			    R"(usage: 
load <NAME>: load ai script from memory
set <NAME> <URL>: download script from url, store as name and load
remove <NAME>: remove script from database
reload <NAME>: re-download script from same url)");
		    return true;
	    }

	    if (rem_cmd.rfind("load", 0) == 0)
	    {
		    std::string name = rem_cmd.substr(4);
		    boost::algorithm::trim(name);

		    if (name.empty())
		    {
			    PSendSysMessage("|cffff0000No script name provided.");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    if (mgr->VerifyScriptExists(name))
		    {
			    if (const QueryResult* load_result = CharacterDatabase.PQuery(
				    "SELECT script FROM scripts WHERE name = '%s'", name.c_str()))
			    {
				    const Field* load_fields = load_result->Fetch();

				    if (const char* script = load_fields[0].GetString(); mgr->ValidateLuaScript(script))
					    PSendSysMessage("Script '%s' read successfully.", name.c_str());
			    }
		    }

		    return false;
	    }

	    if (rem_cmd.rfind("reload", 0) == 0)
	    {
		    std::string name = rem_cmd.substr(6);
		    boost::algorithm::trim(name);

		    if (name.empty())
		    {
			    PSendSysMessage("|cffff0000No script name provided.");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    if (mgr->VerifyScriptExists(name))
		    {
			    if (const QueryResult* load_result = CharacterDatabase.PQuery(
				    "SELECT url FROM scripts WHERE name = '%s'", name.c_str()))
			    {
                    const Field* load_fields = load_result->Fetch();
                    return mgr->LoadAIScript(name, load_fields[0].GetCppString());
			    }
		    }

		    return false;
	    }

	    if (rem_cmd.rfind("remove", 0) == 0)
	    {
		    std::string name = rem_cmd.substr(6);
		    boost::algorithm::trim(name);

		    if (name.empty())
		    {
			    PSendSysMessage("|cffff0000No script name provided.");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    if (mgr->VerifyScriptExists(name))
		    {
			    if (CharacterDatabase.PExecute(
				    "DELETE FROM scripts WHERE name = '%s'", name.c_str()))
			    {
				    PSendSysMessage("Script '%s' removed successfully.", name.c_str());
			    }
		    }

		    return false;
	    }

        if (rem_cmd.rfind("write", 0) == 0)
        {
            std::string message = rem_cmd.substr(5);
            boost::algorithm::trim(message);

            mgr->SetLuaMasterMessage(message);

            return false;
        }

	    if (rem_cmd.rfind("set", 0) == 0)
	    {
		    std::string rem_set_cmd = rem_cmd.substr(3);
		    boost::algorithm::trim(rem_set_cmd);

		    auto first_space = rem_set_cmd.find_first_of(' ');

		    if (first_space == -1)
		    {
			    PSendSysMessage("|cffff0000No script name or url provided. usage example: .bot ai set <NAME> <URL>");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    std::string name = rem_set_cmd.substr(0, first_space);
		    boost::algorithm::trim(name);

		    std::string url = rem_set_cmd.substr(first_space);
		    boost::algorithm::trim(url);

		    if (name.empty())
		    {
			    PSendSysMessage("|cffff0000No script name provided.");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    if (url.empty())
		    {
			    PSendSysMessage("|cffff0000No script url provided.");
			    SetSentErrorMessage(true);
			    return false;
		    }

		    return mgr->LoadAIScript(name, url);
	    }

	    return true;
    }

    char* cmd = strtok((char*) args, " ");
    char* charname = strtok(nullptr, " ");

    if (!cmd || !charname)
    {
        PSendSysMessage("|cffff0000usage: add PLAYERNAME  or  remove PLAYERNAME");
        SetSentErrorMessage(true);
        return false;
    }

    std::string cmdStr = cmd;
    std::string charnameStr = charname;

    if (!normalizePlayerName(charnameStr))
        return false;

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(charnameStr.c_str());
    if (guid == ObjectGuid() || (guid == m_session->GetPlayer()->GetObjectGuid()))
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid);
    if (accountId != m_session->GetAccountId())
    {
        PSendSysMessage("|cffff0000You may only add bots from the same account.");
        SetSentErrorMessage(true);
        return false;
    }

    // create the playerbot manager if it doesn't already exist
    PlayerbotMgr* mgr = m_session->GetPlayer()->GetPlayerbotMgr();
    if (!mgr)
    {
        mgr = new PlayerbotMgr(m_session->GetPlayer());
        m_session->GetPlayer()->SetPlayerbotMgr(mgr);
    }

    QueryResult* resultchar = CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE online = '1' AND account = '%u'", m_session->GetAccountId());
    if (resultchar)
    {
        Field* fields = resultchar->Fetch();
        int acctcharcount = fields[0].GetUInt32();
        int maxnum = botConfig.GetIntDefault("PlayerbotAI.MaxNumBots", 9);
        if (!(m_session->GetSecurity() > SEC_PLAYER))
            if (acctcharcount > maxnum && (cmdStr == "add" || cmdStr == "login"))
            {
                PSendSysMessage("|cffff0000You cannot summon anymore bots.(Current Max: |cffffffff%u)", maxnum);
                SetSentErrorMessage(true);
                delete resultchar;
                return false;
            }
        delete resultchar;
    }

    QueryResult* resultlvl = CharacterDatabase.PQuery("SELECT level, name, race FROM characters WHERE guid = '%u'", guid.GetCounter());
    if (resultlvl)
    {
        Field* fields = resultlvl->Fetch();
        int charlvl = fields[0].GetUInt32();
        int maxlvl = botConfig.GetIntDefault("PlayerbotAI.RestrictBotLevel", 80);
        uint8 race = fields[2].GetUInt8();
        uint32 team = 0;

        team = Player::TeamForRace(race);

        if (!(m_session->GetSecurity() > SEC_PLAYER))
        {
            if (charlvl > maxlvl)
            {
                PSendSysMessage("|cffff0000You cannot summon |cffffffff[%s]|cffff0000, it's level is too high.(Current Max:lvl |cffffffff%u)", fields[1].GetString(), maxlvl);
                SetSentErrorMessage(true);
                delete resultlvl;
                return false;
            }

            // Opposing team bot
            if (!sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_GROUP) && m_session->GetPlayer()->GetTeam() != team)
            {
                PSendSysMessage("|cffff0000You cannot summon |cffffffff[%s]|cffff0000, a member of the enemy side", fields[1].GetString());
                SetSentErrorMessage(true);
                delete resultlvl;
                return false;
            }
        }
        delete resultlvl;
    }
    // end of gmconfig patch
    if (cmdStr == "add" || cmdStr == "login")
    {
        if (mgr->GetPlayerBot(guid))
        {
            PSendSysMessage("Bot already exists in world.");
            SetSentErrorMessage(true);
            return false;
        }
        CharacterDatabase.DirectPExecute("UPDATE characters SET online = 1 WHERE guid = '%u'", guid.GetCounter());
        mgr->LoginPlayerBot(guid);
        PSendSysMessage("Bot added successfully.");
    }
    else if (cmdStr == "remove" || cmdStr == "logout")
    {
        if (!mgr->GetPlayerBot(guid))
        {
            PSendSysMessage("|cffff0000Bot can not be removed because bot does not exist in world.");
            SetSentErrorMessage(true);
            return false;
        }
        CharacterDatabase.DirectPExecute("UPDATE characters SET online = 0 WHERE guid = '%u'", guid.GetCounter());
        mgr->LogoutPlayerBot(guid);
        PSendSysMessage("Bot removed successfully.");
    }

    return true;
}

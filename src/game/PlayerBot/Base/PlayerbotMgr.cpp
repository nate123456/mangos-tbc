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

	if (!m_hasLoadedScript)
		return;

	std::vector<Player*> bots;
	bots.reserve(m_playerBots.size());

	for (auto& [id, bot] : m_playerBots)
		bots.push_back(bot);

	const sol::protected_function act_func = m_luaEnvironment["Main"];

	if (!act_func.valid())
	{
		if (const auto error_msg = "No 'Main' function defined."; m_lastActErrorMsg !=
			error_msg)
		{
			m_masterChatHandler.PSendSysMessage("|cffff0000%s", error_msg);
			m_lastActErrorMsg = error_msg;
		}

		return;
	}

	m_luaEnvironment["_CommandMessage"] = m_lastManagerMessage;
	m_luaEnvironment["_CommandPosition"] = m_lastCommandPosition;
	m_luaEnvironment["_Bots"] = bots;

	sol::table raid_icons = m_luaEnvironment.create("_RaidIcons");

	raid_icons["Star"] = GetRaidIcon(0);
	raid_icons["Circle"] = GetRaidIcon(1);
	raid_icons["Diamond"] = GetRaidIcon(2);
	raid_icons["Triangle"] = GetRaidIcon(3);
	raid_icons["Moon"] = GetRaidIcon(4);
	raid_icons["Square"] = GetRaidIcon(5);
	raid_icons["Cross"] = GetRaidIcon(6);
	raid_icons["Skull"] = GetRaidIcon(7);

	if (const auto act_result = act_func(); !act_result.valid())
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

	if (!m_lastCommandPosition.IsEmpty())
		m_lastCommandPosition = Position();
}

void PlayerbotMgr::InitLua()
{
	m_lua.open_libraries(sol::lib::base,
	                     sol::lib::package,
	                     sol::lib::coroutine,
	                     sol::lib::string,
	                     sol::lib::math,
	                     sol::lib::table);

	m_lua.clear_package_loaders();
	m_lua.add_package_loader([&](const std::string& name)
	{
		const auto account_id = m_master->GetSession()->GetAccountId();

		if (VerifyScriptExists(name))
		{
			if (const QueryResult* load_result = CharacterDatabase.PQuery(
				"SELECT script FROM scripts WHERE name = '%s' AND accountid = %u", name.c_str(), account_id))
			{
				const Field* load_fields = load_result->Fetch();

				const std::string module_script = load_fields[0].GetString();

				if (const auto result = m_lua.load(module_script); result.valid())
				{
					return result.get<sol::object>();
				}
				else
				{
					return make_object(m_lua, static_cast<sol::error>(result).what());
				}
			}
		}		

		return make_object(m_lua, "Could not locate module  '" + name + "'.");
	});

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

	m_lua.script("str = tostring num = tonumber");

	m_lua.script("print('[DEBUG] LUA has been initialized.')");
}

void PlayerbotMgr::InitializeLuaEnvironment()
{
	m_luaEnvironment = sol::environment(m_lua, sol::create, m_lua.globals());

	if (!m_lastActErrorMsg.empty())
		m_lastActErrorMsg = "";

	if (!m_lastManagerMessage.empty())
		m_lastManagerMessage = "";

	m_lastCommandPosition = Position();

	if (!m_lastCommandPosition.IsEmpty())
		m_lastCommandPosition = Position();
}

bool PlayerbotMgr::SafeLoadLuaScript(const std::string& name, const std::string& script)
{
	InitializeLuaEnvironment();
	
	if (script.find("Main()") != std::string::npos)
	{
		if (const auto result = m_lua.safe_script(script, m_luaEnvironment); !result.valid())
		{
			const sol::error error = result;
			m_masterChatHandler.PSendSysMessage("|cffff0000Failed to load ai script:\n%s", error.what());
			return false;
		}

		m_hasLoadedScript = true;
	}
	// we just want to ensure that it's valid lua- let the require finding code locate it on demand.
	else if (const auto result = m_lua.load(script); !result.valid())
	{
		const sol::error error = result;
		m_masterChatHandler.PSendSysMessage("|cffff0000Failed to load ai script module:\n%s", error.what());
		return false;
	}

	return true;
}

void PlayerbotMgr::InitLuaMembers()
{
	m_lua["Master"] = m_master;
	m_lua["PI"] = M_PI_F;

	sol::table class_enum = m_lua.create_table("_Class");

	class_enum[3] = "Mage";
	class_enum[4] = "Warrior";
	class_enum[5] = "Warlock";
	class_enum[6] = "Priest";
	class_enum[7] = "Druid";
	class_enum[8] = "Rogue";
	class_enum[9] = "Hunter";
	class_enum[10] = "Paladin";
	class_enum[11] = "Shaman";

	FlipLuaTable("Class");

	sol::table spell_result_enum = m_lua.create_table("_SpellResult");

	spell_result_enum[0] = "SPELL_FAILED_AFFECTING_COMBAT";
	spell_result_enum[1] = "SPELL_FAILED_ALREADY_AT_FULL_HEALTH";
	spell_result_enum[2] = "SPELL_FAILED_ALREADY_AT_FULL_MANA";
	spell_result_enum[3] = "SPELL_FAILED_ALREADY_AT_FULL_POWER";
	spell_result_enum[4] = "SPELL_FAILED_ALREADY_BEING_TAMED";
	spell_result_enum[5] = "SPELL_FAILED_ALREADY_HAVE_CHARM";
	spell_result_enum[6] = "SPELL_FAILED_ALREADY_HAVE_SUMMON";
	spell_result_enum[7] = "SPELL_FAILED_ALREADY_OPEN";
	spell_result_enum[8] = "SPELL_FAILED_AURA_BOUNCED";
	spell_result_enum[9] = "SPELL_FAILED_AUTOTRACK_INTERRUPTED";
	spell_result_enum[10] = "SPELL_FAILED_BAD_IMPLICIT_TARGETS";
	spell_result_enum[11] = "SPELL_FAILED_BAD_TARGETS";
	spell_result_enum[12] = "SPELL_FAILED_CANT_BE_CHARMED";
	spell_result_enum[13] = "SPELL_FAILED_CANT_BE_DISENCHANTED";
	spell_result_enum[14] = "SPELL_FAILED_CANT_BE_DISENCHANTED_SKILL";
	spell_result_enum[15] = "SPELL_FAILED_CANT_BE_PROSPECTED";
	spell_result_enum[16] = "SPELL_FAILED_CANT_CAST_ON_TAPPED";
	spell_result_enum[17] = "SPELL_FAILED_CANT_DUEL_WHILE_INVISIBLE";
	spell_result_enum[18] = "SPELL_FAILED_CANT_DUEL_WHILE_STEALTHED";
	spell_result_enum[19] = "SPELL_FAILED_CANT_STEALTH";
	spell_result_enum[20] = "SPELL_FAILED_CASTER_AURASTATE";
	spell_result_enum[21] = "SPELL_FAILED_CASTER_DEAD";
	spell_result_enum[22] = "SPELL_FAILED_CHARMED";
	spell_result_enum[23] = "SPELL_FAILED_CHEST_IN_USE";
	spell_result_enum[24] = "SPELL_FAILED_CONFUSED";
	spell_result_enum[25] = "SPELL_FAILED_DONT_REPORT";
	spell_result_enum[26] = "SPELL_FAILED_EQUIPPED_ITEM";
	spell_result_enum[27] = "SPELL_FAILED_EQUIPPED_ITEM_CLASS";
	spell_result_enum[28] = "SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND";
	spell_result_enum[29] = "SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND";
	spell_result_enum[30] = "SPELL_FAILED_ERROR";
	spell_result_enum[31] = "SPELL_FAILED_FIZZLE";
	spell_result_enum[32] = "SPELL_FAILED_FLEEING";
	spell_result_enum[33] = "SPELL_FAILED_FOOD_LOWLEVEL";
	spell_result_enum[34] = "SPELL_FAILED_HIGHLEVEL";
	spell_result_enum[35] = "SPELL_FAILED_HUNGER_SATIATED";
	spell_result_enum[36] = "SPELL_FAILED_IMMUNE";
	spell_result_enum[37] = "SPELL_FAILED_INTERRUPTED";
	spell_result_enum[38] = "SPELL_FAILED_INTERRUPTED_COMBAT";
	spell_result_enum[39] = "SPELL_FAILED_ITEM_ALREADY_ENCHANTED";
	spell_result_enum[40] = "SPELL_FAILED_ITEM_GONE";
	spell_result_enum[41] = "SPELL_FAILED_ITEM_NOT_FOUND";
	spell_result_enum[42] = "SPELL_FAILED_ITEM_NOT_READY";
	spell_result_enum[43] = "SPELL_FAILED_LEVEL_REQUIREMENT";
	spell_result_enum[44] = "SPELL_FAILED_LINE_OF_SIGHT";
	spell_result_enum[45] = "SPELL_FAILED_LOWLEVEL";
	spell_result_enum[46] = "SPELL_FAILED_LOW_CASTLEVEL";
	spell_result_enum[47] = "SPELL_FAILED_MAINHAND_EMPTY";
	spell_result_enum[48] = "SPELL_FAILED_MOVING";
	spell_result_enum[49] = "SPELL_FAILED_NEED_AMMO";
	spell_result_enum[50] = "SPELL_FAILED_NEED_AMMO_POUCH";
	spell_result_enum[51] = "SPELL_FAILED_NEED_EXOTIC_AMMO";
	spell_result_enum[52] = "SPELL_FAILED_NOPATH";
	spell_result_enum[53] = "SPELL_FAILED_NOT_BEHIND";
	spell_result_enum[54] = "SPELL_FAILED_NOT_FISHABLE";
	spell_result_enum[55] = "SPELL_FAILED_NOT_FLYING";
	spell_result_enum[56] = "SPELL_FAILED_NOT_HERE";
	spell_result_enum[57] = "SPELL_FAILED_NOT_INFRONT";
	spell_result_enum[58] = "SPELL_FAILED_NOT_IN_CONTROL";
	spell_result_enum[59] = "SPELL_FAILED_NOT_KNOWN";
	spell_result_enum[60] = "SPELL_FAILED_NOT_MOUNTED";
	spell_result_enum[61] = "SPELL_FAILED_NOT_ON_TAXI";
	spell_result_enum[62] = "SPELL_FAILED_NOT_ON_TRANSPORT";
	spell_result_enum[63] = "SPELL_FAILED_NOT_READY";
	spell_result_enum[64] = "SPELL_FAILED_NOT_SHAPESHIFT";
	spell_result_enum[65] = "SPELL_FAILED_NOT_STANDING";
	spell_result_enum[66] = "SPELL_FAILED_NOT_TRADEABLE";
	spell_result_enum[67] = "SPELL_FAILED_NOT_TRADING";
	spell_result_enum[68] = "SPELL_FAILED_NOT_UNSHEATHED";
	spell_result_enum[69] = "SPELL_FAILED_NOT_WHILE_GHOST";
	spell_result_enum[70] = "SPELL_FAILED_NO_AMMO";
	spell_result_enum[71] = "SPELL_FAILED_NO_CHARGES_REMAIN";
	spell_result_enum[72] = "SPELL_FAILED_NO_CHAMPION";
	spell_result_enum[73] = "SPELL_FAILED_NO_COMBO_POINTS";
	spell_result_enum[74] = "SPELL_FAILED_NO_DUELING";
	spell_result_enum[75] = "SPELL_FAILED_NO_ENDURANCE";
	spell_result_enum[76] = "SPELL_FAILED_NO_FISH";
	spell_result_enum[77] = "SPELL_FAILED_NO_ITEMS_WHILE_SHAPESHIFTED";
	spell_result_enum[78] = "SPELL_FAILED_NO_MOUNTS_ALLOWED";
	spell_result_enum[79] = "SPELL_FAILED_NO_PET";
	spell_result_enum[80] = "SPELL_FAILED_NO_POWER";
	spell_result_enum[81] = "SPELL_FAILED_NOTHING_TO_DISPEL";
	spell_result_enum[82] = "SPELL_FAILED_NOTHING_TO_STEAL";
	spell_result_enum[83] = "SPELL_FAILED_ONLY_ABOVEWATER";
	spell_result_enum[84] = "SPELL_FAILED_ONLY_DAYTIME";
	spell_result_enum[85] = "SPELL_FAILED_ONLY_INDOORS";
	spell_result_enum[86] = "SPELL_FAILED_ONLY_MOUNTED";
	spell_result_enum[87] = "SPELL_FAILED_ONLY_NIGHTTIME";
	spell_result_enum[88] = "SPELL_FAILED_ONLY_OUTDOORS";
	spell_result_enum[89] = "SPELL_FAILED_ONLY_SHAPESHIFT";
	spell_result_enum[90] = "SPELL_FAILED_ONLY_STEALTHED";
	spell_result_enum[91] = "SPELL_FAILED_ONLY_UNDERWATER";
	spell_result_enum[92] = "SPELL_FAILED_OUT_OF_RANGE";
	spell_result_enum[93] = "SPELL_FAILED_PACIFIED";
	spell_result_enum[94] = "SPELL_FAILED_POSSESSED";
	spell_result_enum[95] = "SPELL_FAILED_REAGENTS";
	spell_result_enum[96] = "SPELL_FAILED_REQUIRES_AREA";
	spell_result_enum[97] = "SPELL_FAILED_REQUIRES_SPELL_FOCUS";
	spell_result_enum[98] = "SPELL_FAILED_ROOTED";
	spell_result_enum[99] = "SPELL_FAILED_SILENCED";
	spell_result_enum[100] = "SPELL_FAILED_SPELL_IN_PROGRESS";
	spell_result_enum[101] = "SPELL_FAILED_SPELL_LEARNED";
	spell_result_enum[102] = "SPELL_FAILED_SPELL_UNAVAILABLE";
	spell_result_enum[103] = "SPELL_FAILED_STUNNED";
	spell_result_enum[104] = "SPELL_FAILED_TARGETS_DEAD";
	spell_result_enum[105] = "SPELL_FAILED_TARGET_AFFECTING_COMBAT";
	spell_result_enum[106] = "SPELL_FAILED_TARGET_AURASTATE";
	spell_result_enum[107] = "SPELL_FAILED_TARGET_DUELING";
	spell_result_enum[108] = "SPELL_FAILED_TARGET_ENEMY";
	spell_result_enum[109] = "SPELL_FAILED_TARGET_ENRAGED";
	spell_result_enum[110] = "SPELL_FAILED_TARGET_FRIENDLY";
	spell_result_enum[111] = "SPELL_FAILED_TARGET_IN_COMBAT";
	spell_result_enum[112] = "SPELL_FAILED_TARGET_IS_PLAYER";
	spell_result_enum[113] = "SPELL_FAILED_TARGET_IS_PLAYER_CONTROLLED";
	spell_result_enum[114] = "SPELL_FAILED_TARGET_NOT_DEAD";
	spell_result_enum[115] = "SPELL_FAILED_TARGET_NOT_IN_PARTY";
	spell_result_enum[116] = "SPELL_FAILED_TARGET_NOT_LOOTED";
	spell_result_enum[117] = "SPELL_FAILED_TARGET_NOT_PLAYER";
	spell_result_enum[118] = "SPELL_FAILED_TARGET_NO_POCKETS";
	spell_result_enum[119] = "SPELL_FAILED_TARGET_NO_WEAPONS";
	spell_result_enum[120] = "SPELL_FAILED_TARGET_UNSKINNABLE";
	spell_result_enum[121] = "SPELL_FAILED_THIRST_SATIATED";
	spell_result_enum[122] = "SPELL_FAILED_TOO_CLOSE";
	spell_result_enum[123] = "SPELL_FAILED_TOO_MANY_OF_ITEM";
	spell_result_enum[124] = "SPELL_FAILED_TOTEM_CATEGORY";
	spell_result_enum[125] = "SPELL_FAILED_TOTEMS";
	spell_result_enum[126] = "SPELL_FAILED_TRAINING_POINTS";
	spell_result_enum[127] = "SPELL_FAILED_TRY_AGAIN";
	spell_result_enum[128] = "SPELL_FAILED_UNIT_NOT_BEHIND";
	spell_result_enum[129] = "SPELL_FAILED_UNIT_NOT_INFRONT";
	spell_result_enum[130] = "SPELL_FAILED_WRONG_PET_FOOD";
	spell_result_enum[131] = "SPELL_FAILED_NOT_WHILE_FATIGUED";
	spell_result_enum[132] = "SPELL_FAILED_TARGET_NOT_IN_INSTANCE";
	spell_result_enum[133] = "SPELL_FAILED_NOT_WHILE_TRADING";
	spell_result_enum[134] = "SPELL_FAILED_TARGET_NOT_IN_RAID";
	spell_result_enum[135] = "SPELL_FAILED_DISENCHANT_WHILE_LOOTING";
	spell_result_enum[136] = "SPELL_FAILED_PROSPECT_WHILE_LOOTING";
	spell_result_enum[137] = "SPELL_FAILED_PROSPECT_NEED_MORE";
	spell_result_enum[138] = "SPELL_FAILED_TARGET_FREEFORALL";
	spell_result_enum[139] = "SPELL_FAILED_NO_EDIBLE_CORPSES";
	spell_result_enum[140] = "SPELL_FAILED_ONLY_BATTLEGROUNDS";
	spell_result_enum[141] = "SPELL_FAILED_TARGET_NOT_GHOST";
	spell_result_enum[142] = "SPELL_FAILED_TOO_MANY_SKILLS";
	spell_result_enum[143] = "SPELL_FAILED_TRANSFORM_UNUSABLE";
	spell_result_enum[144] = "SPELL_FAILED_WRONG_WEATHER";
	spell_result_enum[145] = "SPELL_FAILED_DAMAGE_IMMUNE";
	spell_result_enum[146] = "SPELL_FAILED_PREVENTED_BY_MECHANIC";
	spell_result_enum[147] = "SPELL_FAILED_PLAY_TIME";
	spell_result_enum[148] = "SPELL_FAILED_REPUTATION";
	spell_result_enum[149] = "SPELL_FAILED_MIN_SKILL";
	spell_result_enum[150] = "SPELL_FAILED_NOT_IN_ARENA";
	spell_result_enum[151] = "SPELL_FAILED_NOT_ON_SHAPESHIFT";
	spell_result_enum[152] = "SPELL_FAILED_NOT_ON_STEALTHED";
	spell_result_enum[153] = "SPELL_FAILED_NOT_ON_DAMAGE_IMMUNE";
	spell_result_enum[154] = "SPELL_FAILED_NOT_ON_MOUNTED";
	spell_result_enum[155] = "SPELL_FAILED_TOO_SHALLOW";
	spell_result_enum[156] = "SPELL_FAILED_TARGET_NOT_IN_SANCTUARY";
	spell_result_enum[157] = "SPELL_FAILED_TARGET_IS_TRIVIAL";
	spell_result_enum[158] = "SPELL_FAILED_BM_OR_INVISGOD";
	spell_result_enum[159] = "SPELL_FAILED_EXPERT_RIDING_REQUIREMENT";
	spell_result_enum[160] = "SPELL_FAILED_ARTISAN_RIDING_REQUIREMENT";
	spell_result_enum[161] = "SPELL_FAILED_NOT_IDLE";
	spell_result_enum[162] = "SPELL_FAILED_NOT_INACTIVE";
	spell_result_enum[163] = "SPELL_FAILED_PARTIAL_PLAYTIME";
	spell_result_enum[164] = "SPELL_FAILED_NO_PLAYTIME";
	spell_result_enum[165] = "SPELL_FAILED_NOT_IN_BATTLEGROUND";
	spell_result_enum[166] = "SPELL_FAILED_ONLY_IN_ARENA";
	spell_result_enum[167] = "SPELL_FAILED_TARGET_LOCKED_TO_RAID_INSTANCE";
	spell_result_enum[168] = "SPELL_FAILED_UNKNOWN";
	spell_result_enum[253] = "SPELL_FAILED_PVP_CHECK";
	spell_result_enum[254] = "SPELL_NOT_FOUND";
	spell_result_enum[255] = "SPELL_CAST_OK";

	FlipLuaTable("SpellResult");

	sol::table equip_slot_enum = m_lua.create_table("_EquipSlot");

	equip_slot_enum["Head"] = 0;
	equip_slot_enum["Neck"] = 1;
	equip_slot_enum["Shoulders"] = 2;
	equip_slot_enum["Chest"] = 4;
	equip_slot_enum["Waist"] = 5;
	equip_slot_enum["Legs"] = 6;
	equip_slot_enum["Feet"] = 7;
	equip_slot_enum["Wrists"] = 8;
	equip_slot_enum["Hands"] = 9;
	equip_slot_enum["Finger1"] = 10;
	equip_slot_enum["Finger2"] = 11;
	equip_slot_enum["Trinket1"] = 12;
	equip_slot_enum["Trinket2"] = 13;
	equip_slot_enum["Back"] = 14;
	equip_slot_enum["MainHand"] = 15;
	equip_slot_enum["OffHand"] = 16;
	equip_slot_enum["Ranged"] = 17;

	FlipLuaTable("EquipSlot");
}

void PlayerbotMgr::InitLuaFunctions()
{
	m_lua["SpellExists"] = [](const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		return sSpellTemplate.LookupEntry<SpellEntry>(spellId) != nullptr;
	};
	m_lua["CurrentTime"] = sol::property([&] { return m_master->GetMap()->GetCurrentClockTime().time_since_epoch().count();	});
	m_lua["GetRaidIcon"] = [&](const uint8 iconIndex)-> Unit*
	{
		if (iconIndex < 0 || iconIndex > 7)
			return nullptr;

		if (const auto guid = m_master->GetGroup()->GetTargetFromIcon(iconIndex))
			return m_master->GetMap()->GetUnit(*guid);

		return nullptr;
	};
	m_lua["SpellIsPositive"] = [](const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		return IsPositiveSpell(spellId);
	};
	m_lua.set_function("print",
	                   sol::overload(
		                   [this](const char* msg) { m_masterChatHandler.PSendSysMessage("[AI] %s", msg); }
	                   ));
}

void PlayerbotMgr::InitLuaPlayerType()
{
	sol::usertype<Player> player_type = m_lua.new_usertype<Player>("Player",
	                                                               sol::constructors<Player(WorldSession* session)>(),
	                                                               sol::base_classes,
	                                                               sol::bases<Unit, WorldObject, Object>());

	player_type["LastMessage"] = sol::property([](Player* self)
	{
		const auto ai = self->GetPlayerbotAI();

		if (!ai)
			return "";

		return ai->GetLastMessage().c_str();
	});
	player_type["Class"] = sol::property([](const Player* self)
	{
		return self->GetSpellClass();
	});
	player_type["Inventory"] = sol::property([](const Player* self)
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
	player_type["Trinket1"] = sol::property([](const Player* self)
	{
		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1);
	});
	player_type["Trinket2"] = sol::property([](const Player* self)
	{
		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2);
	});
	player_type["Group"] = sol::property([](Player* self)
	{
		return self->GetGroup();
	});
	player_type["Destination"] = sol::property([](Player* self)
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
	player_type["Specialization"] = sol::property([](const Player* self)
	{
		uint32 row = 0, spec = 0;
		uint32 class_mask = self->getClassMask();

		for (unsigned int i = 0; i < sTalentStore.GetNumRows(); ++i)
		{
			const TalentEntry* talent_info = sTalentStore.LookupEntry(i);

			if (!talent_info)
				continue;

			const TalentTabEntry* talent_tab_info = sTalentTabStore.LookupEntry(talent_info->TalentTab);

			if (!talent_tab_info)
				continue;

			if ((class_mask & talent_tab_info->ClassMask) == 0)
				continue;

			for (int32 k = MAX_TALENT_RANK - 1; k > -1; --k)
			{
				if (talent_info->RankID[k] && self->HasSpell(talent_info->RankID[k]))
				{
					if (row == 0 && spec == 0)
						spec = talent_info->TalentTab;

					if (talent_info->Row > row)
					{
						row = talent_info->Row;
						spec = talent_info->TalentTab;
					}
				}
			}
		}

		//Return the tree with the deepest talent
		return spec;
	});

	player_type["Follow"] = [](Player* self, Unit* target, const float dist, const float angle)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		motion_master->Clear();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		target->GetPosition();

		motion_master->MoveFollow(target, dist, angle);
	};
	player_type["Stand"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);
	};
	player_type["Sit"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (self->getStandState() != UNIT_STAND_STATE_SIT)
			self->SetStandState(UNIT_STAND_STATE_SIT);
	};
	player_type["Kneel"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (self->getStandState() != UNIT_STAND_STATE_KNEEL)
			self->SetStandState(UNIT_STAND_STATE_KNEEL);
	};
	player_type["Interrupt"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->InterruptSpell(CURRENT_GENERIC_SPELL);
		self->InterruptSpell(CURRENT_CHANNELED_SPELL);
	};
	player_type["Move"] = sol::overload([](Player* self, const float x, const float y, const float z)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		motion_master->Clear();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		motion_master->MovePoint(0, x, y, z);
	}, [](Player* self, const Position* pos)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		motion_master->Clear();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		motion_master->MovePoint(0, pos->x, pos->y, pos->z);
	}, [](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		motion_master->Clear();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		float x, y, z;
		target->GetClosePoint(x, y, z, self->GetObjectBoundingRadius());
		motion_master->MovePoint(0, x, y, z);
	});
	player_type["Chase"] = [](Player* self, Unit* target, const float distance, const float angle)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		motion_master->Clear();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		motion_master->MoveChase(target, distance, angle);
	};
	player_type["SetChaseDistance"] = [](Player* self, const float distance)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->GetMotionMaster()->DistanceYourself(distance);
	};
	player_type["TeleportTo"] = [](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		float x, y, z;
		target->GetClosePoint(x, y, z, self->GetObjectBoundingRadius());
		self->TeleportTo(target->GetMapId(), x, y, z, self->GetOrientation());
	};
	player_type["ResetMovement"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		const auto motion_master = self->GetMotionMaster();

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		// not sure which one is better yet, clear seem to be used more frequently...
		// self->GetMotionMaster()->Initialize();
		motion_master->Clear();
	};
	player_type["Stop"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (self->getStandState() != UNIT_STAND_STATE_STAND)
			self->SetStandState(UNIT_STAND_STATE_STAND);

		self->StopMoving();
	};
	player_type["Whisper"] = [&](Player* self, const Player* to, const char* text)
	{
		if (!to || !text || !text[0] || self == to)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendWhisper(text, self, to);
	};
	player_type["TellParty"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_PARTY);
	};
	player_type["TellRaid"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_RAID);
	};
	player_type["Say"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_SAY);
	};
	player_type["Yell"] = [&](Player* self, const char* text)
	{
		if (!text || !text[0])
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		SendChatMessage(text, self, CHAT_MSG_YELL);
	};
	player_type["SetTarget"] = [](Player* self, WorldObject* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetTarget(target);
	};
	player_type["ClearTarget"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetSelectionGuid(ObjectGuid());
	};
	player_type["ClearStealth"] = [](Player* self)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
	};
	player_type["Face"] = sol::overload([](Player* self, const Unit* target)
	{
		if (!target)
			return;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		if (!self->HasInArc(target, M_PI_F / 2))
			self->SetFacingTo(self->GetAngle(target));
	}, [](Player* self, const float orientation)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return;

		self->SetFacingTo(orientation);
	});
	player_type["HasPowerToCast"] = [](Player* self, const uint32 spellId)
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
	player_type["CanCast"] = [](const Player* self, const uint32 spellId)
	{
		if (spellId == 0)
			return false;

		// verify player has spell
		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);

		if (!p_spell_info)
			return false;

		return self->IsSpellReady(*p_spell_info);
	};
	player_type["GetCastTime"] = [](Player* self, const uint32 spellId)
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
	player_type["GetItemInEquipSlot"] = [](const Player* self, const uint8 slot)-> Item*
	{
		if (slot < EQUIPMENT_SLOT_START || slot > EQUIPMENT_SLOT_END)
			return nullptr;

		return self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
	};
	player_type["GetItem"] = sol::overload([](const Player* self, const uint32 itemId)-> Item*
	{
		// list out items in main backpack
		for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
			if (Item* const p_item = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
				if (p_item && p_item->GetProto()->ItemId == itemId)
					return p_item;

		// list out items in other removable backpacks
		for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
			if (const Bag* const p_bag = dynamic_cast<Bag*>(self->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
				for (uint32 slot = 0; slot < p_bag->GetBagSize(); ++slot)
					if (Item* const p_item = self->GetItemByPos(bag, slot))
						if (p_item && p_item->GetProto()->ItemId == itemId)
							return p_item;

		return nullptr;
	}, [](const Player* self, const std::string& name)-> Item*
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
	});
	player_type["Cast"] = sol::overload([&](Player* self, Unit* target, const uint32 spellId)
	{
		if (CurrentCast(self, CURRENT_GENERIC_SPELL) > 0 || CurrentCast(self, CURRENT_CHANNELED_SPELL) > 0)
			return SPELL_FAILED_NOT_READY;

		return Cast(self, target, spellId);
	}, [&](Player* self, const uint32 spellId)
	{
		if (CurrentCast(self, CURRENT_GENERIC_SPELL) > 0 || CurrentCast(self, CURRENT_CHANNELED_SPELL) > 0)
			return SPELL_FAILED_NOT_READY;

		return Cast(self, self, spellId);
	});
	player_type["ForceCast"] = sol::overload([&](Player* self, Unit* target, const uint32 spellId)
	{
		if (!target)
			return SPELL_FAILED_BAD_TARGETS;

		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return SPELL_FAILED_ERROR;

		self->InterruptSpell(CURRENT_GENERIC_SPELL);
		self->InterruptSpell(CURRENT_CHANNELED_SPELL);

		return Cast(self, target, spellId);
	}, [&](Player* self, const uint32 spellId)
	{
		if (const auto ai = self->GetPlayerbotAI(); !ai)
			return SPELL_FAILED_ERROR;

		self->InterruptSpell(CURRENT_GENERIC_SPELL);
		self->InterruptSpell(CURRENT_CHANNELED_SPELL);

		return Cast(self, self, spellId);
	});
};

void PlayerbotMgr::InitLuaUnitType()
{
	sol::usertype<Unit> unit_type = m_lua.new_usertype<Unit>("Unit", sol::base_classes,
	                                                         sol::bases<WorldObject, Object>());

	// doesn't seem to be useful- all NPCs are always moving, always false for IRL player
	//unit_type["IsMoving"] = sol::property([](const Unit* self)
	//{
	//	return !self->IsStopped();
	//});

	unit_type["BoundingRadius"] = sol::property(&Unit::GetObjectBoundingRadius);
	unit_type["Pet"] = sol::property(&Unit::GetPet);
	unit_type["InCombat"] = sol::property(&Unit::IsInCombat);
	unit_type["RaidIcon"] = sol::property([&](const Unit* self)
	{
		return m_master->GetGroup()->GetIconFromTarget(self->GetObjectGuid());
	});
	unit_type["Attackers"] = sol::property([](Unit* self)
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
	unit_type["Target"] = sol::property([](const Unit* self)-> Unit*
	{
		return self->GetTarget();
	});
	unit_type["IsAlive"] = sol::property(&Unit::IsAlive);
	unit_type["CrowdControlled"] = sol::property(&Unit::IsCrowdControlled);
	unit_type["Health"] = sol::property(&Unit::GetHealth);
	unit_type["MaxHealth"] = sol::property(&Unit::GetMaxHealth);
	unit_type["Power"] = sol::property([](const Unit* self)
	{
		const Powers power = self->GetPowerType();
		return self->GetPower(power);
	});
	unit_type["MaxPower"] = sol::property([](const Unit* self)
	{
		const Powers power = self->GetPowerType();
		return self->GetMaxPower(power);
	});
	unit_type["CurrentCast"] = sol::property([&](const Unit* self)
	{
		return CurrentCast(self, CURRENT_GENERIC_SPELL);
	});
	unit_type["CurrentAutoAttack"] = sol::property([&](const Unit* self)
	{
			return CurrentCast(self, CURRENT_AUTOREPEAT_SPELL);
	});
	unit_type["CurrentChannel"] = sol::property([&](const Unit* self)
	{
			return CurrentCast(self, CURRENT_CHANNELED_SPELL);
	});

	unit_type["IsAttackedBy"] = [](const Unit* self, Unit* target)
	{
		if (!target)
			return false;

		return self->IsAttackedBy(target);
	};
	unit_type["GetThreat"] = [](Unit* self, const Unit* target)
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
	unit_type["ReachableWithMelee"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanReachWithMeleeAttack(target);
	};
	unit_type["IsEnemy"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanAttack(target);
	};
	unit_type["IsFriendly"] = [](const Unit* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->CanAssist(target);
	};
	unit_type["GetAura"] = [](const Unit* self, const uint32 auraId)-> SpellAuraHolder*
	{
		if (auraId == 0)
			return nullptr;

		return self->GetSpellAuraHolder(auraId);
	};
}

void PlayerbotMgr::InitLuaCreatureType()
{
	sol::usertype<Creature> creature_type = m_lua.new_usertype<Creature>("Creature", sol::base_classes,
	                                                                     sol::bases<Unit, WorldObject, Object>());

	creature_type["IsElite"] = sol::property(&Creature::IsElite);
	creature_type["IsWorldBoss"] = sol::property(&Creature::IsWorldBoss);
	creature_type["CanAggro"] = sol::property(&Creature::CanAggro);
	creature_type["IsTotem"] = sol::property(&Creature::IsTotem);
	creature_type["IsInvisible"] = sol::property(&Creature::IsInvisible);
	creature_type["IsCivilian"] = sol::property(&Creature::IsCivilian);
	creature_type["IsCritter"] = sol::property(&Creature::IsCritter);
	creature_type["IsRegenHp"] = sol::property(&Creature::IsRegeneratingHealth);
	creature_type["IsRegenPower"] = sol::property(&Creature::IsRegeneratingHealth);
	creature_type["CanWalk"] = sol::property(&Creature::CanWalk);
	creature_type["CanSwim"] = sol::property(&Creature::CanSwim);
	creature_type["CanFly"] = sol::property(&Creature::CanFly);
}

void PlayerbotMgr::InitLuaObjectType()
{
	sol::usertype<Object> object_type = m_lua.new_usertype<Object>(
		"Object");

	object_type["IsPlayer"] = sol::property(&Object::IsPlayer);
	object_type["IsCreature"] = sol::property(&Object::IsCreature);
	object_type["IsGameObject"] = sol::property(&Object::IsGameObject);
	object_type["IsUnit"] = sol::property(&Object::IsUnit);

	object_type["AsPlayer"] = [](Object* self)-> Player*
	{
		if (!self->IsPlayer())
			return nullptr;

		return dynamic_cast<Player*>(self);
	};
	object_type["AsCreature"] = [](Object* self)-> Creature*
	{
		if (!self->IsCreature())
			return nullptr;

		return dynamic_cast<Creature*>(self);
	};
	object_type["AsGameObject"] = [](Object* self)-> GameObject*
	{
		if (!self->IsGameObject())
			return nullptr;

		return dynamic_cast<GameObject*>(self);
	};
	object_type["AsUnit"] = [](Object* self)-> Unit*
	{
		if (!self->IsUnit())
			return nullptr;

		return dynamic_cast<Unit*>(self);
	};
}

void PlayerbotMgr::InitLuaWorldObjectType()
{
	sol::usertype<WorldObject> world_object_type = m_lua.new_usertype<WorldObject>(
		"WorldObject", sol::base_classes, sol::bases<Object>());

	world_object_type["Orientation"] = sol::property(&WorldObject::GetOrientation);
	world_object_type["Name"] = sol::property(&WorldObject::GetName);
	world_object_type["Map"] = sol::property(&WorldObject::GetMap);
	world_object_type["ZoneId"] = sol::property(&WorldObject::GetZoneId);
	world_object_type["AreaId"] = sol::property(&WorldObject::GetAreaId);
	world_object_type["Position"] = sol::property([](const WorldObject* self)
	{
		return self->GetPosition();
	});

	world_object_type["GetAngle"] = sol::overload([](const WorldObject* self, const WorldObject* obj)
	{
		return self->GetAngle(obj);
	}, [](const WorldObject* self, const Position* pos)
	{
		return self->GetAngle(pos->x, pos->y);
	});
	world_object_type["GetClosePoint"] = [](const WorldObject* self, const float boundingRadius, const float distance,
		   const float angle)
		{
			float x, y, z;
			self->GetClosePoint(x, y, z, boundingRadius, distance, angle, self);
			return sol::tie(x, y, z);
		};
	world_object_type["HasInArc"] = [](const WorldObject* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->HasInArc(target, M_PI_F / 2);
	};
	world_object_type["IsWithinLos"] = [](const WorldObject* self, const Unit* target)
	{
		if (!target)
			return false;

		return self->IsWithinLOSInMap(target);
	};
	world_object_type["IsInRange"] = [](const WorldObject* self, const Unit* target, const uint32 spellId)
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
	world_object_type["GCD"] = [&](const WorldObject* self, const uint32 spellId)->long
	{		
		const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
		if (!p_spell_info)
		{
			return -1;
		}

		const auto gcd_time = self->GetGCD(p_spell_info).time_since_epoch().count();
		const auto current = m_master->GetMap()->GetCurrentClockTime().time_since_epoch().count();

		return current - gcd_time;
	};
}

void PlayerbotMgr::InitLuaGroupType()
{
	sol::usertype<Group> group_type = m_lua.new_usertype<Group>("Group");

	group_type["IsRaid"] = sol::property(&Group::IsRaidGroup);
	group_type["Leader"] = sol::property([&](const Group* self)
	{
		const ObjectGuid guid = self->GetLeaderGuid();
		return m_master->GetMap()->GetPlayer(guid);
	});
	group_type["Members"] = sol::property([&](const Group* self)
	{
		const Group::MemberSlotList& slots = self->GetMemberSlots();

		std::vector<Player*> members;
		members.reserve(slots.size());

		Map* map = m_master->GetMap();

		for (auto [guid, name, group, assistant, lastMap] : slots)
			members.push_back(map->GetPlayer(guid));

		return members;
	});
}

void PlayerbotMgr::InitLuaMapType()
{
	sol::usertype<Map> map_type = m_lua.new_usertype<Map>("Map");

	map_type["Players"] = sol::property(&Map::GetPlayers);
	map_type["MapName"] = sol::property(&Map::GetMapName);
	map_type["IsHeroic"] = sol::property([&](const Map* self)
	{
		return self->GetDifficulty() == DUNGEON_DIFFICULTY_HEROIC;
	});
}

void PlayerbotMgr::InitLuaGameObjectType()
{
	sol::usertype<GameObject> game_object_type = m_lua.new_usertype<GameObject>(
		"GameObject", sol::base_classes, sol::bases<WorldObject, Object>());

	game_object_type["InUse"] = sol::property(&GameObject::IsInUse);
	game_object_type["Owner"] = sol::property(&GameObject::GetOwner);
	game_object_type["Level"] = sol::property(&GameObject::GetLevel);
}

void PlayerbotMgr::InitLuaPositionType()
{
	sol::usertype<Position> position_type = m_lua.new_usertype<Position>(
		"Position");

	position_type["X"] = sol::property(&Position::x);
	position_type["Y"] = sol::property(&Position::y);
	position_type["Z"] = sol::property(&Position::z);
	position_type["O"] = sol::property(&Position::o);
	position_type["Empty"] = sol::property(&Position::IsEmpty);

	position_type["GetDistanceBetween"] = [](const Position* self, const Position* other)
	{
		return self->GetDistance(*other);
	};
	position_type["GetAngle"] = sol::overload([](const Position* self, const Position* pos)
	{
		return self->GetAngle(pos->x, pos->y);
	}, [](const Position* self, const float x, const float y)
	{
		return self->GetAngle(x, y);
	});
}

void PlayerbotMgr::InitLuaPetType()
{
	sol::usertype<Pet> pet_type = m_lua.new_usertype<Pet>(
		"Pet", sol::base_classes, sol::bases<Creature, Unit, WorldObject, Object>());

	pet_type["PetOwner"] = sol::property(&Pet::GetSpellModOwner);
	pet_type["Happiness"] = sol::property(&Pet::GetHappinessState);
	pet_type["IsFeeding"] = sol::property([](const Pet* self)
	{
		return self->HasAura(1738 /*PET_FEED*/, EFFECT_INDEX_0);
	});
	pet_type["ReactState"] = sol::property([](Pet* self) { return self->AI()->GetReactState(); },
	                                        [](Pet* self, const int state)
	                                        {
		                                        const auto player = self->GetSpellModOwner();

		                                        if (!player)
			                                        return;

		                                        if (player->GetPlayerbotAI())
			                                        return;

		                                        self->AI()->SetReactState(static_cast<ReactStates>(state));
	                                        });
	
	pet_type["SetAutocast"] = [](Pet* self, const uint32 spellId, const bool enable)
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
	pet_type["Summon"] = [](Pet* self)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return;

		if (player->GetPlayerbotAI())
			return;

		self->AddToWorld();
	};
	pet_type["Dismiss"] = [](Pet* self)
	{
		const auto player = self->GetSpellModOwner();

		if (!player)
			return;

		if (player->GetPlayerbotAI())
			return;

		self->RemoveFromWorld();
	};
	pet_type["AttemptFeed"] = [](const Pet* self)
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
	sol::usertype<SpellAuraHolder> aura_type = m_lua.new_usertype<SpellAuraHolder>("Aura");

	aura_type["Stacks"] = sol::property(&SpellAuraHolder::GetStackAmount);
	aura_type["Duration"] = sol::property(&SpellAuraHolder::GetAuraDuration);
	aura_type["MaxDuration"] = sol::property(&SpellAuraHolder::GetAuraMaxDuration);
	aura_type["Charges"] = sol::property(&SpellAuraHolder::GetAuraCharges);
}

void PlayerbotMgr::InitLuaItemType()
{
	sol::usertype<Item> item_type = m_lua.new_usertype<Item>("Item", sol::base_classes, sol::bases<Object>());

	item_type["StackCount"] = sol::property(&Item::GetCount);
	item_type["IsPotion"] = sol::property(&Item::IsPotion);
	item_type["MaxStackCount"] = sol::property(&Item::GetMaxStackCount);
	item_type["Id"] = sol::property([](const Item* self)
	{
		return self->GetProto()->ItemId;
	});
	item_type["Name"] = sol::property([](const Item* self)
	{
		return self->GetProto()->Name1;
	});
	item_type["SpellId"] = sol::property([](const Item* self)
	{
		for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
			if (self->GetProto()->Spells[i].SpellId > 0)
				return self->GetProto()->Spells[i].SpellId;
		return static_cast<uint32>(0);
	});
	item_type["Ready"] = sol::property([](const Item* self)
	{
		for (const auto& spell : self->GetProto()->Spells)
		{
			if (spell.SpellId > 0)
			{
				const auto spell_id = spell.SpellId;

				const auto p_spell_info = sSpellTemplate.LookupEntry<SpellEntry>(spell_id);

				if (!p_spell_info)
					return false;

				return self->GetOwner()->IsSpellReady(*p_spell_info, self->GetProto());
			}
		}

		return false;
	});

	item_type["Use"] = sol::overload([&](Item* self)
	{
		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_UNIT, self->GetOwnerGuid());
	}, [&](Item* self, const uint8 slot)
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
	}, [&](Item* self, const Item* target)
	{
		if (!target)
			return;

		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_ITEM, target->GetObjectGuid());
	}, [&](Item* self, GameObject* obj)
	{
		if (!obj)
			return;

		Player* owner = self->GetOwner();

		if (const auto ai = owner->GetPlayerbotAI(); !ai)
			return;

		UseItem(owner, self, TARGET_FLAG_GAMEOBJECT, obj->GetObjectGuid());
	});
}

Unit* PlayerbotMgr::GetRaidIcon(const uint8 iconIndex) const
{
	if (iconIndex < 0 || iconIndex > 7)
		return nullptr;

	const auto group = m_master->GetGroup();

	if (!group)
		return nullptr;

	if (const auto guid = group->GetTargetFromIcon(iconIndex); guid)
		return m_master->GetMap()->GetUnit(*guid);

	return nullptr;
};

void PlayerbotMgr::FlipLuaTable(const std::string& name)
{
	// add to table by making each value a key with the key as the value
	const std::string script = "for k, v in pairs(" + name + ") do " + name + "[v] = k end";
	m_lua.script(script);
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
	SendWhisper(text, fromPlayer, m_master);
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

			Item* p_item = m_master->GetItemByPos(bag_index, slot);
			if (!p_item)
				return;

			if (p_item->GetObjectGuid() != item_guid)
				return;

			if (p_item->GetProto()->ItemId == 666666)
			{
				SpellCastTargets targets;

				p >> targets.ReadForCaster(m_master);

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
                m_masterChatHandler.PSendSysMessage("%s has already signed the petition", player->GetName());
                delete result;
                return;
            }

            CharacterDatabase.PExecute("INSERT INTO petition_sign (ownerguid,petitionguid, playerguid, player_account) VALUES ('%u', '%u', '%u','%u')",
                                       m_master->GetGUIDLow(), petitionLowGuid, player->GetGUIDLow(), m_master->GetSession()->GetAccountId());

            p.Initialize(SMSG_PETITION_SIGN_RESULTS, (8 + 8 + 4));
            p << ObjectGuid(petitionGuid);
            p << ObjectGuid(playerGuid);
            p << uint32(PETITION_SIGN_OK);

            // close at signer side
            m_master->GetSession()->SendPacket(p);

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

                if (bot->IsWithinDistInMap(m_master, 50))
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
			if (guid != m_master->GetObjectGuid())
				return;

			for (auto it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
			{
				Player* const bot = it->second;
				if (m_master->IsMounted() && !bot->IsMounted())
				{
					// Player Part
					if (!m_master->GetAurasByType(SPELL_AURA_MOUNTED).empty())
					{
						int32 master_speed1 = 0;
						int32 master_speed2 = 0;
						master_speed1 = m_master->GetAurasByType(SPELL_AURA_MOUNTED).front()->GetSpellProto()->
						                             EffectBasePoints[1];
						master_speed2 = m_master->GetAurasByType(SPELL_AURA_MOUNTED).front()->GetSpellProto()->
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
				else if (!m_master->IsMounted() && bot->IsMounted())
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

bool PlayerbotMgr::DownloadSaveAndLoadAIScript(const std::string& name, const std::string& url)
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

	// 

	if (SafeLoadLuaScript(name, script))
	{
		if (CharacterDatabase.PExecute(
			"INSERT INTO scripts (accountid, name, script, url) VALUES ('%u', '%s', '%s', '%s') ON DUPLICATE KEY UPDATE script = '%s', url = '%s'",
			 m_master->GetSession()->GetAccountId(), name.c_str(), script.c_str(), url.c_str(), script.c_str(), url.c_str()))
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
	const auto account_id = m_master->GetSession()->GetAccountId();

	if (const QueryResult* count_result = CharacterDatabase.PQuery(
		"SELECT COUNT(*) FROM scripts WHERE name = '%s' AND accountid = %u", name.c_str(), account_id))
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
        PSendSysMessage("|cffff0000You may only manage bots from an active session");
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
    if (cmd_string.find("ai") != std::string::npos)
    {
		const auto account_id = m_session->GetAccountId();

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
	    if (rem_cmd.empty() || rem_cmd.find("help") != std::string::npos)
	    {
		    PSendSysMessage(
			    R"(usage: 
load <NAME>: load ai script from memory
set <NAME> <URL>: download script from url, store as name and load
remove <NAME>: remove script from database
reload <NAME>: re-download script from same url)");
		    return true;
	    }

		if (rem_cmd.find("reload") != std::string::npos)
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
					"SELECT url FROM scripts WHERE name = '%s' AND accountid = %u", name.c_str(), account_id))
				{
					const Field* load_fields = load_result->Fetch();
					return mgr->DownloadSaveAndLoadAIScript(name, load_fields[0].GetCppString());
				}
			}

			return false;
		}

	    if (rem_cmd.find("load") != std::string::npos)
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
				    "SELECT script FROM scripts WHERE name = '%s' AND accountid = %u", name.c_str(), account_id))
			    {
				    const Field* load_fields = load_result->Fetch();

				    if (const char* script = load_fields[0].GetString(); mgr->SafeLoadLuaScript(name, script))
					    PSendSysMessage("Script '%s' read successfully.", name.c_str());
			    }
		    }

		    return false;
	    }


	    if (rem_cmd.find("remove") != std::string::npos)
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

        if (rem_cmd.find("write") != std::string::npos)
        {
            std::string message = rem_cmd.substr(5);
            boost::algorithm::trim(message);

            mgr->SetLuaMasterMessage(message);

            return false;
        }

	    if (rem_cmd.find("set") != std::string::npos)
	    {
		    std::string rem_set_cmd = rem_cmd.substr(3);
		    boost::algorithm::trim(rem_set_cmd);

		    auto first_space = rem_set_cmd.find(' ');

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

		    return mgr->DownloadSaveAndLoadAIScript(name, url);
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

    /*uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid);
    if (accountId != m_session->GetAccountId())
    {
        PSendSysMessage("|cffff0000You may only add bots from the same account.");
        SetSentErrorMessage(true);
        return false;
    }*/

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
        int maxnum = botConfig.GetIntDefault("PlayerbotAI.MaxNumBots", 40);
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
        mgr->LoginPlayerBot(guid, m_session->GetAccountId());
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

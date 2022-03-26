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

#ifndef _PLAYERBOTMGR_H
#define _PLAYERBOTMGR_H

#include "Common.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "World/World.h"

class WorldPacket;
class Player;
class Unit;
class Object;
class Item;
class PlayerbotClassAI;

using PlayerBotMap = std::unordered_map<ObjectGuid, Player*>;

class PlayerbotMovementPolicy
{
public:
	void GetDestination(float& x, float& y, float& z) const
	{
		x = m_x;
		y = m_y;
		z = m_z;
	}

	void MoveTo(const float x, const float y, const float z)
	{
		m_x = x;
		m_y = y;
		m_z = z;

		m_followTarget = nullptr;
		m_followDist = 0.0f;
		m_followAngle = 0.0f;

		m_chaseTarget = nullptr;
		m_chaseAngle = 0.0f;
		m_chaseDist = 0.0f;

		m_lastPolicyUpdate = sWorld.GetCurrentMSTime();
	}

	Unit* GetFollowTarget() const { return m_followTarget; }
	float GetFollowDistance() const { return m_followAngle; }
	float GetFollowAngle() const { return m_followDist; }

	void Follow(Unit* target, const float dist, const float angle)
	{
		m_followTarget = target;
		m_followDist = dist;
		m_followAngle = angle;

		m_chaseTarget = nullptr;
		m_chaseAngle = 0.0f;
		m_chaseDist = 0.0f;

		m_x = 0;
		m_y = 0;
		m_z = 0;

		m_lastPolicyUpdate = sWorld.GetCurrentMSTime();
	}

	Unit* GetChaseTarget() const { return m_chaseTarget; }
	float GetChaseDistance() const { return m_chaseAngle; }
	float GetChaseAngle() const { return m_chaseDist; }

	void Chase(Unit* target, const float dist, const float angle)
	{
		m_chaseTarget = target;
		m_chaseAngle = dist;
		m_chaseDist = angle;

		m_followTarget = nullptr;
		m_followDist = 0.0f;
		m_followAngle = 0.0f;

		m_x = 0;
		m_y = 0;
		m_z = 0;

		m_lastPolicyUpdate = sWorld.GetCurrentMSTime();
	}

	bool CanUpdatePolicy() const { return sWorld.GetCurrentMSTime() - m_lastPolicyUpdate > m_minPolicyChangeWaitTime; }

private:
	float m_x = 0.0f;
	float m_y = 0.0f;
	float m_z = 0.0f;
	Unit* m_followTarget = nullptr;
	float m_followAngle = 0.0f;
	float m_followDist = 0.0f;
	float m_lastPolicyUpdate = 0.0f;
	Unit* m_chaseTarget = nullptr;
	float m_chaseAngle = 0.0f;
	float m_chaseDist = 0.0f;
	inline static float m_minPolicyChangeWaitTime = 500;
};

using PlayerbotMovementPolicyMap = std::unordered_map<ObjectGuid, PlayerbotMovementPolicy*>;

class PlayerbotMgr
{
	// static functions, available without a PlayerbotMgr instance
public:
	static void SetInitialWorldSettings();

public:
	PlayerbotMgr(Player* master);
	virtual ~PlayerbotMgr();

	void InitLua();
	void InitMqtt();
	void InitLuaEnvironment();
	void ClearNonStandardModules();
	bool LoadUserLuaScript();

	void InitLuaMembers();
	void InitLuaFunctions();

	void InitLuaPlayerType();
	void InitMovementPolicy();
	void InitLuaUnitType();
	void InitLuaCreatureType();
	void InitLuaObjectType();
	void InitLuaWorldObjectType();
	void InitLuaGameObjectType();
	void InitLuaPositionType();
	void InitLuaPetType();
	void InitLuaAuraType();
	void InitLuaItemType();

	Unit* GetRaidIcon(uint8 iconIndex) const;
	void FlipLuaTable(const std::string& name);
	void SendMsg(const std::string& msg, bool isError = false, bool sendLog = true, bool sendSysMessage = true);
	SpellCastResult Cast(Player* bot, Unit* target, uint32 spellId, bool checkIsAlive = true,
	                     TriggerCastFlags flags = TRIGGERED_NONE) const;
	static uint32 CurrentCast(const Unit* unit, CurrentSpellTypes type);
	SpellCastResult UseItem(Player* bot, Item* item, uint32 targetFlag, ObjectGuid targetGuid) const;
	void MoveToPoint2d(Player* bot, float destX, float destY);
	void MoveTo(Player* bot, float destX, float destY, float destZ);
	void Follow(Player* bot, Unit* target, float dist, float angle);
	void Chase(Player* bot, Unit* target, float dist, float angle);
	void TellMaster(const std::string& text, const Player* fromPlayer) const;
	void SendWhisper(const std::string& text, const Player* fromPlayer, const Player* toPlayer) const;
	void SendChatMessage(const std::string& text, const Player* fromPlayer, uint32 opCode) const;

	// remove marked bot
	// should be called from worldsession::Update only to avoid possible problem with invalid session or player pointer
	void RemoveBots();

	// This is called from Unit.cpp and is called every ~50ms or so depending on main thread performance
	void UpdateAI(uint32 time);

	// This is called whenever the master sends a packet to the server.
	// These packets can be viewed, but not edited.
	// It allows bot creators to craft AI in response to a master's actions.
	// For a list of opcodes that can be caught see Opcodes.cpp (CMSG_* opcodes only)
	// Notice: that this is static which means it is called once for all bots of the master.
	void HandleMasterIncomingPacket(const WorldPacket& packet);
	void HandleMasterOutgoingPacket(const WorldPacket& packet) const;

	void LoginPlayerBot(ObjectGuid playerGuid, uint32 masterAccountId);
	void LogoutPlayerBot(ObjectGuid guid); // mark bot to be removed on next update
	Player* GetPlayerBot(ObjectGuid guid) const;
	Player* GetMaster() const { return m_master; }

	PlayerBotMap::const_iterator GetPlayerBotsBegin() const { return m_playerBots.begin(); }
	PlayerBotMap::const_iterator GetPlayerBotsEnd() const { return m_playerBots.end(); }

	void LogoutAllBots(bool fullRemove = false); // mark all bots to be removed on next update
	void RemoveAllBotsFromGroup();
	bool VerifyScriptExists(const std::string& name, uint32 accountId);
	std::string GenerateToken() const;
	void OnBotLogin(Player* bot);
	void Stay();
	void SetLuaMasterMessage(const std::string& message) { m_lastManagerMessage = message; }
	void UseLuaAI(bool useLua);
	bool IsUsingLuaAI() const { return m_usingLuaAI; }

public:
	// config variables
	uint32 m_confRestrictBotLevel;
	uint32 m_confDisableBotsInRealm;
	uint32 m_confMaxNumBots;
	bool m_confDisableBots;
	bool m_confDebugWhisper;
	float m_confFollowDistance[2];
	uint32 gConfigSellLevelDiff;
	bool m_confCollectCombat;
	bool m_confCollectQuest;
	bool m_confCollectProfession;
	bool m_confCollectLoot;
	bool m_confCollectSkin;
	bool m_confCollectObjects;
	uint32 m_confCollectDistance;
	uint32 m_confCollectDistanceMax;

private:
	Player* const m_master;
	PlayerBotMap m_playerBots;
	GuidSet m_botToRemove;

	// lua VM for the bot
	sol::state m_lua;
	sol::environment m_luaEnvironment;
	std::string m_lastActErrorMsg;
	std::string m_lastManagerMessage;
	bool m_hasLoadedScript;
	Position m_lastCommandPosition;
	ChatHandler m_masterChatHandler;
	uint32 m_masterAccountId;
	bool m_usingLuaAI;
	PlayerbotMovementPolicyMap m_movementPolicies;
};

#endif

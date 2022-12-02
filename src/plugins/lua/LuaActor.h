/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-2022 MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include "LuaCommon.h"

namespace mq::lua {
struct LuaResponse
{
	bool m_received = false;
	sol::object m_value = sol::lua_nil;
	sol::state_view m_targetState;
};

class LuaMailbox
{
public:
	LuaMailbox(std::string_view name);
	~LuaMailbox();

	int Receive(std::string_view topic, sol::object payload);
	void AddResponse(int id, const std::shared_ptr<LuaResponse>& response);
	static sol::table Register(sol::this_state s);
	static void Process();

private:
	std::string_view m_name;
	sol::table m_mailbox;
	std::unordered_map<int, std::weak_ptr<LuaResponse>> m_responses;
};

class LuaActor
{
public:
	LuaActor(std::string_view name, std::weak_ptr<LuaMailbox> target) : m_name(name), m_target(target) {}
	void Tell(std::string_view topic, sol::object payload, sol::this_state s);
	std::shared_ptr<LuaResponse> Ask(std::string_view topic, sol::object payload, sol::this_state s);

private:
	std::string m_name;
	std::weak_ptr<LuaMailbox> m_target;
};

// create a class for userdata to pass as a return from mq.actors
class LuaActors
{
public:
	static void RegisterLua(sol::state_view sv);
};
} // namespace mq::lua

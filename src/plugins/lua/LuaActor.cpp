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

#include "pch.h"

#include "LuaActor.h"
#include "LuaThread.h"

#include "common/StringUtils.h"

namespace mq::lua {

static ci_unordered::map<std::string_view, std::shared_ptr<LuaMailbox>> s_mailboxes;

bool Exists(std::string_view name)
{
	return s_mailboxes.find(name) != s_mailboxes.end();
}

std::unique_ptr<LuaActor> Get(std::string_view name)
{
	auto it = s_mailboxes.find(name);
	if (it != s_mailboxes.end())
		return std::make_unique<LuaActor>(name, it->second);

	return {};
}

int LuaMailbox::Receive(std::string_view topic, sol::object payload)
{
	return m_mailbox["receive"](m_mailbox, topic, payload);
}

void LuaMailbox::AddResponse(int id, const std::shared_ptr<LuaResponse>& response)
{
	m_responses[id] = response;
}

sol::table LuaMailbox::Register(sol::this_state s)
{
	auto ptr = LuaThread::get_from(s);
	if (ptr && s_mailboxes.find(ptr->GetName()) == s_mailboxes.end())
	{
		auto mailbox_ptr = std::make_shared<LuaMailbox>(ptr->GetName(), ptr);

		sol::state_view sv(s);

		sol::function receive = sv.script(R"(
			return function(self, topic, payload)
				-- 1 trillion messages before wrap seems quite safe
				if self.__current_id == 1000000000000 then
					self.__current_id = 1
				else
					self.__current_id = self.__current_id + 1
				end
				-- insert at the front
				table.insert(self.__messages, 1, { ['id'] = self.__current_id, ['topic'] = topic, ['payload'] = payload })
				return self.__current_id
			end
		)");

		sol::function process = sv.script(R"(
			return function(self)
				local message = table.remove(self.__messages)
				if self.__callbacks[message.topic] then
					return message.id, self.__callbacks[message.topic](message.payload)
				end
				return message.id, nil
			end
		)");

		auto mailbox_table = sv.create_table_with(
			"__current_id", 0,
			"__messages", sv.create_table(),
			"__callbacks", sv.create_table(),
			"receive", receive,
			"process", process,
			"messages_per_frame", 10 // can either configure this or just let it be the default here. The user can always change it in the script.
		);

		mailbox_ptr->m_mailbox = mailbox_table;
		return mailbox_table;
	}

	return sol::lua_nil;
}

void LuaMailbox::Process()
{
	for (const auto& [_, mailbox] : s_mailboxes)
	{
		for (int m_num = 0; m_num < mailbox->m_mailbox.get<int>("messages_per_frame") && mailbox->m_mailbox.size() > 0; ++m_num)
		{
			int id;
			sol::object val;
			sol::tie(id, val) = mailbox->m_mailbox["process"](mailbox->m_mailbox);
			auto it = mailbox->m_responses.find(id);
			if (it != mailbox->m_responses.end())
			{
				auto response = it->second.lock();
				if (response)
				{
					response->m_received = true;
					response->m_value = val;
				}
			}
		}
	}
}

void LuaActor::Tell(std::string_view topic, sol::object payload)
{
	auto mailbox = m_target.lock();
	if (mailbox)
	{
		mailbox->Receive(topic, payload);
	}
}

std::shared_ptr<LuaResponse> LuaActor::Ask(std::string_view topic, sol::object payload)
{
	auto mailbox = m_target.lock();
	if (mailbox)
	{
		auto ptr = std::make_shared<LuaResponse>();
		mailbox->AddResponse(mailbox->Receive(topic, payload), ptr);
		return ptr;
	}

	// no mailbox, response will be nil
	return std::make_shared<LuaResponse>(LuaResponse{ true, sol::lua_nil });
}

LuaMailbox::LuaMailbox(std::string_view name, std::shared_ptr<LuaThread> thread)
	: m_name(name)
	, m_thread(thread)
{}

LuaMailbox::~LuaMailbox()
{
	s_mailboxes.erase(m_name);
}

sol::object StatelessIterator(sol::object, sol::object k, sol::this_state s)
{
	if (s_mailboxes.begin() == s_mailboxes.end())
		return sol::lua_nil;

	if (k == sol::lua_nil)
		return sol::make_object(s, s_mailboxes.begin()->first);

	if (k.is<std::string_view>())
	{
		auto it = std::next(s_mailboxes.find(k.as<std::string_view>()));
		if (it != s_mailboxes.end())
			return sol::make_object(s, it->first);
	}

	return sol::lua_nil;
}

sol::object Iterator(sol::this_state s)
{
	return sol::make_object(s, std::make_tuple(StatelessIterator, sol::lua_nil, sol::lua_nil));
}

void LuaActors::RegisterLua(sol::state_view sv)
{
	sv.new_usertype<LuaResponse>(
		"response" , sol::no_constructor,
		"received" , sol::readonly(&LuaResponse::m_received),
		"value"    , sol::readonly(&LuaResponse::m_value));

	sv.new_usertype<LuaActor>(
		"actor", sol::no_constructor,
		"tell" , &LuaActor::Tell,
		"ask"  , &LuaActor::Ask);

	sv.new_usertype<LuaActors>(
		"actors"                 , sol::no_constructor,
		"exists"                 , &Exists,
		"get"                    , &Get,
		"register"               , &LuaMailbox::Register,
		sol::meta_function::call , &Iterator);
}

} // namespace mq::lua

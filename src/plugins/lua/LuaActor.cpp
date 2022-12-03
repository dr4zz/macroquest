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

static ci_unordered::map<std::string_view, std::weak_ptr<LuaMailbox>> s_mailboxes;

sol::object LuaResponse::GetValue(sol::this_state s)
{
	auto ptr = LuaThread::get_from(s);
	if (ptr)
		return ptr->CopyObject(m_value);

	return sol::lua_nil;
}

void SetResponse(std::weak_ptr<LuaResponse> response, sol::object value)
{
	auto ptr = response.lock();
	if (ptr)
	{
		ptr->SetReceived(true);
		ptr->SetValue(value);
	}
}

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

int LuaMailbox::Send(sol::object header, sol::object payload)
{
	auto ptr = LuaThread::get_from(m_mailbox.lua_state());
	if (ptr)
	{
		sol::function func = sol::function(ptr->GetState(), sol::function(m_mailbox["__queue"]));
		return func(m_mailbox, ptr->CopyObject(header), ptr->CopyObject(payload));
	}

	// no lua state, it doesn't make sense to call the queue function
	return -1;
}

void LuaMailbox::AddResponse(int id, const std::shared_ptr<LuaResponse>& response)
{
	//m_responses[id] = response;
	m_mailbox["__responses"][id] = std::weak_ptr<LuaResponse>(response);
}

sol::table LuaMailbox::Register(sol::this_state s)
{
	auto ptr = LuaThread::get_from(s);
	if (ptr && s_mailboxes.find(ptr->GetName()) == s_mailboxes.end())
	{
		auto mailbox_ptr = std::make_shared<LuaMailbox>(ptr->GetName());

		sol::state_view sv(s);

		sol::function queue = sv.script(R"(
			return function(self, header, payload)
				-- 1 trillion messages before wrap seems quite safe
				if self.__current_id == 1000000000000 then
					self.__current_id = 1
				else
					self.__current_id = self.__current_id + 1
				end
				-- insert at the front
				table.insert(self.__messages, 1, { ['id'] = self.__current_id, ['header'] = header, ['payload'] = payload })
				return self.__current_id
			end
		)");

		sol::function receive = sv.script(R"(
			return function(self)
				-- just pop off the back of the messages queue (it's inserted from the front)
				return table.remove(tbl.__messages)
			end
		)");

		sol::function messages = sv.script(R"(
			return function(self)
				return self.receive, self
			end
		)");

		sol::function respond = sv.script(R"(
			return function(self, message, value)
				local response = self.__responses[message.id]
				if response then
					self.__set_response(response, value)
					self.__responses[message.id] = nil
				end
			end
		)");

		auto mailbox_table = sv.create_table_with(
			"__mailbox", mailbox_ptr,
			"__current_id", 0,
			"__messages", sv.create_table(),
			"__responses", sv.create_table(),
			"__queue", queue,
			"receive", receive,
			"messages", messages,
			"respond", respond,
			"__set_response", &SetResponse
		);

		mailbox_ptr->m_mailbox = mailbox_table;
		s_mailboxes[mailbox_ptr->m_name] = mailbox_ptr;

		return mailbox_table;
	}

	return sol::lua_nil;
}

void LuaActor::Tell(sol::object header, sol::object payload, sol::this_state s)
{
	auto mailbox = m_target.lock();
	if (mailbox)
	{
		mailbox->Send(header, payload);
	}
}

std::shared_ptr<LuaResponse> LuaActor::Ask(sol::object header, sol::object payload, sol::this_state s)
{
	auto mailbox = m_target.lock();
	if (mailbox)
	{
		auto ptr = std::make_shared<LuaResponse>(LuaResponse{ false, sol::lua_nil, s });
		mailbox->AddResponse(mailbox->Send(header, payload), ptr);
		return ptr;
	}

	// no mailbox, response will be nil
	return std::make_shared<LuaResponse>(LuaResponse{ true, sol::lua_nil, s });
}

LuaMailbox::LuaMailbox(std::string_view name)
	: m_name(name)
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
		auto it = s_mailboxes.find(k.as<std::string_view>());
		if (it != s_mailboxes.end()) it = std::next(it);
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
		"received" , sol::property(&LuaResponse::GetReceived),
		"value"    , sol::property(&LuaResponse::GetValue));

	sv.new_usertype<LuaActor>(
		"actor", sol::no_constructor,
		"tell" , &LuaActor::Tell,
		"ask"  , &LuaActor::Ask);

	sv.new_usertype<LuaMailbox>(
		"mailbox", sol::no_constructor);

	sv.new_usertype<LuaActors>(
		"actors"                 , sol::no_constructor,
		"exists"                 , &Exists,
		"get"                    , &Get,
		"register"               , &LuaMailbox::Register,
		sol::meta_function::call , &Iterator);
}

} // namespace mq::lua

#pragma once
#include "../common/types.h"

bool CmdHookInstall(Client& client, std::uint64_t va, std::uint64_t cookie);
bool CmdHookRemove(Client& client, std::uint32_t id);
bool CmdHookList(Client& client);
bool CmdHookProbe(Client& client, std::uint64_t value);
void CmdHookWatch(Client& client, std::uint32_t id);

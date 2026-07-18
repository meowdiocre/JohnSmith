#pragma once
#include "../common/types.h"

bool CmdMemRead(Client& client, std::uint64_t va, std::uint64_t size, const char* outFile);
bool CmdMemWrite(Client& client, std::uint64_t va, const void* data, std::uint64_t size);
void CmdMemDump(Client& client, std::uint64_t va, std::uint64_t size);
void CmdMemScan(Client& client, std::uint64_t va, std::uint64_t size, const char* pattern);

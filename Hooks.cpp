
/*  
 *  Version: MPL 1.1
 *  
 *  The contents of this file are subject to the Mozilla Public License Version 
 *  1.1 (the "License"); you may not use this file except in compliance with 
 *  the License. You may obtain a copy of the License at 
 *  http://www.mozilla.org/MPL/
 *  
 *  Software distributed under the License is distributed on an "AS IS" basis,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 *  for the specific language governing rights and limitations under the
 *  License.
 *  
 *  The Original Code is the YSI 2.0 SA:MP plugin.
 *  
 *  The Initial Developer of the Original Code is Alex "Y_Less" Cole.
 *  Portions created by the Initial Developer are Copyright (C) 2008
 *  the Initial Developer. All Rights Reserved.
 *  
 *  Contributor(s):
 *  
 *  Peter Beverloo
 *  Marcus Bauer
 *  MaVe;
 *  Sammy91
 *  Incognito
 *  
 *  Special Thanks to:
 *  
 *  SA:MP Team past, present and future
 */

#include "main.h"
#include "Utils.h"

#ifdef WIN32
	#define WIN32_LEAN_AND_MEAN
	#define VC_EXTRALEAN
	#include <Windows.h>
	#include <Psapi.h>
#else
	#include <stdio.h>
	#include <sys/mman.h>
	#include <limits.h>
	#include <string.h>
	#include <algorithm>
	#include <unistd.h>
#endif

#include "subhook/subhook.h"

#ifndef PAGESIZE
	#define PAGESIZE (4096)
#endif

extern void *pAMXFunctions;

static SubHook Namecheck_hook;
static SubHook amx_Register_hook;
static SubHook GetPacketID_hook;

// Y_Less - original YSF
bool Unlock(void *address, size_t len)
{
	#ifdef WIN32
		DWORD
			oldp;
		// Shut up the warnings :D
		return !!VirtualProtect(address, len, PAGE_EXECUTE_READWRITE, &oldp);
	#else
		size_t
			iPageSize = getpagesize(),
			iAddr = ((reinterpret_cast <uint32_t>(address) / iPageSize) * iPageSize);
		return !mprotect(reinterpret_cast <void*>(iAddr), len, PROT_READ | PROT_WRITE | PROT_EXEC);
	#endif
}

// Y_Less - fixes2
void AssemblySwap(char * addr, char * dat, int len)
{
	char
		swp;
	while (len--)
	{
		swp = addr[len];
		addr[len] = dat[len];
		dat[len] = swp;
	}
}

void AssemblyRedirect(void * from, void * to, char * ret)
{
	#ifdef LINUX
		size_t
			iPageSize = getpagesize(),
			iAddr = ((reinterpret_cast <uint32_t>(from) / iPageSize) * iPageSize),
			iCount = (5 / iPageSize) * iPageSize + iPageSize * 2;
		mprotect(reinterpret_cast <void*>(iAddr), iCount, PROT_READ | PROT_WRITE | PROT_EXEC);
		//mprotect(from, 5, PROT_READ | PROT_WRITE | PROT_EXEC);
	#else
		DWORD
			old;
		VirtualProtect(from, 5, PAGE_EXECUTE_READWRITE, &old);
	#endif
	*((unsigned char *)ret) = 0xE9;
	*((char **)(ret + 1)) = (char *)((char *)to - (char *)from) - 5;
	AssemblySwap((char *)from, ret, 5);
}

bool memory_compare(const BYTE *data, const BYTE *pattern, const char *mask)
{
	for(; *mask; ++mask, ++data, ++pattern)
	{
		if(*mask == 'x' && *data != *pattern)
			return false;
	}
	return (*mask) == NULL;
}

DWORD FindPattern(char *pattern, char *mask)
{
	DWORD i;
	DWORD size;
	DWORD address;
#ifdef WIN32
	MODULEINFO info = { 0 };

	address = (DWORD)GetModuleHandle(NULL);
	GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &info, sizeof(MODULEINFO));
	size = (DWORD)info.SizeOfImage;
#else
	address = 0x804b480; // around the elf base
	size = 0x8128B80 - address;
#endif
	for(i = 0; i < size; ++i)
	{
		if(memory_compare((BYTE *)(address + i), (BYTE *)pattern, mask))
			return (DWORD)(address + i);
	}
	return 0;
}

///////////////////////////////////////////////////////////////
// Hooks //
///////////////////////////////////////////////////////////////

// Custom name check
std::vector <char> gValidNameCharacters;

bool HOOK_ContainsInvalidChars(char * szString)
{
	bool bIllegal = false;

	while(*szString) 
	{
		if( (*szString >= '0' && *szString <= '9') || (*szString >= 'A' && *szString <= 'Z') || (*szString >= 'a' && *szString <= 'z')  ||
			*szString == ']' || *szString == '[' || *szString == '_'  || *szString == '$' || *szString == ':' || *szString == '=' || 
			*szString == '(' || *szString == ')' || *szString == '@' ||  *szString == '.' ) 
		{

			szString++;
		} 
		else 
		{
			if(Contains(gValidNameCharacters, *szString)) 
			{
				szString++;
			}
			else
			{
				return true;
			}
		}
	}
	return false;
}

// amx_Register hook for redirect natives
bool g_bNativesHooked;

static void HOOK_amx_Register(AMX *amx, AMX_NATIVE_INFO *nativelist, int number)
{
	SubHook::ScopedRemove remove(&amx_Register_hook);

	if (!g_bNativesHooked)
	{
		int i = 0;
		while (nativelist[i].name)
		{
			int x = 0;
			while (RedirectNatives[x].name)
			{
				if (!strcmp(nativelist[i].name, RedirectNatives[x].name))
				{
					if (!g_bNativesHooked) g_bNativesHooked = true;
					nativelist[i].func = RedirectNatives[x].func;
				}
				x++;
			}
			i++;
		}
	}

	amx_Register(amx, nativelist, number);
}

// GetPacketID hook
BYTE GetPacketID(Packet *p)
{
	if (p == 0) return 255;

	if ((unsigned char)p->data[0] == 36) 
	{
		assert(p->length > sizeof(unsigned char) + sizeof(unsigned long));
		return (unsigned char)p->data[sizeof(unsigned char) + sizeof(unsigned long)];
	}
	else return (unsigned char)p->data[0];
}

bool IsPlayerUpdatePacket(unsigned char packetId)
{
	return (packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC || packetId == ID_PASSENGER_SYNC || packetId == ID_SPECTATOR_SYNC);
}

static BYTE HOOK_GetPacketID(Packet *p)
{
	SubHook::ScopedRemove remove(&GetPacketID_hook);

	BYTE packetId = GetPacketID(p);
	WORD playerid = p->playerIndex;

	//logprintf("packetid: %d, playeird: %d", packetId, playerid);

	if (!IsPlayerConnected(playerid)) return 0xFF;

	// AFK
	if (IsPlayerUpdatePacket(packetId))
	{
		if (pNetGame->pPlayerPool->pPlayer[playerid]->byteState != 0 && pNetGame->pPlayerPool->pPlayer[playerid]->byteState != 7)
		{
			pPlayerData[playerid]->dwLastUpdateTick = GetTickCount();
			pPlayerData[playerid]->bEverUpdated = true;
		}
	}

	if (packetId == ID_PLAYER_SYNC)
	{
		//logprintf("ID_PLAYER_SYNC");
		RakNet::BitStream bsData(p->data, p->length, false);
		CSyncData pSyncData;

		bsData.SetReadOffset(8);
		bsData.Read((char*)&pSyncData, sizeof(pSyncData));

		//logprintf("health: %d, weapon: %d, specialaction: %d", pSyncData.byteHealth, pSyncData.byteWeapon, pSyncData.byteSpecialAction);

		if (pSyncData.byteWeapon == 44 || pSyncData.byteWeapon == 45)
		{
			pSyncData.byteWeapon = 0;
			//logprintf("nightvision");
		}
	}

	if (packetId == ID_BULLET_SYNC)
	{
		RakNet::BitStream bsData(p->data, p->length, false);
		BULLET_SYNC_DATA pBulletSync;

		bsData.SetReadOffset(8);
		bsData.Read((char*)&pBulletSync, sizeof(pBulletSync));

		if (pBulletSync.vecCenterOfHit.fX < -20000.0 || pBulletSync.vecCenterOfHit.fX > 20000.0 ||
			pBulletSync.vecCenterOfHit.fY < -20000.0 || pBulletSync.vecCenterOfHit.fY > 20000.0 ||
			pBulletSync.vecCenterOfHit.fZ < -20000.0 || pBulletSync.vecCenterOfHit.fZ > 20000.0)
		{
			logprintf("bullet crasher detected. id = %d", playerid);
			return 0xFF;
		}

	}
	return packetId;
}

void InstallPreHooks()
{
	Namecheck_hook.Install((void *)CAddress::FUNC_ContainsInvalidChars, (void *)HOOK_ContainsInvalidChars);
	amx_Register_hook.Install((void*)*(DWORD*)((DWORD)pAMXFunctions + (PLUGIN_AMX_EXPORT_Register * 4)), (void*)HOOK_amx_Register);
	GetPacketID_hook.Install((void*)CAddress::FUNC_GetPacketID, (void*)HOOK_GetPacketID);	
}
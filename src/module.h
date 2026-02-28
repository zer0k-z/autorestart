#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include "metamod_oslink.h"
#include "strtools.h"
#include "dbg.h"

#ifdef _WIN32
#include <Psapi.h>
#define MODULE_PREFIX ""
#define MODULE_EXT    ".dll"
#define GAMEBIN       "/csgo/bin/win64/"
#else
#define MODULE_PREFIX "lib"
#define MODULE_EXT    ".so"
#define GAMEBIN       "/csgo/bin/linuxsteamrt64/"
#endif

struct Section
{
	std::string m_szName;
	void *m_pBase;
	size_t m_iSize;
};

#ifndef _WIN32
int GetModuleInformation(HINSTANCE module, void **base, size_t *length, std::vector<Section> &m_sections);
#endif

// Scans memory for a byte pattern, yielding one match at a time via FindNext().
class SignatureIterator
{
public:
	SignatureIterator(void *pBase, size_t iSize, const byte *pSignature, size_t iSigLength)
		: m_pBase((byte *)pBase), m_iSize(iSize), m_pSignature(pSignature), m_iSigLength(iSigLength),
		  m_pCurrent((byte *)pBase)
	{}

	void *FindNext(bool allowWildcard)
	{
		for (size_t i = 0; i < m_iSize; i++)
		{
			size_t Matches = 0;
			while (*(m_pCurrent + i + Matches) == m_pSignature[Matches]
				   || (allowWildcard && m_pSignature[Matches] == '\x2A'))
			{
				Matches++;
				if (Matches == m_iSigLength)
				{
					m_pCurrent += i + 1;
					return m_pCurrent - 1;
				}
			}
		}
		return nullptr;
	}

private:
	byte *m_pBase;
	size_t m_iSize;
	const byte *m_pSignature;
	size_t m_iSigLength;
	byte *m_pCurrent;
};

class CModule
{
public:
	CModule(const char *path, const char *module) : m_pszModule(module), m_pszPath(path)
	{
		char szModule[MAX_PATH];
		V_snprintf(szModule, MAX_PATH, "%s%s%s%s%s", Plat_GetGameDirectory(), path, MODULE_PREFIX, m_pszModule, MODULE_EXT);

		m_hModule = dlmount(szModule);
		if (!m_hModule)
			Error("Could not find %s\n", szModule);

#ifdef _WIN32
		MODULEINFO info;
		GetModuleInformation(GetCurrentProcess(), m_hModule, &info, sizeof(info));
		m_base = (void *)info.lpBaseOfDll;
		m_size = info.SizeOfImage;
		InitializeSections();
#else
		if (int e = GetModuleInformation(m_hModule, &m_base, &m_size, m_sections))
			Error("Failed to get module info for %s, error %d\n", szModule, e);
#endif
	}

	Section *GetSection(std::string_view name)
	{
		for (auto &section : m_sections)
		{
			if (section.m_szName == name)
				return &section;
		}
		return nullptr;
	}

#ifdef _WIN32
	void InitializeSections();
#endif

	void *FindVirtualTable(const std::string &name);

public:
	const char *m_pszModule;
	const char *m_pszPath;
	HINSTANCE m_hModule;
	void *m_base;
	size_t m_size;
	std::vector<Section> m_sections;
};

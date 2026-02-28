#ifdef __linux__
#include "module.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <elf.h>
#include <link.h>

#define PAGE_SIZE        4096
#define PAGE_ALIGN_UP(x) ((x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

// https://github.com/alliedmodders/sourcemod/blob/master/core/logic/MemoryUtils.cpp#L502-L587
// https://github.com/komashchenko/DynLibUtils/blob/5eb95475170becfcc64fd5d32d14ec2b76dcb6d4/module_linux.cpp#L95
int GetModuleInformation(HINSTANCE hModule, void **base, size_t *length, std::vector<Section> &m_sections)
{
	link_map *lmap;
	if (dlinfo(hModule, RTLD_DI_LINKMAP, &lmap) != 0)
	{
		dlclose(hModule);
		return 1;
	}

	int fd = open(lmap->l_name, O_RDONLY);
	if (fd == -1)
	{
		dlclose(hModule);
		return 2;
	}

	struct stat st;
	if (fstat(fd, &st) == 0)
	{
		void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map != MAP_FAILED)
		{
			ElfW(Ehdr) *ehdr = static_cast<ElfW(Ehdr) *>(map);
			ElfW(Shdr) *shdrs = reinterpret_cast<ElfW(Shdr) *>(reinterpret_cast<uintptr_t>(ehdr) + ehdr->e_shoff);
			const char *strTab = reinterpret_cast<const char *>(reinterpret_cast<uintptr_t>(ehdr) + shdrs[ehdr->e_shstrndx].sh_offset);

			for (int i = 0; i < ehdr->e_phnum; ++i)
			{
				ElfW(Phdr) *phdr = reinterpret_cast<ElfW(Phdr) *>(reinterpret_cast<uintptr_t>(ehdr) + ehdr->e_phoff + i * ehdr->e_phentsize);
				if (phdr->p_type == PT_LOAD && phdr->p_flags & PF_X)
				{
					*base = reinterpret_cast<void *>(lmap->l_addr + phdr->p_vaddr);
					*length = phdr->p_filesz;
					break;
				}
			}

			for (int i = 0; i < ehdr->e_shnum; ++i)
			{
				ElfW(Shdr) *shdr = reinterpret_cast<ElfW(Shdr) *>(reinterpret_cast<uintptr_t>(shdrs) + i * ehdr->e_shentsize);
				if (*(strTab + shdr->sh_name) == '\0')
					continue;

				Section section;
				section.m_szName = strTab + shdr->sh_name;
				section.m_pBase = reinterpret_cast<void *>(lmap->l_addr + shdr->sh_addr);
				section.m_iSize = shdr->sh_size;
				m_sections.push_back(section);
			}

			munmap(map, st.st_size);
		}
	}

	close(fd);
	return 0;
}

void *CModule::FindVirtualTable(const std::string &name)
{
	auto readOnlyData = GetSection(".rodata");
	auto readOnlyRelocations = GetSection(".data.rel.ro");

	if (!readOnlyData || !readOnlyRelocations)
	{
		Warning("Failed to find .rodata or .data.rel.ro section\n");
		return nullptr;
	}

	std::string decoratedTableName = std::to_string(name.length()) + name;

	SignatureIterator sigIt(readOnlyData->m_pBase, readOnlyData->m_iSize, (const byte *)decoratedTableName.c_str(), decoratedTableName.size() + 1);
	void *classNameString = sigIt.FindNext(false);

	if (!classNameString)
	{
		Warning("Failed to find type descriptor for %s\n", name.c_str());
		return nullptr;
	}

	SignatureIterator sigIt2(readOnlyRelocations->m_pBase, readOnlyRelocations->m_iSize, (const byte *)&classNameString, sizeof(void *));
	void *typeName = sigIt2.FindNext(false);

	if (!typeName)
	{
		Warning("Failed to find type name for %s\n", name.c_str());
		return nullptr;
	}

	void *typeInfo = (void *)((uintptr_t)typeName - 0x8);

	for (const auto &sectionName : {std::string_view(".data.rel.ro"), std::string_view(".data.rel.ro.local")})
	{
		auto section = GetSection(sectionName);
		if (!section)
			continue;

		SignatureIterator sigIt3(section->m_pBase, section->m_iSize, (const byte *)&typeInfo, sizeof(void *));

		while (void *vtable = sigIt3.FindNext(false))
		{
			if (*(int64_t *)((uintptr_t)vtable - 0x8) == 0)
				return (void *)((uintptr_t)vtable + 0x8);
		}
	}

	Warning("Failed to find vtable for %s\n", name.c_str());
	return nullptr;
}
#endif

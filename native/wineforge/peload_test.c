/*
 * peload_test — in-address-space PE mapping + relocation (experiment 0003c).
 *
 * The centerpiece mechanic of the in-process loader: load a Windows PE into
 * the host address space at a base OTHER than its preferred ImageBase (because
 * the parent already occupies it), fix it up via the .reloc table, and run it.
 * This is exactly what spawn_process_inproc must do for a child .exe, minus
 * the ntdll/PEB init (which is 0003b/0003d).
 *
 * To keep the test self-contained and rigorous it builds its own minimal
 * PE64 in memory (no toolchain / no prebuilt binary needed):
 *   - .text : entry() returns 0x2A
 *   - .data : an 8-byte absolute pointer = ImageBase + MARKER_RVA
 *   - .reloc: one IMAGE_REL_BASED_DIR64 fixup for that pointer
 * then loads it TWICE at two different bases at once (proving two images can
 * coexist in one address space — parent+child), relocates each, and checks:
 *   (a) the relocated .data pointer == actual_load_base + MARKER_RVA
 *   (b) calling entry() returns 0x2A
 *
 * Portable host code (x86-64 Linux here); the same logic is what runs on iOS.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- minimal PE64 structures (only the fields we use) ------------------ */
#pragma pack(push, 1)
struct dos_header { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; };
struct file_header {
    uint16_t Machine, NumberOfSections;
    uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    uint16_t SizeOfOptionalHeader, Characteristics;
};
struct data_dir { uint32_t VirtualAddress, Size; };
struct opt_header64 {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint, BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOS, MinorOS, MajorImage, MinorImage, MajorSub, MinorSub;
    uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
    struct data_dir DataDirectory[16];
};
struct section_header {
    char Name[8];
    uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
    uint32_t PointerToRelocations, PointerToLinenumbers;
    uint16_t NumberOfRelocations, NumberOfLinenumbers;
    uint32_t Characteristics;
};
struct base_reloc_block { uint32_t VirtualAddress, SizeOfBlock; };
#pragma pack(pop)

#define DIR_BASERELOC   5
#define REL_BASED_DIR64 10
#define SUBSYS_CUI      3
#define SEC_ALIGN 0x1000u
#define FILE_ALIGN 0x200u

#define TEXT_RVA   0x1000u
#define DATA_RVA   0x2000u
#define RELOC_RVA  0x3000u
#define MARKER_RVA DATA_RVA      /* the .data pointer points here (self-ref) */
#define IMAGE_SIZE 0x4000u
#define PREFERRED_BASE 0x140000000ull


/* Build a minimal PE64 into buf; return total file size. */
static size_t build_pe(uint8_t *buf)
{
    memset(buf, 0, IMAGE_SIZE);
    struct dos_header *dos = (void *)buf;
    dos->e_magic = 0x5A4D;                 /* 'MZ' */
    dos->e_lfanew = 0x40;

    uint8_t *pe = buf + dos->e_lfanew;
    *(uint32_t *)pe = 0x00004550;          /* 'PE\0\0' */
    struct file_header *fh = (void *)(pe + 4);
    fh->Machine = 0x8664;                  /* AMD64 */
    fh->NumberOfSections = 3;
    fh->SizeOfOptionalHeader = sizeof(struct opt_header64);
    fh->Characteristics = 0x0022;          /* EXECUTABLE | LARGE_ADDRESS_AWARE */

    struct opt_header64 *oh = (void *)(fh + 1);
    oh->Magic = 0x20b;                     /* PE32+ */
    oh->AddressOfEntryPoint = TEXT_RVA;
    oh->BaseOfCode = TEXT_RVA;
    oh->ImageBase = PREFERRED_BASE;
    oh->SectionAlignment = SEC_ALIGN;
    oh->FileAlignment = FILE_ALIGN;
    oh->MajorOS = 6; oh->MajorSub = 6;
    oh->SizeOfImage = IMAGE_SIZE;
    oh->SizeOfHeaders = FILE_ALIGN;
    oh->Subsystem = SUBSYS_CUI;
    oh->SizeOfStackReserve = 0x100000;
    oh->SizeOfHeapReserve = 0x100000;
    oh->NumberOfRvaAndSizes = 16;
    oh->DataDirectory[DIR_BASERELOC].VirtualAddress = RELOC_RVA;
    oh->DataDirectory[DIR_BASERELOC].Size = sizeof(struct base_reloc_block) + 2;

    struct section_header *sec = (void *)(oh + 1);
    /* .text */
    memcpy(sec[0].Name, ".text", 5);
    sec[0].VirtualSize = 0x20; sec[0].VirtualAddress = TEXT_RVA;
    sec[0].SizeOfRawData = FILE_ALIGN; sec[0].PointerToRawData = TEXT_RVA;
    sec[0].Characteristics = 0x60000020;   /* CODE | EXECUTE | READ */
    /* .data */
    memcpy(sec[1].Name, ".data", 5);
    sec[1].VirtualSize = 8; sec[1].VirtualAddress = DATA_RVA;
    sec[1].SizeOfRawData = FILE_ALIGN; sec[1].PointerToRawData = DATA_RVA;
    sec[1].Characteristics = 0xC0000040;   /* INITIALIZED_DATA | READ | WRITE */
    /* .reloc */
    memcpy(sec[2].Name, ".reloc", 6);
    sec[2].VirtualSize = 0x10; sec[2].VirtualAddress = RELOC_RVA;
    sec[2].SizeOfRawData = FILE_ALIGN; sec[2].PointerToRawData = RELOC_RVA;
    sec[2].Characteristics = 0x42000040;   /* INITIALIZED_DATA | DISCARDABLE | READ */

    /* .text code: mov eax, 0x2A ; ret  (as a raw C-callable int(void)) */
    uint8_t code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    memcpy(buf + TEXT_RVA, code, sizeof(code));

    /* .data: absolute pointer to ImageBase + MARKER_RVA (needs relocation) */
    *(uint64_t *)(buf + DATA_RVA) = PREFERRED_BASE + MARKER_RVA;

    /* .reloc: one block for page DATA_RVA, one DIR64 entry at offset 0 */
    struct base_reloc_block *blk = (void *)(buf + RELOC_RVA);
    blk->VirtualAddress = DATA_RVA;        /* page base */
    blk->SizeOfBlock = sizeof(*blk) + 2;
    *(uint16_t *)(buf + RELOC_RVA + sizeof(*blk)) = (REL_BASED_DIR64 << 12) | 0x000;

    return IMAGE_SIZE;
}

/* Map the PE at a chosen base, copy sections to RVAs, apply .reloc. */
static uint8_t *load_pe(const uint8_t *file, uintptr_t want_base)
{
    const struct dos_header *dos = (const void *)file;
    const uint8_t *pe = file + dos->e_lfanew;
    const struct file_header *fh = (const void *)(pe + 4);
    const struct opt_header64 *oh = (const void *)(fh + 1);
    const struct section_header *sec = (const void *)((const uint8_t *)oh + fh->SizeOfOptionalHeader);

    /* reserve the whole image RW first (like NtAllocateVirtualMemory) */
    void *base = mmap((void *)want_base, oh->SizeOfImage, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED) { perror("mmap image"); return NULL; }

    memcpy(base, file, oh->SizeOfHeaders);
    for (int i = 0; i < fh->NumberOfSections; i++)
        memcpy((uint8_t *)base + sec[i].VirtualAddress,
               file + sec[i].PointerToRawData, sec[i].VirtualSize);

    /* apply base relocations: add (actual_base - preferred_base) to each DIR64 */
    int64_t delta = (int64_t)((uintptr_t)base - oh->ImageBase);
    uint32_t reloc_rva = oh->DataDirectory[DIR_BASERELOC].VirtualAddress;
    uint32_t reloc_sz  = oh->DataDirectory[DIR_BASERELOC].Size;
    uint32_t off = 0;
    while (off < reloc_sz) {
        struct base_reloc_block *blk = (void *)((uint8_t *)base + reloc_rva + off);
        if (blk->SizeOfBlock < sizeof(*blk)) break;
        uint32_t n = (blk->SizeOfBlock - sizeof(*blk)) / 2;
        uint16_t *ents = (uint16_t *)(blk + 1);
        for (uint32_t i = 0; i < n; i++) {
            int type = ents[i] >> 12, ofs = ents[i] & 0xfff;
            if (type == REL_BASED_DIR64)
                *(uint64_t *)((uint8_t *)base + blk->VirtualAddress + ofs) += delta;
        }
        off += blk->SizeOfBlock;
    }

    /* make .text executable (W^X: RW during load, RX to run) */
    if (mprotect((uint8_t *)base + TEXT_RVA, SEC_ALIGN, PROT_READ | PROT_EXEC)) {
        perror("mprotect text"); munmap(base, oh->SizeOfImage); return NULL;
    }
    return base;
}

int main(void)
{
    static uint8_t file[IMAGE_SIZE];
    build_pe(file);

    /* Load two independent instances at two bases at once (parent + child). */
    struct { uintptr_t want; const char *label; } inst[] = {
        { 0x210000000ull, "image A" },
        { 0x330000000ull, "image B" },
    };
    int failures = 0;
    uint8_t *bases[2];

    for (int k = 0; k < 2; k++) {
        uint8_t *base = load_pe(file, inst[k].want);
        if (!base) { printf("FAIL: %s did not load\n", inst[k].label); return 1; }
        bases[k] = base;

        /* (a) relocation: the .data pointer must now be base + MARKER_RVA */
        uint64_t got = *(uint64_t *)(base + DATA_RVA);
        uint64_t expect = (uint64_t)(uintptr_t)base + MARKER_RVA;
        if (got != expect) {
            printf("FAIL: %s reloc: got %#llx expected %#llx\n",
                   inst[k].label, (unsigned long long)got, (unsigned long long)expect);
            failures++;
        } else {
            printf("ok: %s relocated .data pointer -> %#llx (base %#llx)\n",
                   inst[k].label, (unsigned long long)got, (unsigned long long)(uintptr_t)base);
        }

        /* (b) execution: entry() returns 0x2A */
        int (*entry)(void) = (int (*)(void))(base + TEXT_RVA);
        int rc = entry();
        if (rc != 0x2A) { printf("FAIL: %s entry returned %d\n", inst[k].label, rc); failures++; }
        else printf("ok: %s entry() ran in-address-space -> %d\n", inst[k].label, rc);
    }

    /* Both images must occupy distinct memory simultaneously. */
    if (bases[0] == bases[1]) { printf("FAIL: images overlap\n"); failures++; }
    else printf("ok: both images resident at once (%#llx and %#llx)\n",
                (unsigned long long)(uintptr_t)bases[0], (unsigned long long)(uintptr_t)bases[1]);

    if (failures) { printf("peload_test: %d failure(s)\n", failures); return 1; }
    printf("peload_test: PASS — PE mapped, relocated, and run in-address-space "
           "(two images coexisting)\n");
    return 0;
}

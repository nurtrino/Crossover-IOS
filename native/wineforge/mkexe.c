/*
 * mkexe — emit a minimal, import-free Win64 console .exe (experiment 0003f).
 *
 * The in-process child needs a program it can RUN. A normal Windows .exe
 * imports kernel32 and so needs ntdll's PE-side loader to build a per-process
 * module list — the documented long tail (docs/DESIGN-0003-inproc-spawn.md).
 * This tool emits a PE with NO imports whose entry point simply returns a
 * chosen exit code, which is exactly the program class an in-process child can
 * execute today with Wine's own SKIP_LOADER_INIT path.
 *
 * The PE layout mirrors peload_test.c (same structures, same relocation
 * proof), but written to a file so a real `wine` prefix can CreateProcess it.
 *
 *   usage: mkexe <output.exe> [exit_code]
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define SEC_ALIGN  0x1000u
#define FILE_ALIGN 0x200u

#define TEXT_RVA   0x1000u
#define DATA_RVA   0x2000u
#define RELOC_RVA  0x3000u
#define IMAGE_SIZE 0x4000u
#define PREFERRED_BASE 0x140000000ull

int main(int argc, char **argv)
{
    static uint8_t buf[IMAGE_SIZE];
    unsigned code_val = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 42;
    FILE *f;

    if (argc < 2) { fprintf(stderr, "usage: %s <output.exe> [exit_code]\n", argv[0]); return 2; }

    memset(buf, 0, sizeof(buf));
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
    memcpy(sec[0].Name, ".text", 5);
    sec[0].VirtualSize = 0x20; sec[0].VirtualAddress = TEXT_RVA;
    sec[0].SizeOfRawData = FILE_ALIGN; sec[0].PointerToRawData = TEXT_RVA;
    sec[0].Characteristics = 0x60000020;   /* CODE | EXECUTE | READ */
    memcpy(sec[1].Name, ".data", 5);
    sec[1].VirtualSize = 8; sec[1].VirtualAddress = DATA_RVA;
    sec[1].SizeOfRawData = FILE_ALIGN; sec[1].PointerToRawData = DATA_RVA;
    sec[1].Characteristics = 0xC0000040;   /* INITIALIZED_DATA | READ | WRITE */
    memcpy(sec[2].Name, ".reloc", 6);
    sec[2].VirtualSize = 0x10; sec[2].VirtualAddress = RELOC_RVA;
    sec[2].SizeOfRawData = FILE_ALIGN; sec[2].PointerToRawData = RELOC_RVA;
    sec[2].Characteristics = 0x42000040;   /* INITIALIZED_DATA | DISCARDABLE | READ */

    /* .text: mov eax, <code> ; ret
     * The entry is reached through RtlUserThreadStart, which passes it to
     * kernel32's thread thunk; returning hands <code> back as the exit code. */
    uint8_t code[] = { 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 };
    code[1] = (uint8_t)(code_val & 0xff);
    code[2] = (uint8_t)((code_val >> 8) & 0xff);
    code[3] = (uint8_t)((code_val >> 16) & 0xff);
    code[4] = (uint8_t)((code_val >> 24) & 0xff);
    memcpy(buf + TEXT_RVA, code, sizeof(code));

    /* .data: an absolute self-pointer, so the image genuinely needs relocating
     * when the parent already occupies its preferred base */
    *(uint64_t *)(buf + DATA_RVA) = PREFERRED_BASE + DATA_RVA;

    struct base_reloc_block *blk = (void *)(buf + RELOC_RVA);
    blk->VirtualAddress = DATA_RVA;
    blk->SizeOfBlock = sizeof(*blk) + 2;
    *(uint16_t *)(buf + RELOC_RVA + sizeof(*blk)) = (REL_BASED_DIR64 << 12) | 0x000;

    if (!(f = fopen(argv[1], "wb"))) { perror("fopen"); return 1; }
    if (fwrite(buf, 1, IMAGE_SIZE, f) != IMAGE_SIZE) { perror("fwrite"); fclose(f); return 1; }
    fclose(f);
    printf("wrote %s (%u bytes), entry returns %u\n", argv[1], IMAGE_SIZE, code_val);
    return 0;
}

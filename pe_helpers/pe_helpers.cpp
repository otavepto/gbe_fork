#include "pe_helpers/pe_helpers.hpp"
#include <vector>
#include <utility>

static inline bool is_hex(const char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static inline uint8_t char_to_byte(const char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a') + 0xa;
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A') + 0xa;
    return (uint8_t)c;
}

static inline PIMAGE_NT_HEADERS get_nt_header(HMODULE hModule)
{
    // https://dev.to/wireless90/validating-the-pe-signature-my-av-flagged-me-windows-pe-internals-2m5o/
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)(char*)hModule;
    LONG newExeHeaderOffset = dosHeader->e_lfanew;

    return (PIMAGE_NT_HEADERS)((char*)hModule + newExeHeaderOffset);
}

static inline PIMAGE_FILE_HEADER get_file_header(HMODULE hModule)
{
    return &get_nt_header(hModule)->FileHeader;
}

static inline PIMAGE_OPTIONAL_HEADER get_optional_header(HMODULE hModule)
{
    return &get_nt_header(hModule)->OptionalHeader;
}

uint8_t* pe_helpers::search_memory(uint8_t *mem, size_t size, const std::string &search_patt)
{
    if (!mem || !size || search_patt.empty()) return nullptr;

    if (search_patt.find_first_not_of(" \t", 0) == std::string::npos) {
        return nullptr; // empty search patt
    }

    const uint8_t *end = mem + size;
    for (uint8_t *base = mem; base < end; ++base)
    {
        // incremental offset after each byte match
        size_t search_patt_offset = 0;
        bool error = false;
        for (const uint8_t *displacement = base; displacement < end; ++displacement)
        {
            uint8_t mask = 0xFF;
            uint8_t s_byte = 0;

            // skip spaces
            search_patt_offset = search_patt.find_first_not_of(" \t", search_patt_offset);
            if (search_patt_offset == std::string::npos) {
                break;
            }

            const auto this_char = search_patt[search_patt_offset];
            const auto next_char = (search_patt_offset + 1) < search_patt.size()
                ? search_patt[search_patt_offset + 1]
                : '\0';
            if (this_char == '?') {
                if (next_char == '?' || // "??"
                    next_char == ' ' || // "? "
                    next_char == 't') { // "?    "
                    mask = 0x00;
                    s_byte = 0;
                } else if (is_hex(next_char)) { // "?c"
                    mask = 0x0F;
                    s_byte = char_to_byte(next_char);
                } else { // unknown
                    return nullptr;
                }

                // skip
                search_patt_offset += 2;
            } else if (is_hex(this_char)) {
                if (next_char == '?') { // "c?"
                    mask = 0xF0;
                    s_byte = char_to_byte(this_char) << 4;
                } else if (is_hex(next_char)) { // "34"
                    mask = 0xFF;
                    s_byte = (char_to_byte(this_char) << 4) | char_to_byte(next_char);
                } else { // unknown
                    return nullptr;
                }
                
                // skip
                search_patt_offset += 2;
            } else { // unknown
                return nullptr;
            }

            if ((*displacement & mask) != (s_byte & mask)) {
                error = true;
                break;
            }
        }

        if (!error && (search_patt_offset >= search_patt.size())) {
            return base;
        }
    }

    return nullptr;
}

bool pe_helpers::replace_memory(uint8_t *mem, size_t size, const std::string &replace_patt, HANDLE hProcess)
{
    if (!mem || !size || replace_patt.empty()) return false;

    size_t replace_patt_offset = replace_patt.find_first_not_of(" \t", 0);
    if (replace_patt_offset == std::string::npos) {
        return false; // empty patt
    }

    // mask - byte
    std::vector<std::pair<uint8_t, uint8_t>> replace_bytes{};
    for (;
         replace_patt_offset < replace_patt.size();
         replace_patt_offset = replace_patt.find_first_not_of(" \t", replace_patt_offset)) {
        const auto this_char = replace_patt[replace_patt_offset];
        const auto next_char = (replace_patt_offset + 1) < replace_patt.size()
            ? replace_patt[replace_patt_offset + 1]
            : '\0';
        
        if (this_char == '?') {
            if (next_char == '?' || // "??"
                next_char == ' ' || // "? "
                next_char == 't') { // "?    "
                replace_bytes.push_back({
                    0x00,
                    0,
                });
            } else if (is_hex(next_char)) { // "?c"
                replace_bytes.push_back({
                    0x0F,
                    char_to_byte(next_char),
                });
            } else { // unknown
                return false;
            }

            // skip
            replace_patt_offset += 2;
        } else if (is_hex(this_char)) {
            if (next_char == '?') { // "c?"
                replace_bytes.push_back({
                    0xF0,
                    char_to_byte(this_char) << 4,
                });
            } else if (is_hex(next_char)) { // "34"
                replace_bytes.push_back({
                    0xFF,
                    (char_to_byte(this_char) << 4) | char_to_byte(next_char),
                });
            } else { // unknown
                return false;
            }
            
            // skip
            replace_patt_offset += 2;
        } else { // unknown
            return false;
        }
    }

    // remove trailing "??"
    // while last element == "??"
    while (replace_bytes.size() &&
           replace_bytes.back().first == 0x00)
    {
        replace_bytes.pop_back();
    }
    if (replace_bytes.empty() || replace_bytes.size() > size) return false;

    // change protection
    DWORD current_protection = 0;
    if (!VirtualProtectEx(hProcess, mem, replace_bytes.size(), PAGE_READWRITE, &current_protection)) {
        return false;
    }

    for (auto &rp : replace_bytes) {
        if (rp.first == 0x00) continue;

        const uint8_t b_mem = (uint8_t)(*mem & (uint8_t)~rp.first);
        const uint8_t b_replace = (uint8_t)(rp.second & rp.first);
        const uint8_t new_b_mem = b_mem | b_replace;
        *mem = b_mem | b_replace;
        ++mem;
    }

    // restore protection
    if (!VirtualProtectEx(hProcess, mem, replace_bytes.size(), current_protection, &current_protection)) {
        return false;
    }

    return true;
}

// https://learn.microsoft.com/en-us/windows/win32/debug/retrieving-the-last-error-code
std::string pe_helpers::get_err_string(DWORD code)
{
    std::string err_str(8192, '\0');

    DWORD msg_chars = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
        (LPSTR)&err_str[0],
        err_str.size(),
        NULL);
    
    if (!msg_chars) return std::string();

    err_str = err_str.substr(0, msg_chars);

    return err_str;
}

bool pe_helpers::is_module_64(HMODULE hModule)
{
    return !!(get_file_header(hModule)->Machine == IMAGE_FILE_MACHINE_AMD64);
}

bool pe_helpers::is_module_32(HMODULE hModule)
{
    return !!(get_file_header(hModule)->Machine == IMAGE_FILE_MACHINE_I386);
}

pe_helpers::SectionHeadersResult pe_helpers::get_section_headers(HMODULE hModule)
{
    PIMAGE_NT_HEADERS ntHeader = get_nt_header(hModule);
    PIMAGE_OPTIONAL_HEADER optionalHeader = &ntHeader->OptionalHeader;

    PIMAGE_FILE_HEADER fileHeader = get_file_header(hModule);
    WORD optionalHeadrSize = fileHeader->SizeOfOptionalHeader;

    struct SectionHeadersResult res {};
    res.count = fileHeader->NumberOfSections;
    res.ptr = fileHeader->NumberOfSections
        ? (PIMAGE_SECTION_HEADER)((char *)optionalHeader + optionalHeadrSize)
        : nullptr;

    return res;
}

PIMAGE_SECTION_HEADER pe_helpers::get_section_header_with_name(HMODULE hModule, const char *name)
{
    if (!name) return nullptr;

    auto res = get_section_headers(hModule);
    if (!res.count) return nullptr;

    for (size_t i = 0; i < res.count; ++i) {
        if (strncmp((const char *)res.ptr[i].Name, name, sizeof(res.ptr[i].Name)) == 0) {
            return &res.ptr[i];
        }
    }

    return nullptr;
}

DWORD pe_helpers::loadlib_remote(HANDLE hProcess, const std::wstring &lib_fullpath, const char** err_reason) {

  // create a remote page
  const size_t lib_path_str_bytes = lib_fullpath.size() * sizeof(lib_fullpath[0]);
  LPVOID lib_remote_page = VirtualAllocEx(
    hProcess,
    NULL,
    lib_path_str_bytes + sizeof(lib_fullpath[0]) * 2, // *2 just to be safe
    MEM_RESERVE | MEM_COMMIT,
    PAGE_READWRITE
  );

  if (!lib_remote_page) {
    if (err_reason) {
      *err_reason = "Failed to remotely allocate page with VirtualAllocEx()";
    }
    return GetLastError();
  }

  SIZE_T bytes_written = 0;
  BOOL written = WriteProcessMemory(
    hProcess,
    lib_remote_page,
    (LPCVOID)&lib_fullpath[0],
    lib_path_str_bytes,
    &bytes_written
  );

  if (!written || bytes_written < lib_path_str_bytes) {
    // cleanup allcoated page
    VirtualFreeEx(
      hProcess,
      lib_remote_page,
      0,
      MEM_RELEASE);

    if (err_reason) {
      *err_reason = "Failed to remotely write dll path with WriteProcessMemory()";
    }
    return GetLastError();
  }

  // call LoadLibraryA() and pass "launc.dll"
  HANDLE remote_thread = CreateRemoteThread(
    hProcess,
    NULL,
    0,
    (LPTHREAD_START_ROUTINE)LoadLibraryW,
    lib_remote_page,
    0,
    NULL);

  if (!remote_thread) {
    // cleanup allcoated page
    VirtualFreeEx(
      hProcess,
      lib_remote_page,
      0,
      MEM_RELEASE);

    if (err_reason) {
      *err_reason = "Failed to create/run remote thread with CreateRemoteThread()";
    }
    return GetLastError();
  }

  WaitForSingleObject(remote_thread, INFINITE);

  // cleanup allcoated page
  VirtualFreeEx(
    hProcess,
    lib_remote_page,
    0,
    MEM_RELEASE);
  
  return ERROR_SUCCESS;
}

size_t pe_helpers::get_pe_size(HMODULE hModule)
{
    // https://stackoverflow.com/a/34695773
    // https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
    // "The PE file header consists of a Microsoft MS-DOS stub, the PE signature, the COFF file header, and an optional header"
    // "The combined size of an MS-DOS stub, PE header, and section headers rounded up to a multiple of FileAlignment."
    size_t size = get_optional_header(hModule)->SizeOfHeaders;
    SectionHeadersResult headers = get_section_headers(hModule);
    for (size_t i = 0; i < headers.count; ++i) {
        size += headers.ptr[i].SizeOfRawData;
    }

    return size;
}
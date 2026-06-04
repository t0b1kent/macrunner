#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>

struct export_probe {
  const char *name;
};

struct module_probe {
  const wchar_t *dll_name_w;
  const char *dll_name;
  const struct export_probe *exports;
  unsigned int export_count;
};

static int probe_module(const struct module_probe *probe) {
  wchar_t module_path[MAX_PATH];
  HMODULE module;
  DWORD attrs;
  unsigned int i;
  int pass = 1;

  lstrcpyW(module_path, L".\\");
  if (2 + lstrlenW(probe->dll_name_w) >= MAX_PATH)
    return 0;
  lstrcatW(module_path, probe->dll_name_w);

  SetLastError(0);
  attrs = GetFileAttributesW(module_path);
  printf("vkd3d_runtime_file module=%s attrs=0x%08lx gle=%lu\n",
         probe->dll_name, attrs, GetLastError());
  if (attrs == INVALID_FILE_ATTRIBUTES)
    return 0;

  SetLastError(0);
  module = LoadLibraryExW(module_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  printf("vkd3d_runtime_load module=%s handle=%p gle=%lu\n",
         probe->dll_name, module, GetLastError());
  if (!module)
    return 0;

  for (i = 0; i < probe->export_count; ++i) {
    FARPROC proc = GetProcAddress(module, probe->exports[i].name);
    printf("vkd3d_runtime_export module=%s symbol=%s proc=%p gle=%lu\n",
           probe->dll_name, probe->exports[i].name, proc, GetLastError());
    if (!proc)
      pass = 0;
  }

  return pass;
}

int main(void) {
  static const struct export_probe d3d12_exports[] = {
      {"D3D12CreateDevice"},
      {"D3D12CreateRootSignatureDeserializer"},
      {"D3D12CreateVersionedRootSignatureDeserializer"},
      {"D3D12EnableExperimentalFeatures"},
      {"D3D12GetDebugInterface"},
      {"D3D12GetInterface"},
      {"D3D12SerializeRootSignature"},
      {"D3D12SerializeVersionedRootSignature"},
  };
  static const struct module_probe modules[] = {
      {L"d3d12.dll", "d3d12.dll", d3d12_exports,
       sizeof(d3d12_exports) / sizeof(d3d12_exports[0])},
  };
  unsigned int i;
  int pass = 1;

  setvbuf(stdout, NULL, _IONBF, 0);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);

  for (i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
    if (!probe_module(&modules[i]))
      pass = 0;
  }

  printf("vkd3d_runtime_load_result=%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

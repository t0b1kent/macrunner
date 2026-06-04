#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>

#include <d3d12.h>
#include <stdio.h>

typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(
    const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob **blob, ID3DBlob **error_blob);

typedef HRESULT(WINAPI *PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER)(
    const void *data, SIZE_T data_size, REFIID iid, void **deserializer);

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

static int probe_d3d12_root_signature(void) {
  HMODULE module;
  PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_root_signature;
  PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER create_deserializer;
  D3D12_ROOT_SIGNATURE_DESC desc;
  ID3DBlob *blob = NULL;
  ID3DBlob *error_blob = NULL;
  ID3D12RootSignatureDeserializer *deserializer = NULL;
  const D3D12_ROOT_SIGNATURE_DESC *decoded;
  HRESULT hr;
  int pass = 1;

  module = GetModuleHandleW(L"d3d12.dll");
  printf("vkd3d_runtime_semantic module=d3d12.dll handle=%p\n", module);
  if (!module)
    return 0;

  serialize_root_signature =
      (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(
          module, "D3D12SerializeRootSignature");
  create_deserializer =
      (PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER)GetProcAddress(
          module, "D3D12CreateRootSignatureDeserializer");
  if (!serialize_root_signature || !create_deserializer)
    return 0;

  ZeroMemory(&desc, sizeof(desc));
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  hr = serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                &error_blob);
  printf("vkd3d_runtime_call api=D3D12SerializeRootSignature hr=0x%08lx blob=%p error=%p\n",
         (unsigned long)hr, blob, error_blob);
  if (FAILED(hr) || !blob) {
    pass = 0;
    goto cleanup;
  }

  hr = create_deserializer(ID3D10Blob_GetBufferPointer(blob),
                           ID3D10Blob_GetBufferSize(blob),
                           &IID_ID3D12RootSignatureDeserializer,
                           (void **)&deserializer);
  printf("vkd3d_runtime_call api=D3D12CreateRootSignatureDeserializer hr=0x%08lx deserializer=%p\n",
         (unsigned long)hr, deserializer);
  if (FAILED(hr) || !deserializer) {
    pass = 0;
    goto cleanup;
  }

  decoded = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(deserializer);
  printf("vkd3d_runtime_rootsig params=%u samplers=%u flags=0x%08x\n",
         decoded ? decoded->NumParameters : 0xffffffffu,
         decoded ? decoded->NumStaticSamplers : 0xffffffffu,
         decoded ? decoded->Flags : 0xffffffffu);
  if (!decoded || decoded->NumParameters || decoded->NumStaticSamplers ||
      decoded->Flags != desc.Flags)
    pass = 0;

cleanup:
  if (deserializer)
    ID3D12RootSignatureDeserializer_Release(deserializer);
  if (blob)
    ID3D10Blob_Release(blob);
  if (error_blob)
    ID3D10Blob_Release(error_blob);

  printf("vkd3d_runtime_rootsig_result=%s\n", pass ? "PASS" : "FAIL");
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

  if (!probe_d3d12_root_signature())
    pass = 0;

  printf("vkd3d_runtime_load_result=%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

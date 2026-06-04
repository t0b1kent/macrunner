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

typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
    ID3DBlob **error_blob);

typedef HRESULT(WINAPI *PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER)(
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

static int probe_d3d12_versioned_root_signature(void) {
  HMODULE module;
  PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize_root_signature;
  PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER create_deserializer;
  D3D12_ROOT_PARAMETER1 parameter;
  D3D12_STATIC_SAMPLER_DESC sampler;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc;
  ID3DBlob *blob = NULL;
  ID3DBlob *error_blob = NULL;
  ID3D12VersionedRootSignatureDeserializer *deserializer = NULL;
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *decoded = NULL;
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *unconverted = NULL;
  HRESULT hr;
  int pass = 1;

  module = GetModuleHandleW(L"d3d12.dll");
  printf("vkd3d_runtime_versioned_semantic module=d3d12.dll handle=%p\n",
         module);
  if (!module)
    return 0;

  serialize_root_signature =
      (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)GetProcAddress(
          module, "D3D12SerializeVersionedRootSignature");
  create_deserializer =
      (PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER)GetProcAddress(
          module, "D3D12CreateVersionedRootSignatureDeserializer");
  if (!serialize_root_signature || !create_deserializer)
    return 0;

  ZeroMemory(&parameter, sizeof(parameter));
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants.ShaderRegister = 3;
  parameter.Constants.RegisterSpace = 2;
  parameter.Constants.Num32BitValues = 4;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  ZeroMemory(&sampler, sizeof(sampler));
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
  sampler.MaxLOD = 16.0f;
  sampler.ShaderRegister = 1;
  sampler.RegisterSpace = 2;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  ZeroMemory(&desc, sizeof(desc));
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 1;
  desc.Desc_1_1.pParameters = &parameter;
  desc.Desc_1_1.NumStaticSamplers = 1;
  desc.Desc_1_1.pStaticSamplers = &sampler;
  desc.Desc_1_1.Flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;

  hr = serialize_root_signature(&desc, &blob, &error_blob);
  printf("vkd3d_runtime_call api=D3D12SerializeVersionedRootSignature hr=0x%08lx blob=%p error=%p\n",
         (unsigned long)hr, blob, error_blob);
  if (FAILED(hr) || !blob) {
    pass = 0;
    goto cleanup;
  }

  hr = create_deserializer(ID3D10Blob_GetBufferPointer(blob),
                           ID3D10Blob_GetBufferSize(blob),
                           &IID_ID3D12VersionedRootSignatureDeserializer,
                           (void **)&deserializer);
  printf("vkd3d_runtime_call api=D3D12CreateVersionedRootSignatureDeserializer hr=0x%08lx deserializer=%p\n",
         (unsigned long)hr, deserializer);
  if (FAILED(hr) || !deserializer) {
    pass = 0;
    goto cleanup;
  }

  hr = ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(
      deserializer, D3D_ROOT_SIGNATURE_VERSION_1_1, &decoded);
  printf("vkd3d_runtime_versioned_rootsig_desc hr=0x%08lx desc=%p\n",
         (unsigned long)hr, decoded);
  if (FAILED(hr) || !decoded) {
    pass = 0;
    goto cleanup;
  }

  unconverted =
      ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(
          deserializer);
  printf("vkd3d_runtime_versioned_rootsig version=%u params=%u samplers=%u flags=0x%08x unconverted=%p\n",
         decoded->Version, decoded->Desc_1_1.NumParameters,
         decoded->Desc_1_1.NumStaticSamplers, decoded->Desc_1_1.Flags,
         unconverted);

  if (decoded->Version != D3D_ROOT_SIGNATURE_VERSION_1_1 ||
      decoded->Desc_1_1.NumParameters != 1 ||
      decoded->Desc_1_1.NumStaticSamplers != 1 ||
      decoded->Desc_1_1.Flags != desc.Desc_1_1.Flags ||
      !decoded->Desc_1_1.pParameters || !decoded->Desc_1_1.pStaticSamplers)
    pass = 0;
  else if (decoded->Desc_1_1.pParameters[0].ParameterType !=
               D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
           decoded->Desc_1_1.pParameters[0].Constants.ShaderRegister != 3 ||
           decoded->Desc_1_1.pParameters[0].Constants.RegisterSpace != 2 ||
           decoded->Desc_1_1.pParameters[0].Constants.Num32BitValues != 4 ||
           decoded->Desc_1_1.pParameters[0].ShaderVisibility !=
               D3D12_SHADER_VISIBILITY_VERTEX ||
           decoded->Desc_1_1.pStaticSamplers[0].ShaderRegister != 1 ||
           decoded->Desc_1_1.pStaticSamplers[0].RegisterSpace != 2 ||
           decoded->Desc_1_1.pStaticSamplers[0].ShaderVisibility !=
               D3D12_SHADER_VISIBILITY_PIXEL)
    pass = 0;

cleanup:
  if (deserializer)
    ID3D12VersionedRootSignatureDeserializer_Release(deserializer);
  if (blob)
    ID3D10Blob_Release(blob);
  if (error_blob)
    ID3D10Blob_Release(error_blob);

  printf("vkd3d_runtime_versioned_rootsig_result=%s\n",
         pass ? "PASS" : "FAIL");
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
  if (!probe_d3d12_versioned_root_signature())
    pass = 0;

  printf("vkd3d_runtime_load_result=%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

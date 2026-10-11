#pragma once
#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12::gowj {

struct DlssEval {
  ID3D12Resource* color = nullptr;   // SRV state, render-size region at the top-left
  ID3D12Resource* depth = nullptr;   // SRV state (inverted depth)
  ID3D12Resource* motion = nullptr;  // SRV state, RG16F pixels, current -> previous
  ID3D12Resource* output = nullptr;  // UAV state
  uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
  float jitter_x = 0, jitter_y = 0;  // render pixels
  bool reset = false;
  bool hdr = false;
  int quality = 5;  // NVSDK_NGX_PerfQuality_Value: 0 MaxPerf, 1 Balanced, 2 MaxQuality, 3 UltraPerf, 4 UltraQuality, 5 DLAA
  float frame_ms = 16.7f;
};

bool DlssCompiledIn();
bool DlssInit(ID3D12Device* device);
bool DlssEvaluate(ID3D12GraphicsCommandList* list, const DlssEval& eval);
void DlssShutdown();

}  // namespace rex::graphics::d3d12::gowj

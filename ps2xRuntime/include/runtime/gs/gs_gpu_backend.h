#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>

// D3D11 compute renderer. GS local memory stays GPU-authoritative until an
// explicit CPU readback; no CPU rasterizer is used by this backend.
class GSGpuBackend final : public GSRasterBackend
{
  public:
    GSGpuBackend();
    ~GSGpuBackend() override;
    void Initialize(uint8_t *, uint32_t) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &) override;
    void BeginTransfer(const GSTransferCommand &) override;
    void UploadImage(const uint8_t *, uint32_t) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason) override;
    PresentationFrame Present(const GSPresentationRequest &) override;
    bool ClearFramebuffer(const GSContext &, uint32_t) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override;
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override;
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override;
    void SnapshotVram(std::vector<uint8_t> &) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

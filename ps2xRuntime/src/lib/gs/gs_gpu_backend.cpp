#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>
#include "Raster.h"
#include "Transfer.h"
#include "Convert.h"
#include "Compose.h"
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT VramBytes = 4 * 1024 * 1024, FramePixels = 640 * 512, OutputBytes = 64 + 5 * FramePixels * 4;
using Params = std::array<uint32_t, 192>;
void check(HRESULT hr, const char *operation)
{
    if (FAILED(hr))
    {
        char text[180];
        std::snprintf(text, sizeof(text), "GS GPU %s failed (0x%08lx)", operation,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
uint32_t rgba(const GSVertex &v)
{
    return v.r | (uint32_t(v.g) << 8) | (uint32_t(v.b) << 16) | (uint32_t(v.a) << 24);
}
// Conservative circular byte ranges in the GS's 4 MiB address space.
struct Range
{
    uint64_t first, last;
};
Range range(uint32_t p, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t w = p == 19 || p == 20 ? 128 : 64, h = p == 20                                              ? 128
                                                    : p == 19 || p == 2 || p == 10 || p == 50 || p == 58 ? 64
                                                                                                         : 32;
    uint64_t begin = uint64_t(base) * 256, end = begin + (uint64_t(y / h) * (bw * 64 / w) + x / w + 1) * 8192;
    return end - begin >= VramBytes ? Range{0, VramBytes} : Range{begin, end};
}
bool overlaps(Range a, Range b)
{
    const Range aa[] = {{a.first, std::min<uint64_t>(a.last, VramBytes)},
                        {0, a.last > VramBytes ? a.last - VramBytes : 0}};
    const Range bb[] = {{b.first, std::min<uint64_t>(b.last, VramBytes)},
                        {0, b.last > VramBytes ? b.last - VramBytes : 0}};
    for (auto x : aa)
        for (auto y : bb)
            if (x.first < x.last && y.first < y.last && x.first < y.last && y.first < x.last)
                return true;
    return false;
}
bool validPsm(uint32_t p)
{
    return GSMem::IsValidPsm(static_cast<GSMem::PixelStorageMode>(p));
}
bool depthAliases(const Params &p, int x0, int y0, int x1, int y1)
{
    if (p[4] > 1 || (p[7] != 48 && p[7] != 49))
        return true;
    // Z32 permutes blocks inside a page. Overlapping page ranges alone do not
    // imply feedback (the menu's small particles use disjoint colour/Z blocks).
    std::bitset<VramBytes / 256> blocks;
    for (int y = y0 & ~7; y <= y1; y += 8)
        for (int x = x0 & ~7; x <= x1; x += 8)
            blocks.set((GSPSMCT32::addrPSMCT32(p[2], p[3], x, y) & (VramBytes - 1)) / 256);
    for (int y = y0 & ~7; y <= y1; y += 8)
        for (int x = x0 & ~7; x <= x1; x += 8)
            if (blocks.test(((GSPSMCT32::addrPSMCT32(p[6], p[3], x, y) ^ 6144u) & (VramBytes - 1)) / 256))
                return true;
    return false;
}
} // namespace

struct GSGpuBackend::Impl
{
    mutable std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> vram, output, upload, constants, readback;
    ComPtr<ID3D11UnorderedAccessView> vramUav, outputUav;
    ComPtr<ID3D11ShaderResourceView> uploadSrv;
    ComPtr<ID3D11ComputeShader> raster, transfer, convert, compose;
    ComPtr<ID3D11Query> fence;
    GSTransferCommand command{};
    GSTransferSnapshot snapshot{};
    std::vector<uint8_t> hostBytes;
    size_t hostOffset = 0;
    UINT outputCapacity = OutputBytes;
    bool initialized = false;

    void create()
    {
        if (device)
            return;
        D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0, actual{};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1, D3D11_SDK_VERSION,
                                &device, &actual, &ctx),
              "create hardware device");
        D3D11_FEATURE_DATA_DOUBLES doubles{};
        check(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles)),
              "query double precision");
        if (!doubles.DoublePrecisionFloatShaderOps)
            throw std::runtime_error("GS GPU requires shader double precision for GS depth interpolation");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        check(device.As(&dxgi), "DXGI device");
        check(dxgi->GetAdapter(&adapter), "adapter");
        check(adapter->GetDesc(&desc), "adapter name");
        std::printf("[gs] D3D11 GPU: %ls (hardware compute)\n", desc.Description);
        auto buffer = [&](UINT size, UINT flags, ComPtr<ID3D11Buffer> &out) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = size;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = flags;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            check(device->CreateBuffer(&d, nullptr, &out), "buffer");
        };
        buffer(VramBytes, D3D11_BIND_UNORDERED_ACCESS, vram);
        buffer(OutputBytes, D3D11_BIND_UNORDERED_ACCESS, output);
        buffer(VramBytes + 4, D3D11_BIND_SHADER_RESOURCE, upload);
        auto uav = [&](ID3D11Buffer *b, UINT size, ComPtr<ID3D11UnorderedAccessView> &out) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = size / 4;
            d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            check(device->CreateUnorderedAccessView(b, &d, &out), "UAV");
        };
        uav(vram.Get(), VramBytes, vramUav);
        uav(output.Get(), OutputBytes, outputUav);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements = (VramBytes + 4) / 4;
        srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        check(device->CreateShaderResourceView(upload.Get(), &srv, &uploadSrv), "upload SRV");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Params);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&cb, nullptr, &constants), "constants");
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = std::max(VramBytes, OutputBytes);
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateBuffer(&staging, nullptr, &readback), "readback");
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        check(device->CreateQuery(&query, &fence), "fence");
        check(device->CreateComputeShader(kGsRaster, sizeof(kGsRaster), nullptr, &raster), "raster shader");
        check(device->CreateComputeShader(kGsTransfer, sizeof(kGsTransfer), nullptr, &transfer),
              "transfer shader");
        check(device->CreateComputeShader(kGsConvert, sizeof(kGsConvert), nullptr, &convert),
              "conversion shader");
        check(device->CreateComputeShader(kGsCompose, sizeof(kGsCompose), nullptr, &compose),
              "composition shader");
    }
    void dispatch(ID3D11ComputeShader *shader, const Params &p, UINT x, UINT y = 1)
    {
        if (!x || !y)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "map constants");
        std::memcpy(mapped.pData, p.data(), sizeof(p));
        ctx->Unmap(constants.Get(), 0);
        ID3D11Buffer *cb = constants.Get();
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ID3D11UnorderedAccessView *uavs[] = {vramUav.Get(), outputUav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ID3D11ShaderResourceView *srv = uploadSrv.Get();
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->Dispatch(x, y, 1);
    }
    void read(ID3D11Buffer *source, UINT offset, void *data, UINT size)
    {
        D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
        ctx->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, source, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
        std::memcpy(data, mapped.pData, size);
        ctx->Unmap(readback.Get(), 0);
    }
    void ensureOutput(UINT bytes)
    {
        if (bytes <= outputCapacity)
            return;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ComPtr<ID3D11Buffer> buffer, staging;
        ComPtr<ID3D11UnorderedAccessView> view;
        check(device->CreateBuffer(&d, nullptr, &buffer), "large transfer output");
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = bytes / 4;
        u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        check(device->CreateUnorderedAccessView(buffer.Get(), &u, &view), "large transfer UAV");
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateBuffer(&d, nullptr, &staging), "large transfer staging");
        output = std::move(buffer);
        outputUav = std::move(view);
        readback = std::move(staging);
        outputCapacity = bytes;
    }
    void wait()
    {
        ctx->End(fence.Get());
        ctx->Flush();
        HRESULT hr;
        while ((hr = ctx->GetData(fence.Get(), nullptr, 0, 0)) == S_FALSE)
            Sleep(0);
        check(hr, "GPU completion");
        check(device->GetDeviceRemovedReason(), "device status");
    }
    Params transferParams(uint32_t op) const
    {
        Params p{};
        auto &b = command.bitbltbuf;
        auto &t = command.trxpos;
        p[0] = op;
        p[1] = command.trxreg.rrw;
        p[2] = command.trxreg.rrh;
        p[3] = b.sbp;
        p[4] = std::max(1u, uint32_t(b.sbw));
        p[5] = b.spsm;
        p[6] = t.ssax;
        p[7] = t.ssay;
        p[8] = b.dpsm;
        p[9] = b.dbp;
        p[10] = std::max(1u, uint32_t(b.dbw));
        p[11] = t.dsax;
        p[12] = t.dsay;
        p[14] = t.dir;
        return p;
    }
    void transferDispatch(Params p, uint32_t start, uint32_t count, bool serial)
    {
        if (!p[1] || !count)
            return;
        p[19] = start;
        if (serial)
        {
            p[18] = 1;
            for (uint32_t i = 0; i < count; i += 1024)
            {
                p[15] = start + i;
                p[13] = std::min(1024u, count - i);
                dispatch(transfer.Get(), p, 1);
            }
        }
        else
        {
            constexpr uint32_t limit = 65535 * 64;
            for (uint32_t i = 0; i < count; i += limit)
            {
                p[15] = start + i;
                p[13] = std::min(limit, count - i);
                dispatch(transfer.Get(), p, (p[13] + 63) / 64);
            }
        }
    }
};

GSGpuBackend::GSGpuBackend() : m(std::make_unique<Impl>())
{
}
GSGpuBackend::~GSGpuBackend() = default;
void GSGpuBackend::Initialize(uint8_t *vram, uint32_t size)
{
    std::lock_guard lock(m->mutex);
    if (!vram || !size)
    {
        m->initialized = false;
        return;
    }
    if (size != VramBytes)
        throw std::runtime_error("GS GPU expects 4 MiB local memory");
    m->create();
    m->ctx->UpdateSubresource(m->vram.Get(), 0, nullptr, vram, 0, 0);
    m->initialized = true;
    m->command = {};
    m->snapshot = {};
    m->hostBytes.clear();
    m->hostOffset = 0;
}
void GSGpuBackend::Reset()
{
    std::lock_guard lock(m->mutex);
    m->command = {};
    m->snapshot = {};
    m->hostBytes.clear();
    m->hostOffset = 0;
}
void GSGpuBackend::Flush()
{
    std::lock_guard lock(m->mutex);
    if (m->initialized)
        m->ctx->Flush();
}
void GSGpuBackend::TextureFlush()
{
} // Every draw reads coherent GPU local memory.
void GSGpuBackend::Sync(GSSyncReason reason)
{
    std::lock_guard lock(m->mutex);
    if (m->initialized && (reason == GSSyncReason::Finish || reason == GSSyncReason::Reset))
        m->wait();
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &b)
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized || !b.vertexCount || b.state.prim.type > GS_PRIM_SPRITE)
        return;
    const auto &s = b.state;
    const auto &c = s.context;
    const auto &t = c.tex0;
    if (!validPsm(c.frame.psm) || !validPsm(c.zbuf.psm))
        throw std::runtime_error("GS GPU invalid framebuffer/depth format");
    Params p{};
    p[0] = s.prim.type;
    p[1] = uint32_t(s.prim.iip) | s.prim.tme << 1 | s.prim.fge << 2 | s.prim.abe << 3 | s.prim.fst << 4 |
           s.linearFilter << 5 | s.pabe << 6;
    p[2] = c.frame.fbp * 32;
    p[3] = std::max(1u, c.frame.fbw);
    p[4] = c.frame.psm;
    p[5] = c.frame.fbmsk;
    p[6] = c.zbuf.zbp * 32;
    p[7] = c.zbuf.psm;
    p[8] = c.zbuf.zmask;
    p[9] = uint32_t(c.test);
    p[10] = uint32_t(c.alpha);
    p[11] = uint32_t(c.alpha >> 32) & 255;
    p[12] = c.fba & 1;
    p[13] = s.fogR | (uint32_t(s.fogG) << 8) | (uint32_t(s.fogB) << 16);
    p[14] = c.xyoffset.ofx >> 4;
    p[15] = c.xyoffset.ofy >> 4;
    p[16] = c.scissor.x0;
    p[17] = c.scissor.x1;
    p[18] = c.scissor.y0;
    p[19] = c.scissor.y1;
    p[20] = t.tbp0;
    p[21] = t.tbw;
    p[22] = t.psm;
    p[23] = s.textureWidth;
    p[24] = s.textureHeight;
    p[25] = t.tcc;
    p[26] = t.tfx;
    p[27] = t.cbp;
    p[28] = t.cpsm;
    p[29] = t.csm;
    p[30] = t.csa;
    p[31] = s.texclut.cbw;
    p[32] = s.texclut.cou;
    p[33] = s.texclut.cov;
    p[34] = s.texa.ta0;
    p[35] = s.texa.aem;
    p[36] = s.texa.ta1;
    p[37] = c.clamp & 3;
    p[38] = (c.clamp >> 2) & 3;
    p[39] = (c.clamp >> 4) & 1023;
    p[40] = (c.clamp >> 14) & 1023;
    p[41] = (c.clamp >> 24) & 1023;
    p[42] = (c.clamp >> 34) & 1023;
    for (uint32_t i = 0; i < 3; ++i)
    {
        auto &v = b.vertices[i];
        uint32_t o = 64 + i * 16;
        p[o] = std::bit_cast<uint32_t>(v.x);
        p[o + 1] = std::bit_cast<uint32_t>(v.y);
        std::memcpy(&p[o + 2], &v.z, 8);
        p[o + 4] = rgba(v);
        p[o + 5] = std::bit_cast<uint32_t>(v.q);
        p[o + 6] = std::bit_cast<uint32_t>(v.s);
        p[o + 7] = std::bit_cast<uint32_t>(v.t);
        p[o + 8] = v.u;
        p[o + 9] = v.v;
        p[o + 10] = v.fog;
    }
    int x0, y0, x1, y1;
    if (p[0] == 6 || p[0] == 0)
    {
        x0 = int(b.vertices[0].x) - int(p[14]);
        y0 = int(b.vertices[0].y) - int(p[15]);
        x1 = x0;
        y1 = y0;
        if (p[0] == 6)
        {
            x1 = int(b.vertices[1].x) - int(p[14]);
            y1 = int(b.vertices[1].y) - int(p[15]);
            if (x0 > x1)
                std::swap(x0, x1);
            if (y0 > y1)
                std::swap(y0, y1);
            x1 = x0 + std::max(1, x1 - x0) - 1;
            y1 = y0 + std::max(1, y1 - y0) - 1;
        }
    }
    else
    {
        uint32_t n = p[0] <= 2 ? 2 : 3;
        float lx = b.vertices[0].x, hx = lx, ly = b.vertices[0].y, hy = ly;
        for (uint32_t i = 1; i < n; ++i)
        {
            lx = std::min(lx, b.vertices[i].x);
            hx = std::max(hx, b.vertices[i].x);
            ly = std::min(ly, b.vertices[i].y);
            hy = std::max(hy, b.vertices[i].y);
        }
        x0 = int(std::floor(lx)) - p[14];
        x1 = int(std::ceil(hx)) - p[14];
        y0 = int(std::floor(ly)) - p[15];
        y1 = int(std::ceil(hy)) - p[15];
    }
    if (x1 < int(p[16]) || x0 > int(p[17]) || y1 < int(p[18]) || y0 > int(p[19]))
        return;
    x0 = std::max(x0, int(p[16]));
    x1 = std::min(x1, int(p[17]));
    y0 = std::max(y0, int(p[18]));
    y1 = std::min(y1, int(p[19]));
    if (x1 < x0 || y1 < y0)
        return;
    p[44] = x0;
    p[45] = y0;
    p[46] = x1 - x0 + 1;
    p[47] = y1 - y0 + 1;
    Range frame = range(p[4], p[2], p[3], x1, y1), depth = range(p[7], p[6], p[3], x1, y1);
    bool zUsed = !p[8] || ((p[9] >> 17) & 3) >= 2;
    bool serial = x1 >= int(p[3] * 64) || frame.last >= VramBytes;
    bool depthFeedback = zUsed && overlaps(frame, depth) && depthAliases(p, x0, y0, x1, y1);
    bool orderedPages = !serial && depthFeedback && p[2] == p[6] && p[4] <= 1 && (p[7] == 48 || p[7] == 49);
    serial |= depthFeedback;
    if (s.prim.tme)
    {
        uint32_t tx = p[37] >= 2 ? std::max(p[39], p[40]) | p[40] : p[23] - 1,
                 ty = p[38] >= 2 ? std::max(p[41], p[42]) | p[42] : p[24] - 1;
        Range texture = range(p[22], p[20], p[21], tx, ty);
        bool feedback = overlaps(frame, texture) || (zUsed && overlaps(depth, texture));
        if (GSMem::IsPaletted(static_cast<GSMem::PixelStorageMode>(p[22])))
        {
            Range palette = range(p[28], p[27], std::max(1u, p[31]), p[32] + 15, p[33] + 31);
            feedback |= overlaps(frame, palette) || (zUsed && overlaps(depth, palette));
        }
        serial |= feedback;
        orderedPages &= !feedback;
    }
    if (p[0] == 1 || p[0] == 2)
        m->dispatch(m->raster.Get(), p, 1);
    else if (orderedPages)
    {
        p[48] = 2;
        p[49] = uint32_t(x1 / 64 - x0 / 64 + 1);
        p[50] = uint32_t(y1 / 32 - y0 / 32 + 1);
        m->dispatch(m->raster.Get(), p, (p[49] * p[50] + 63) / 64);
    }
    else if (serial)
    {
        p[48] = 1;
        uint32_t count = p[46] * p[47];
        for (uint32_t i = 0; i < count; i += 1024)
        {
            p[49] = i;
            p[50] = std::min(1024u, count - i);
            m->dispatch(m->raster.Get(), p, 1);
        }
    }
    else
        m->dispatch(m->raster.Get(), p, (p[46] * p[47] + 63) / 64);
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard lock(m->mutex);
    m->command = command;
    m->snapshot = {command.trxpos.dsax,
                   command.trxpos.dsay,
                   uint32_t(command.trxreg.rrw) * command.trxreg.rrh,
                   0,
                   command.direction,
                   0};
    if (!m->initialized)
        return;
    uint32_t total = m->snapshot.totalPixels;
    if (command.direction == 2)
    {
        auto p = m->transferParams(1);
        bool serial = overlaps(range(p[5], p[3], p[4], p[6] + p[1] - 1, p[7] + p[2] - 1),
                               range(p[8], p[9], p[10], p[11] + p[1] - 1, p[12] + p[2] - 1)) ||
                      p[11] + p[1] > p[10] * 64;
        m->transferDispatch(p, 0, total, serial);
        m->snapshot.copiedPixels = total;
        m->snapshot.direction = 3;
    }
    else if (command.direction == 1)
    {
        uint32_t n =
            uint32_t(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(command.bitbltbuf.spsm)));
        size_t bytes = (size_t(total) * n + 7) / 8;
        if (bytes > UINT32_MAX - 3u)
            throw std::runtime_error("GS GPU transfer size exceeds D3D11 buffer addressing");
        m->ensureOutput(UINT((bytes + 3) & ~size_t(3)));
        const UINT zero[4]{};
        m->ctx->ClearUnorderedAccessViewUint(m->outputUav.Get(), zero);
        m->transferDispatch(m->transferParams(2), 0, total, false);
        m->hostBytes.resize((bytes + 3) & ~size_t(3));
        m->hostOffset = 0;
        if (bytes)
            m->read(m->output.Get(), 0, m->hostBytes.data(), UINT(m->hostBytes.size()));
        m->hostBytes.resize(bytes);
        m->snapshot.copiedPixels = total;
        m->snapshot.localToHostPendingBytes = bytes;
    }
}
void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t size)
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized || !data || !size || m->snapshot.direction != 0 || !m->command.trxreg.rrw)
        return;
    uint32_t n =
        uint32_t(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(m->command.bitbltbuf.dpsm)));
    uint32_t count =
        std::min(m->snapshot.totalPixels - m->snapshot.copiedPixels, uint32_t(uint64_t(size) * 8 / n));
    if (!count)
        return;
    uint32_t bytes = (count * n + 7) / 8;
    if (bytes > VramBytes)
        throw std::runtime_error("GS GPU upload chunk exceeds 4 MiB");
    std::vector<uint8_t> padded((bytes + 7) & ~3u);
    std::memcpy(padded.data(), data, bytes);
    D3D11_BOX box{0, 0, 0, UINT(padded.size()), 1, 1};
    m->ctx->UpdateSubresource(m->upload.Get(), 0, &box, padded.data(), 0, 0);
    auto p = m->transferParams(0);
    const auto dstRange = range(p[8], p[9], p[10], p[11] + p[1] - 1, p[12] + p[2] - 1);
    bool serial = p[11] + p[1] > p[10] * 64 || dstRange.last - dstRange.first >= VramBytes ||
                  ((p[8] == 19 || p[8] == 20) && p[10] < 2 && p[12] + p[2] > (p[8] == 20 ? 128u : 64u));
    m->transferDispatch(p, m->snapshot.copiedPixels, count, serial);
    m->snapshot.copiedPixels += count;
    if (m->snapshot.copiedPixels == m->snapshot.totalPixels)
    {
        m->snapshot.direction = 3;
        m->snapshot.totalPixels = 0;
    }
    else
    {
        m->snapshot.x = m->command.trxpos.dsax + m->snapshot.copiedPixels % m->command.trxreg.rrw;
        m->snapshot.y = m->command.trxpos.dsay + m->snapshot.copiedPixels / m->command.trxreg.rrw;
    }
}
uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t size)
{
    std::lock_guard lock(m->mutex);
    if (!dst)
        return 0;
    size = uint32_t(std::min<size_t>(size, m->hostBytes.size() - m->hostOffset));
    std::memcpy(dst, m->hostBytes.data() + m->hostOffset, size);
    m->hostOffset += size;
    m->snapshot.localToHostPendingBytes = m->hostBytes.size() - m->hostOffset;
    return size;
}
GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    std::lock_guard lock(m->mutex);
    return m->snapshot;
}
uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized || !validPsm(psm))
        return 0;
    Params p{};
    p[0] = 3;
    p[1] = 1;
    p[3] = base;
    p[4] = bw;
    p[5] = psm;
    p[6] = x;
    p[7] = y;
    p[13] = 1;
    m->dispatch(m->transfer.Get(), p, 1);
    uint32_t v;
    m->read(m->output.Get(), 0, &v, 4);
    return v;
}
void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t v)
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized || !validPsm(psm))
        return;
    Params p{};
    p[0] = 4;
    p[1] = 1;
    p[8] = psm;
    p[9] = base;
    p[10] = bw;
    p[11] = x;
    p[12] = y;
    p[13] = 1;
    p[16] = v;
    m->dispatch(m->transfer.Get(), p, 1);
}
void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized)
    {
        out.clear();
        return;
    }
    out.resize(VramBytes);
    m->read(m->vram.Get(), 0, out.data(), VramBytes);
}
bool GSGpuBackend::ClearFramebuffer(const GSContext &c, uint32_t color)
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized || !c.frame.fbw)
        return false;
    if (c.frame.psm != 0 && c.frame.psm != 1 && c.frame.psm != 2 && c.frame.psm != 10)
        return false;
    if ((c.fba & 1) && c.frame.psm != 1)
        color |= 0x80000000;
    if (c.frame.psm == 2 || c.frame.psm == 10)
        color = ((color >> 3) & 31) | ((color >> 6) & 0x3e0) | ((color >> 9) & 0x7c00) |
                ((color >> 24) >= 0x40 ? 0x8000 : 0);
    Params p{};
    p[0] = 5;
    p[1] = std::max(c.scissor.x0, c.scissor.x1) - c.scissor.x0 + 1;
    p[2] = std::max(c.scissor.y0, c.scissor.y1) - c.scissor.y0 + 1;
    p[8] = c.frame.psm;
    p[9] = c.frame.fbp * 32;
    p[10] = c.frame.fbw;
    p[11] = c.scissor.x0;
    p[12] = c.scissor.y0;
    p[16] = color;
    p[17] = c.frame.fbmsk;
    m->transferDispatch(p, 0, p[1] * p[2], p[11] + p[1] > p[10] * 64);
    return true;
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &r)
{
    std::lock_guard lock(m->mutex);
    if (!m->initialized)
        return {};
    struct Source
    {
        GSFrameReg frame;
        uint32_t x, y, w, h;
        bool valid, preferred = false;
    };
    auto decode = [&](uint64_t f, uint64_t d, bool enabled) {
        Source s{{uint32_t(f & 511), uint32_t((f >> 9) & 63), uint8_t((f >> 15) & 31), 0},
                 uint32_t((f >> 32) & 2047),
                 uint32_t((f >> 43) & 2047),
                 uint32_t(((d >> 32) & 4095) + 1) / uint32_t(((d >> 23) & 15) + 1),
                 uint32_t(((d >> 44) & 2047) + 1),
                 false};
        s.valid = enabled && (s.frame.fbw || ((d >> 32) & 4095) || ((d >> 44) & 2047) || ((d >> 23) & 15));
        if (s.w < 64 || s.h < 64)
        {
            s.w = 640;
            s.h = 448;
        }
        s.w = std::min(s.w, 640u);
        s.h = std::min(s.h, 512u);
        return s;
    };
    Source a = decode(r.dispfb1, r.display1, r.pmode & 1), b = decode(r.dispfb2, r.display2, r.pmode & 2);
    if (!a.valid && !b.valid)
        return {};
    const UINT zero[4]{};
    m->ctx->ClearUnorderedAccessViewUint(m->outputUav.Get(), zero);
    bool dual = a.valid && b.valid;
    auto convert = [&](Source &s, uint32_t slot, bool allowPreferred) {
        uint32_t base = s.frame.fbp * 32;
        if (allowPreferred && r.hasPreferredSource && r.preferredDestFbp == s.frame.fbp &&
            (r.preferredSource.fbw || r.preferredSource.fbp != s.frame.fbp))
        {
            s.frame = r.preferredSource;
            base = s.frame.fbp;
            s.x = s.y = 0;
            s.preferred = true;
        }
        auto run = [&](uint32_t ptr) {
            Params p{};
            p[0] = ptr;
            p[1] = s.frame.fbw ? s.frame.fbw : 10;
            p[2] = s.frame.psm;
            p[3] = s.w;
            p[4] = s.h;
            p[5] = s.x;
            p[6] = s.y;
            p[7] = slot;
            m->dispatch(m->convert.Get(), p, (s.w + 7) / 8, (s.h + 7) / 8);
        };
        run(base);
        if (!s.preferred && s.frame.fbp == 0)
        {
            uint32_t count = 0;
            m->read(m->output.Get(), slot * 4, &count, 4);
            if (!count)
                for (auto &candidate : r.contextFrames)
                {
                    if (candidate.fbp == s.frame.fbp && candidate.fbw == s.frame.fbw &&
                        candidate.psm == s.frame.psm)
                        continue;
                    Source original = s;
                    s.frame = candidate;
                    s.x = s.y = 0;
                    run(candidate.fbp * 32);
                    m->read(m->output.Get(), slot * 4, &count, 4);
                    if (count)
                        break;
                    s = original;
                }
        }
    };
    uint32_t displayFbp = a.valid ? a.frame.fbp : b.frame.fbp;
    if (a.valid)
        convert(a, 0, !dual);
    if (b.valid)
        convert(b, 1, !dual);
    Source &first = a.valid ? a : b;
    Params p{};
    p[0] = dual ? std::max(a.w, b.w) : first.w;
    p[1] = dual ? std::max(a.h, b.h) : first.h;
    p[2] = uint32_t(r.smode2);
    p[3] = r.vsyncTick & 1;
    p[4] = dual;
    p[5] = a.valid ? 0 : 1;
    p[6] = 1;
    p[7] = a.w;
    p[8] = b.w;
    p[9] = b.h;
    p[10] = a.h;
    p[11] = uint32_t(r.pmode);
    p[12] = uint32_t(r.bgcolor) & 0xffffff;
    m->dispatch(m->compose.Get(), p, (p[0] + 7) / 8, (p[1] + 7) / 8);
    PresentationFrame result{};
    result.width = p[0];
    result.height = p[1];
    result.displayFbp = displayFbp;
    result.sourceFbp = first.frame.fbp;
    result.usedPreferred = first.preferred;
    result.pixels.resize(FramePixels * 4);
    m->read(m->output.Get(), 64 + 4 * FramePixels * 4, result.pixels.data(), FramePixels * 4);
    return result;
}

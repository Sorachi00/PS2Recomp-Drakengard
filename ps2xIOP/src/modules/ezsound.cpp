#include "module_factories.h"

#include <array>
#include <cstdint>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kEzSoundSid = 0x04649765u;

        class EzSoundService  final : public IopService
        {
        public:
            explicit EzSoundService(IopHost& host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "ezsound";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
                m_transferCode = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest& request) override
            {
                if (request.sid != kEzSoundSid)
                {
                    return {};
                }


                (void)m_host.readGuest(request.send.address + 4u, &m_transferCode, sizeof(m_transferCode));
                (void)m_host.writeGuest(request.receive.address, &m_transferCode, sizeof(m_transferCode));
   
                RpcResult result;
                result.handled = true;
                result.resultAddress = request.receive.address;
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{ kEzSoundSid };

            IopHost& m_host;
            uint32_t m_transferCode = 0u;
        };
    }

    std::unique_ptr<IopService> createEzSoundService(IopHost& host)
    {
        return std::make_unique<EzSoundService>(host);
    }
}

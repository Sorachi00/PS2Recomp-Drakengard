#include "module_factories.h"

#include <array>
#include <cstdint>
#include <cstddef>

//This is the template that other services uses like libsd
namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t CD_SERVER_SEARCHFILE = 0x80000597u;
        //from the sdk we have cdvdfsv_rpc4_sz12c_inpacket_

        typedef struct cdvdfsv_rpc4_sz12c_inpacket_
        {
            sceCdlFILE m_fp;
            int32_t m_file_attributes;
            char m_path[256];
            uint32_t m_eedest;
            int32_t m_layer;
        } cdvdfsv_rpc4_sz12c_inpacket_t;

        typedef struct cdvdfsv_rpc4_sz128_inpacket_
        {
            sceCdlFILE m_fp;
            int32_t m_file_attributes;
            char m_path[256];
            uint32_t m_eedest;
        } cdvdfsv_rpc4_sz128_inpacket_t;

        typedef struct cdvdfsv_rpc4_sz124_inpacket_
        {
            sceCdlFILE m_fp;
            char m_path[256];
            uint32_t m_eedest;
        } cdvdfsv_rpc4_sz124_inpacket_t;

        typedef union cdvdfsv_rpc4_inpacket_
        {
            cdvdfsv_rpc4_sz12c_inpacket_t m_pkt_sz12c;
            cdvdfsv_rpc4_sz128_inpacket_t m_pkt_sz128;
            cdvdfsv_rpc4_sz124_inpacket_t m_pkt_sz124;
        } cdvdfsv_rpc4_inpacket_t;

        typedef struct cdvdfsv_rpc4_outpacket_
        {
            int32_t m_retres;
            int32_t m_padding[3];
        } cdvdfsv_rpc4_outpacket_t;

        static cdvdfsv_rpc4_outpacket_t g_cdvdfsv_srchres;
        static int g_rpc_buffer4[76];

        class CdvdService final : public IopService
        {
        public:
            explicit CdvdService(IopHost& host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "cdvd";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
            }

            [[nodiscard]] RpcResult handleRpc(
                const RpcRequest& request) override
            {
                if (request.sid != CD_SERVER_SEARCHFILE)
                {
                    return {};
                }

                //this is an adaptation of the sdk

                (void)request.function; //(void)fno;
                if(!m_host.readGuest(request.send.address, g_rpc_buffer4,request.send.size))
                    return{};

                cdvdfsv_rpc4_inpacket_t* inbuf = reinterpret_cast<cdvdfsv_rpc4_inpacket_t*>(g_rpc_buffer4);
                RpcResult result{};
                switch (request.send.size)
                {
                    case sizeof(inbuf->m_pkt_sz12c) :
                        g_cdvdfsv_srchres.m_retres = m_host.sceCdLayerSearchFile(&inbuf->m_pkt_sz12c.m_fp, inbuf->m_pkt_sz12c.m_path, inbuf->m_pkt_sz12c.m_layer);
                        (void)m_host.writeGuest(inbuf->m_pkt_sz12c.m_eedest, g_rpc_buffer4, sizeof(sceCdlFILE) + 4);
                        break;
                    case sizeof(inbuf->m_pkt_sz128) :
                        (void)inbuf->m_pkt_sz128;
                        g_cdvdfsv_srchres.m_retres = 0;
                        return result;
                    default:
                        (void)inbuf->m_pkt_sz124;
                        g_cdvdfsv_srchres.m_retres = 0;
                        return result;
                }


                (void)m_host.writeGuest(
                    request.receive.address,
                    &g_cdvdfsv_srchres,
                    request.receive.size);


                result.handled = true;
                result.resultAddress = request.receive.address;

                result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;

                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{ CD_SERVER_SEARCHFILE };

            IopHost& m_host;
        };
    }

    std::unique_ptr<IopService> createCdvdService(IopHost& host)
    {
        return std::make_unique<CdvdService>(host);
    }
}
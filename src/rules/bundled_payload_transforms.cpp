#include <streamview/rules/payload_transform.h>

#include <streamview/rules/h264_rbsp_payload_transform_provider.h>

#include <QtGlobal>

#include <memory>

namespace streamview::rules {
namespace {

struct BundledPayloadTransforms final {
    BundledPayloadTransforms() {
        const bool registered = registry.registerProvider(
            std::make_shared<H264RbspPayloadTransformProvider>());
        Q_ASSERT(registered);
        (void)registered;
    }

    PayloadTransformRegistry registry;
};

} // namespace

const PayloadTransformRegistry& bundledPayloadTransformRegistry() {
    static const BundledPayloadTransforms transforms;
    return transforms.registry;
}

} // namespace streamview::rules

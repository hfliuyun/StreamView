#pragma once

#include <streamview/rules/payload_transform.h>

namespace streamview::rules {

class H264RbspPayloadTransformProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("rbsp");
    }

    [[nodiscard]] PayloadTransformResult transform(
        const PayloadTransformRequest& request) const override;
};

} // namespace streamview::rules

#include <streamview/rules/payload_transform.h>

#include <limits>

namespace streamview::rules {

PayloadTransformResult IdentityPayloadTransformProvider::transform(
    const PayloadTransformRequest& request) const {
    PayloadTransformResult result;

    if (request.source == nullptr) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Source is null");
        return result;
    }
    if (request.inputMapping == nullptr) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Input mapping is null");
        return result;
    }
    if ((request.logicalBitStart % 8U) != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Logical bit start is not byte-aligned");
        return result;
    }
    if ((request.logicalBitLength % 8U) != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Logical bit length is not byte-aligned");
        return result;
    }
    if (request.logicalBitLength == 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Logical bit length is zero");
        return result;
    }

    const quint64 inputBitLength = request.inputMapping->logicalBitLength();
    if (request.logicalBitStart > inputBitLength) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Logical bit start is out of range");
        return result;
    }
    if (request.logicalBitLength > (inputBitLength - request.logicalBitStart)) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Logical range exceeds input mapping length");
        return result;
    }

    if (request.cancellation && request.cancellation->isCancellationRequested()) {
        result.status = DslExecutionStatus::Cancelled;
        result.errorMessage = QStringLiteral("Transform was cancelled before starting");
        return result;
    }

    const quint64 byteLength = request.logicalBitLength / 8U;
    if (request.maximumInspectedBytes > 0 && byteLength > request.maximumInspectedBytes) {
        result.status = DslExecutionStatus::ResourceLimit;
        result.inspectedByteCount = request.maximumInspectedBytes;
        result.errorMessage = QStringLiteral("Inspection budget exceeded");
        return result;
    }

    const auto logicalAddress = core::LogicalBitAddress(
        request.inputMapping->viewId(), request.logicalBitStart);
    const auto logicalRange = core::LogicalRange::create(logicalAddress, request.logicalBitLength);
    if (!logicalRange) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Invalid logical range coordinates");
        return result;
    }
    const auto location = request.inputMapping->locate(*logicalRange);
    if (!location) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Failed to locate logical range in input mapping");
        return result;
    }

    auto mappingOpt = core::SourceMapping::create(
        request.inputMapping->viewId(), location->sourceSpans());
    if (!mappingOpt) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Failed to create forwarded mapping");
        return result;
    }

    result.status = DslExecutionStatus::Materialized;
    result.forwardedMapping = std::move(mappingOpt);
    result.inspectedByteCount = byteLength;
    return result;
}

PayloadTransformRegistry::PayloadTransformRegistry() {
    registerBuiltins();
}

PayloadTransformRegistry& PayloadTransformRegistry::instance() {
    static PayloadTransformRegistry registry;
    return registry;
}

void PayloadTransformRegistry::registerBuiltins() {
    providers_[QStringLiteral("none")] = std::make_shared<IdentityPayloadTransformProvider>();
}

bool PayloadTransformRegistry::registerProvider(
    std::shared_ptr<const PayloadTransformProvider> provider) {
    if (!provider) {
        return false;
    }
    const QString id = provider->identifier().trimmed();
    if (id.isEmpty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (providers_.find(id) != providers_.end()) {
        return false;
    }
    providers_.emplace(id, std::move(provider));
    return true;
}

bool PayloadTransformRegistry::unregisterProvider(const QString& identifier) {
    const QString id = identifier.trimmed();
    if (id == QStringLiteral("none")) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.erase(id) > 0;
}

std::shared_ptr<const PayloadTransformProvider>
PayloadTransformRegistry::findProvider(const QString& identifier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = providers_.find(identifier.trimmed());
    if (it != providers_.end()) {
        return it->second;
    }
    return nullptr;
}

void PayloadTransformRegistry::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.clear();
    registerBuiltins();
}

} // namespace streamview::rules

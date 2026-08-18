#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_vm.h>

#include <QString>
#include <QtGlobal>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace streamview::rules {

struct PayloadExcludedSpan final {
    core::SourceSpan sourceSpan;
    quint64 outputBitOffset = 0;
};

struct PayloadTransformRequest final {
    const core::RandomAccessSource* source = nullptr;
    const core::SourceMapping* inputMapping = nullptr;
    quint64 logicalBitStart = 0;
    quint64 logicalBitLength = 0;
    std::optional<core::CancellationToken> cancellation;
    quint64 maximumInspectedBytes = 0;
};

struct PayloadTransformResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::optional<core::SourceMapping> forwardedMapping;
    std::vector<PayloadExcludedSpan> excludedSpans;
    quint64 inspectedByteCount = 0;
    std::vector<core::ParseDiagnostic> diagnostics;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == DslExecutionStatus::Materialized && forwardedMapping.has_value();
    }
};

class PayloadTransformProvider {
public:
    virtual ~PayloadTransformProvider() = default;

    [[nodiscard]] virtual QString identifier() const = 0;
    [[nodiscard]] virtual PayloadTransformResult transform(
        const PayloadTransformRequest& request) const = 0;
};

class IdentityPayloadTransformProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("none");
    }

    [[nodiscard]] PayloadTransformResult transform(
        const PayloadTransformRequest& request) const override;
};

class PayloadTransformRegistry final {
public:
    [[nodiscard]] static PayloadTransformRegistry& instance();

    /// Register a provider. Returns false if provider is null, identifier is empty, or already registered.
    bool registerProvider(std::shared_ptr<const PayloadTransformProvider> provider);

    /// Unregister a provider. Built-in "none" cannot be unregistered (returns false).
    bool unregisterProvider(const QString& identifier);

    /// Look up provider by identifier. Returns nullptr if not found.
    [[nodiscard]] std::shared_ptr<const PayloadTransformProvider>
    findProvider(const QString& identifier) const;

    /// Reset registry back to built-ins only.
    void reset();

    PayloadTransformRegistry();

private:
    void registerBuiltins();

    mutable std::mutex mutex_;
    std::unordered_map<QString, std::shared_ptr<const PayloadTransformProvider>> providers_;
};

} // namespace streamview::rules

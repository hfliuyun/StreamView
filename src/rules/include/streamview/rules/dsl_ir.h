#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/rules/dsl.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::rules {

enum class DslValueTypeKind : quint8 {
    UnsignedBits,
};

struct DslValueType final {
    DslValueTypeKind kind = DslValueTypeKind::UnsignedBits;
    quint8 bitWidth = 0;
};

struct DslTypedField final {
    QString name;
    DslValueType type;
    std::optional<quint64> equalsConstraint;
    core::AnalysisNodeMetadata metadata;
    DslSourceRange range;
};

struct DslTypedStruct final {
    QString name;
    core::AnalysisNodeMetadata metadata;
    std::vector<DslTypedField> fields;
    quint32 bytecodeOffset = 0;
    quint32 bytecodeLength = 0;
};

enum class DslScannerKind : quint8 {
    H264StartCode,
};

struct DslTypedScan final {
    QString name;
    quint32 elementStructIndex = 0;
    DslScannerKind scanner = DslScannerKind::H264StartCode;
    DslSourceRange range;
};

enum class DslEntryKind : quint8 {
    None,
    Structure,
    Sequence,
};

struct DslTypedEntry final {
    DslEntryKind kind = DslEntryKind::None;
    quint32 targetIndex = 0;
};

enum class DslOpcode : quint8 {
    BeginStructure,
    ReadUnsignedBits,
    AssertEquals,
    EndStructure,
};

struct DslInstruction final {
    DslOpcode opcode = DslOpcode::BeginStructure;
    quint32 operand = 0;
    quint64 immediate = 0;
};

struct DslTypedProgram final {
    std::vector<DslTypedStruct> structs;
    std::vector<DslTypedScan> scans;
    std::vector<DslInstruction> bytecode;
    DslTypedEntry entry;

    [[nodiscard]] std::optional<quint32> structureIndex(const QString& name) const;
    [[nodiscard]] std::optional<quint32> scanIndex(const QString& name) const;
};

struct DslCompileResult final {
    std::optional<DslTypedProgram> program;
    std::vector<DslDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return diagnostics.empty() && program.has_value();
    }
};

class DslCompiler final {
public:
    [[nodiscard]] static DslCompileResult compile(const DslProgram& program);
};

} // namespace streamview::rules

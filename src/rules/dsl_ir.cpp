#include <streamview/rules/dsl_ir.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

void addDiagnostic(std::vector<DslDiagnostic>& diagnostics,
                   DslDiagnosticCode code,
                   const QString& message,
                   const DslSourceRange& range) {
    diagnostics.push_back({code, message, range});
}

[[nodiscard]] core::AnalysisNodeMetadata metadataForAnnotations(
    const std::vector<DslAnnotation>& annotations,
    std::optional<core::AnalysisSpecification> inheritedSpecification = std::nullopt) {
    core::AnalysisNodeMetadata metadata;
    metadata.specification = std::move(inheritedSpecification);
    for (const DslAnnotation& annotation : annotations) {
        if (annotation.name == QStringLiteral("spec") && annotation.arguments.size() == 2 &&
            annotation.arguments.at(0).kind == DslAnnotationValueKind::String &&
            annotation.arguments.at(1).kind == DslAnnotationValueKind::String) {
            metadata.specification = core::AnalysisSpecification{
                annotation.arguments.at(0).text, annotation.arguments.at(1).text};
        } else if (annotation.name == QStringLiteral("description") &&
                   annotation.arguments.size() == 1 &&
                   annotation.arguments.front().kind == DslAnnotationValueKind::String) {
            metadata.description = annotation.arguments.front().text;
        }
    }
    return metadata;
}

[[nodiscard]] std::optional<quint64> equalsConstraint(
    const DslBitField& field,
    std::vector<DslDiagnostic>& diagnostics) {
    std::optional<quint64> result;
    bool seen = false;
    for (const DslAnnotation& annotation : field.annotations) {
        if (annotation.name != QStringLiteral("equals")) {
            continue;
        }
        if (seen) {
            addDiagnostic(diagnostics,
                          DslDiagnosticCode::InvalidAnnotation,
                          QStringLiteral("@equals may appear at most once on a field"),
                          annotation.range);
        }
        seen = true;
        if (annotation.arguments.size() != 1 ||
            annotation.arguments.front().kind != DslAnnotationValueKind::Integer) {
            addDiagnostic(diagnostics,
                          DslDiagnosticCode::InvalidAnnotation,
                          QStringLiteral("@equals requires one integer argument"),
                          annotation.range);
            continue;
        }
        if (!result) {
            result = annotation.arguments.front().integerValue;
        }
    }
    return result;
}

[[nodiscard]] std::optional<QString> enumTypeName(
    const DslBitField& field,
    std::vector<DslDiagnostic>& diagnostics) {
    std::optional<QString> result;
    bool seen = false;
    for (const DslAnnotation& annotation : field.annotations) {
        if (annotation.name != QStringLiteral("enum")) {
            continue;
        }
        if (seen) {
            addDiagnostic(diagnostics,
                          DslDiagnosticCode::InvalidAnnotation,
                          QStringLiteral("@enum may appear at most once on a field"),
                          annotation.range);
        }
        seen = true;
        if (annotation.arguments.size() != 1 ||
            annotation.arguments.front().kind != DslAnnotationValueKind::Identifier) {
            addDiagnostic(diagnostics,
                          DslDiagnosticCode::InvalidAnnotation,
                          QStringLiteral("@enum requires one enum type name"),
                          annotation.range);
            continue;
        }
        if (!result) {
            result = annotation.arguments.front().text;
        }
    }
    return result;
}

[[nodiscard]] std::optional<quint32> findEnum(const DslTypedProgram& program,
                                              const QString& name) {
    for (quint32 index = 0; index < program.enums.size(); ++index) {
        if (program.enums.at(index).name == name) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<quint32> findStruct(const DslTypedProgram& program,
                                                const QString& name) {
    for (quint32 index = 0; index < program.structs.size(); ++index) {
        if (program.structs.at(index).name == name) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<quint32> DslTypedProgram::enumIndex(const QString& name) const {
    return findEnum(*this, name);
}

std::optional<quint32> DslTypedProgram::structureIndex(const QString& name) const {
    return findStruct(*this, name);
}

std::optional<quint32> DslTypedProgram::scanIndex(const QString& name) const {
    for (quint32 index = 0; index < scans.size(); ++index) {
        if (scans.at(index).name == name) {
            return index;
        }
    }
    return std::nullopt;
}

DslCompileResult DslCompiler::compile(const DslProgram& program) {
    DslCompileResult result;
    DslTypedProgram typed;
    constexpr std::size_t maximumIndexedSize = std::numeric_limits<quint32>::max();
    const auto appendInstruction = [&](DslInstruction instruction) {
        if (typed.bytecode.size() >= maximumIndexedSize) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("DSL bytecode is too large"),
                          {});
            return false;
        }
        typed.bytecode.push_back(instruction);
        return true;
    };
    if (program.enums.size() > std::numeric_limits<quint32>::max() ||
        program.structs.size() > std::numeric_limits<quint32>::max() ||
        program.scans.size() > std::numeric_limits<quint32>::max()) {
        addDiagnostic(result.diagnostics,
                      DslDiagnosticCode::InvalidType,
                      QStringLiteral("DSL program contains too many declarations"),
                      {});
        return result;
    }

    typed.enums.reserve(program.enums.size());
    for (const DslEnum& enumeration : program.enums) {
        DslTypedEnum typedEnum;
        typedEnum.name = enumeration.name;
        typedEnum.metadata = metadataForAnnotations(enumeration.annotations);
        typedEnum.metadata.typeName = QStringLiteral("enum");
        typedEnum.range = enumeration.range;
        if (enumeration.values.empty()) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::EmptyEnum,
                          QStringLiteral("An enum must contain at least one member"),
                          enumeration.range);
        }
        for (std::size_t valueIndex = 0; valueIndex < enumeration.values.size(); ++valueIndex) {
            const DslEnumValue& value = enumeration.values.at(valueIndex);
            for (std::size_t previous = 0; previous < valueIndex; ++previous) {
                if (value.name == enumeration.values.at(previous).name) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::DuplicateName,
                                  QStringLiteral("Duplicate enum member name"),
                                  value.range);
                    break;
                }
            }
            typedEnum.values.push_back({value.name, value.value});
        }
        typed.enums.push_back(std::move(typedEnum));
    }

    typed.structs.reserve(program.structs.size());
    for (const DslStruct& structure : program.structs) {
        DslTypedStruct typedStruct;
        typedStruct.name = structure.name;
        typedStruct.metadata = metadataForAnnotations(structure.annotations);
        typedStruct.metadata.typeName = QStringLiteral("struct");
        if (structure.fields.size() > maximumIndexedSize) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("A structure contains too many fields"),
                          structure.range);
        }
        if (structure.fields.empty()) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::EmptyStruct,
                          QStringLiteral("A structure must contain at least one field"),
                          structure.range);
        }

        quint64 fieldOffset = 0;
        for (const DslBitField& field : structure.fields) {
            if (field.width == 0 || field.width > 64) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidBitWidth,
                              QStringLiteral("Bit field width must be in the range 1..64"),
                              field.range);
                continue;
            }
            if (field.endian != DslEndian::Big && field.endian != DslEndian::Little) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidEndian,
                              QStringLiteral("Bit field byte order is invalid"),
                              field.range);
            }
            if (field.endian == DslEndian::Little && field.width % 8 != 0) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidEndian,
                              QStringLiteral(
                                  "Little-endian fields must have a width that is a multiple of 8"),
                              field.range);
            }
            if (field.endian == DslEndian::Little && fieldOffset % 8 != 0) {
                addDiagnostic(
                    result.diagnostics,
                    DslDiagnosticCode::InvalidEndian,
                    QStringLiteral(
                        "Little-endian fields must begin at a byte boundary within the structure"),
                    field.range);
            }
            for (const DslTypedField& previous : typedStruct.fields) {
                if (previous.name == field.name) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::DuplicateName,
                                  QStringLiteral("Duplicate field name"),
                                  field.range);
                    break;
                }
            }

            DslTypedField typedField;
            typedField.name = field.name;
            typedField.type =
                {DslValueTypeKind::UnsignedBits, field.width, field.endian, std::nullopt};
            typedField.metadata =
                metadataForAnnotations(field.annotations, typedStruct.metadata.specification);
            typedField.metadata.typeName = QStringLiteral("bits");
            typedField.range = field.range;
            const std::optional<QString> enumName = enumTypeName(field, result.diagnostics);
            if (enumName) {
                const auto enumIndex = typed.enumIndex(*enumName);
                if (!enumIndex) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::UnknownReference,
                                  QStringLiteral("Field enum type is not declared"),
                                  field.range);
                } else {
                    typedField.type.kind = DslValueTypeKind::Enum;
                    typedField.type.enumIndex = *enumIndex;
                    typedField.metadata.typeName = *enumName;
                    if (field.width < 64) {
                        const quint64 exclusiveLimit = quint64{1} << field.width;
                        for (const DslTypedEnumValue& value : typed.enums.at(*enumIndex).values) {
                            if (value.value >= exclusiveLimit) {
                                addDiagnostic(
                                    result.diagnostics,
                                    DslDiagnosticCode::EnumValueOutOfRange,
                                    QStringLiteral(
                                        "Enum member value does not fit the field width"),
                                    field.range);
                                break;
                            }
                        }
                    }
                }
            }
            typedField.equalsConstraint = equalsConstraint(field, result.diagnostics);
            if (typedField.equalsConstraint && field.width < 64 &&
                *typedField.equalsConstraint >= (quint64{1} << field.width)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::ConstraintOutOfRange,
                              QStringLiteral("@equals value does not fit the field width"),
                              [&field]() {
                                  for (const DslAnnotation& annotation : field.annotations) {
                                      if (annotation.name == QStringLiteral("equals")) {
                                          return annotation.range;
                                      }
                                  }
                                  return field.range;
                              }());
            }
            typedStruct.fields.push_back(std::move(typedField));
            if (fieldOffset > std::numeric_limits<quint64>::max() - field.width) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Structure bit width is too large"),
                              structure.range);
            } else {
                fieldOffset += field.width;
            }
        }
        typed.structs.push_back(std::move(typedStruct));
    }

    for (std::size_t index = 0; index < program.enums.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (program.enums.at(index).name == program.enums.at(previous).name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Duplicate enum name"),
                              program.enums.at(index).range);
                break;
            }
        }
        for (const DslStruct& structure : program.structs) {
            if (program.enums.at(index).name == structure.name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral(
                                  "Enum, structure, and sequence names must be unique"),
                              program.enums.at(index).range);
                break;
            }
        }
        for (const DslProgressiveScan& scan : program.scans) {
            if (program.enums.at(index).name == scan.name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral(
                                  "Enum, structure, and sequence names must be unique"),
                              program.enums.at(index).range);
                break;
            }
        }
    }

    for (std::size_t index = 0; index < program.structs.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (program.structs.at(index).name == program.structs.at(previous).name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Duplicate structure name"),
                              program.structs.at(index).range);
                break;
            }
        }
        for (const DslEnum& enumeration : program.enums) {
            if (program.structs.at(index).name == enumeration.name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral(
                                  "Enum, structure, and sequence names must be unique"),
                              program.structs.at(index).range);
                break;
            }
        }
    }

    typed.scans.reserve(program.scans.size());
    for (std::size_t scanIndex = 0; scanIndex < program.scans.size(); ++scanIndex) {
        const DslProgressiveScan& scan = program.scans.at(scanIndex);
        for (std::size_t previous = 0; previous < scanIndex; ++previous) {
            if (scan.name == program.scans.at(previous).name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Duplicate sequence name"),
                              scan.range);
                break;
            }
        }
        for (const DslStruct& structure : program.structs) {
            if (scan.name == structure.name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Structure and sequence names must be unique"),
                              scan.range);
                break;
            }
        }
        for (const DslEnum& enumeration : program.enums) {
            if (scan.name == enumeration.name) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral(
                                  "Enum, structure, and sequence names must be unique"),
                              scan.range);
                break;
            }
        }
        const auto elementIndex = [&]() -> std::optional<quint32> {
            for (quint32 index = 0; index < typed.structs.size(); ++index) {
                if (typed.structs.at(index).name == scan.elementType) {
                    return index;
                }
            }
            return std::nullopt;
        }();
        if (!elementIndex) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::UnknownReference,
                          QStringLiteral("Sequence element type is not declared"),
                          scan.range);
            continue;
        }
        if (scan.scannerName != QStringLiteral("h264_start_code")) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::UnsupportedScanner,
                          QStringLiteral("Only h264_start_code is supported"),
                          scan.range);
            continue;
        }
        DslTypedScan typedScan;
        typedScan.name = scan.name;
        typedScan.elementStructIndex = *elementIndex;
        typedScan.scanner = DslScannerKind::H264StartCode;
        typedScan.range = scan.range;
        typed.scans.push_back(std::move(typedScan));
    }

    if (!program.hasEntry) {
        addDiagnostic(result.diagnostics,
                      DslDiagnosticCode::MissingEntry,
                      QStringLiteral("A DSL program requires an entry"),
                      {});
    } else if (const auto structureIndex = typed.structureIndex(program.entry.targetName)) {
        typed.entry.kind = DslEntryKind::Structure;
        typed.entry.targetIndex = *structureIndex;
    } else if (const auto scanIndex = typed.scanIndex(program.entry.targetName)) {
        typed.entry.kind = DslEntryKind::Sequence;
        typed.entry.targetIndex = *scanIndex;
    } else {
        addDiagnostic(result.diagnostics,
                      DslDiagnosticCode::UnknownReference,
                      QStringLiteral("Entry target is not declared"),
                      program.entry.range);
    }

    for (quint32 structIndex = 0; structIndex < typed.structs.size(); ++structIndex) {
        DslTypedStruct& structure = typed.structs.at(structIndex);
        if (typed.bytecode.size() > maximumIndexedSize) {
            break;
        }
        structure.bytecodeOffset = static_cast<quint32>(typed.bytecode.size());
        bool emitted = appendInstruction({DslOpcode::BeginStructure, structIndex, 0});
        for (std::size_t fieldIndex = 0; emitted && fieldIndex < structure.fields.size();
             ++fieldIndex) {
            emitted = appendInstruction(
                {DslOpcode::ReadUnsignedBits, static_cast<quint32>(fieldIndex), 0});
            if (emitted && structure.fields.at(fieldIndex).equalsConstraint) {
                emitted = appendInstruction(
                    {DslOpcode::AssertEquals,
                     static_cast<quint32>(fieldIndex),
                     *structure.fields.at(fieldIndex).equalsConstraint});
            }
        }
        if (!emitted || !appendInstruction({DslOpcode::EndStructure, structIndex, 0})) {
            break;
        }
        structure.bytecodeLength =
            static_cast<quint32>(typed.bytecode.size()) - structure.bytecodeOffset;
    }

    if (!result.diagnostics.empty()) {
        return result;
    }
    result.program = std::move(typed);
    return result;
}

} // namespace streamview::rules

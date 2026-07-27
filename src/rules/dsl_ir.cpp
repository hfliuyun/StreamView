#include <streamview/rules/dsl_ir.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

constexpr quint64 maximumExpandedFieldsPerStructure = 99'999;

void addDiagnostic(std::vector<DslDiagnostic>& diagnostics,
                   DslDiagnosticCode code,
                   const QString& message,
                   const DslSourceRange& range) {
    diagnostics.push_back({code, message, range});
}

void collectFields(const std::vector<DslStructItem>& items,
                   std::vector<const DslBitField*>& fields) {
    for (const DslStructItem& item : items) {
        if (item.kind == DslStructItemKind::Field) {
            fields.push_back(&item.field);
        } else if (item.kind == DslStructItemKind::Conditional) {
            collectFields(item.thenItems, fields);
            collectFields(item.elseItems, fields);
        } else if (item.kind == DslStructItemKind::Switch) {
            for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                collectFields(arm.items, fields);
            }
        } else if (item.kind == DslStructItemKind::Repeat) {
            collectFields(item.repeatItems, fields);
        }
    }
}

[[nodiscard]] quint64 expandedFieldProjection(const std::vector<DslStructItem>& items) {
    constexpr quint64 overflowProjection = maximumExpandedFieldsPerStructure + 1;
    const auto add = [](quint64 left, quint64 right) {
        return left >= overflowProjection || right >= overflowProjection ||
                       left > overflowProjection - right
                   ? overflowProjection
                   : left + right;
    };
    quint64 projection = 0;
    for (const DslStructItem& item : items) {
        quint64 itemProjection = 0;
        if (item.kind == DslStructItemKind::Field) {
            itemProjection = item.field.arrayLength.value_or(1);
        } else if (item.kind == DslStructItemKind::Conditional) {
            itemProjection = add(expandedFieldProjection(item.thenItems),
                                 expandedFieldProjection(item.elseItems));
        } else if (item.kind == DslStructItemKind::Switch) {
            for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                itemProjection = add(itemProjection, expandedFieldProjection(arm.items));
            }
        } else if (item.kind == DslStructItemKind::Repeat) {
            const quint64 bodyProjection = expandedFieldProjection(item.repeatItems);
            if (item.repeatMaximum == 0) {
                itemProjection = 0;
            } else {
                itemProjection = bodyProjection <=
                                         overflowProjection / item.repeatMaximum
                                     ? bodyProjection * item.repeatMaximum
                                     : overflowProjection;
            }
        }
        projection = add(projection, itemProjection);
    }
    return projection;
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
        std::vector<const DslBitField*> sourceFields;
        collectFields(structure.items, sourceFields);
        DslTypedStruct typedStruct;
        typedStruct.name = structure.name;
        typedStruct.metadata = metadataForAnnotations(structure.annotations);
        typedStruct.metadata.typeName = QStringLiteral("struct");
        if (sourceFields.size() > maximumIndexedSize) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("A structure contains too many fields"),
                          structure.range);
        }
        if (sourceFields.empty()) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::EmptyStruct,
                          QStringLiteral("A structure must contain at least one field"),
                          structure.range);
        }

        std::vector<QString> declaredFieldNames;
        declaredFieldNames.reserve(sourceFields.size());
        for (const DslBitField* field : sourceFields) {
            if (std::find(declaredFieldNames.begin(),
                          declaredFieldNames.end(),
                          field->name) != declaredFieldNames.end()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Duplicate field name"),
                              field->range);
            }
            declaredFieldNames.push_back(field->name);
        }

        struct DeclaredField final {
            const DslBitField* source = nullptr;
            std::optional<quint32> typedIndex;
            std::vector<DslTypedFieldCondition> conditions;
        };
        struct ResolvedController final {
            quint32 fieldIndex = 0;
            quint8 width = 0;
            DslFieldEncoding encoding = DslFieldEncoding::Bits;
        };
        std::vector<DeclaredField> declaredFields;
        declaredFields.reserve(sourceFields.size());
        std::vector<quint64> repeatIndices;
        const auto sameCondition = [](const DslTypedFieldCondition& left,
                                      const DslTypedFieldCondition& right) {
            return left.fieldIndex == right.fieldIndex &&
                   left.expectedValue == right.expectedValue &&
                   left.negated == right.negated && left.op == right.op;
        };
        const auto resolveController = [&](const QString& fieldName,
                                           const DslSourceRange& range,
                                           const std::vector<DslTypedFieldCondition>& active,
                                           bool allowUnsignedExpGolomb)
            -> std::optional<ResolvedController> {
            const auto found = std::find_if(
                declaredFields.rbegin(),
                declaredFields.rend(),
                [&fieldName](const DeclaredField& declared) {
                    return declared.source->name == fieldName;
                });
            if (found == declaredFields.rend()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::UnknownReference,
                              QStringLiteral(
                                  "Controller field must be declared before the statement"),
                              range);
                return std::nullopt;
            }
            const bool supportedEncoding =
                found->source->encoding == DslFieldEncoding::Bits ||
                (allowUnsignedExpGolomb &&
                 found->source->encoding == DslFieldEncoding::UnsignedExpGolomb);
            if (!supportedEncoding || found->source->arrayLength || !found->typedIndex) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              allowUnsignedExpGolomb
                                  ? QStringLiteral(
                                        "Repeat counts require a previous scalar bits, enum, or "
                                        "ue field")
                                  : QStringLiteral(
                                        "Controllers require a previous scalar bits or enum "
                                        "field"),
                              range);
                return std::nullopt;
            }
            const bool available = std::all_of(
                found->conditions.begin(),
                found->conditions.end(),
                [&active, &sameCondition](const DslTypedFieldCondition& required) {
                    return std::any_of(active.begin(),
                                       active.end(),
                                       [&required, &sameCondition](
                                           const DslTypedFieldCondition& candidate) {
                                           return sameCondition(required, candidate);
                                       });
                });
            if (!available) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidCondition,
                              QStringLiteral(
                                  "Controller field is not guaranteed on the current branch"),
                              range);
                return std::nullopt;
            }
            return ResolvedController{
                *found->typedIndex, found->source->width, found->source->encoding};
        };
        const auto resolveConditionValue = [&](const std::optional<ResolvedController>& controller,
                                               quint64 expectedValue,
                                               const DslSourceRange& range)
            -> std::optional<DslTypedFieldCondition> {
            if (!controller) {
                return std::nullopt;
            }
            if (controller->width != 0 && controller->width < 64 &&
                expectedValue >= (quint64{1} << controller->width)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::ConstraintOutOfRange,
                              QStringLiteral(
                                  "Condition value does not fit the controlling field"),
                              range);
                return std::nullopt;
            }
            return DslTypedFieldCondition{controller->fieldIndex, expectedValue, false};
        };
        const auto resolveCondition = [&](const DslEqualityCondition& condition,
                                          const std::vector<DslTypedFieldCondition>& active) {
            const auto controller = resolveController(
                condition.fieldName, condition.range, active, false);
            return resolveConditionValue(
                controller, condition.expectedValue, condition.range);
        };
        const auto compileField = [&](const DslBitField& field,
                                      const std::vector<DslTypedFieldCondition>& conditions,
                                      std::optional<quint64> fieldOffset)
            -> std::optional<quint64> {
            const quint64 elementCount = field.arrayLength.value_or(1);
            if (elementCount == 0 ||
                elementCount > maximumExpandedFieldsPerStructure - typedStruct.fields.size()) {
                addDiagnostic(
                    result.diagnostics,
                    DslDiagnosticCode::InvalidArrayLength,
                    QStringLiteral(
                        "Fixed array expansion exceeds the structure materialization limit"),
                    field.range);
                declaredFields.push_back({&field, std::nullopt, conditions});
                return std::nullopt;
            }
            const bool isBits = field.encoding == DslFieldEncoding::Bits;
            const bool isUnsignedExpGolomb =
                field.encoding == DslFieldEncoding::UnsignedExpGolomb;
            const bool isSignedExpGolomb =
                field.encoding == DslFieldEncoding::SignedExpGolomb;
            if (!isBits && !isUnsignedExpGolomb && !isSignedExpGolomb) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Field encoding is invalid"),
                              field.range);
                declaredFields.push_back({&field, std::nullopt, conditions});
                return std::nullopt;
            }
            if (isBits && (field.width == 0 || field.width > 64)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidBitWidth,
                              QStringLiteral("Bit field width must be in the range 1..64"),
                              field.range);
                declaredFields.push_back({&field, std::nullopt, conditions});
                return std::nullopt;
            }
            if (!isBits && field.width != 0) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Exp-Golomb fields cannot have a fixed bit width"),
                              field.range);
            }
            if (field.endian != DslEndian::Big && field.endian != DslEndian::Little) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidEndian,
                              QStringLiteral("Field byte order is invalid"),
                              field.range);
            }
            if (!isBits && field.endian != DslEndian::Big) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidEndian,
                              QStringLiteral("Exp-Golomb fields use the default bit order"),
                              field.range);
            }
            if (isBits && field.endian == DslEndian::Little && field.width % 8 != 0) {
                addDiagnostic(
                    result.diagnostics,
                    DslDiagnosticCode::InvalidEndian,
                    QStringLiteral(
                        "Little-endian fields must have a width that is a multiple of 8"),
                    field.range);
            }
            if (isBits && field.endian == DslEndian::Little &&
                (!fieldOffset || *fieldOffset % 8 != 0)) {
                addDiagnostic(
                    result.diagnostics,
                    DslDiagnosticCode::InvalidEndian,
                    QStringLiteral(
                        "Little-endian fields must begin at a byte boundary within the structure"),
                    field.range);
            }
            DslTypedField typedField;
            typedField.name = field.name;
            for (const quint64 repeatIndex : repeatIndices) {
                typedField.name += QStringLiteral("[%1]").arg(repeatIndex);
            }
            const DslValueTypeKind valueKind =
                isBits ? DslValueTypeKind::UnsignedBits
                       : (isUnsignedExpGolomb ? DslValueTypeKind::UnsignedExpGolomb
                                              : DslValueTypeKind::SignedExpGolomb);
            typedField.type = {valueKind, isBits ? field.width : quint8(0),
                               isBits ? field.endian : DslEndian::Big, std::nullopt};
            typedField.conditions = conditions;
            typedField.metadata =
                metadataForAnnotations(field.annotations, typedStruct.metadata.specification);
            typedField.metadata.typeName = isBits ? QStringLiteral("bits")
                                                  : (isUnsignedExpGolomb ? QStringLiteral("ue")
                                                                         : QStringLiteral("se"));
            typedField.range = field.range;
            const auto hasAnnotation = [&field](const QString& name) {
                return std::any_of(field.annotations.begin(),
                                   field.annotations.end(),
                                   [&name](const DslAnnotation& annotation) {
                                       return annotation.name == name;
                                   });
            };
            if (!isBits && hasAnnotation(QStringLiteral("enum"))) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidAnnotation,
                              QStringLiteral("@enum is only supported on bits fields"),
                              field.range);
            }
            if (!isBits && hasAnnotation(QStringLiteral("equals"))) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidAnnotation,
                              QStringLiteral("@equals is only supported on bits fields"),
                              field.range);
            }
            const std::optional<QString> enumName =
                isBits ? enumTypeName(field, result.diagnostics) : std::nullopt;
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
            typedField.equalsConstraint =
                isBits ? equalsConstraint(field, result.diagnostics) : std::nullopt;
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
            const quint32 firstTypedIndex =
                static_cast<quint32>(typedStruct.fields.size());
            if (field.arrayLength) {
                for (quint64 elementIndex = 0; elementIndex < elementCount; ++elementIndex) {
                    DslTypedField element = typedField;
                    element.name += QStringLiteral("[%1]").arg(elementIndex);
                    typedStruct.fields.push_back(std::move(element));
                }
            } else {
                typedStruct.fields.push_back(std::move(typedField));
            }
            declaredFields.push_back({&field, firstTypedIndex, conditions});
            if (!isBits) {
                return std::nullopt;
            }
            if (elementCount > std::numeric_limits<quint64>::max() / field.width) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidArrayLength,
                              QStringLiteral("Fixed array bit width is too large"),
                              field.range);
                return std::nullopt;
            }
            if (!fieldOffset) {
                return std::nullopt;
            }
            const quint64 totalWidth = elementCount * field.width;
            if (*fieldOffset > std::numeric_limits<quint64>::max() - totalWidth) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Structure bit width is too large"),
                              structure.range);
                return std::nullopt;
            }
            return *fieldOffset + totalWidth;
        };
        const auto compileItems = [&](const auto& self,
                                      const std::vector<DslStructItem>& items,
                                      const std::vector<DslTypedFieldCondition>& conditions,
                                      std::optional<quint64> fieldOffset)
            -> std::optional<quint64> {
            for (const DslStructItem& item : items) {
                if (item.kind == DslStructItemKind::Field) {
                    fieldOffset = compileField(item.field, conditions, fieldOffset);
                    continue;
                }
                if (item.kind == DslStructItemKind::Conditional) {
                    const auto resolved = resolveCondition(item.condition, conditions);
                    std::vector<DslTypedFieldCondition> thenConditions = conditions;
                    std::vector<DslTypedFieldCondition> elseConditions = conditions;
                    if (resolved) {
                        thenConditions.push_back(*resolved);
                        DslTypedFieldCondition negated = *resolved;
                        negated.negated = true;
                        elseConditions.push_back(negated);
                    }
                    const auto thenOffset =
                        self(self, item.thenItems, thenConditions, fieldOffset);
                    const auto elseOffset = item.elseItems.empty()
                                                ? fieldOffset
                                                : self(self,
                                                       item.elseItems,
                                                       elseConditions,
                                                       fieldOffset);
                    fieldOffset = thenOffset && elseOffset && *thenOffset == *elseOffset
                                      ? thenOffset
                                      : std::nullopt;
                    continue;
                }
                if (item.kind == DslStructItemKind::Repeat) {
                    const auto controller = resolveController(item.repeatCountFieldName,
                                                              item.repeatCountFieldRange,
                                                              conditions,
                                                              true);
                    if (item.repeatMaximum == 0) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::InvalidArrayLength,
                                      QStringLiteral(
                                          "Bounded repeat maximum must be at least one"),
                                      item.repeatMaximumRange);
                    } else if (controller &&
                               controller->encoding == DslFieldEncoding::Bits &&
                               controller->width != 0 && controller->width < 64 &&
                               item.repeatMaximum >= (quint64{1} << controller->width)) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::ConstraintOutOfRange,
                                      QStringLiteral(
                                          "Repeat maximum does not fit the controlling field"),
                                      item.repeatMaximumRange);
                    }

                    const quint64 bodyProjection = expandedFieldProjection(item.repeatItems);
                    const quint64 currentFields =
                        static_cast<quint64>(typedStruct.fields.size());
                    const quint64 remaining = maximumExpandedFieldsPerStructure - currentFields;
                    if (bodyProjection == 0) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::InvalidCondition,
                                      QStringLiteral(
                                          "A bounded repeat body must contain at least one field"),
                                      item.range);
                    } else if (item.repeatMaximum != 0 &&
                               (bodyProjection > remaining ||
                                item.repeatMaximum > remaining / bodyProjection)) {
                        addDiagnostic(
                            result.diagnostics,
                            DslDiagnosticCode::InvalidArrayLength,
                            QStringLiteral(
                                "Bounded repeat expansion exceeds the structure materialization "
                                "limit"),
                            item.range);
                    } else if (item.repeatMaximum != 0) {
                        const quint32 firstFieldIndex =
                            static_cast<quint32>(typedStruct.fields.size());
                        if (controller) {
                            typedStruct.repeatBounds.push_back({controller->fieldIndex,
                                                                firstFieldIndex,
                                                                item.repeatMaximum,
                                                                conditions,
                                                                item.range});
                        }
                        for (quint64 iteration = 0; iteration < item.repeatMaximum;
                             ++iteration) {
                            const std::size_t scopeStart = declaredFields.size();
                            std::vector<DslTypedFieldCondition> iterationConditions =
                                conditions;
                            if (controller) {
                                iterationConditions.push_back(
                                    {controller->fieldIndex,
                                     iteration,
                                     false,
                                     DslConditionOperator::GreaterThan});
                            }
                            repeatIndices.push_back(iteration);
                            fieldOffset =
                                self(self, item.repeatItems, iterationConditions, fieldOffset);
                            repeatIndices.pop_back();
                            declaredFields.resize(scopeStart);
                        }
                    }
                    fieldOffset = std::nullopt;
                    continue;
                }
                if (item.kind != DslStructItemKind::Switch) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidCondition,
                                  QStringLiteral("Structure item kind is invalid"),
                                  item.range);
                    fieldOffset = std::nullopt;
                    continue;
                }

                std::vector<std::optional<DslTypedFieldCondition>> caseGuards;
                caseGuards.reserve(item.switchArms.size());
                const auto controller = resolveController(
                    item.switchFieldName, item.switchFieldRange, conditions, false);
                std::vector<quint64> caseValues;
                caseValues.reserve(item.switchArms.size());
                bool defaultSeen = false;
                for (std::size_t armIndex = 0;
                     armIndex < item.switchArms.size();
                     ++armIndex) {
                    const DslStructItem::SwitchArm& arm =
                        item.switchArms.at(armIndex);
                    if (arm.kind == DslSwitchArmKind::Default) {
                        if (defaultSeen) {
                            addDiagnostic(
                                result.diagnostics,
                                DslDiagnosticCode::InvalidCondition,
                                QStringLiteral("A switch may contain at most one default arm"),
                                arm.range);
                        }
                        defaultSeen = true;
                        if (armIndex + 1 != item.switchArms.size()) {
                            addDiagnostic(result.diagnostics,
                                          DslDiagnosticCode::InvalidCondition,
                                          QStringLiteral(
                                              "The default switch arm must appear last"),
                                          arm.range);
                        }
                        continue;
                    }
                    if (arm.kind != DslSwitchArmKind::Case) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::InvalidCondition,
                                      QStringLiteral("Switch arm kind is invalid"),
                                      arm.range);
                        continue;
                    }
                    if (defaultSeen) {
                        addDiagnostic(
                            result.diagnostics,
                            DslDiagnosticCode::InvalidCondition,
                            QStringLiteral("Switch case arms may not follow the default arm"),
                            arm.range);
                    }
                    if (std::find(caseValues.begin(),
                                  caseValues.end(),
                                  arm.caseValue) != caseValues.end()) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::InvalidCondition,
                                      QStringLiteral("Duplicate switch case value"),
                                      arm.valueRange);
                    }
                    caseValues.push_back(arm.caseValue);
                    caseGuards.push_back(resolveConditionValue(
                        controller, arm.caseValue, arm.valueRange));
                }
                if (caseValues.empty()) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidCondition,
                                  QStringLiteral("A switch must contain at least one case arm"),
                                  item.range);
                }

                std::vector<std::optional<quint64>> armOffsets;
                armOffsets.reserve(item.switchArms.size() +
                                   (defaultSeen ? 0 : 1));
                std::size_t caseGuardIndex = 0;
                for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                    std::vector<DslTypedFieldCondition> armConditions = conditions;
                    if (arm.kind == DslSwitchArmKind::Case) {
                        if (caseGuardIndex < caseGuards.size() &&
                            caseGuards.at(caseGuardIndex)) {
                            armConditions.push_back(*caseGuards.at(caseGuardIndex));
                        }
                        ++caseGuardIndex;
                    } else if (arm.kind == DslSwitchArmKind::Default) {
                        for (const auto& resolvedGuard : caseGuards) {
                            if (!resolvedGuard) {
                                continue;
                            }
                            DslTypedFieldCondition guard = *resolvedGuard;
                            guard.negated = true;
                            armConditions.push_back(guard);
                        }
                    }
                    armOffsets.push_back(
                        self(self, arm.items, armConditions, fieldOffset));
                }
                if (!defaultSeen) {
                    armOffsets.push_back(fieldOffset);
                }
                const auto firstKnown = std::find_if(
                    armOffsets.begin(),
                    armOffsets.end(),
                    [](const std::optional<quint64>& offset) {
                        return offset.has_value();
                    });
                if (firstKnown == armOffsets.end() ||
                    std::any_of(
                        armOffsets.begin(),
                        armOffsets.end(),
                        [&firstKnown](const std::optional<quint64>& offset) {
                            return !offset || *offset != **firstKnown;
                        })) {
                    fieldOffset = std::nullopt;
                } else {
                    fieldOffset = *firstKnown;
                }
            }
            return fieldOffset;
        };
        (void)compileItems(compileItems, structure.items, {}, quint64(0));
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
        std::size_t repeatBoundIndex = 0;
        for (std::size_t fieldIndex = 0; emitted && fieldIndex <= structure.fields.size();
             ++fieldIndex) {
            while (emitted && repeatBoundIndex < structure.repeatBounds.size() &&
                   structure.repeatBounds.at(repeatBoundIndex).firstFieldIndex == fieldIndex) {
                emitted = appendInstruction(
                    {DslOpcode::AssertRepeatCount,
                     static_cast<quint32>(repeatBoundIndex),
                     structure.repeatBounds.at(repeatBoundIndex).maximumCount});
                ++repeatBoundIndex;
            }
            if (fieldIndex == structure.fields.size()) {
                break;
            }
            const DslOpcode readOpcode = [&]() {
                switch (structure.fields.at(fieldIndex).type.kind) {
                case DslValueTypeKind::UnsignedBits:
                case DslValueTypeKind::Enum:
                    return DslOpcode::ReadUnsignedBits;
                case DslValueTypeKind::UnsignedExpGolomb:
                    return DslOpcode::ReadUnsignedExpGolomb;
                case DslValueTypeKind::SignedExpGolomb:
                    return DslOpcode::ReadSignedExpGolomb;
                }
                return DslOpcode::ReadUnsignedBits;
            }();
            emitted = appendInstruction({readOpcode, static_cast<quint32>(fieldIndex), 0});
            if (emitted && structure.fields.at(fieldIndex).equalsConstraint) {
                emitted = appendInstruction(
                    {DslOpcode::AssertEquals,
                     static_cast<quint32>(fieldIndex),
                     *structure.fields.at(fieldIndex).equalsConstraint});
            }
        }
        if (repeatBoundIndex != structure.repeatBounds.size()) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidCondition,
                          QStringLiteral("Typed repeat bound position is invalid"),
                          {});
            emitted = false;
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

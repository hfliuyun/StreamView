#include <streamview/rules/dsl_ir.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

constexpr quint64 maximumExpandedFieldsPerStructure = 99'999;
constexpr std::size_t maximumExpressionExpansionWork = 16 * 256;

[[nodiscard]] bool validScalarType(DslScalarType type) noexcept {
    return type == DslScalarType::Bool || type == DslScalarType::U64;
}

void addDiagnostic(std::vector<DslDiagnostic>& diagnostics,
                   DslDiagnosticCode code,
                   const QString& message,
                   const DslSourceRange& range) {
    diagnostics.push_back({code, message, range});
}

void collectFields(const std::vector<DslStructItem>& items,
                   std::vector<const DslStructItem*>& fields) {
    for (const DslStructItem& item : items) {
        if (item.kind == DslStructItemKind::Field ||
            item.kind == DslStructItemKind::Computed) {
            fields.push_back(&item);
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
        } else if (item.kind == DslStructItemKind::Computed) {
            itemProjection = 1;
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
    if (program.pureFunctions.size() > std::numeric_limits<quint32>::max() ||
        program.enums.size() > std::numeric_limits<quint32>::max() ||
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

    struct ExpressionWorkState final {
        std::size_t stepCount = 0;
        bool reported = false;
    };
    struct ExpressionBuildState final {
        ExpressionBuildState() = default;
        ExpressionBuildState(const ExpressionBuildState&) = delete;
        ExpressionBuildState& operator=(const ExpressionBuildState&) = delete;

        ExpressionWorkState ownedWork;
        ExpressionWorkState* work = &ownedWork;
        std::size_t nodeCount = 0;
        bool sizeReported = false;
        bool depthReported = false;
    };
    using ExpressionResolver = std::function<std::optional<DslTypedExpression>(
        const QString&, const DslSourceRange&)>;
    const auto claimExpressionNode = [&](ExpressionBuildState& state,
                                         std::size_t depth,
                                         const DslSourceRange& range) {
        if (depth > 64) {
            if (!state.depthReported) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidExpression,
                              QStringLiteral("Expanded expressions may have depth at most 64"),
                              range);
                state.depthReported = true;
            }
            return false;
        }
        ++state.nodeCount;
        if (state.nodeCount > 256) {
            if (!state.sizeReported) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidExpression,
                              QStringLiteral(
                                  "Expanded expressions may contain at most 256 nodes"),
                              range);
                state.sizeReported = true;
            }
            return false;
        }
        return true;
    };
    const auto claimExpressionWork = [&](ExpressionBuildState& state,
                                         const DslSourceRange& range) {
        ++state.work->stepCount;
        if (state.work->stepCount <= maximumExpressionExpansionWork) {
            return true;
        }
        if (!state.work->reported) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidExpression,
                          QStringLiteral("Expression expansion work exceeds the sandbox limit"),
                          range);
            state.work->reported = true;
        }
        return false;
    };
    std::function<std::optional<DslTypedExpression>(
        const DslTypedExpression&,
        std::size_t,
        ExpressionBuildState&,
        const DslSourceRange&)>
        cloneExpression;
    cloneExpression = [&](const DslTypedExpression& source,
                          std::size_t depth,
                          ExpressionBuildState& state,
                          const DslSourceRange& range)
        -> std::optional<DslTypedExpression> {
        if (!claimExpressionWork(state, range) ||
            !claimExpressionNode(state, depth, range)) {
            return std::nullopt;
        }
        DslTypedExpression cloned = source;
        cloned.operands.clear();
        for (const DslTypedExpression& operand : source.operands) {
            const auto clonedOperand = cloneExpression(operand, depth + 1, state, range);
            if (!clonedOperand) {
                return std::nullopt;
            }
            cloned.operands.push_back(*clonedOperand);
        }
        return cloned;
    };

    std::function<std::optional<DslTypedExpression>(
        const DslExpression&,
        const ExpressionResolver&,
        std::size_t,
        std::size_t,
        ExpressionBuildState&)>
        compileExpression;
    compileExpression = [&](const DslExpression& expression,
                            const ExpressionResolver& resolveIdentifier,
                            std::size_t availableFunctionCount,
                            std::size_t depth,
                            ExpressionBuildState& state)
        -> std::optional<DslTypedExpression> {
        if (!claimExpressionWork(state, expression.range)) {
            return std::nullopt;
        }
        if (expression.kind == DslExpressionKind::Identifier) {
            const auto resolved = resolveIdentifier(expression.name, expression.range);
            if (!resolved) {
                return std::nullopt;
            }
            return cloneExpression(*resolved, depth, state, expression.range);
        }
        if (expression.kind == DslExpressionKind::Call) {
            const std::size_t functionCount =
                std::min(availableFunctionCount, program.pureFunctions.size());
            const auto functionsEnd = program.pureFunctions.begin() +
                                      static_cast<std::ptrdiff_t>(functionCount);
            const auto found = std::find_if(
                program.pureFunctions.begin(),
                functionsEnd,
                [&expression](const DslPureFunction& function) {
                    return function.name == expression.name;
                });
            if (found == functionsEnd) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::UnknownReference,
                              QStringLiteral(
                                  "Pure function is not declared before this call"),
                              expression.range);
                return std::nullopt;
            }
            if (found->parameters.size() != expression.operands.size()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Pure function argument count does not match"),
                              expression.range);
                return std::nullopt;
            }
            std::vector<std::optional<DslTypedExpression>> arguments;
            arguments.reserve(expression.operands.size());
            for (const DslExpression& argument : expression.operands) {
                ExpressionBuildState argumentState;
                argumentState.work = state.work;
                arguments.push_back(compileExpression(argument,
                                                       resolveIdentifier,
                                                       functionCount,
                                                       depth + 1,
                                                       argumentState));
            }
            bool argumentsValid = true;
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                if (!arguments.at(index)) {
                    argumentsValid = false;
                    continue;
                }
                if (arguments.at(index)->type != found->parameters.at(index).type) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidType,
                                  QStringLiteral("Pure function argument type does not match"),
                                  expression.operands.at(index).range);
                    argumentsValid = false;
                }
            }
            if (!argumentsValid) {
                return std::nullopt;
            }
            const ExpressionResolver resolveParameter =
                [&found, &arguments, &result](const QString& name,
                                             const DslSourceRange& range)
                -> std::optional<DslTypedExpression> {
                const auto parameter = std::find_if(
                    found->parameters.begin(),
                    found->parameters.end(),
                    [&name](const DslFunctionParameter& candidate) {
                        return candidate.name == name;
                    });
                if (parameter == found->parameters.end()) {
                    addDiagnostic(
                        result.diagnostics,
                        DslDiagnosticCode::UnknownReference,
                        QStringLiteral(
                            "Pure function expressions may reference only parameters"),
                        range);
                    return std::nullopt;
                }
                const std::size_t parameterIndex = static_cast<std::size_t>(
                    std::distance(found->parameters.begin(), parameter));
                if (parameterIndex >= arguments.size()) {
                    return std::nullopt;
                }
                return arguments.at(parameterIndex);
            };
            const std::size_t functionIndex = static_cast<std::size_t>(
                std::distance(program.pureFunctions.begin(), found));
            return compileExpression(
                found->expression, resolveParameter, functionIndex, depth + 1, state);
        }

        if (!claimExpressionNode(state, depth, expression.range)) {
            return std::nullopt;
        }
        DslTypedExpression typedExpression;
        switch (expression.kind) {
        case DslExpressionKind::UnsignedLiteral:
            typedExpression.kind = DslTypedExpressionKind::UnsignedLiteral;
            typedExpression.type = DslScalarType::U64;
            typedExpression.unsignedValue = expression.unsignedValue;
            return typedExpression;
        case DslExpressionKind::BooleanLiteral:
            typedExpression.kind = DslTypedExpressionKind::BooleanLiteral;
            typedExpression.type = DslScalarType::Bool;
            typedExpression.booleanValue = expression.booleanValue;
            return typedExpression;
        case DslExpressionKind::Unary: {
            if (expression.operands.size() != 1 ||
                expression.unaryOperator != DslUnaryOperator::LogicalNot) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidExpression,
                              QStringLiteral("Unary expression is invalid"),
                              expression.range);
                return std::nullopt;
            }
            const auto operand = compileExpression(expression.operands.front(),
                                                   resolveIdentifier,
                                                   availableFunctionCount,
                                                   depth + 1,
                                                   state);
            if (!operand) {
                return std::nullopt;
            }
            if (operand->type != DslScalarType::Bool) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Logical negation requires a bool operand"),
                              expression.range);
                return std::nullopt;
            }
            typedExpression.kind = DslTypedExpressionKind::Unary;
            typedExpression.type = DslScalarType::Bool;
            typedExpression.unaryOperator = expression.unaryOperator;
            typedExpression.operands.push_back(*operand);
            return typedExpression;
        }
        case DslExpressionKind::Binary:
            break;
        case DslExpressionKind::Identifier:
        case DslExpressionKind::Call:
            return std::nullopt;
        default:
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidExpression,
                          QStringLiteral("Expression kind is invalid"),
                          expression.range);
            return std::nullopt;
        }
        if (expression.operands.size() != 2) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidExpression,
                          QStringLiteral("Binary expression requires two operands"),
                          expression.range);
            return std::nullopt;
        }
        const auto left = compileExpression(expression.operands.at(0),
                                            resolveIdentifier,
                                            availableFunctionCount,
                                            depth + 1,
                                            state);
        const auto right = compileExpression(expression.operands.at(1),
                                             resolveIdentifier,
                                             availableFunctionCount,
                                             depth + 1,
                                             state);
        if (!left || !right) {
            return std::nullopt;
        }
        DslScalarType resultType = DslScalarType::U64;
        bool typesValid = true;
        switch (expression.binaryOperator) {
        case DslBinaryOperator::Multiply:
        case DslBinaryOperator::Divide:
        case DslBinaryOperator::Remainder:
        case DslBinaryOperator::Add:
        case DslBinaryOperator::Subtract:
            typesValid = left->type == DslScalarType::U64 &&
                         right->type == DslScalarType::U64;
            resultType = DslScalarType::U64;
            break;
        case DslBinaryOperator::Equal:
        case DslBinaryOperator::NotEqual:
            typesValid = left->type == right->type;
            resultType = DslScalarType::Bool;
            break;
        case DslBinaryOperator::Less:
        case DslBinaryOperator::LessEqual:
        case DslBinaryOperator::Greater:
        case DslBinaryOperator::GreaterEqual:
            typesValid = left->type == DslScalarType::U64 &&
                         right->type == DslScalarType::U64;
            resultType = DslScalarType::Bool;
            break;
        case DslBinaryOperator::LogicalAnd:
        case DslBinaryOperator::LogicalOr:
            typesValid = left->type == DslScalarType::Bool &&
                         right->type == DslScalarType::Bool;
            resultType = DslScalarType::Bool;
            break;
        default:
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidExpression,
                          QStringLiteral("Binary expression operator is invalid"),
                          expression.range);
            return std::nullopt;
        }
        if (!typesValid) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("Expression operand types do not match the operator"),
                          expression.range);
            return std::nullopt;
        }
        typedExpression.kind = DslTypedExpressionKind::Binary;
        typedExpression.type = resultType;
        typedExpression.binaryOperator = expression.binaryOperator;
        typedExpression.operands.push_back(*left);
        typedExpression.operands.push_back(*right);
        return typedExpression;
    };

    for (std::size_t index = 0; index < program.pureFunctions.size(); ++index) {
        const DslPureFunction& function = program.pureFunctions.at(index);
        if (!validScalarType(function.returnType)) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("Pure function return type is invalid"),
                          function.range);
        }
        if (function.parameters.size() > 16) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("Pure functions may declare at most 16 parameters"),
                          function.range);
        }
        const bool duplicateName = std::any_of(
            program.pureFunctions.begin(),
            program.pureFunctions.begin() + static_cast<std::ptrdiff_t>(index),
            [&function](const DslPureFunction& previous) {
                return previous.name == function.name;
            });
        const bool conflictsWithTopLevel =
            std::any_of(program.enums.begin(),
                        program.enums.end(),
                        [&function](const DslEnum& enumeration) {
                            return enumeration.name == function.name;
                        }) ||
            std::any_of(program.structs.begin(),
                        program.structs.end(),
                        [&function](const DslStruct& structure) {
                            return structure.name == function.name;
                        }) ||
            std::any_of(program.scans.begin(),
                        program.scans.end(),
                        [&function](const DslProgressiveScan& scan) {
                            return scan.name == function.name;
                        });
        if (duplicateName || conflictsWithTopLevel) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::DuplicateName,
                          QStringLiteral(
                              "Pure function names share the top-level namespace"),
                          function.range);
        }
        for (std::size_t parameterIndex = 0;
             parameterIndex < function.parameters.size();
             ++parameterIndex) {
            const DslFunctionParameter& parameter =
                function.parameters.at(parameterIndex);
            if (!validScalarType(parameter.type)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Pure function parameter type is invalid"),
                              parameter.range);
            }
            if (std::any_of(
                    function.parameters.begin(),
                    function.parameters.begin() +
                        static_cast<std::ptrdiff_t>(parameterIndex),
                    [&parameter](const DslFunctionParameter& previous) {
                        return previous.name == parameter.name;
                    })) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral(
                                  "Pure function parameter names must be unique"),
                              parameter.range);
            }
        }
        const ExpressionResolver resolveParameter =
            [&function, &result](const QString& name,
                                const DslSourceRange& range)
            -> std::optional<DslTypedExpression> {
            const auto found = std::find_if(
                function.parameters.begin(),
                function.parameters.end(),
                [&name](const DslFunctionParameter& parameter) {
                    return parameter.name == name;
                });
            if (found == function.parameters.end()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::UnknownReference,
                              QStringLiteral(
                                  "Pure function expressions may reference only parameters"),
                              range);
                return std::nullopt;
            }
            DslTypedExpression placeholder;
            placeholder.kind = found->type == DslScalarType::Bool
                                   ? DslTypedExpressionKind::BooleanLiteral
                                   : DslTypedExpressionKind::UnsignedLiteral;
            placeholder.type = found->type;
            return placeholder;
        };
        ExpressionBuildState state;
        const auto compiled = compileExpression(
            function.expression, resolveParameter, index, 1, state);
        if (compiled && compiled->type != function.returnType) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral(
                              "Pure function return expression type does not match"),
                          function.expression.range);
        }
    }

    typed.structs.reserve(program.structs.size());
    for (const DslStruct& structure : program.structs) {
        std::vector<const DslStructItem*> fieldDeclarations;
        collectFields(structure.items, fieldDeclarations);
        DslTypedStruct typedStruct;
        typedStruct.name = structure.name;
        typedStruct.metadata = metadataForAnnotations(structure.annotations);
        typedStruct.metadata.typeName = QStringLiteral("struct");
        if (fieldDeclarations.size() > maximumIndexedSize) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::InvalidType,
                          QStringLiteral("A structure contains too many fields"),
                          structure.range);
        }
        if (fieldDeclarations.empty()) {
            addDiagnostic(result.diagnostics,
                          DslDiagnosticCode::EmptyStruct,
                          QStringLiteral("A structure must contain at least one field"),
                          structure.range);
        }

        std::vector<QString> declaredFieldNames;
        declaredFieldNames.reserve(fieldDeclarations.size());
        for (const DslStructItem* item : fieldDeclarations) {
            const QString& name = item->kind == DslStructItemKind::Field
                                      ? item->field.name
                                      : item->computed.name;
            const DslSourceRange& range = item->kind == DslStructItemKind::Field
                                              ? item->field.range
                                              : item->computed.range;
            if (std::find(declaredFieldNames.begin(),
                          declaredFieldNames.end(),
                          name) != declaredFieldNames.end()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::DuplicateName,
                              QStringLiteral("Duplicate field name"),
                              range);
            }
            declaredFieldNames.push_back(name);
        }

        struct DeclaredField final {
            QString name;
            const DslBitField* source = nullptr;
            const DslComputedField* computed = nullptr;
            DslScalarType scalarType = DslScalarType::U64;
            std::optional<quint32> typedIndex;
            std::vector<DslTypedFieldCondition> conditions;
        };
        enum class ControllerUse : quint8 {
            Equality,
            Repeat,
            Boolean,
        };
        struct ResolvedController final {
            quint32 fieldIndex = 0;
            quint8 width = 0;
            DslFieldEncoding encoding = DslFieldEncoding::Bits;
            bool computed = false;
        };
        std::vector<DeclaredField> declaredFields;
        declaredFields.reserve(fieldDeclarations.size());
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
                                           ControllerUse use)
            -> std::optional<ResolvedController> {
            const auto found = std::find_if(
                declaredFields.rbegin(),
                declaredFields.rend(),
                [&fieldName](const DeclaredField& declared) {
                    return declared.name == fieldName;
                });
            if (found == declaredFields.rend()) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::UnknownReference,
                              QStringLiteral(
                                  "Controller field must be declared before the statement"),
                              range);
                return std::nullopt;
            }
            const bool syntaxScalar = found->source != nullptr &&
                                      !found->source->arrayLength;
            const bool supported =
                use == ControllerUse::Boolean
                    ? found->computed != nullptr &&
                          found->scalarType == DslScalarType::Bool
                    : found->computed != nullptr
                          ? found->scalarType == DslScalarType::U64
                          : syntaxScalar &&
                                (found->source->encoding == DslFieldEncoding::Bits ||
                                 (use == ControllerUse::Repeat &&
                                  found->source->encoding ==
                                      DslFieldEncoding::UnsignedExpGolomb));
            if (!supported || !found->typedIndex) {
                QString message = QStringLiteral(
                    "Controllers require a previous scalar bits, enum, or computed<u64> field");
                if (use == ControllerUse::Repeat) {
                    message = QStringLiteral(
                        "Repeat counts require a previous scalar bits, enum, ue, or "
                        "computed<u64> field");
                } else if (use == ControllerUse::Boolean) {
                    message = QStringLiteral(
                        "Boolean conditions require a previous computed<bool> field");
                }
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              message,
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
                *found->typedIndex,
                found->source != nullptr ? found->source->width : quint8(0),
                found->source != nullptr ? found->source->encoding
                                         : DslFieldEncoding::Bits,
                found->computed != nullptr,
            };
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
            const ControllerUse use = condition.booleanShorthand
                                          ? ControllerUse::Boolean
                                          : ControllerUse::Equality;
            const auto controller = resolveController(
                condition.fieldName, condition.range, active, use);
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
                declaredFields.push_back({field.name,
                                          &field,
                                          nullptr,
                                          DslScalarType::U64,
                                          std::nullopt,
                                          conditions});
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
                declaredFields.push_back({field.name,
                                          &field,
                                          nullptr,
                                          DslScalarType::U64,
                                          std::nullopt,
                                          conditions});
                return std::nullopt;
            }
            if (isBits && (field.width == 0 || field.width > 64)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidBitWidth,
                              QStringLiteral("Bit field width must be in the range 1..64"),
                              field.range);
                declaredFields.push_back({field.name,
                                          &field,
                                          nullptr,
                                          DslScalarType::U64,
                                          std::nullopt,
                                          conditions});
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
            declaredFields.push_back({field.name,
                                      &field,
                                      nullptr,
                                      DslScalarType::U64,
                                      firstTypedIndex,
                                      conditions});
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
        const auto compileComputed =
            [&](const DslComputedField& field,
                const std::vector<DslTypedFieldCondition>& conditions,
                std::optional<quint64> fieldOffset) -> std::optional<quint64> {
            if (typedStruct.fields.size() >= maximumExpandedFieldsPerStructure) {
                addDiagnostic(
                    result.diagnostics,
                    DslDiagnosticCode::InvalidArrayLength,
                    QStringLiteral(
                        "Computed field expansion exceeds the structure materialization limit"),
                    field.range);
                declaredFields.push_back({field.name,
                                          nullptr,
                                          &field,
                                          field.type,
                                          std::nullopt,
                                          conditions});
                return fieldOffset;
            }
            if (!validScalarType(field.type)) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral("Computed field type is invalid"),
                              field.range);
            }
            for (const DslAnnotation& annotation : field.annotations) {
                const bool descriptionValid =
                    annotation.name == QStringLiteral("description") &&
                    annotation.arguments.size() == 1 &&
                    annotation.arguments.front().kind ==
                        DslAnnotationValueKind::String;
                const bool specificationValid =
                    annotation.name == QStringLiteral("spec") &&
                    annotation.arguments.size() == 2 &&
                    annotation.arguments.at(0).kind ==
                        DslAnnotationValueKind::String &&
                    annotation.arguments.at(1).kind ==
                        DslAnnotationValueKind::String;
                if (!descriptionValid && !specificationValid) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidAnnotation,
                                  QStringLiteral(
                                      "Computed fields accept only valid @description and @spec "
                                      "annotations"),
                                  annotation.range);
                }
            }
            const ExpressionResolver resolveField =
                [&](const QString& name,
                    const DslSourceRange& range)
                -> std::optional<DslTypedExpression> {
                const auto found = std::find_if(
                    declaredFields.rbegin(),
                    declaredFields.rend(),
                    [&name](const DeclaredField& declared) {
                        return declared.name == name;
                    });
                if (found == declaredFields.rend()) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::UnknownReference,
                                  QStringLiteral(
                                      "Computed field dependency must be declared earlier"),
                                  range);
                    return std::nullopt;
                }
                const bool available = std::all_of(
                    found->conditions.begin(),
                    found->conditions.end(),
                    [&conditions, &sameCondition](
                        const DslTypedFieldCondition& required) {
                        return std::any_of(
                            conditions.begin(),
                            conditions.end(),
                            [&required, &sameCondition](
                                const DslTypedFieldCondition& candidate) {
                                return sameCondition(required, candidate);
                            });
                    });
                if (!available) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidCondition,
                                  QStringLiteral(
                                      "Computed field dependency is not guaranteed on the "
                                      "current branch"),
                                  range);
                    return std::nullopt;
                }
                if (!found->typedIndex ||
                    (found->source != nullptr &&
                     (found->source->arrayLength ||
                      found->source->encoding == DslFieldEncoding::SignedExpGolomb))) {
                    addDiagnostic(result.diagnostics,
                                  DslDiagnosticCode::InvalidType,
                                  QStringLiteral(
                                      "Computed expressions require scalar unsigned fields"),
                                  range);
                    return std::nullopt;
                }
                DslTypedExpression reference;
                reference.kind = DslTypedExpressionKind::FieldReference;
                reference.type = found->computed != nullptr
                                     ? found->scalarType
                                     : DslScalarType::U64;
                reference.fieldIndex = *found->typedIndex;
                return reference;
            };
            ExpressionBuildState state;
            const auto expression = compileExpression(field.expression,
                                                      resolveField,
                                                      program.pureFunctions.size(),
                                                      1,
                                                      state);
            if (expression && expression->type != field.type) {
                addDiagnostic(result.diagnostics,
                              DslDiagnosticCode::InvalidType,
                              QStringLiteral(
                                  "Computed expression type does not match its declaration"),
                              field.expression.range);
            }

            DslTypedField typedField;
            typedField.name = field.name;
            for (const quint64 repeatIndex : repeatIndices) {
                typedField.name += QStringLiteral("[%1]").arg(repeatIndex);
            }
            typedField.type.kind = field.type == DslScalarType::Bool
                                       ? DslValueTypeKind::ComputedBool
                                       : DslValueTypeKind::ComputedUnsigned;
            typedField.computedExpression = expression;
            typedField.conditions = conditions;
            typedField.metadata = metadataForAnnotations(
                field.annotations, typedStruct.metadata.specification);
            typedField.metadata.typeName = field.type == DslScalarType::Bool
                                               ? QStringLiteral("computed<bool>")
                                               : QStringLiteral("computed<u64>");
            typedField.range = field.range;
            const quint32 typedIndex = static_cast<quint32>(typedStruct.fields.size());
            typedStruct.fields.push_back(std::move(typedField));
            declaredFields.push_back(
                {field.name, nullptr, &field, field.type, typedIndex, conditions});
            return fieldOffset;
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
                if (item.kind == DslStructItemKind::Computed) {
                    fieldOffset = compileComputed(item.computed, conditions, fieldOffset);
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
                                                              ControllerUse::Repeat);
                    if (item.repeatMaximum == 0) {
                        addDiagnostic(result.diagnostics,
                                      DslDiagnosticCode::InvalidArrayLength,
                                      QStringLiteral(
                                          "Bounded repeat maximum must be at least one"),
                                      item.repeatMaximumRange);
                    } else if (controller && !controller->computed &&
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
                    item.switchFieldName,
                    item.switchFieldRange,
                    conditions,
                    ControllerUse::Equality);
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
                case DslValueTypeKind::ComputedBool:
                case DslValueTypeKind::ComputedUnsigned:
                    return DslOpcode::EvaluateComputed;
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

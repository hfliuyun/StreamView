#include <streamview/core/analysis_model.h>
#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/language_version.h>

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

namespace {

[[nodiscard]] const char* diagnosticCodeName(streamview::core::DiagnosticCode code) noexcept {
    switch (code) {
    case streamview::core::DiagnosticCode::TruncatedSource:
        return "truncated-source";
    case streamview::core::DiagnosticCode::InvalidSyntax:
        return "invalid-syntax";
    case streamview::core::DiagnosticCode::UnsupportedSyntax:
        return "unsupported-syntax";
    case streamview::core::DiagnosticCode::Cancelled:
        return "cancelled";
    case streamview::core::DiagnosticCode::SourceError:
        return "source-error";
    case streamview::core::DiagnosticCode::ResourceLimit:
        return "resource-limit";
    case streamview::core::DiagnosticCode::DependencyUnavailable:
        return "dependency-unavailable";
    }
    return "invalid-syntax";
}

void printLocation(QTextStream& stream, const streamview::core::FieldLocation& location) {
    const auto& spans = location.sourceSpans();
    for (std::size_t index = 0; index < spans.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        const streamview::core::SourceSpan& span = spans.at(index);
        stream << "source bits [" << span.start().absoluteBitOffset() << ", "
               << span.endExclusive().absoluteBitOffset() << ')';
    }
}

void printNode(const streamview::core::AnalysisTree& tree,
               streamview::core::AnalysisNodeId id,
               int depth,
               QTextStream& output,
               QTextStream& errors) {
    const auto node = tree.node(id);
    if (!node) {
        return;
    }

    output << QString(depth * 2, QLatin1Char(' ')) << node->name();
    if (node->value().isValid()) {
        output << " = " << node->value().toString();
    }
    if (node->location() && !node->location()->sourceSpans().empty()) {
        output << " @ ";
        printLocation(output, *node->location());
    }
    output << '\n';

    for (const streamview::core::ParseDiagnostic& diagnostic : node->diagnostics()) {
        errors << diagnosticCodeName(diagnostic.code) << ": " << diagnostic.fieldPath << ": "
               << diagnostic.message;
        if (diagnostic.location && !diagnostic.location->sourceSpans().empty()) {
            errors << " @ ";
            printLocation(errors, *diagnostic.location);
        }
        errors << '\n';
    }

    for (const streamview::core::AnalysisNodeId child : node->children()) {
        printNode(tree, child, depth + 1, output, errors);
    }
}

int checkRule(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream(stderr) << path << ": " << file.errorString() << '\n';
        return 2;
    }

    const auto parsed =
        streamview::rules::DslParser::parse(QString::fromUtf8(file.readAll()));
    if (!parsed.succeeded()) {
        QTextStream errorStream(stderr);
        for (const streamview::rules::DslDiagnostic& diagnostic : parsed.diagnostics) {
            errorStream << path << ':' << diagnostic.range.start.line << ':'
                        << diagnostic.range.start.column << ": error: " << diagnostic.message
                        << '\n';
        }
        return 1;
    }

    const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
    if (!compiled.succeeded()) {
        QTextStream errorStream(stderr);
        if (compiled.diagnostics.empty()) {
            errorStream << path << ":1:1: error: unable to produce executable typed IR\n";
        }
        for (const streamview::rules::DslDiagnostic& diagnostic : compiled.diagnostics) {
            errorStream << path << ':' << diagnostic.range.start.line << ':'
                        << diagnostic.range.start.column << ": error: " << diagnostic.message
                        << '\n';
        }
        return 1;
    }

    QTextStream(stdout) << "Rule OK: " << path << '\n';
    return 0;
}

int analyzeSource(const QString& path) {
    QString errorMessage;
    auto source = streamview::core::FileSource::open(path, &errorMessage);
    if (!source) {
        QTextStream(stderr) << path << ": " << errorMessage << '\n';
        return 2;
    }

    auto analyzer = streamview::rules::H264AnnexBAnalyzer::create(*source, &errorMessage);
    if (!analyzer) {
        QTextStream(stderr) << path << ": " << errorMessage << '\n';
        return 2;
    }

    streamview::rules::H264AnnexBAnalysisStatus finalStatus =
        streamview::rules::H264AnnexBAnalysisStatus::InProgress;
    while (!analyzer->finished()) {
        const auto batch = analyzer->analyzeBatch();
        finalStatus = batch.status;
    }

    QTextStream output(stdout);
    QTextStream errors(stderr);
    printNode(analyzer->tree(), analyzer->tree().rootId(), 0, output, errors);
    if (finalStatus != streamview::rules::H264AnnexBAnalysisStatus::Complete ||
        analyzer->tree().hasPartialResults()) {
        return 1;
    }
    return 0;
}

void printUsage() {
    QTextStream(stderr) << "Usage:\n"
                        << "  svtool --version\n"
                        << "  svtool rule check <rule.svfmt>\n"
                        << "  svtool analyze <source>\n";
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();

    if (arguments.size() == 2 &&
        (arguments.at(1) == QStringLiteral("--version") ||
         arguments.at(1) == QStringLiteral("version"))) {
        QTextStream(stdout) << "svtool " << streamview::core::version() << " (DSL "
                            << streamview::rules::languageVersion() << ")\n";
        return 0;
    }
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("rule") &&
        arguments.at(2) == QStringLiteral("check")) {
        return checkRule(arguments.at(3));
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("analyze")) {
        return analyzeSource(arguments.at(2));
    }

    printUsage();
    return 2;
}

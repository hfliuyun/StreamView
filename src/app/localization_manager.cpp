#include "localization_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QString>
#include <QTranslator>
#include <QWidget>

#include <string_view>
#include <unordered_map>

namespace streamview::app {

namespace {

class ChineseTranslator final : public QTranslator {
public:
    ChineseTranslator() = default;
    ~ChineseTranslator() override = default;

    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return false; }
};

QString ChineseTranslator::translate(const char* /*context*/, const char* sourceText,
                                     const char* /*disambiguation*/, int /*n*/) const {
    if (sourceText == nullptr) {
        return {};
    }

    static const std::unordered_map<std::string_view, QString> s_translations = {
        // Main Menus
        {"&File", QString::fromUtf8("文件(&F)")},
        {"&Open...", QString::fromUtf8("打开(&O)...")},
        {"Open &Session...", QString::fromUtf8("打开会话(&S)...")},
        {"&Save Session", QString::fromUtf8("保存会话(&S)")},
        {"Save Session &As...", QString::fromUtf8("另存会话为(&A)...")},
        {"E&xit", QString::fromUtf8("退出(&X)")},
        {"&Edit", QString::fromUtf8("编辑(&E)")},
        {"&View", QString::fromUtf8("视图(&V)")},
        {"Theme", QString::fromUtf8("主题")},
        {"System", QString::fromUtf8("跟随系统")},
        {"Light", QString::fromUtf8("浅色")},
        {"Dark", QString::fromUtf8("深色")},
        {"Language", QString::fromUtf8("语言")},
        {"English", QString::fromUtf8("English")},
        {"Simplified Chinese (简体中文)", QString::fromUtf8("简体中文")},
        {"&Diagnostics", QString::fromUtf8("诊断(&D)")},
        {"&Timeline", QString::fromUtf8("时间线(&T)")},
        {"&Analysis", QString::fromUtf8("分析(&A)")},
        {"&Override Format...", QString::fromUtf8("覆盖格式(&O)...")},
        {"&Tools", QString::fromUtf8("工具(&T)")},
        {"&Manage Rules...", QString::fromUtf8("管理规则(&M)...")},
        {"&Help", QString::fromUtf8("帮助(&H)")},
        {"&About StreamView", QString::fromUtf8("关于 StreamView(&A)")},

        // Docks & Navigation
        {"Analysis Tree", QString::fromUtf8("分析树")},
        {"Field Inspector", QString::fromUtf8("字段检视器")},
        {"Timeline", QString::fromUtf8("时间线")},
        {"Diagnostics", QString::fromUtf8("诊断")},
        {"Return to parent", QString::fromUtf8("返回上一级")},
        {"Return to parent format", QString::fromUtf8("返回父级格式")},

        // Status bar & progress & cancel
        {"Cancel", QString::fromUtf8("取消")},
        {"Ready", QString::fromUtf8("就绪")},
        {"Session saved", QString::fromUtf8("会话已保存")},
        {"Analysis cancelled: %1 nodes", QString::fromUtf8("分析已取消：%1 个节点")},
        {"Analysing: %1% (%2 nodes)", QString::fromUtf8("正在分析：%1% (%2 个节点)")},
        {"Analysis completed: %1 nodes", QString::fromUtf8("分析完成：%1 个节点")},

        // Ambiguity banner
        {"Ambiguous media format detected.", QString::fromUtf8("检测到存在歧义的媒体格式。")},
        {"Resolve Ambiguity...", QString::fromUtf8("解决歧义...")},

        // Diagnostics summary dock
        {"Total: 0", QString::fromUtf8("总计: 0")},
        {"Total: %1 | Errors: %2 | Warnings: %3 | Info: %4", QString::fromUtf8("总计: %1 | 错误: %2 | 警告: %3 | 信息: %4")},
        {"Filter:", QString::fromUtf8("过滤:")},
        {"All Severities", QString::fromUtf8("全部严重性")},
        {"Errors Only", QString::fromUtf8("仅错误")},
        {"Warnings & Errors", QString::fromUtf8("警告与错误")},
        {"Info", QString::fromUtf8("信息")},
        {"Severity", QString::fromUtf8("严重性")},
        {"Message", QString::fromUtf8("诊断消息")},
        {"Field / Node", QString::fromUtf8("字段/节点")},
        {"Offset / Range", QString::fromUtf8("偏移/区间")},

        // Format override dialog
        {"Override Format", QString::fromUtf8("覆盖格式")},
        {"Select format rule to parse this file:", QString::fromUtf8("选择解析该文件的格式规则：")},
        {"OK", QString::fromUtf8("确定")},

        // Rule manager dialog
        {"Rule Manager", QString::fromUtf8("规则管理器")},
        {"Install Package (.svrule)...", QString::fromUtf8("安装规则包 (.svrule)...")},
        {"Close", QString::fromUtf8("关闭")},
        {"Version", QString::fromUtf8("版本")},
        {"Origin", QString::fromUtf8("来源")},
        {"Content Hash", QString::fromUtf8("内容哈希")},
        {"Entry Points", QString::fromUtf8("入口点")},
        {"Description", QString::fromUtf8("描述")},
        {"[Bundled]", QString::fromUtf8("[内置]")},
        {"[Installed]", QString::fromUtf8("[已安装]")},

        // Dialog prompts
        {"Save Changes", QString::fromUtf8("保存更改")},
        {"Discard", QString::fromUtf8("放弃")},
        {"The document has been modified.\nDo you want to save your changes?",
         QString::fromUtf8("文档已被修改。\n是否保存所做的更改？")},
        {"No data", QString::fromUtf8("无数据")},
        {"Previous source page", QString::fromUtf8("上一源页面")},
        {"Next source page", QString::fromUtf8("下一源页面")}
    };

    auto it = s_translations.find(std::string_view(sourceText));
    if (it != s_translations.end()) {
        return it->second;
    }
    return {};
}

} // namespace

LocalizationManager& LocalizationManager::instance() {
    static LocalizationManager manager;
    return manager;
}

LocalizationManager::LocalizationManager(QObject* parent) : QObject(parent) {
}

void LocalizationManager::setLanguage(Language lang) {
    if (currentLanguage_ == lang) {
        return;
    }
    currentLanguage_ = lang;
    if (lang == Language::SimplifiedChinese) {
        if (!chineseTranslator_) {
            chineseTranslator_ = std::make_unique<ChineseTranslator>();
        }
        QCoreApplication::installTranslator(chineseTranslator_.get());
    } else {
        if (chineseTranslator_) {
            QCoreApplication::removeTranslator(chineseTranslator_.get());
        }
    }

    for (QWidget* widget : QApplication::topLevelWidgets()) {
        QCoreApplication::postEvent(widget, new QEvent(QEvent::LanguageChange));
    }

    emit languageChanged(lang);
}

} // namespace streamview::app

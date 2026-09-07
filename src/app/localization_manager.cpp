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

const std::unordered_map<std::string_view, QString>& chineseTranslations() {
    static const std::unordered_map<std::string_view, QString> s_translations = {
        // Ambiguity & Format Overrides
        {" [Candidate]", QString::fromUtf8(" [候选]")},
        {" [Current]", QString::fromUtf8(" [当前]")},
        {"Ambiguous media format detected.", QString::fromUtf8("检测到存在歧义的媒体格式。")},
        {"Resolve Ambiguity...", QString::fromUtf8("解决歧义...")},
        {"Select the target format to override automatic format detection:",
         QString::fromUtf8("选择目标格式以覆盖自动格式检测：")},
        {"Override Format", QString::fromUtf8("覆盖格式")},
        {"Override", QString::fromUtf8("覆盖")},
        {"Format Override Failed", QString::fromUtf8("格式覆盖失败")},
        {"Failed to override format:\n%1", QString::fromUtf8("覆盖格式失败：\n%1")},
        {"Warning: Ambiguous format (container vs elementary stream detected)",
         QString::fromUtf8("警告：检测到歧义格式（同时检测到容器与基本流）")},

        // Main Menus & Actions
        {"&File", QString::fromUtf8("文件(&F)")},
        {"&Open...", QString::fromUtf8("打开(&O)...")},
        {"Open &Session...", QString::fromUtf8("打开会话(&S)...")},
        {"&Save Session", QString::fromUtf8("保存会话(&S)")},
        {"Save Session &As...", QString::fromUtf8("另存会话为(&A)...")},
        {"E&xit", QString::fromUtf8("退出(&X)")},
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

        // Docks, Panes & Navigation
        {"Analysis Tree", QString::fromUtf8("分析树")},
        {"Field Inspector", QString::fromUtf8("字段检视器")},
        {"Timeline & Samples", QString::fromUtf8("时间线与样本")},
        {"Diagnostics", QString::fromUtf8("诊断")},
        {"Return to parent", QString::fromUtf8("返回上一级")},
        {"Return to parent format", QString::fromUtf8("返回父级格式")},

        // Status Bar & Progress & Cancellation
        {"Cancel", QString::fromUtf8("取消")},
        {"Ready", QString::fromUtf8("就绪")},
        {"Analyzing %1: %2/%3 bytes, %4 nodes", QString::fromUtf8("正在分析 %1：%2/%3 字节，%4 个节点")},
        {"Analysis complete: %1 nodes", QString::fromUtf8("分析完成：%1 个节点")},
        {"Analysis finished with partial results: %1 nodes",
         QString::fromUtf8("分析完成（部分结果）：%1 个节点")},
        {"Analysis stopped: %1 (%2 nodes)", QString::fromUtf8("分析停止：%1（%2 个节点）")},
        {"Analysis cancelled: %1 nodes", QString::fromUtf8("分析已取消：%1 个节点")},
        {"Analysis batch rejected: %1", QString::fromUtf8("分析批次被拒绝：%1")},
        {"Analysis tree publication failed", QString::fromUtf8("分析树发布失败")},
        {"unknown analysis error", QString::fromUtf8("未知分析错误")},
        {"Operation cancelled by user", QString::fromUtf8("用户取消了操作")},

        // Diagnostics Summary Dock
        {"Total: 0", QString::fromUtf8("总计: 0")},
        {"Total: %1 (Errors: %2, Warnings: %3, Info: %4)",
         QString::fromUtf8("总计：%1（错误：%2，警告：%3，信息：%4）")},
        {"Filter:", QString::fromUtf8("过滤:")},
        {"All Severities", QString::fromUtf8("全部严重性")},
        {"Errors Only", QString::fromUtf8("仅错误")},
        {"Warnings & Errors", QString::fromUtf8("警告与错误")},
        {"Info", QString::fromUtf8("信息")},
        {"Severity", QString::fromUtf8("严重性")},
        {"Message", QString::fromUtf8("消息")},
        {"Field / Node", QString::fromUtf8("字段/节点")},
        {"Offset / Range", QString::fromUtf8("偏移/范围")},
        {"Error", QString::fromUtf8("错误")},
        {"Warning", QString::fromUtf8("警告")},

        // Field Inspector
        {"Field", QString::fromUtf8("字段")},
        {"Value", QString::fromUtf8("值")},
        {"Type", QString::fromUtf8("类型")},
        {"Width", QString::fromUtf8("位宽")},
        {"Source spans", QString::fromUtf8("源区间")},
        {"Logical range", QString::fromUtf8("逻辑范围")},
        {"Description", QString::fromUtf8("描述")},
        {"Specification", QString::fromUtf8("规范")},
        {"No field selected.", QString::fromUtf8("未选择字段。")},

        // Raw Data View
        {"Hex", QString::fromUtf8("十六进制")},
        {"Binary", QString::fromUtf8("二进制")},
        {"Combined", QString::fromUtf8("组合")},
        {"Previous source page", QString::fromUtf8("上一源页面")},
        {"Next source page", QString::fromUtf8("下一源页面")},
        {"No data", QString::fromUtf8("无数据")},
        {"Page %1 of %2", QString::fromUtf8("第 %1 页，共 %2 页")},
        {"Offset", QString::fromUtf8("偏移")},

        // Analysis Tree Model Columns
        {"Name", QString::fromUtf8("名称")},
        {"Source Bits", QString::fromUtf8("源比特")},
        {"State", QString::fromUtf8("状态")},

        // Timeline & Tracks
        {"Track:", QString::fromUtf8("轨道:")},
        {"Track %1", QString::fromUtf8("轨道 %1")},
        {"Track %1 (%2)", QString::fromUtf8("轨道 %1 (%2)")},
        {"Track %1 (%2, %3 samples)", QString::fromUtf8("轨道 %1 (%2，%3 个样本)")},
        {"Container tracks unavailable for elementary stream",
         QString::fromUtf8("基本流无容器轨道")},
        {"File is truncated; track list may be incomplete",
         QString::fromUtf8("文件已截断；轨道列表可能不完整")},
        {"No tracks found", QString::fromUtf8("未找到轨道")},
        {"No tracks: %1", QString::fromUtf8("无轨道：%1")},
        {"No samples", QString::fromUtf8("无样本")},
        {"Previous sample page", QString::fromUtf8("上一页样本")},
        {"Next sample page", QString::fromUtf8("下一页样本")},
        {"Page %1 of %2 (samples %3-%4 of %5)",
         QString::fromUtf8("第 %1 页，共 %2 页（样本 %3-%4，共 %5 个）")},
        {"Sample #", QString::fromUtf8("样本序号")},
        {"Sample #%1%2", QString::fromUtf8("样本 #%1%2")},
        {"DTS", QString::fromUtf8("DTS")},
        {"DTS: %1 s", QString::fromUtf8("DTS: %1 秒")},
        {"PTS", QString::fromUtf8("PTS")},
        {"PTS: %1 s", QString::fromUtf8("PTS: %1 秒")},
        {"Duration", QString::fromUtf8("时长")},
        {"Size (B)", QString::fromUtf8("大小 (字节)")},
        {"Bit Offset", QString::fromUtf8("位偏移")},
        {"Desc Idx", QString::fromUtf8("描述符索引")},
        {"Sync sample / Keyframe (random access point)",
         QString::fromUtf8("同步样本 / 关键帧 (随机访问点)")},
        {"Non-sync sample / Delta frame", QString::fromUtf8("非同步样本 / 差异帧")},
        {"Cannot enter sample: %1", QString::fromUtf8("无法进入样本：%1")},
        {"Cannot enter sample: child root is unavailable",
         QString::fromUtf8("无法进入样本：子根节点不可用")},
        {"Cannot enter sub-format: %1", QString::fromUtf8("无法进入子格式：%1")},
        {"Cannot enter sub-format: child root is unavailable",
         QString::fromUtf8("无法进入子格式：子根节点不可用")},

        // Rule Manager Dialog
        {"Rule Package Manager", QString::fromUtf8("规则包管理器")},
        {"ID", QString::fromUtf8("ID")},
        {"Version", QString::fromUtf8("版本")},
        {"Origin", QString::fromUtf8("来源")},
        {"Content Hash", QString::fromUtf8("内容哈希")},
        {"Entry Points", QString::fromUtf8("入口点")},
        {"[Bundled]", QString::fromUtf8("[内置]")},
        {"[Installed]", QString::fromUtf8("[已安装]")},
        {"Install Package (.svrule)...", QString::fromUtf8("安装规则包 (.svrule)...")},
        {"Install Rule Package", QString::fromUtf8("安装规则包")},
        {"StreamView Rule Package (*.svrule);;All Files (*)",
         QString::fromUtf8("StreamView 规则包 (*.svrule);;所有文件 (*)")},
        {"Invalid Rule Package", QString::fromUtf8("无效规则包")},
        {"Unknown archive corruption or format error", QString::fromUtf8("未知归档损坏或格式错误")},
        {"Failed to read package archive %1:\n%2",
         QString::fromUtf8("读取规则包归档失败 %1：\n%2")},
        {"Package Installation Rejected", QString::fromUtf8("规则包安装被拒绝")},
        {"Cannot overwrite or replace bundled official rule package: %1",
         QString::fromUtf8("无法覆盖或替换内置官方规则包：%1")},
        {"Installation Failed", QString::fromUtf8("安装失败")},
        {"Failed to store package content:\n%1", QString::fromUtf8("存储规则包内容失败：\n%1")},
        {"Registration Conflict", QString::fromUtf8("注册冲突")},
        {"Failed to register package:\n%1", QString::fromUtf8("注册规则包失败：\n%1")},
        {"Package Installed", QString::fromUtf8("规则包已安装")},
        {"Successfully installed rule package %1 (v%2).",
         QString::fromUtf8("成功安装规则包 %1 (v%2)。")},

        // File & Session Dialogs
        {"Open Media File", QString::fromUtf8("打开媒体文件")},
        {"H.264 Annex B (*.264 *.h264 *.bin);;All Files (*)",
         QString::fromUtf8("H.264 Annex B (*.264 *.h264 *.bin);;所有文件 (*)")},
        {"Cannot Open File", QString::fromUtf8("无法打开文件")},
        {"Could not open %1:\n%2", QString::fromUtf8("无法打开 %1：\n%2")},
        {"Opened %1, but raw data could not be read: %2",
         QString::fromUtf8("已打开 %1，但无法读取原始数据：%2")},
        {"Open Session", QString::fromUtf8("打开会话")},
        {"StreamView Session (*.svsession);;All Files (*)",
         QString::fromUtf8("StreamView 会话 (*.svsession);;所有文件 (*)")},
        {"Cannot Open Session", QString::fromUtf8("无法打开会话")},
        {"Save Session", QString::fromUtf8("保存会话")},
        {"Save Session Failed", QString::fromUtf8("保存会话失败")},
        {"Could not save session to %1:\n%2", QString::fromUtf8("无法将会话保存到 %1：\n%2")},
        {"Session saved: %1", QString::fromUtf8("会话已保存：%1")},
        {"Failed to restore session", QString::fromUtf8("恢复会话失败")},
        {"Unsaved Changes", QString::fromUtf8("未保存的更改")},
        {"The current session has unsaved changes. Do you want to save your changes before proceeding?",
         QString::fromUtf8("当前会话有未保存的更改。是否在继续之前保存更改？")},
        {"unknown", QString::fromUtf8("未知")},
    };
    return s_translations;
}

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
        return QString();
    }

    const auto& table = chineseTranslations();
    const auto it = table.find(std::string_view(sourceText));
    if (it != table.end()) {
        return it->second;
    }
    // Return null QString to allow Qt translation fallback to original source text
    return QString();
}

} // namespace

LocalizationManager& LocalizationManager::instance() {
    static LocalizationManager manager;
    return manager;
}

LocalizationManager::LocalizationManager(QObject* parent) : QObject(parent) {
}

bool LocalizationManager::hasChineseTranslation(std::string_view sourceText) {
    const auto& table = chineseTranslations();
    return table.find(sourceText) != table.end();
}

QString LocalizationManager::translateToChinese(std::string_view sourceText) {
    const auto& table = chineseTranslations();
    const auto it = table.find(sourceText);
    return it != table.end() ? it->second : QString();
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

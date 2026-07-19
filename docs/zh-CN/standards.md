# 首个里程碑规范基线

StreamView 只记录字段名、带版本的章节引用和项目原创简释，不把标准正文提交到仓库或随应用分发。

| 范围 | 规范基线 |
|---|---|
| H.264 elementary stream 语法 | ITU-T Recommendation H.264 (08/2024) |
| AAC-LC 与 AudioSpecificConfig | ISO/IEC 14496-3:2019，第 5 版 |
| MPEG-4 Systems 描述符（`esds`） | ISO/IEC 14496-1:2010 |
| ISO Base Media File Format | ISO/IEC 14496-12:2026，第 8 版 |
| ISO BMFF 中的 AVC 承载 | ISO/IEC 14496-15:2024 及 Amendment 1:2025 |

不得静默替换成预发布版或草案。更新规范基线前必须增加 ADR、完成字段级兼容评审并更新一致性测试样例。

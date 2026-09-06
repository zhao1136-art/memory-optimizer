#ifndef REPORT_H
#define REPORT_H
#include "sampler.h"
#include "json.h"

/* 生成静态 HTML 报告字符串（malloc，调用方 free） */
char *report_build_html(sampler *s);
/* 生成某进程时间序列 CSV（malloc，调用方 free）；pid=0 时导出系统序列 */
char *report_build_csv(sampler *s, uint32_t pid);
/* 生成完整报告 JSON（malloc，调用方 free）；含系统/热点/泄漏/建议/进程 */
char *report_build_json(sampler *s);
/* 生成 Excel 可打开的 HTML 表格 .xls（malloc，调用方 free） */
char *report_build_xls(sampler *s);
/* 生成原生 PDF（含中文，内嵌字体子集；字体缺失时降级为 ASCII），malloc */
char *report_build_pdf(sampler *s, const char *font_path);

#endif
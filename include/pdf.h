#ifndef PDF_H
#define PDF_H
#include "sampler.h"

/* 生成原生 PDF（含中文，内嵌 simhei 字形子集）。
 * font_path 可为空(NULL/空串)时使用内置默认 C:\Windows\Fonts\simhei.ttf。
 * 字体缺失时自动降级为纯 ASCII 输出，数据完整性不受影响。
 * 返回 malloc 缓冲（调用方 free）。out_len 可为 NULL，否则回填字节长度（PDF 为二进制，不能用 strlen）。 */
char *pdf_build_report(sampler *s, const char *font_path, size_t *out_len);

#endif
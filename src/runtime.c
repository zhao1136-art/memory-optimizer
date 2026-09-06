/* 运行环境识别：按进程可执行文件名匹配常见运行时 */
#include <string.h>
#include <ctype.h>
#include "runtime.h"
#include "memtool.h"

static void lower(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        char c = s[i];
        d[i] = (char)tolower((unsigned char)c);
    }
    d[i] = '\0';
}

rt_kind runtime_identify(const char *name) {
    char b[96];
    lower(b, name ? name : "", sizeof b);
    if (strstr(b, "python")) return RT_PYTHON;
    if (strstr(b, "java") || strstr(b, "jvm") || strstr(b, "javaw") ||
        strstr(b, "jdk") || strstr(b, "jre")) return RT_JVM;
    if (strstr(b, "node.exe") || strstr(b, "nodejs") || strstr(b, "npx") ||
        strstr(b, "npm") || strstr(b, "pnpm") || strstr(b, "yarn")) return RT_NODE;
    if (strstr(b, "powershell")) return RT_POWERSHELL;
    if (strstr(b, "cmd.exe") || strstr(b, "cmd.exe")) return RT_CMD;
    if (strstr(b, "dotnet") || strstr(b, "csc") || strstr(b, "msbuild")) return RT_DOTNET;
    if (strstr(b, "go.exe") || strstr(b, "gogui") ) return RT_GO;
    if (strstr(b, "cargo") || strstr(b, "rustc")) return RT_RUST;
    return RT_CPP_NATIVE;
}

const char *runtime_label(rt_kind rt) {
    switch (rt) {
        case RT_PYTHON: return "Python";
        case RT_JVM: return "JVM/Java";
        case RT_NODE: return "Node.js";
        case RT_POWERSHELL: return "PowerShell";
        case RT_CMD: return "CMD";
        case RT_DOTNET: return ".NET";
        case RT_GO: return "Go";
        case RT_RUST: return "Rust";
        case RT_CPP_NATIVE: return "Native";
        default: return "Unknown";
    }
}
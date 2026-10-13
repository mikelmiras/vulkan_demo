#include "app/exe_metadata.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace exemeta {

namespace {

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t b = (uint8_t)s[i];
        int n = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : 0;
        uint32_t cp = n == 0 ? b : b & (0x3F >> n);
        for (int k = 1; k <= n && i + (size_t)k < s.size(); ++k) cp = (cp << 6) | ((uint8_t)s[i + (size_t)k] & 0x3F);
        i += (size_t)n + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

// The version's four numbers (missing ones 0)
void versionNumbers(const std::string& v, uint16_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = 0;
    size_t at = 0;
    for (int i = 0; i < 4 && at < v.size(); ++i) {
        out[i] = (uint16_t)std::strtoul(v.c_str() + at, nullptr, 10);
        size_t dot = v.find('.', at);
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
}

// One block of the resource: wLength, wValueLength, wType, its key (UTF-16, ended), padding to 4 bytes, its value,
// padding, its children
struct Block {
    std::u16string key;
    std::string value;        // (bytes)
    uint16_t valueLength = 0; // (in WORDs for text, bytes for binary)
    bool text = false;
    std::vector<Block> children;
};

void put16(std::string& b, uint16_t v) {
    b += (char)(v & 0xFF);
    b += (char)(v >> 8);
}

void pad(std::string& b) {
    while (b.size() % 4) b += '\0';
}

std::string write(const Block& block) {
    std::string b;
    put16(b, 0);   // (the length: filled in at the end)
    put16(b, block.valueLength);
    put16(b, block.text ? 1 : 0);
    for (char16_t c : block.key) put16(b, (uint16_t)c);
    put16(b, 0);
    pad(b);
    b += block.value;
    for (const Block& c : block.children) {
        pad(b);
        b += write(c);
    }
    uint16_t length = (uint16_t)b.size();
    b[0] = (char)(length & 0xFF);
    b[1] = (char)(length >> 8);
    return b;
}

Block stringBlock(const std::string& name, const std::string& value) {
    Block s;
    s.key = utf16(name);
    std::u16string v = utf16(value);
    for (char16_t c : v) put16(s.value, (uint16_t)c);
    put16(s.value, 0);
    s.valueLength = (uint16_t)(v.size() + 1);
    s.text = true;
    return s;
}

std::string xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

}  // namespace

std::string versionResource(const Info& info) {
    uint16_t v[4];
    versionNumbers(info.version, v);
    std::string text = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." + std::to_string(v[3]);
    Block root;
    root.key = u"VS_VERSION_INFO";
    const uint32_t fixed[13] = {0xFEEF04BDu, 0x00010000u, (uint32_t)v[0] << 16 | v[1], (uint32_t)v[2] << 16 | v[3],
                                (uint32_t)v[0] << 16 | v[1], (uint32_t)v[2] << 16 | v[3], 0x3Fu, 0u,
                                0x00040004u /* VOS_NT_WINDOWS32 */, 1u /* VFT_APP */, 0u, 0u, 0u};
    for (uint32_t d : fixed) {
        put16(root.value, (uint16_t)(d & 0xFFFF));
        put16(root.value, (uint16_t)(d >> 16));
    }
    root.valueLength = 52;   // (VS_FIXEDFILEINFO)
    Block strings;
    strings.key = u"StringFileInfo";
    strings.text = true;
    Block table;
    table.key = u"040904B0";   // (English (US), Unicode)
    table.text = true;
    std::vector<std::pair<std::string, std::string>> fields = {
        {"CompanyName", info.company}, {"FileDescription", info.product}, {"FileVersion", text},
        {"InternalName", info.product}, {"LegalCopyright", info.company.empty() ? "" : "(c) " + info.company},
        {"OriginalFilename", info.executable}, {"ProductName", info.product}, {"ProductVersion", info.version}};
    for (const auto& [name, value] : fields)
        if (!value.empty()) table.children.push_back(stringBlock(name, value));
    strings.children.push_back(table);
    Block vars;
    vars.key = u"VarFileInfo";
    vars.text = true;
    Block translation;
    translation.key = u"Translation";
    put16(translation.value, 0x0409);
    put16(translation.value, 0x04B0);
    translation.valueLength = 4;
    vars.children.push_back(translation);
    root.children = {strings, vars};
    return write(root);
}

std::string identifierFor(const Info& info) {
    if (!info.identifier.empty()) return info.identifier;
    std::string id = "org.vkworld.game.";
    for (char c : info.product)
        if (std::isalnum((unsigned char)c)) id += (char)std::tolower((unsigned char)c);
    return id;
}

std::string infoPlist(const Info& info) {
    std::string version = info.version.empty() ? "1.0.0" : info.version;
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "  <key>CFBundleName</key><string>" + xml(info.product) + "</string>\n"
        "  <key>CFBundleDisplayName</key><string>" + xml(info.product) + "</string>\n"
        "  <key>CFBundleExecutable</key><string>" + xml(info.executable) + "</string>\n"
        "  <key>CFBundleIdentifier</key><string>" + xml(identifierFor(info)) + "</string>\n"
        "  <key>CFBundlePackageType</key><string>APPL</string>\n"
        "  <key>CFBundleShortVersionString</key><string>" + xml(version) + "</string>\n"
        "  <key>CFBundleVersion</key><string>" + xml(version) + "</string>\n"
        "  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>\n"
        "  <key>NSHighResolutionCapable</key><true/>\n"
        "  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>\n";   // (the integrated GPU unless asked)
    if (!info.company.empty()) out += "  <key>NSHumanReadableCopyright</key><string>(c) " + xml(info.company) + "</string>\n";
    if (info.icon) out += "  <key>CFBundleIconFile</key><string>icon</string>\n";
    return out + "</dict></plist>\n";
}

}  // namespace exemeta

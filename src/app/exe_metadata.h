// What an exported game says about itself to the operating system: on Windows a version resource in the executable
// (Explorer's Properties > Details: product, company, version, copyright), on macOS the bundle's Info.plist
// (identifier, version, copyright). Plain functions making the bytes (tested); app/export.cpp puts them in place.
#pragma once

#include <string>

namespace exemeta {

struct Info {
    std::string product;      // the game's name
    std::string company;      // its publisher (may be empty)
    std::string version;      // "1.2.0" (up to four numbers)
    std::string identifier;   // reverse-DNS ("com.example.coinhunt"); "" for one made from the name
    std::string executable;   // the executable's file name ("Coin Hunt.exe", "Coin Hunt")
    bool icon = false;        // (macOS) the bundle has Resources/icon.icns
};

// A Windows VS_VERSIONINFO resource (RT_VERSION), English (US), Unicode
std::string versionResource(const Info& info);
// A macOS application bundle's Info.plist
std::string infoPlist(const Info& info);
// The identifier to use: `identifier`, else "org.vkworld.game." and the name's letters and digits
std::string identifierFor(const Info& info);

}  // namespace exemeta

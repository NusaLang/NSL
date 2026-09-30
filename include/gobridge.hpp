#pragma once

#include <string>
#include <vector>

// `nusa go ...`: use Go packages from Nusantara. A generic reflection-based bridge
// (tools/gobridge/) is built once per wrapped module into a plugin, so running the result
// needs no Go toolchain -- only *building* it does.
namespace gobridge {

struct EmbeddedFile {
    const char* path;
    std::string content;
};

const std::vector<EmbeddedFile>& embeddedFiles();  // generated: gobridge_embed.cpp

int runCommand(int argc, char** argv);

}  // namespace gobridge

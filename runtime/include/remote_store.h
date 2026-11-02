#pragma once

// A game server on the local network (configured with [remote] url in Config.toml) that holds
// the disc files the device does not keep, and a copy of the game's saves.
//
// Disc files: at DVD start-up the server's file list is merged into the disc index, so a file
// that is missing locally is read from the server instead. Reads go through a small in-memory
// chunk cache with read-ahead, so sequential readers (movies, streamed music) rarely wait on
// the network.
//
// Saves: when the game starts and a save the server has is missing locally (tvOS can delete an
// app's Caches folder to make room), it is downloaded before the game reads it. While the game
// runs, saves that changed and then stayed unchanged for a few seconds are uploaded; the server
// keeps earlier versions.
//
// Protocol (HTTP/1.1, plain text on the local network, X-Wii-Token header on every request):
//   GET  /g/<game>/manifest          "<size>\t<path>\n" per disc file, paths relative to files/
//   GET  /g/<game>/data/<path>       a disc file; honours "Range: bytes=a-b"
//   GET  /g/<game>/save-index        "<size>\t<fnv1a64 hex>\t<path>\n" per save file
//   GET  /g/<game>/save/<path>       a save file
//   PUT  /g/<game>/save/<path>       store a save file (the server keeps the previous version)

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace RemoteStore {

// Disc files that live on the server are indexed under host paths that start with this prefix,
// followed by the file's path relative to the disc's files/ directory.
inline constexpr const char* kHostPrefix = "wiicompiled-remote:";

struct ManifestEntry {
    std::string path; // relative to files/, forward slashes
    uint64_t size = 0;
};

// Whether a server is configured.
bool Enabled();

// The configured server, for messages ("http://host:port").
std::string ServerText();

// The disc files the server has for this game ("RMCP"). False with a reason when it cannot be
// reached.
bool FetchManifest(const std::string& gameCode, std::vector<ManifestEntry>& entries, std::string& error);

bool IsRemoteHostPath(const std::filesystem::path& hostPath);
std::filesystem::path MakeRemoteHostPath(const std::string& relativePath);

// Reads exactly `length` bytes at `offset` of a server disc file into `destination`. Retries for
// a while before giving up, because the game cannot continue without the data.
bool ReadDiscRange(const std::filesystem::path& hostPath, uint64_t offset, uint32_t length,
                   std::vector<uint8_t>& destination, std::string& error);

// Restores saves missing under <nandRoot>/<scope> from the server, then keeps uploading changes
// from a background thread. Call once, before the game opens its save.
void StartSaveSync(const std::filesystem::path& nandRoot, const std::string& gameCode);

} // namespace RemoteStore

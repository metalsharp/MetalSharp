/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <utility>

#include "log.hpp"

#include "../util_env.hpp"
#include "util_string.hpp"

namespace dxmt {

namespace {

bool enableWineLogOutput() {
  const std::string value = env::getEnvVar("DXMT_ENABLE_WINE_LOG_OUTPUT");
  return value == "1" || value == "true" || value == "TRUE";
}

std::string sanitizeLogLine(const std::string &line) {
  constexpr size_t MaxLogLineBytes = 4096;
  std::string sanitized;
  sanitized.reserve(std::min(line.size(), MaxLogLineBytes) + 32);

  size_t copied = 0;
  for (unsigned char ch : line) {
    if (copied >= MaxLogLineBytes) {
      sanitized += "...<truncated>";
      break;
    }

    if (ch == '\t' || ch == '\r' || (ch >= 0x20 && ch != 0x7f)) {
      sanitized.push_back(static_cast<char>(ch));
      copied++;
      continue;
    }

    char escaped[5] = {};
    std::snprintf(escaped, sizeof(escaped), "\\x%02x", ch);
    sanitized += escaped;
    copied += 4;
  }

  return sanitized;
}

std::string buildLogFileName(const std::string &base) {
  std::string path = env::getEnvVar("DXMT_LOG_PATH");

  if (path == "none")
    return std::string();

  if (!path.empty() && *path.rbegin() != '/' && *path.rbegin() != '\\')
    path += '/';

  path += env::getExeBaseName() + "_" + base;
  return path;
}

} // namespace

Logger::Logger(const std::string &fileName)
    : m_minLevel(getMinLogLevel()), m_fileName(fileName) {}

Logger::~Logger() {}

void Logger::trace(const std::string &message) {
  s_instance.emitMsg(LogLevel::Trace, message);
}

void Logger::debug(const std::string &message) {
  s_instance.emitMsg(LogLevel::Debug, message);
}

void Logger::info(const std::string &message) {
  s_instance.emitMsg(LogLevel::Info, message);
}

void Logger::warn(const std::string &message) {
  s_instance.emitMsg(LogLevel::Warn, message);
}

void Logger::err(const std::string &message) {
  s_instance.emitMsg(LogLevel::Error, message);
}

void Logger::log(LogLevel level, const std::string &message) {
  s_instance.emitMsg(level, message);
}

void Logger::emitMsg(LogLevel level, const std::string &message) {
  if (level >= m_minLevel) {
    std::lock_guard<dxmt::mutex> lock(m_mutex);

    static std::array<const char *, 5> s_prefixes = {
        {"trace: ", "debug: ", "info:  ", "warn:  ", "err:   "}};

    const char *prefix = s_prefixes.at(static_cast<uint32_t>(level));

    if (!std::exchange(m_initialized, true)) {
#ifdef _WIN32
      HMODULE ntdll = enableWineLogOutput() ? GetModuleHandleA("ntdll.dll")
                                            : nullptr;

      if (ntdll)
        m_wineLogOutput = reinterpret_cast<PFN_wineLogOutput>(
            GetProcAddress(ntdll, "__wine_dbg_output"));
#endif
      auto path = getFileName(m_fileName);

      if (!path.empty()) {
        auto mode = std::ios_base::out;
        m_fileStream = std::ofstream(str::topath(path.c_str()).c_str(), mode);
      }
    }

    std::stringstream stream(message);
    std::string line;

    while (std::getline(stream, line, '\n')) {
      std::stringstream outstream;
      outstream << prefix << sanitizeLogLine(line) << std::endl;

      std::string adjusted = outstream.str();

      if (!adjusted.empty()) {
        if (m_wineLogOutput)
          m_wineLogOutput(adjusted.c_str());
        else
          std::cerr << adjusted;
      }

      if (m_fileStream)
        m_fileStream << adjusted;
    }
  }
}

std::string Logger::getFileName(const std::string &base) {
  std::string path = buildLogFileName(base);

  if (path == "none")
    return std::string();

  // Don't create a log file if we're writing to wine's console output
  if (path.empty() && m_wineLogOutput)
    return std::string();

  return path;
}

LogLevel Logger::getMinLogLevel() {
  const std::array<std::pair<const char *, LogLevel>, 6> logLevels = {{
      {"trace", LogLevel::Trace},
      {"debug", LogLevel::Debug},
      {"info", LogLevel::Info},
      {"warn", LogLevel::Warn},
      {"error", LogLevel::Error},
      {"none", LogLevel::None},
  }};

  const std::string logLevelStr = env::getEnvVar("DXMT_LOG_LEVEL");

  for (const auto &pair : logLevels) {
    if (logLevelStr == pair.first)
      return pair.second;
  }

  return LogLevel::Info;
}

FILE *openDiagnosticLog(const char *base) {
  // MetalSharp's VKD3D launcher sets DXMT_LOG_PATH to the per-title pipeline log
  // folder. Focused DXGI/D3D12/DXIL probes write separate files there instead
  // of hard-coded temp paths that would be invisible to the app.
  std::string path = buildLogFileName(base && base[0] ? base : "dxmt-diagnostic.log");

  if (path == "none")
    return nullptr;

  if (path.empty())
    return nullptr;

#ifdef _WIN32
  return _wfopen(str::topath(path.c_str()).c_str(), L"a");
#else
  return std::fopen(str::topath(path.c_str()).c_str(), "a");
#endif
}

} // namespace dxmt
